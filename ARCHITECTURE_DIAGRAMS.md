# Framework Architecture - Visual Overview

## System Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                         USER SPACE                                   │
│                                                                      │
│  ┌──────────────────────────────────────────────────────────────┐  │
│  │                     skeleton.c                                │  │
│  │                                                               │  │
│  │  ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌─────────┐        │  │
│  │  │Thread 0 │  │Thread 1 │  │Thread 2 │  │Thread N │        │  │
│  │  │         │  │         │  │         │  │         │        │  │
│  │  │ Insert  │  │ Search  │  │ Delete  │  │  Mixed  │        │  │
│  │  └────┬────┘  └────┬────┘  └────┬────┘  └────┬────┘        │  │
│  │       │            │            │            │               │  │
│  │       └────────────┴────────────┴────────────┘               │  │
│  │                         │                                     │  │
│  │                   Direct Access                               │  │
│  │                    (no syscalls)                              │  │
│  └─────────────────────────┼─────────────────────────────────────┘  │
│                            │                                         │
│                            ↓                                         │
├─────────────────────────────────────────────────────────────────────┤
│                       BPF ARENA                                      │
│              (Shared Memory - up to 4GB)                             │
│                                                                      │
│  ┌──────────────────────────────────────────────────────────────┐  │
│  │  Page 0: Data Structure Head                                 │  │
│  │    • ds_list_head (or ds_tree_head, etc.)                    │  │
│  │    • Statistics counters                                     │  │
│  │    • Control variables                                       │  │
│  ├──────────────────────────────────────────────────────────────┤  │
│  │  Pages 1-N: Dynamically Allocated Nodes                      │  │
│  │                                                               │  │
│  │    Node 1          Node 2          Node 3                    │  │
│  │  ┌────────┐      ┌────────┐      ┌────────┐                 │  │
│  │  │key: 42 │  ┌──→│key: 73 │  ┌──→│key: 91 │                 │  │
│  │  │val: 84 │  │   │val: 146│  │   │val: 182│                 │  │
│  │  │next: ───┼──┘   │next: ───┼──┘   │next:NULL                │  │
│  │  └────────┘      └────────┘      └────────┘                 │  │
│  │                                                               │  │
│  │  Memory allocated by: bpf_arena_alloc()                      │  │
│  │  Memory freed by: bpf_arena_free()                           │  │
│  └──────────────────────────────────────────────────────────────┘  │
│                            ↑                                         │
│                      Access from both:                               │
│                            │                                         │
├─────────────────────────────────────────────────────────────────────┤
│                       KERNEL SPACE                                   │
│                                                                      │
│  ┌──────────────────────────────────────────────────────────────┐  │
│  │                   skeleton.bpf.c                              │  │
│  │                                                               │  │
│  │  ┌────────────────────┐         ┌────────────────────┐      │  │
│  │  │  Tracepoints       │         │  Manual Triggers   │      │  │
│  │  │                    │         │                    │      │  │
│  │  │  sys_enter_execve  │         │  manual_operation  │      │  │
│  │  │  → Insert op       │         │  batch_operations  │      │  │
│  │  │                    │         │  verify_structure  │      │  │
│  │  │  sys_enter_exit    │         │  reset_structure   │      │  │
│  │  │  → Delete op       │         │                    │      │  │
│  │  └────────────────────┘         └────────────────────┘      │  │
│  │                                                               │  │
│  │               handle_operation()                              │  │
│  │                      ↓                                        │  │
│  │          Dispatch to DS operations                            │  │
│  └──────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────┘
```

## Component Interaction Flow

```
┌─────────────┐
│   User      │
│  (runs test)│
└──────┬──────┘
       │
       │ sudo ./skeleton -t 4 -o 1000
       ↓
