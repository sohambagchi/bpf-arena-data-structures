# Architecture Diagrams (Current Relay Model)

## End-to-end data path

```text
                inode_create event (e.g., touch /tmp/x)
                                 |
                                 v
  +---------------------------------------------------------------+
  |                     Kernel BPF Program                        |
  |  SEC("lsm.s/inode_create") producer                          |
  |  insert(pid, timestamp) -> KU lane                            |
  +---------------------------------------------------------------+
                                 |
                                 v
  +---------------------------------------------------------------+
  |             BPF_MAP_TYPE_ARENA (mmap shared region)           |
  |   KU lane (kernel->user)      UK lane (user->kernel)          |
  +---------------------------------------------------------------+
                 ^                                  |
                 |                                  v
  +---------------------------------------------------------------+
  |                 Userspace relay binary (build/skeleton_*)     |
  |  relay thread: pop KU -> push UK                              |
  |  Ctrl+C: trigger uprobe symbol in same process                |
  +---------------------------------------------------------------+
                                 |
                                 v
  +---------------------------------------------------------------+
  |                     Kernel BPF Program                        |
  |  SEC("uprobe.s") consumer                                     |
  |  pop UK, update consume counters                              |
  +---------------------------------------------------------------+
```

## Control flow per relay binary

```text
main()
  -> open/load skeleton
  -> setup userspace allocator range
  -> attach LSM + uprobe programs
  -> spawn relay thread
  -> wait for SIGINT/SIGTERM
  -> join relay thread
  -> trigger uprobe consumer loop
  -> optional verify (-v)
  -> print statistics (-s, default on)
```

## Build pipeline

```text
src/skeleton_<name>.bpf.c
  -> clang -target bpf
  -> .output/skeleton_<name>.bpf.o
  -> bpftool gen skeleton
  -> .output/skeleton_<name>.skel.h

src/skeleton_<name>.c
  + .output/skeleton_<name>.skel.h
  + include/ds_<name>.h
  -> build/skeleton_<name>
```

## Current app matrix

```text
Algorithm          Relay binary                   Header
---------          ------------                   ------
MS Queue           build/skeleton_msqueue         include/ds_msqueue.h
Vyukhov MPMC       build/skeleton_vyukhov         include/ds_vyukhov.h
Folly SPSC         build/skeleton_folly_spsc      include/ds_folly_spsc.h
CK FIFO SPSC       build/skeleton_ck_fifo_spsc    include/ds_ck_fifo_spsc.h
CK Ring SPSC       build/skeleton_ck_ring_spsc    include/ds_ck_ring_spsc.h
CK Stack UPMC      build/skeleton_ck_stack_upmc   include/ds_ck_stack_upmc.h
```

## Testing paths

```text
BPF relay path:
  sudo build/skeleton_<name> [-v]

Userspace-only path:
  python3 scripts/usertests.py --build
```

Note: legacy `scripts/test_*.sh` and `scripts/benchmark.sh` are template scripts from an older CLI model.
