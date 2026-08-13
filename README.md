# BPF Arena Data Structures

This repository tests lock-free data structures backed by `BPF_MAP_TYPE_ARENA` using a kernel->userspace->kernel relay flow.

Current focus: six relay implementations, each with:
- a BPF program that produces into a kernel->user lane on `lsm.s/inode_create`
- a userspace relay thread that moves items into a user->kernel lane
- a uprobe-triggered kernel consumer for the return lane

## What is implemented

### Data structures
- `include/ds_msqueue.h` (Michael-Scott queue)
- `include/ds_vyukhov.h` (Vyukhov bounded MPMC queue)
- `include/ds_folly_spsc.h` (Folly-style SPSC ring)
- `include/ds_ck_fifo_spsc.h` (CK FIFO SPSC)
- `include/ds_ck_ring_spsc.h` (CK ring SPSC)
- `include/ds_ck_stack_upmc.h` (CK stack UPMC)

### BPF relay apps
- `build/skeleton_msqueue`
- `build/skeleton_vyukhov`
- `build/skeleton_folly_spsc`
- `build/skeleton_ck_fifo_spsc`
- `build/skeleton_ck_ring_spsc`
- `build/skeleton_ck_stack_upmc`

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
```

In another shell while it runs, create files to trigger `inode_create` events:

```bash
touch /tmp/bpf-arena-relay-{1..20}
```

On `Ctrl+C`, the app triggers its uprobe consumer and prints statistics.

## CLI options

All `build/skeleton_*` relay binaries support:
- `-v` verify both lanes on exit
- `-s` print stats (enabled by default)
- `-h` show help

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
```

## Repository layout

- `include/` data structure headers, common API, arena atomics
- `src/` BPF relay pairs (`skeleton_*.bpf.c` + `skeleton_*.c`)
- `usertest/` userspace-only pthread tests
- `scripts/` helpers (`usertests.py`, plus legacy shell templates)
- `docs/` architecture notes, LKMM notes, and design docs

## Notes about older docs/scripts

- The project no longer contains list/BST/bintree/mpsc skeleton apps.
- Shell scripts in `scripts/test_*.sh` and `scripts/benchmark.sh` are legacy templates and still mention older CLI flags (`-t`, `-o`, `-w`).
- The reliable automated test entrypoint today is `scripts/usertests.py`.

## Requirements

- Linux kernel 6.10+, configured per `kernel/configs/bpf-arena.config` (below)
- Clang/LLVM with BPF target support (the Makefile defaults to `clang-20`, with fallback to `clang`)
- `libelf`, `zlib`, `gcc`, `make`
- root privileges for loading/attaching BPF programs

For a fast setup path, see `QUICKSTART.md`.

## Kernel configuration

Check whether the running kernel already works:

```bash
python3 scripts/check_kconfig.py            # reads /boot/config-$(uname -r)
python3 scripts/check_kconfig.py path/to/.config
```

It exits non-zero and names what is missing. To build a kernel that
satisfies it, merge the fragment with the kernel's own tooling:

```bash
cd /path/to/linux
./scripts/kconfig/merge_config.sh -m .config \
    /path/to/bpf-arena-data-structures/kernel/configs/bpf-arena.config
make olddefconfig
```

Or install the fragment into the kernel tree and use the built-in target:

```bash
cp kernel/configs/bpf-arena.config /path/to/linux/kernel/configs/
cd /path/to/linux && make bpf-arena.config
```

Four things that are easy to get wrong:

- **There is no `CONFIG_BPF_ARENA`.** Arena support follows from
  `CONFIG_BPF_SYSCALL` on any 64-bit MMU arch — upstream
  `kernel/bpf/Makefile` gates `arena.o` on `MMU && 64BIT` plus
  `BPF_SYSCALL`. Earlier revisions of this README told you to look for a
  menu entry that does not exist.
- **`CONFIG_BPF_LSM=y` alone is not enough.** `CONFIG_LSM` must also list
  `bpf`, otherwise the `lsm.s/inode_create` programs fail to attach at
  runtime on a kernel that otherwise looks correct. Booting with
  `lsm=<your list>,bpf` works without a rebuild.
- **`CONFIG_DEBUG_INFO_BTF=y` is a runtime requirement, not just a build
  one.** libbpf reads `/sys/kernel/btf/vmlinux` to attach the BTF-typed
  programs, even though `third_party/vmlinux.h` is checked in.
- **`CONFIG_KCOV` and `CONFIG_IO_URING` are optional.** The `kcov` and
  `io_uring` skeletons are arena rings modeled on those subsystems'
  designs; neither calls into the real subsystem. They are in the
  fragment so one kernel can also host comparisons against the genuine
  article. If you enable `CONFIG_KCOV` for that, leave
  `CONFIG_KCOV_INSTRUMENT_ALL` off — it instruments every kernel function
  and will skew the `ds_metrics` latency and throughput numbers.