┌──────────────────┐
│   skeleton.c     │ ←── Loads BPF program
│   (userspace)    │ ←── Creates pthread workers
└────────┬─────────┘
         │
         │ skeleton_bpf__open_and_load()
         ↓
    ┌────────────┐
    │ BPF Loader │ ←── Verifies BPF bytecode
    │  (libbpf)  │ ←── Maps arena to userspace
    └─────┬──────┘
          │
          │ Creates arena map
          ↓
    ┌─────────────────┐
    │   BPF Arena     │ ←── Shared memory region
    │  (kernel mem)   │ ←── Accessible from both sides
    └─────┬───────────┘
          │
          ├──────────────────┬──────────────────┐
          ↓                  ↓                  ↓
    ┌──────────┐      ┌──────────┐      ┌──────────┐
    │ Thread 1 │      │ Thread 2 │      │ skeleton │
    │(userspace)│     │(userspace)│     │  .bpf.c  │
    │          │      │          │      │ (kernel) │
    │ Insert() │      │ Search() │      │ Delete() │
    └──────────┘      └──────────┘      └──────────┘
          │                  │                  │
          └──────────────────┴──────────────────┘
                             │
                        All modify
                        same arena
                             ↓
                    ┌────────────────┐
                    │  Data Structure│
                    │  (in arena)    │
                    │                │
                    │  list/tree/etc │
                    └────────────────┘
```

## Data Structure API Flow

```
Adding New Data Structure:

┌─────────────────────┐
│ 1. Create           │
│    ds_<name>.h      │
│                     │
│  • Define node      │
│  • Define head      │
│  • Implement API    │
└──────┬──────────────┘
       │
       ↓
┌─────────────────────┐
│ 2. Modify           │
│    skeleton.bpf.c   │
│                     │
│  • Include header   │
│  • Declare head     │
│  • Update dispatch  │
└──────┬──────────────┘
       │
       ↓
┌─────────────────────┐
│ 3. Modify           │
│    skeleton.c       │
│                     │
│  • Include header   │
│  • Update types     │
│  • Update workers   │
└──────┬──────────────┘
       │
       ↓
┌─────────────────────┐
│ 4. Build            │
│                     │
│  make -f            │
│  Makefile.new       │
└──────┬──────────────┘
       │
       ↓
┌─────────────────────┐
│ 5. Test             │
│                     │
│  sudo ./skeleton    │
│  sudo ./test_*.sh   │
└─────────────────────┘
```

## Memory Allocation Flow

```
BPF Program Allocates Memory:

┌────────────────────┐
│   bpf_arena_alloc  │
│   (size)           │
└─────────┬──────────┘
          │
          ↓
    ┌─────────────┐
    │  Get CPU ID │
    └──────┬──────┘
           │
           ↓
    ┌────────────────────┐
    │  Check current page│
    │  for this CPU      │
    └──────┬─────────────┘
           │
           ├── Has space? ──→ YES ──┐
           │                        │
           └── NO                   │
               │                    │
               ↓                    │
        ┌──────────────┐           │
        │ Allocate new │           │
        │ page from    │           │
        │ kernel       │           │
        └──────┬───────┘           │
               │                    │
               └────────────────────┤
                                    ↓
                          ┌─────────────────┐
                          │ Return pointer  │
                          │ to arena memory │
                          └─────────────────┘
                                    ↓
                            ┌──────────────┐
                            │ Update stats │
                            │ • total_allocs++│
                            │ • bytes_alloc+=│
                            └──────────────┘

BPF Program Frees Memory:

┌────────────────────┐
│   bpf_arena_free   │
│   (ptr)            │
└─────────┬──────────┘
          │
          ↓
    ┌─────────────────┐
    │ Find page       │
    │ containing ptr  │
    └──────┬──────────┘
           │
           ↓
    ┌─────────────────┐
    │ Decrement       │
    │ ref count       │
    └──────┬──────────┘
           │
           ├── Count == 0? ──→ YES ──┐
           │                         │
           └── NO                    │
               │                     │
               ↓                     ↓
          ┌────────┐        ┌──────────────┐
          │ Return │        │ Free page    │
          │        │        │ back to      │
          └────────┘        │ kernel       │
                            └──────────────┘
