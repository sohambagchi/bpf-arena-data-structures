# BPF Arena Data Structures

This repository tests lock-free data structures backed by `BPF_MAP_TYPE_ARENA` using a kernel->userspace->kernel relay flow.

Current focus: eight relay implementations, each with:
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
- `include/ds_io_uring.h` (io_uring-style ring; models the design, does not call the syscall)
- `include/ds_kcov.h` (kcov-style buffer; models the design, does not open `/sys/kernel/debug/kcov`)

### BPF relay apps
- `build/skeleton_msqueue`
- `build/skeleton_vyukhov`
- `build/skeleton_folly_spsc`
- `build/skeleton_ck_fifo_spsc`
- `build/skeleton_ck_ring_spsc`
- `build/skeleton_ck_stack_upmc`
- `build/skeleton_io_uring`
- `build/skeleton_kcov`

### Userspace-only pthread tests

Six of the eight have a userspace-only counterpart:

- `build/usertest_msqueue`
- `build/usertest_vyukhov`
- `build/usertest_folly_spsc`
- `build/usertest_ck_fifo_spsc`
- `build/usertest_ck_ring_spsc`
- `build/usertest_ck_stack_upmc`

## Running the artifact: four steps

Steps 1–3 prepare the machine; step 4 is the experiment. Step 2 takes 20–60
minutes and needs ~22 GB free, and step 3 reboots the machine.

