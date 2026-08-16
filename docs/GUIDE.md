# BPF Arena Relay Guide

How the relay model and its measurements work. `README.md` has the steps to
build and run it.

## Overview

Arena-backed concurrent data structures tested with a two-lane relay:

1. Kernel producer inserts into lane `KU` on `lsm.s/inode_create`.
2. Userspace relay thread pops from `KU` and pushes into lane `UK`.
3. On shutdown (`Ctrl+C`), userspace triggers a uprobe-backed kernel consumer
   to drain `UK`.

Each relay is a `src/skeleton_*.bpf.c` + `src/skeleton_*.c` pair over one
`include/ds_*.h` header. Build outputs go to `build/`, intermediates to
`.output/`.

Two of the eight are ports of existing kernel designs rather than published
algorithms:

| Name | Header | Notes |
|---|---|---|
| io_uring Ring | `ds_io_uring.h` | io_uring's SPSC ring memory model: power-of-2 mask indexing, u32 natural wrap, store-release/load-acquire pairs, `sq_flags` as an atomic field. No SQ indirection array. |
| kcov Buffer | `ds_kcov.h` | kcov's flat append array: `area[0]` is the entry count, counter-first write ordering for interrupt re-entrancy, compiler barrier only, silent overflow drop. |

## Performance metrics

All eight relays collect per-operation latency in arena memory and print a
table on exit.

### Categories

| Category | Side | Description |
|---|---|---|
| **LKMM Producer** | kernel | LSM `inode_create` handler inserting into the KU lane |
| **User Consumer** | userspace | Relay thread popping from the KU lane |
| **User Producer** | userspace | Relay thread inserting into the UK lane |
| **LKMM Consumer** | kernel | Uprobe handler popping from the UK lane |

### What is measured

Only the critical section — the lock-free algorithm portion, e.g. CAS retry
loops — is timed; allocation and other overhead is excluded. Each sample
records latency in nanoseconds and whether the operation succeeded.

Clock sources: `bpf_ktime_get_ns()` on the kernel side,
`clock_gettime(CLOCK_MONOTONIC)` in userspace. Storage is a fixed 8192-entry
ring buffer per category, wrapping when full, with atomic running counters:

```c
struct ds_metrics_store   // top-level container
  -> struct ds_metrics_ring[4]   // one per category
```

The printed table gives, per category: total ops, successful ops, success
rate, average latency over all ops, average over successful ops only, and
throughput. `scripts/runner.py` parses that table into
`build/runner_metrics_<timestamp>.csv`.

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

`usertest/*.c` are pthread tests that load no BPF programs.
`scripts/usertests.py` runs them and validates return codes plus
produced/consumed key-value consistency:

```bash
python3 scripts/usertests.py --build
python3 scripts/usertests.py --list
```

## Adding a relay data structure

Mirror an existing `skeleton_*` pair (`skeleton_msqueue*` is the reference):

1. Add `include/ds_<name>.h` with kernel and userspace helpers. `ds_api.h` is a
   generic template API; concrete headers use per-structure signatures where
   needed.
2. Add `src/skeleton_<name>.bpf.c` with the arena map, an
   `lsm.s/inode_create` producer into `KU`, and a `SEC("uprobe.s")` consumer
   for `UK`.
3. Add `src/skeleton_<name>.c` with the userspace allocator setup
   (`bpf_arena_userspace_set_range`), the `KU` -> `UK` relay thread, and the
   uprobe trigger symbol plus shutdown logic.
4. Register the app in `Makefile` (`BPF_APPS`, and `USERTEST_APPS` if it has a
   pthread test).
