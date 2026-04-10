# Tail Latency Benchmarking

This document describes the tail latency and throughput benchmarking
infrastructure: what it measures, how the measurements are collected,
and how to run and interpret benchmarks.

## Table of contents

1. [Architecture overview](#architecture-overview)
2. [Metrics infrastructure (`ds_metrics.h`)](#metrics-infrastructure)
3. [End-to-end latency measurement](#end-to-end-latency-measurement)
4. [Per-category latency measurement](#per-category-latency-measurement)
5. [Percentile computation](#percentile-computation)
6. [Machine-parseable output format](#machine-parseable-output-format)
7. [Benchmarking script (`benchmarking.py`)](#benchmarking-script)
8. [Running benchmarks](#running-benchmarks)
9. [Interpreting results](#interpreting-results)
10. [Design decisions and caveats](#design-decisions-and-caveats)
11. [File reference](#file-reference)

---

## Architecture overview

Every skeleton benchmark follows a **two-lane relay** architecture.
A message traverses four stages before it is fully consumed:

```text
Stage 1          Stage 2           Stage 3          Stage 4
LKMM Producer -> User Consumer -> User Producer -> LKMM Consumer
(LSM hook)       (relay pop KU)   (relay push UK)  (uprobe pop UK)
   kernel           userspace        userspace         kernel
```

The **end-to-end latency** of a message is the wall-clock time from the
moment it is produced (Stage 1) to the moment it is finally consumed
(Stage 4).  Only messages that complete all four stages are counted.

The `touch` command triggers `inode_create` LSM hooks, which drive Stage 1.
The userspace relay thread continuously pops from the KU queue and pushes
to the UK queue (Stages 2-3).  On shutdown, the uprobe consumer loop
drains the UK queue (Stage 4).

## Metrics infrastructure

All metrics live in `include/ds_metrics.h`, which compiles under both BPF
(`__BPF__` defined, `clang -target bpf`) and userspace (`gcc`).

### Data types

```c
struct ds_metric_sample {
    __u64 latency_ns;   /* operation latency in nanoseconds */
    __u8  success;       /* 1 = DS_SUCCESS, 0 = failure     */
    __u8  pad[7];
};

struct ds_metrics_ring {
    __u64 write_idx;          /* next write position (wraps via & mask)   */
    __u64 count;              /* total operations recorded                */
    __u64 success_count;      /* total successful operations              */
    __u64 total_latency_ns;   /* sum of all latencies                    */
    __u64 success_latency_ns; /* sum of successful-operation latencies   */
    struct ds_metric_sample samples[DS_METRICS_RING_SIZE];  /* 8192 slots */
};

struct ds_metrics_store {
    struct ds_metrics_ring rings[DS_METRICS_NUM_CATEGORIES];  /* 5 rings */
};
```

Each ring stores up to `DS_METRICS_RING_SIZE` (8192) samples in a
power-of-two circular buffer.  When the ring fills, the oldest samples
are overwritten.  Running counters (`count`, `success_count`,
`total_latency_ns`, `success_latency_ns`) are maintained atomically and
never wrap (they accumulate across the entire program lifetime).

### Categories

There are five measurement categories, each backed by its own ring:

| Enum                         | Index | Tag              | Description                              |
|------------------------------|-------|------------------|------------------------------------------|
| `DS_METRICS_LKMM_PRODUCER`  | 0     | `lkmm_producer`  | Kernel LSM hook insert into KU queue     |
| `DS_METRICS_USER_CONSUMER`  | 1     | `user_consumer`  | Userspace relay pop from KU queue        |
| `DS_METRICS_USER_PRODUCER`  | 2     | `user_producer`  | Userspace relay insert into UK queue     |
| `DS_METRICS_LKMM_CONSUMER`  | 3     | `lkmm_consumer`  | Kernel uprobe pop from UK queue          |
| `DS_METRICS_END_TO_END`     | 4     | `end_to_end`     | Full pipeline traversal (Stage 1 to 4)   |

Categories 0-3 measure the latency of a **single operation** (one insert
or one pop).  Category 4 measures the **total wall-clock time** from
production to final consumption.

### Clock source

Both BPF and userspace use the same monotonic clock:

- **BPF**: `bpf_ktime_get_ns()` (reads `CLOCK_MONOTONIC`)
- **Userspace**: `clock_gettime(CLOCK_MONOTONIC, &ts)`

These return timestamps on the same time base, so latency deltas are
directly comparable across kernel and userspace contexts.

### Recording macros

```c
/* Time a single operation and record success/failure */
DS_METRICS_RECORD_OP(store, category, {
    result = ds_xyz_insert(queue, key, value);
}, result);

/* Time a batch of N operations, record one aggregate sample */
DS_METRICS_RECORD_BATCH(store, category, batch_size, {
    result = ds_xyz_insert(queue, keys[__batch_idx], values[__batch_idx]);
}, result);

/* Record end-to-end latency (call in LKMM consumer after successful pop) */
DS_METRICS_RECORD_E2E(store, production_timestamp);
```

`DS_METRICS_MAX_BATCH_SIZE` is 256.  Batch sizes are clamped to this
upper bound (required by the BPF verifier for provably-bounded loops).

## End-to-end latency measurement

### How it works

The end-to-end latency measurement piggybacks on the existing
`struct ds_kv { __u64 key; __u64 value; }` message payload.

1. **Stage 1 (LKMM Producer)**: The LSM hook calls
   `bpf_ktime_get_ns()` and passes the result as the `value`
   parameter to the data structure's insert function.  This stores a
   production timestamp inside the message.

   ```c
   /* In every skeleton_*.bpf.c producer (SEC("lsm.s/inode_create")) */
   ts = bpf_ktime_get_ns();
   result = ds_xyz_insert_lkmm(queue, pid, ts);
   /*                                       ^^ production timestamp */
   ```

2. **Stages 2-3 (Userspace Relay)**: The relay thread pops from KU
   and pushes to UK, passing `data.key` and `data.value` through
   unchanged.  No modification occurs.

3. **Stage 4 (LKMM Consumer)**: After a successful pop, the uprobe
   computes the end-to-end latency and records it:

   ```c
   /* In every skeleton_*.bpf.c consumer (SEC("uprobe.s")) */
   ret = ds_xyz_pop_lkmm(queue, &data);
   if (ret == DS_SUCCESS) {
       DS_METRICS_RECORD_E2E(&global_metrics, data.value);
       /* data.value = original bpf_ktime_get_ns() from producer */
       /* macro computes: bpf_ktime_get_ns() - data.value        */
   }
   ```

The `DS_METRICS_RECORD_E2E` macro expands to:

```c
__u64 e2e = bpf_ktime_get_ns() - production_timestamp;
ds_metrics_record(store, DS_METRICS_END_TO_END, e2e, DS_SUCCESS);
```

### Why this is accurate

- **Same clock source**: Both the producer (`bpf_ktime_get_ns()`) and
  the consumer (`bpf_ktime_get_ns()`) read `CLOCK_MONOTONIC`.  The
  delta is exact wall-clock time.

- **Only completed messages**: The recording only happens on
  `DS_SUCCESS` in the consumer.  Messages that fail to enqueue, get
  dropped due to capacity limits, or never reach the consumer do not
  contribute any end-to-end sample.

- **No relay modification**: Every data structure stores and retrieves
  the `value` field as an opaque `__u64`.  The relay thread passes it
  through unchanged.  This has been verified for all 8 data structures.

### What is excluded

- **Failed polls**: The consumer's `DS_METRICS_RECORD_OP` for category
  `DS_METRICS_LKMM_CONSUMER` records both successes and failures, but
  `DS_METRICS_RECORD_E2E` is called only on success.

- **Incomplete pipeline traversals**: Some data structures have capacity
  limits (e.g., Vyukhov 512, Folly SPSC 512, io_uring 512, KCOV 254).
  The LKMM producer may insert ~800 messages, but only the ones that
  survive through all four stages generate end-to-end samples.

## Per-category latency measurement

Each of the four operational stages also records its own per-operation
latency.  These are independent of the end-to-end measurement.

The `DS_METRICS_RECORD_OP` macro wraps an operation block with
before/after clock reads:

```c
__u64 start = bpf_ktime_get_ns();      /* or clock_gettime in userspace */
/* ... operation ... */
__u64 elapsed = bpf_ktime_get_ns() - start;
ds_metrics_record(store, category, elapsed, result);
```

Per-category metrics include both successful and failed operations.  The
ring tracks `success_count` and `success_latency_ns` separately so that
percentile computations can filter to successful operations only.

## Percentile computation

Percentiles are computed using the **nearest-rank method**:

```c
__u64 ds_metrics_percentile(ring, percentile, success_only);
```

The algorithm:

1. Copy up to `DS_METRICS_RING_SIZE` (8192) latency samples from the
   ring into a temporary array.  If `success_only` is set, skip
   failure samples.
2. Sort the array with `qsort`.
3. Compute the index: `idx = round(pct / 100 * (n - 1))`.
4. Return `sorted[idx]`.

This runs in the userspace skeleton binary at shutdown time (inside
`ds_metrics_print` / `ds_metrics_dump_parseable`).  It does not run in
BPF context.

The two percentiles reported are:

- **p50**: median latency (50th percentile)
- **p99**: tail latency (99th percentile)

Both are computed over **successful operations only** (`success_only=1`).

## Machine-parseable output format

Every skeleton binary emits a parseable metrics block at shutdown via
`ds_metrics_dump_parseable()`.  This is called automatically from
`ds_metrics_print()`.

```text
BENCH_METRICS_BEGIN MSQueue
BENCH lkmm_producer total=800 success=800 avg_ns=1234 p50_ns=1100 p99_ns=5600 tput=810000
BENCH user_consumer total=9500 success=800 avg_ns=450 p50_ns=300 p99_ns=2100 tput=2200000
BENCH user_producer total=800 success=800 avg_ns=380 p50_ns=350 p99_ns=1800 tput=2600000
BENCH lkmm_consumer total=850 success=800 avg_ns=900 p50_ns=800 p99_ns=4200 tput=1100000
BENCH end_to_end total=800 success=800 avg_ns=55000 p50_ns=48000 p99_ns=120000 tput=18000
BENCH_ELAPSED_SEC 10.023456
BENCH_METRICS_END
```

Fields per `BENCH` line:

| Field     | Description                                              |
|-----------|----------------------------------------------------------|
| `total`   | Total operations recorded in this ring                   |
| `success` | Number of successful operations                          |
| `avg_ns`  | Average latency of successful operations (ns)            |
| `p50_ns`  | 50th percentile latency of successful operations (ns)    |
| `p99_ns`  | 99th percentile latency of successful operations (ns)    |
| `tput`    | Throughput: `success / (success_latency / 1e9)` (ops/s)  |

The `BENCH_ELAPSED_SEC` line is emitted when the caller passes a nonzero
elapsed time.  The benchmarking script also computes wall-clock time
externally.

## Benchmarking script

`scripts/benchmarking.py` orchestrates the full benchmark matrix:

```text
for each executable (8 skeleton programs by default):
  for each batch size (1, 2, 4, 8, 16, 32, 64, 128, 256):
    for each run (default 10):
      1. Spawn touch workers (batch_size files per burst)
      2. Launch skeleton executable
      3. Let it run for --duration seconds (default 10)
      4. Send SIGTERM, wait up to 15s for cleanup
      5. Parse stdout for BENCH block (or legacy table fallback)
      6. Parse "KU popped=N" for ground-truth relay count
      7. Record TrialResult
    Aggregate: median throughput, median p50, median p99
```

### Throughput computation

Throughput is determined with a priority chain:

1. **Primary**: `KU popped=N` from skeleton output / elapsed seconds.
   This is the ground-truth count of messages that traversed the full
   relay pipeline.
2. **Fallback**: The `tput` field from the best available metrics
   category (`user_consumer` > `lkmm_producer` > `user_producer` >
   `lkmm_consumer`), with a sanity cap of 10 billion msg/s.

### Latency (p50/p99) computation

Latency percentiles are sourced from the `end_to_end` category when
available, falling back to per-category metrics:

```
end_to_end > user_consumer > lkmm_producer > user_producer > lkmm_consumer
```

The final reported p50 and p99 are the **median across runs** of the
per-run p50/p99 values.

### Output formats

- **Console table**: formatted summary with columns for executable,
  batch size, run count, median throughput (msg/s), median p50 (us),
  and median p99 (us).
- **CSV** (`--output file.csv`): machine-readable with the same fields.
- **Per-run verbose** (`-v`): prints each trial's return code, elapsed
  time, and throughput.

## Running benchmarks

### Prerequisites

```bash
make            # Build all 14 targets (8 skeleton + 6 usertest)
```

### Basic usage

```bash
# Benchmark all skeleton programs with default settings
# (batch sizes 1-256, 10 runs per config, 10s per trial)
sudo python3 scripts/benchmarking.py

# Benchmark a single data structure
sudo python3 scripts/benchmarking.py skeleton_msqueue

# Custom batch sizes and fewer runs (faster iteration)
sudo python3 scripts/benchmarking.py --batch-sizes 1,16,64 --runs 3

# Shorter trial duration
sudo python3 scripts/benchmarking.py --duration 5

# Export to CSV
sudo python3 scripts/benchmarking.py --output results.csv

# Verbose per-trial output
sudo python3 scripts/benchmarking.py -v
```

### Including usertest programs

By default, only BPF skeleton programs are benchmarked.  Usertest
programs (pure userspace, no BPF) can be included:

```bash
# Add usertests alongside skeletons
sudo python3 scripts/benchmarking.py --include-usertest

# Benchmark usertests only (no sudo required)
python3 scripts/benchmarking.py --usertest-only
```

### Full option reference

| Flag                  | Default                     | Description                             |
|-----------------------|-----------------------------|-----------------------------------------|
| `executables`         | all found in `build/`       | Positional; filter to specific binaries  |
| `--batch-sizes`, `-b` | `1,2,4,8,16,32,64,128,256` | Comma-separated batch sizes              |
| `--runs`, `-r`        | `10`                        | Runs per (executable, batch_size) pair   |
| `--duration`, `-d`    | `10`                        | Seconds per skeleton trial               |
| `--output`, `-o`      | none                        | Write CSV to this path                   |
| `--show-output`       | off                         | Stream executable stdout to terminal     |
| `--include-usertest`  | off                         | Also benchmark usertest binaries         |
| `--usertest-only`     | off                         | Benchmark only usertest binaries         |
| `-v`, `--verbose`     | off                         | Print per-trial details                  |

### Typical run time

With defaults (8 executables, 9 batch sizes, 10 runs, 10s each):
8 x 9 x 10 x ~12s = ~14,400s (~4 hours).

For quick validation, use fewer runs and batch sizes:

```bash
sudo python3 scripts/benchmarking.py --batch-sizes 1,64 --runs 2 --duration 5
```

This completes in roughly 8 x 2 x 2 x ~7s = ~224s (~4 minutes).

## Interpreting results

### Example output

```text
========================================================================
              BENCHMARK RESULTS
========================================================================
Executable                       Batch Runs  Med Tput (msg/s)  Med p50 (us)  Med p99 (us)
----------------------------------------------------------------------
skeleton_ck_fifo_spsc                1   10             1,234          48.20        120.50
skeleton_ck_fifo_spsc               64   10             5,678          52.10        185.30
skeleton_msqueue                     1   10               890          62.40        210.80
skeleton_msqueue                    64   10             3,456          71.50        310.20
========================================================================
```

- **Med Tput**: Median throughput across runs.  Higher is better.
  Derived from the relay count (`KU popped=N / elapsed`).
- **Med p50**: Median of per-run 50th percentile end-to-end latency.
  This is the "typical" latency for a message traversing the full
  pipeline.  Lower is better.
- **Med p99**: Median of per-run 99th percentile end-to-end latency.
  This captures tail behavior.  Lower is better.

### What affects the numbers

- **Batch size**: Larger batches amortize per-message overhead but may
  increase tail latency due to queuing delay.
- **Data structure capacity**: Bounded structures (Vyukhov, Folly SPSC,
  CK Ring, io_uring, KCOV) drop messages when full; the throughput
  plateaus at capacity, and only messages that fit contribute to
  end-to-end latency.
- **System load**: Other processes triggering `inode_create` add noise.
  Run benchmarks on a quiet system.
- **CPU scheduling**: The relay thread and touch workers compete for
  CPU.  Pinning (`sched_setaffinity`) is done per-worker.

### Zero p50/p99

If p50 and p99 are both 0.00, it means the `end_to_end` ring had no
successful samples.  Common causes:

- The skeleton did not run long enough for messages to complete the
  full pipeline.
- The executable crashed or was killed before printing metrics
  (check `--verbose` output for nonzero return codes).
- A capacity-limited structure dropped all messages before they reached
  the consumer.

## Design decisions and caveats

### Reuse of the `value` field

The production timestamp is carried in `ds_kv.value`, which the
producer already set to `bpf_ktime_get_ns()`.  All 8 data structures
treat `value` as an opaque `__u64` (verified: pure store/load, no
interpretation).  The relay passes it through unchanged.  This means
no additional per-message overhead and no changes to the message
format or data structure internals.

### Ring buffer size

The 8192-sample ring is a fixed compile-time constant
(`DS_METRICS_RING_SIZE`).  If more than 8192 messages complete the
pipeline, the oldest samples are overwritten.  The percentile
computation operates on the most recent window of up to 8192 samples.
For programs with high throughput, this means the p50/p99 reflect
recent steady-state behavior rather than startup transients.

### Nearest-rank percentile method

The nearest-rank method was chosen for simplicity and predictability.
It requires only a sort and an index computation, with no interpolation.
For the 8192-sample window this provides sub-0.1% resolution at p99.

### Throughput from relay count

The relay count (`KU popped=N`) is used as the primary throughput
numerator because the per-category metrics rings can be inflated by
busy-loop empty-queue polls.  The relay count is always exactly the
number of messages that completed the KU pop, regardless of how many
times the relay thread polled an empty queue.

### SIGTERM timeout

After sending `SIGTERM` to a skeleton process, the benchmarking script
waits up to 15 seconds for shutdown.  The skeleton must: (1) join the
relay thread, (2) loop the uprobe consumer to drain the UK queue
(up to `uk_enqueued_count + 1024` iterations), and (3) print all
statistics including percentile computation.  The original 5-second
timeout was insufficient for large batch sizes and caused truncated
output.

### LIFO data structures

`ck_stack_upmc` and `kcov` use LIFO (stack) pop order.  Messages come
back in reverse insertion order.  This does not affect end-to-end
latency correctness (each message still carries its own production
timestamp), but it means that under bursty workloads the most recently
produced message is consumed first.

## File reference

| File | Role |
|------|------|
| `include/ds_metrics.h` | Metrics infrastructure: rings, recording macros, percentile computation, parseable output |
| `include/ds_api.h` | `struct ds_kv` definition (`key` + `value` fields), result codes |
| `src/skeleton_*.bpf.c` | BPF programs: LKMM producer (LSM hook) stamps `bpf_ktime_get_ns()` in `value`; LKMM consumer (uprobe) calls `DS_METRICS_RECORD_E2E` on successful pop |
| `src/skeleton_*.c` | Userspace skeleton binaries: relay thread (passthrough), `print_statistics()` calls `ds_metrics_print()` which emits BENCH block |
| `scripts/benchmarking.py` | Benchmark orchestration: batch-size matrix, multi-run trials, output parsing, aggregation, reporting |
