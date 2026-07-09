# BPF Arena Data Structures

This repository tests lock-free data structures backed by `BPF_MAP_TYPE_ARENA` and now also includes a `BPF_MAP_TYPE_RINGBUF` baseline for arena-independent transport comparison.

Current benchmark modes:
- **two-lane relay**: kernel producer -> userspace relay -> kernel consumer
- **one-way transport**: kernel producer -> userspace consumer (`DS_ONE_WAY=1` or `-1` on arena skeletons)
- **ringbuf baseline**: kernel producer -> userspace consumer via `BPF_MAP_TYPE_RINGBUF`

## What is implemented

### Data structures
- `include/ds_msqueue.h` (Michael-Scott queue)
- `include/ds_vyukhov.h` (Vyukhov bounded MPMC queue)
- `include/ds_folly_spsc.h` (Folly-style SPSC ring)
- `include/ds_ck_fifo_spsc.h` (CK FIFO SPSC)
- `include/ds_ck_ring_spsc.h` (CK ring SPSC)
- `include/ds_ck_stack_upmc.h` (CK stack UPMC)
- `include/ds_io_uring.h` (io_uring-style SPSC ring)
- `include/ds_iouring_liburing.h` (io_uring/liburing-faithful ring)
- `include/ds_kcov.h` (kcov-style flat buffer)
- `include/ringbuf_bench.h` (`BPF_MAP_TYPE_RINGBUF` one-way benchmark helpers)

### BPF apps
- Relay + one-way capable arena apps:
  - `build/skeleton_msqueue`
  - `build/skeleton_vyukhov`
  - `build/skeleton_folly_spsc`
  - `build/skeleton_ck_fifo_spsc`
  - `build/skeleton_ck_ring_spsc`
  - `build/skeleton_ck_stack_upmc`
  - `build/skeleton_io_uring`
  - `build/skeleton_iouring_liburing`
  - `build/skeleton_kcov`
- Ringbuf baseline:
  - `build/skeleton_ringbuf`

### Userspace-only pthread tests
- `build/usertest_msqueue`
- `build/usertest_vyukhov`
- `build/usertest_folly_spsc`
- `build/usertest_ck_fifo_spsc`
- `build/usertest_ck_ring_spsc`
- `build/usertest_ck_stack_upmc`

## Quick start

```bash
git submodule update --init --recursive
make

# Run one relay app (Ctrl+C to stop)
sudo build/skeleton_msqueue -v

# Run the same arena structure in one-way mode
DS_ONE_WAY=1 sudo build/skeleton_msqueue -v

# Run the ringbuf baseline
sudo build/skeleton_ringbuf -v
```

In another shell while it runs, create files to trigger `inode_create` events:

```bash
touch /tmp/bpf-arena-relay-{1..20}
```

On `Ctrl+C`, relay mode drains the kernel consumer and prints statistics. One-way mode and ringbuf mode stop directly after userspace delivery stats are finalized.

## CLI options

Arena `build/skeleton_*` binaries support:
- `-1` one-way kernel->userspace mode
- `-v` verify on exit
- `-s` print stats (enabled by default)
- `-h` show help

`build/skeleton_ringbuf` supports `-v`, `-s`, `-h`.

## Build and test

```bash
# Build everything into build/
make

# Build only userspace pthread tests
make usertest

# Run all userspace tests and validate output
python3 scripts/usertests.py --build

# List detected usertests
python3 scripts/usertests.py --list

# Benchmark arena one-way transports against ringbuf
sudo python3 scripts/benchmarking.py --one-way
```

## Repository layout

- `include/` data structure headers, common API, arena atomics
- `src/` BPF relay / one-way / ringbuf benchmark pairs (`skeleton_*.bpf.c` + `skeleton_*.c`)
- `usertest/` userspace-only pthread tests
- `scripts/` helpers (`usertests.py`, plus legacy shell templates)
- `docs/` architecture notes, LKMM notes, and design docs

## Notes about older docs/scripts

- The project no longer contains list/BST/bintree/mpsc skeleton apps.
- Shell scripts in `scripts/test_*.sh` and `scripts/benchmark.sh` are legacy templates and still mention older CLI flags (`-t`, `-o`, `-w`).
- The reliable automated correctness entrypoint today is `scripts/usertests.py`.
- `scripts/benchmarking.py --one-way` is the maintained benchmark entrypoint for arena-vs-ringbuf transport comparisons.

## Requirements

- Linux kernel 6.10+ with `CONFIG_BPF_ARENA=y` for arena benchmarks
- Linux kernel with `BPF_MAP_TYPE_RINGBUF` support for the ringbuf baseline
- Clang/LLVM with BPF target support (the Makefile defaults to `clang-20`, with fallback to `clang`)
- `libelf`, `zlib`, `gcc`, `make`
- root privileges for loading/attaching BPF programs

For a fast setup path, see `QUICKSTART.md`.