Anything that does not go to plan is covered in the [FAQ](#faq) at the end.

### Step 1 — Initialize the environment

Both environments provide the toolchain (clang-20, gcc, libelf, zlib, libbpf,
python3). Pick one:

```bash
git submodule update --init --recursive   # required by both

# Nix
nix develop                               # toolchain + kernel tools, repo in place

# Docker
docker build -t bpf-arena-ds .
docker run --rm -it -v "$PWD:/artifact" bpf-arena-ds
```

If the tool is not installed, `DOCKER_NIX_INSTALL.md` has the full procedure for
both. The short version:

- **Nix** — `scripts/install-nix.sh` installs it and enables the flakes support
  `nix develop` needs (a stock install has flakes off, so `nix develop` fails
  until `experimental-features` is set). By hand:
  <https://nixos.org/download/>, or the Determinate Systems installer at
  <https://github.com/DeterminateSystems/nix-installer>, which enables flakes
  for you. Needs Nix >= 2.27.
- **Docker** — <https://docs.docker.com/engine/install/> (Linux), or Docker
  Desktop at <https://docs.docker.com/desktop/>.

### Step 2 — Build and install the kernel

One command downloads Linux 6.18.44, verifies its SHA-256, configures it with
this repo's BPF fragment, builds it, and installs it as `6.18.44-bpf-arena`:

```bash
./scripts/build-kernel.sh --base running -y
```

Run it as a normal user — it requests `sudo` itself, and only for the install
step. `--base running` starts from this machine's own config, which is what you
want when booting the result on real hardware. Other flags:

- `--base full` — the paper's reference `.config`
  (`kernel/configs/full-6.18.2-bpf.config`), verbatim.
- `--base defconfig` (default) — generic x86.
- `-y` — skip the confirmation prompt. `-j N` — parallelism (default: `nproc`).
  `--no-install` — build only, touch nothing outside the tree.

It checks every dependency up front, names the missing package for
apt/dnf/pacman, and validates the generated `.config` with `check_kconfig.py`
*before* the compile rather than after.

### Step 3 — Reboot

Boot the `6.18.44-bpf-arena` entry, then confirm:

```bash
uname -r                        # 6.18.44-bpf-arena
cat /sys/kernel/security/lsm    # must contain 'bpf'
```

### Step 4 — Run the pipeline

One endpoint runs kernel-config check, build, userspace tests, and the relay
experiment in order. It needs root: the relays load BPF programs and read
`trace_pipe`.

```bash
sudo python3 scripts/run_all.py
```

Expected output, abridged (the ops/sec figures below are illustrative
formatting, not measurements from any particular machine — absolute numbers and
the resulting order vary by hardware):

```
========================================================================
BPF Arena Data Structures -- full pipeline
========================================================================
Repo root : /path/to/bpf-arena-data-structures
Stages    : kconfig -> build -> usertests -> runner
On failure: stop

========================================================================
STAGE: kconfig
$ python3 scripts/check_kconfig.py
========================================================================
Checking /boot/config-6.18.44-bpf-arena (2431 set, 1210 unset)
  ...
OK: all requirements met

========================================================================
STAGE: build
$ make all -j<nproc>
========================================================================
  ...
Build complete! Built applications:
  - build/skeleton_msqueue
  ...

========================================================================
STAGE: usertests
$ python3 scripts/usertests.py --keep-going
========================================================================
  ...
[usertest_msqueue] ok
[usertest_vyukhov] ok
[usertest_folly_spsc] ok
[usertest_ck_fifo_spsc] ok
[usertest_ck_ring_spsc] ok
[usertest_ck_stack_upmc] ok

========================================================================
STAGE: runner
$ python3 scripts/runner.py --csv-dir build
========================================================================
  ...
  Trace validation PASSED: 1284 consumed events matched initial touch input PIDs
  Aggregate throughput: 7,553,540 ops/sec over 3,915 successful ops
  ...

Raw metrics CSV: build/runner_metrics_20260814-113052.csv

========================================================================
PERFORMANCE RANKING (slowest first)
========================================================================
Ranked by aggregate successful ops/sec across all four relay stages
(LKMM producer, user consumer, user producer, LKMM consumer).

 #  Data structure              Ops/sec  End-to-end(ns)     Ops-OK
 1. MSQueue                     866,971           4,630      1,860
 2. Vyukhov MPMC              1,204,318           3,322      2,140
 3. KCOV Buffer               2,918,440           1,371      3,006
 4. IO_URING Ring             3,344,102           1,196      3,171
 5. CK Stack UPMC             4,010,775             997      3,288
 6. CK FIFO SPSC              5,772,301             693      3,590
 7. Folly SPSC                6,918,004             578      3,802
 8. CK Ring SPSC              7,553,540             530      3,915

Ascending performance order (slowest first):
  MSQueue < Vyukhov MPMC < KCOV Buffer < IO_URING Ring < CK Stack UPMC < CK FIFO SPSC < Folly SPSC < CK Ring SPSC

========================================================================
PIPELINE SUMMARY
========================================================================
  [PASS] kconfig          0.1s
  [PASS] build           47.9s
  [PASS] usertests       38.3s
  [PASS] runner          92.4s

  Raw metrics CSV:
    build/runner_metrics_20260814-113052.csv

========================================================================
==  PIPELINE PASSED
==
==  All 4 stage(s) completed successfully.
========================================================================
```

Exit status is 0 only if every stage passed.

The per-stage raw numbers — total ops, successful ops, average latency,
summed successful latency, and throughput for each of the four relay lanes —
land in `build/runner_metrics_<timestamp>.csv`, one row per
(data structure, lane) plus an `ALL` aggregate row.

Useful variations:

```bash
python3 scripts/run_all.py --dry-run            # print the stage commands only
python3 scripts/run_all.py --only usertests     # one stage (repeatable)
python3 scripts/run_all.py --skip kconfig --skip build
python3 scripts/run_all.py --keep-going         # run every stage, report at end
sudo python3 scripts/run_all.py --csv-dir results --show-output
sudo python3 scripts/run_all.py skeleton_msqueue skeleton_ck_ring_spsc
```

## Running a single relay by hand

```bash
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

`scripts/run_all.py` (step 4 above) wraps all of these. To drive the individual
stages by hand:

```bash
# Build everything into build/
make

# Build only userspace pthread tests
make usertest

# Run all userspace tests and validate output
python3 scripts/usertests.py --build

# List detected usertests
python3 scripts/usertests.py --list

# Run the eight relays, rank them, and write the metrics CSV
sudo python3 scripts/runner.py
```

## Repository layout

- `include/` data structure headers, common API, arena atomics
- `src/` BPF relay pairs (`skeleton_*.bpf.c` + `skeleton_*.c`)
- `usertest/` userspace-only pthread tests
- `scripts/` `run_all.py` (pipeline endpoint), `check_kconfig.py`,
  `build-kernel.sh`, `runner.py`, `usertests.py`, plus legacy shell templates
- `docs/` architecture notes, LKMM notes, and design docs

## Reproducible environments

Two packaged environments cover the toolchain.

### Nix (`flake.nix`)

```bash
nix develop                  # one shell: clang-20, gcc, libelf, zlib, openssl,
                             # python3, plus bison/flex/bc/pahole/QEMU for the
                             # kernel step

make                         # builds into build/

nix build .#artifact         # same 14 binaries, hermetically
nix build .#kernel           # Linux 6.18 from kernel/configs/full-6.18.2-bpf.config
nix run .#vm                 # boot that kernel in QEMU, repo mounted at /repo
nix flake check              # builds the artifact + validates the reference .config
```

`nix develop` is a single shell that covers every step, `scripts/build-kernel.sh`
included — there is no separate kernel shell to remember (`.#kernel` still
resolves, as an alias for the default). The one thing kept out of it is
`nix run .#vm`, which is a booted machine rather than a set of tools: it builds
`full-6.18.2-bpf.config` against the 6.18 source, boots it under QEMU with
`lsm=...,bpf` on the command line, and drops you at a root shell with the
toolchain on PATH. That is the only environment here that gives you the paper's
kernel at runtime.

