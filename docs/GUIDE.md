# BPF Arena Relay Guide

This guide describes the current repository state.

## Overview

The project implements and tests arena-backed concurrent data structures with a two-lane relay model:

1. Kernel producer inserts into lane `KU` on `lsm.s/inode_create`.
2. Userspace relay thread pops from `KU` and pushes into lane `UK`.
3. On shutdown (`Ctrl+C`), userspace triggers a uprobe-backed kernel consumer to drain `UK`.

All relay apps use the same control shape (`-v`, `-s`, `-h`) and print lane statistics.

## Implemented relay apps

- `skeleton_msqueue` -> `include/ds_msqueue.h`
- `skeleton_vyukhov` -> `include/ds_vyukhov.h`
- `skeleton_folly_spsc` -> `include/ds_folly_spsc.h`
- `skeleton_ck_fifo_spsc` -> `include/ds_ck_fifo_spsc.h`
- `skeleton_ck_ring_spsc` -> `include/ds_ck_ring_spsc.h`
- `skeleton_ck_stack_upmc` -> `include/ds_ck_stack_upmc.h`
- `skeleton_io_uring` -> `include/ds_io_uring.h`
- `skeleton_kcov` -> `include/ds_kcov.h`

## Implemented Data Structures

| **Name** | **Header** | **Relay app** | **Notes** |
|---|---|---|---|
| **io_uring Ring** | `ds_io_uring.h` | `skeleton_io_uring` | BPF arena port of io_uring's SPSC ring memory model. Power-of-2 mask indexing, u32 natural wrap, store-release/load-acquire barrier pairs, and `sq_flags` atomic field (arena_atomic_or/and). No SQ indirection array. |
| **kcov Buffer** | `ds_kcov.h` | `skeleton_kcov` | Faithful BPF arena port of Linux kcov's flat append array. area[0] = entry count, counter-first write ordering for interrupt re-entrancy safety, compiler barrier only (no hardware fences). Silent overflow drop. |

Source pairs live in `src/` as `skeleton_*.bpf.c` and `skeleton_*.c`.

## Build system behavior

- Build outputs go to `build/`.
- Intermediate artifacts go to `.output/`.
- Default build target compiles all relay apps and usertests listed in `Makefile`.

Common commands:

```bash
git submodule update --init --recursive
make
make usertest
make clean
make help
```

## Running relay binaries

Run as root because BPF load/attach requires privileges:

```bash
sudo build/skeleton_msqueue -v
```

While running, generate `inode_create` events:

```bash
touch /tmp/relay-{1..50}
```

Stop with `Ctrl+C` to trigger the uprobe consumer and print stats.

### Runtime options

All `build/skeleton_*` binaries:
- `-v` verify both lanes on exit
- `-s` print statistics (enabled by default)
- `-h` show usage

## Performance Metrics

All 8 skeleton relay programs automatically collect per-operation latency
metrics. Measurements are stored in BPF Arena shared memory and printed as a
statistics table on program exit.

### Measurement categories

Four categories match the relay lane model:

| Category | Side | Description |
|---|---|---|
| **LKMM Producer** | kernel | LSM `inode_create` handler inserting into the KU lane |
| **User Consumer** | userspace | Relay thread popping from the KU lane |
| **User Producer** | userspace | Relay thread inserting into the UK lane |
| **LKMM Consumer** | kernel | Uprobe handler popping from the UK lane |

### What is measured

Only the critical section (the lock-free algorithm portion — e.g. CAS retry
loops) is timed. Memory allocation and other overhead is excluded. Each sample
records latency in nanoseconds and whether the operation succeeded.

### Clock sources

- **BPF / kernel side**: `bpf_ktime_get_ns()`
- **Userspace side**: `clock_gettime(CLOCK_MONOTONIC)`

### Storage

A fixed 8192-entry ring buffer per category in arena memory. Wraps around when
full; running counters are maintained atomically.

```c
struct ds_metrics_store   // top-level container
  -> struct ds_metrics_ring[4]   // one per category
```

### Output format

On program exit, a table is printed per category:

- Total ops
- Successful ops
- Success rate %
- Average latency (all ops)
- Average latency (successful ops only)
- Throughput (ops/sec)

### Key API

Header: `include/ds_metrics.h`

```c
// Time an operation block and record the sample.
DS_METRICS_RECORD_OP(store, category, op_block, result_var)

// Direct recording function.
ds_metrics_record(store, category, latency_ns, success)

// Print the statistics table (userspace only).
ds_metrics_print(store, ds_name)
```

## Userspace-only tests

`usertest/*.c` are pthread tests that do not load BPF programs.

Use the maintained runner:

```bash
python3 scripts/usertests.py --build
python3 scripts/usertests.py --list
python3 scripts/usertests.py --keep-going
```

The runner validates return codes and produced/consumed key-value consistency.

## Current documentation mismatches to be aware of

- Legacy shell scripts in `scripts/test_*.sh` and `scripts/benchmark.sh` still refer to older flags (`-t`, `-o`, `-w`) and non-current binaries.
- Some Makefile help text still references a `skeleton` target that is not part of the current app list.
- `ds_api.h` provides a generic template API; concrete `ds_*` headers in this repo use per-structure signatures where needed.

Treat `scripts/usertests.py` and `build/skeleton_* --help` output as the source of truth for the current test flow.

## Repository structure

```text
bpf-arena-data-structures/
  include/     # data structures + arena/common API
  src/         # BPF relay programs and userspace drivers
  usertest/    # pthread-only test binaries
  scripts/     # test runners/helpers (usertests.py is current)
  docs/        # architecture and design notes
  Makefile
```

## Extending with a new relay data structure

To add a new relay implementation, mirror an existing `skeleton_*` pair:

1. Add `include/ds_<name>.h` with kernel/userspace helpers.
2. Add `src/skeleton_<name>.bpf.c` with:
   - arena map
   - `lsm.s/inode_create` producer into `KU`
   - `SEC("uprobe.s")` consumer for `UK`
3. Add `src/skeleton_<name>.c` with:
   - userspace allocator setup (`bpf_arena_userspace_set_range`)
   - relay thread (`KU` -> `UK`)
   - uprobe trigger symbol and shutdown logic
4. Register app names in `Makefile` (`BPF_APPS`, and optional `USERTEST_APPS`).

Use existing files as templates (`skeleton_msqueue*`, `skeleton_vyukhov*`).
