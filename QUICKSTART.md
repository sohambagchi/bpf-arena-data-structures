# Quick Start

## 1) Prerequisites

```bash
uname -r
zgrep CONFIG_BPF_ARENA /proc/config.gz
clang --version
```

You need kernel 6.10+ with `CONFIG_BPF_ARENA=y` for arena benchmarks. The ringbuf baseline also requires `BPF_MAP_TYPE_RINGBUF` support.

## 2) Build

```bash
git submodule update --init --recursive
make
```

Binaries are placed in `build/`.

## 3) Run one relay app

```bash
sudo build/skeleton_msqueue -v
```

Or run the same structure in one-way mode for arena-vs-ringbuf comparison:

```bash
DS_ONE_WAY=1 sudo build/skeleton_msqueue -v
```

While it is running, create files in another shell to generate kernel producer events:

```bash
touch /tmp/bpf-arena-smoke-{1..20}
```

Press `Ctrl+C` in the relay app. It triggers the uprobe consumer and prints per-lane stats.

## 4) Try other apps

```bash
sudo build/skeleton_vyukhov -v
sudo build/skeleton_folly_spsc -v
sudo build/skeleton_ck_fifo_spsc -v
sudo build/skeleton_ck_ring_spsc -v
sudo build/skeleton_ck_stack_upmc -v
sudo build/skeleton_io_uring -v
sudo build/skeleton_iouring_liburing -v
sudo build/skeleton_kcov -v
sudo build/skeleton_ringbuf -v
```

## 5) Run userspace-only tests

```bash
python3 scripts/usertests.py --build
```

Optional:

```bash
python3 scripts/usertests.py --list
python3 scripts/usertests.py --keep-going
```

## Current state caveats

- `scripts/test_smoke.sh`, `scripts/test_stress.sh`, `scripts/test_verify.sh`, and `scripts/benchmark.sh` are legacy templates and still reference older flags not used by current relay binaries.
- The maintained automated correctness runner is `scripts/usertests.py`.
- The maintained benchmark runner for arena-vs-ringbuf comparison is `scripts/benchmarking.py --one-way`.