Needs Nix >= 2.27 with flakes enabled (`inputs.self.submodules` pulls in
`third_party/`), and `git submodule update --init --recursive` beforehand. If
Nix is not installed yet, or `nix develop` fails with "experimental Nix feature
'nix-command' is disabled", see `DOCKER_NIX_INSTALL.md` — or run
`scripts/install-nix.sh`, which does the whole install-and-enable-flakes dance.

### Docker (`Dockerfile`)

```bash
docker build -t bpf-arena-ds .
docker run --rm -it -v "$PWD:/artifact" bpf-arena-ds   # build + userspace tests
```

### Building a kernel directly (`scripts/build-kernel.sh`)

This is step 2 above. It installs as `6.18.44-bpf-arena`, so nothing already on
the system is overwritten, and the existing kernel remains bootable.

## Requirements

- Linux kernel 6.10+, configured per `kernel/configs/delta-bpf-arena.config` (below)
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
    /path/to/bpf-arena-data-structures/kernel/configs/delta-bpf-arena.config
make olddefconfig
```

Or install the fragment into the kernel tree and use the built-in target:

```bash
cp kernel/configs/delta-bpf-arena.config \
    /path/to/linux/kernel/configs/bpf-arena.config
cd /path/to/linux && make bpf-arena.config
```

## FAQ

### Can I skip steps 2 and 3?

Yes, if the kernel you are already running satisfies the requirements. Check
before spending an hour on a kernel build:

```bash
python3 scripts/check_kconfig.py
```

If it prints `OK: all requirements met`, go straight to step 4.

### `bpf` is missing from `/sys/kernel/security/lsm`

Then the `lsm.s/inode_create` producer cannot attach and every relay fails.
`/sys/kernel/security/lsm` is what the kernel *actually* activated, and it is
not always what `CONFIG_LSM` says: an `lsm=` kernel command line replaces that
list outright. Fixing it is a command line change plus a reboot — no second
kernel build.

`check_kconfig.py` detects this and prints the exact line to set:

```bash
python3 scripts/check_kconfig.py --print-lsm-fix
```

It reads this machine's state rather than emitting a canned string, which
matters: **`lsm=` replaces the compiled-in list — it does not add to it.**
A short list silently disables every LSM you left out (AppArmor on Ubuntu,
SELinux on Fedora), so the fix starts from the list already in effect — the
`lsm=` line if your bootloader pins one, otherwise `CONFIG_LSM` — and appends
`,bpf`. The output also names where to put it for GRUB, grubby, systemd-boot,
and NixOS.

The kernel this repo builds already lists `bpf` in `CONFIG_LSM`, so you only
hit this on a distro kernel, or if your bootloader pins its own `lsm=` line.

After rebooting, `capability` and `ima` may appear in the active list without
being on your command line. That is normal and not a sign the setting was
ignored.

### `check_kconfig.py` passes but the relays still fail to attach

Run it against the *running* kernel rather than a config file. Given a path it
does text analysis only; given no argument it also checks runtime state
(the LSM list above) and says `[config text only]` in the header when it
cannot. `--runtime` forces the runtime check on.

Also confirm `/sys/kernel/btf/vmlinux` exists: `CONFIG_DEBUG_INFO_BTF=y` is a
*runtime* requirement, not just a build one. libbpf reads it to attach the
BTF-typed programs even though `third_party/vmlinux.h` is checked in.

### The new kernel does not appear in the boot menu

Regenerate it, then reboot:

```bash
sudo update-grub                              # Debian/Ubuntu
sudo grubby --info=ALL                        # Fedora/RHEL (verify the entry)
sudo grub-mkconfig -o /boot/grub/grub.cfg     # Arch
```

### The new kernel does not boot

Boot your previous kernel — it is untouched, and the new one installs under a
distinct `6.18.44-bpf-arena` name specifically so it cannot displace it.

The usual cause is `--base defconfig`, which is generic x86 and may omit the
storage or filesystem drivers your root device needs. Rebuild with
`--base running`, which starts from your machine's working config.

### A pipeline stage failed — what do I get?

A boxed `STAGE FAILED` banner naming the command, exit status, the last 20
lines of that stage's output, and the fix to try; the failure is repeated at
the bottom of the summary so it survives a long scrollback. The stages that
did not run because of it are listed too.

By default the pipeline stops at the first failure, since each stage depends on
the ones before it — an unmet kernel config makes the relays unattachable, and
a failing usertest makes their numbers meaningless. `--keep-going` runs
everything and reports at the end; `--only <stage>` re-runs one stage.

### Why does the BPF half not work under Docker?

A container shares the host's kernel; there is no such thing as a Docker image
with Linux 6.18 inside it. So the relays run only against a compliant *host*
kernel, and then only with:

```bash
docker run --rm -it --privileged --pid=host \
  -v "$PWD:/artifact" \
  -v /sys/kernel/debug:/sys/kernel/debug \
  -v /sys/fs/bpf:/sys/fs/bpf \
  bpf-arena-ds