```

## Testing Flow

```
Test Execution:

┌──────────────┐
│ User runs    │
│ ./skeleton   │
└──────┬───────┘
       │
       ↓
┌──────────────────┐
│ Parse args       │
│ • -t threads     │
│ • -o operations  │
│ • -w workload    │
└──────┬───────────┘
       │
       ↓
┌──────────────────┐
│ Load BPF program │
│ • Open skeleton  │
│ • Load & verify  │
│ • Attach hooks   │
└──────┬───────────┘
       │
       ↓
┌──────────────────┐
│ Initialize DS    │
│ • Call init()    │
│ • Setup arena    │
└──────┬───────────┘
       │
       ↓
┌──────────────────┐
│ Spawn threads    │
│ • Create N       │
│ • Wait at barrier│
└──────┬───────────┘
       │
       ↓
┌──────────────────────────────────────┐
│ All threads start simultaneously     │
│                                      │
│ Thread 1   Thread 2   ...   Thread N│
│    ↓           ↓              ↓     │
│ for i in ops:                       │
│   • Generate key                    │
│   • Perform operation               │
│   • Update stats                    │
└──────┬───────────────────────────────┘
       │
       ↓
┌──────────────────┐
│ Threads complete │
│ • Join all       │
└──────┬───────────┘
       │
       ↓
┌──────────────────┐
│ Collect stats    │
│ • Per-thread     │
│ • Kernel-side    │
│ • Arena memory   │
└──────┬───────────┘
       │
       ↓
┌──────────────────┐
│ Print results    │
│ • Throughput     │
│ • Failures       │
│ • Memory usage   │
└──────┬───────────┘
       │
       ├── -v flag? ──→ YES ──┐
       │                      │
       └── NO                 ↓
           │           ┌──────────────┐
           │           │ Verify DS    │
           │           │ • Integrity  │
           │           │ • Consistency│
           │           └──────────────┘
           ↓
      ┌─────────┐
      │  Exit   │
      └─────────┘
```

## File Dependencies

```
skeleton.c
    │
    ├─→ skeleton.skel.h (generated)
    │       │
    │       └─→ skeleton.bpf.o (generated)
    │               │
    │               └─→ skeleton.bpf.c
    │                       │
    │                       ├─→ ds_list.h
    │                       │       │
    │                       │       └─→ ds_api.h
    │                       │               │
    │                       │               └─→ libarena_ds.h
    │                       │                       │
    │                       │                       └─→ bpf_arena_common.h
    │                       │
    │                       └─→ vmlinux.h
    │
    ├─→ ds_list.h
    │       │
    │       └─→ ds_api.h
    │               │
    │               └─→ libarena_ds.h
    │                       │
    │                       └─→ bpf_arena_common.h
    │
    └─→ libbpf.a
```

## Build Pipeline

```
Source Files                Build Steps                Output

skeleton.bpf.c    ──→  clang -target bpf      ──→  skeleton.tmp.bpf.o
    +                  -D__TARGET_ARCH_x86
ds_list.h              -D__BPF_FEATURE_ADDR_SPACE_CAST
    +                  -O2 -g
libarena_ds.h                   │
    +                           ↓
vmlinux.h              bpftool gen object     ──→  skeleton.bpf.o
                                │
                                ↓
                       bpftool gen skeleton   ──→  skeleton.skel.h
                                                         │
                                                         ↓
skeleton.c     ──→  gcc -c                   ──→  skeleton.o
    +              -I. -Ioutput                         │
skeleton.skel.h    -O2 -g                               │
    +                                                   ↓
ds_list.h                                          gcc -o skeleton
    +                                                   │
libarena_ds.h                                          ↓
                                                   skeleton (binary)
    +              ←──  libbpf.a
                        libelf.so
                        libz.so
                        libpthread.so
```

This visual overview provides a complete picture of how all components interact!
