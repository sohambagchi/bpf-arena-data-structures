# Documentation Index

## Start here
- `README.md` project overview and current feature set
- `QUICKSTART.md` minimal setup and run flow
- `DOCKER_NIX_INSTALL.md` installing Nix or Docker on a fresh machine —
  including the flakes setting a stock Nix install leaves off, and the
  conflicting distro packages Docker's own instructions have you remove first;
  automated by `scripts/install-nix.sh` and `scripts/install-docker.sh`

## Core technical docs
- `docs/GUIDE.md` current architecture, build/test workflow, and extension points
- `docs/ARCHITECTURE_DIAGRAMS.md` relay-centric diagrams
- `docs/USERSPACE_ALLOCATOR.md` userspace allocator details for arena-backed tests
- `docs/LKMM_OPTIMIZATIONS.md` memory-ordering notes

## Deep-dive notes
- `docs/litmus/` litmus analyses for selected algorithms
- `docs/spec/` implementation and assembly notes
- `docs/analysis/` additional algorithm notes

## Source map
- `include/` data structure headers (`ds_msqueue`, `ds_vyukhov`, `ds_folly_spsc`, `ds_ck_*`, `ds_io_uring`, `ds_kcov`)
  - `include/ds_io_uring.h` BPF arena port of io_uring's SPSC ring memory model
  - `include/ds_kcov.h` BPF arena port of Linux kcov's flat append buffer
- `src/` relay apps (`skeleton_*.bpf.c` + `skeleton_*.c`)
  - `src/skeleton_io_uring.bpf.c` + `src/skeleton_io_uring.c` io_uring ring relay
  - `src/skeleton_kcov.bpf.c` + `src/skeleton_kcov.c` kcov buffer relay
- `usertest/` userspace-only pthread tests
- `scripts/run_all.py` pipeline endpoint: kconfig -> usertests -> runner, over
  the binaries `make` produced (it compiles nothing itself)
- `scripts/usertests.py` maintained test runner
- `scripts/check_kconfig.py` kernel-config check; `scripts/build-kernel.sh`
  builds and installs the 6.18 reference kernel
- `scripts/run-docker.sh` runs the pipeline in a BPF-capable container — the
  privileges, mounts and PID namespace a plain `docker run` leaves out, plus a
  host-kernel preflight (a container cannot supply a kernel)

## Important status note

Legacy shell scripts in `scripts/test_*.sh` and `scripts/benchmark.sh` still document an older CLI model and should be treated as templates. Use `scripts/usertests.py` for automated testing in the current tree.