```

`--privileged` is needed for CAP_BPF/CAP_PERFMON and because the default
seccomp profile blocks `bpf(2)`. On Docker Desktop (macOS/Windows) it cannot
work at all — the host "kernel" there is Docker's own LinuxKit VM. To get a
6.18 kernel out of the Docker path, build one in the container with
`./scripts/build-kernel.sh --no-install` and boot it under QEMU; see the header
comment in `Dockerfile`. The equivalent on the Nix side is `nix run .#vm`,
which skips steps 2 and 3 entirely.

### `build-kernel.sh` refuses to install on NixOS

By design: NixOS manages kernels declaratively and has no writable `/boot`
layout for `make install` to use. Use `nix build .#kernel`, or `nix run .#vm`
to boot the reference kernel under QEMU.

### `build-kernel.sh` exited with a number — what does it mean?

Every failure has a documented exit code and the table is at the top of the
script (1 usage, 2 unsupported platform, 3 missing dependency, 4 disk space,
5 download/checksum, 6 extract, 7 configuration, 8 config does not satisfy the
artifact, 9 compile, 10 install).

### Is there a `CONFIG_BPF_ARENA`?

No. Arena support follows from `CONFIG_BPF_SYSCALL` on any 64-bit MMU arch —
upstream `kernel/bpf/Makefile` gates `arena.o` on `MMU && 64BIT` plus
`BPF_SYSCALL`. Earlier revisions of this README told you to look for a menu
entry that does not exist.

### Do I need `CONFIG_KCOV` and `CONFIG_IO_URING`?

No. The `kcov` and `io_uring` skeletons are arena rings modeled on those
subsystems' designs; neither calls into the real subsystem. They are in the
fragment so one kernel can also host comparisons against the genuine article.

If you do enable `CONFIG_KCOV` for that, leave `CONFIG_KCOV_INSTRUMENT_ALL`
off — it instruments every kernel function and will skew the `ds_metrics`
latency and throughput numbers.

### Why do only six of the eight have userspace tests?

The `io_uring` and `kcov` skeletons exist only as arena relays; there is no
pthread-only counterpart for them. `scripts/usertests.py --list` prints what is
actually detected, read from `USERTEST_APPS` in the Makefile.

### What about the older scripts and docs?

- The project no longer contains list/BST/bintree/mpsc skeleton apps.
- Shell scripts in `scripts/test_*.sh` and `scripts/benchmark.sh` are legacy
  templates and still mention older CLI flags (`-t`, `-o`, `-w`).
- The automated entrypoint today is `scripts/run_all.py`, which orchestrates
  `check_kconfig.py`, `make`, `usertests.py`, and `runner.py`.
