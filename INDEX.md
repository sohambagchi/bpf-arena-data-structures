# Documentation Index

## Start here
- `README.md` project overview and current feature set
- `QUICKSTART.md` minimal setup and run flow

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
- `include/` data structure headers (`ds_msqueue`, `ds_vyukhov`, `ds_folly_spsc`, `ds_ck_*`)
- `src/` relay apps (`skeleton_*.bpf.c` + `skeleton_*.c`)
- `usertest/` userspace-only pthread tests
- `scripts/usertests.py` maintained test runner

## Important status note

Legacy shell scripts in `scripts/test_*.sh` and `scripts/benchmark.sh` still document an older CLI model and should be treated as templates. Use `scripts/usertests.py` for automated testing in the current tree.
