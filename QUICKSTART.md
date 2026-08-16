# Quick Start

## 1) Prerequisites

```bash
uname -r
zgrep CONFIG_BPF_ARENA /proc/config.gz
clang --version
```

You need kernel 6.10+ with `CONFIG_BPF_ARENA=y`.

To get the toolchain instead of installing it piecemeal, `nix develop` gives you
all of it in one shell (clang-20, gcc, python3, plus the kernel build tools).
`DOCKER_NIX_INSTALL.md` covers installing Nix itself, or run
`scripts/install-nix.sh`. For the Docker route instead, `scripts/install-docker.sh`
(Ubuntu) does the same for Docker Engine.

## 2) Build

```bash
git submodule update --init --recursive
make
```

Binaries are placed in `build/`. Build as your normal user — never under
`sudo`, which resets the environment and takes the dev shell's compiler
settings with it (`make` then fails on `libelf.h: No such file or directory`).
Everything below that needs root needs it only to *run* what this step built.

## 3) Run one relay app

```bash
sudo build/skeleton_msqueue -v
```

While it is running, create files in another shell to generate kernel producer events:

```bash
touch /tmp/bpf-arena-smoke-{1..20}
```

Press `Ctrl+C` in the relay app. It triggers the uprobe consumer and prints per-lane stats.

## 4) Try other relay apps

```bash
sudo build/skeleton_vyukhov -v
sudo build/skeleton_folly_spsc -v
sudo build/skeleton_ck_fifo_spsc -v
sudo build/skeleton_ck_ring_spsc -v
sudo build/skeleton_ck_stack_upmc -v
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

## 6) Run the whole pipeline

```bash
sudo python3 scripts/run_all.py
```

Kernel-config check, userspace tests, then the eight relays with a ranking and
a metrics CSV. It runs the binaries from step 2 and compiles nothing; if they
are missing it says so up front and names each one. `README.md` has the full
walkthrough.

In Docker, use `scripts/run-docker.sh -- python3 scripts/run_all.py` instead: a
plain `docker run` cannot load BPF programs however root you are inside it, and
the wrapper adds the capabilities and mounts it leaves out.

## Current state caveats

- `scripts/test_smoke.sh`, `scripts/test_stress.sh`, `scripts/test_verify.sh`, and `scripts/benchmark.sh` are legacy templates and still reference older flags not used by current relay binaries.
- The maintained automated test runner is `scripts/usertests.py`.
