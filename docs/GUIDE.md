# BPF Arena Concurrent Data Structures Testing Framework

## Table of Contents
1. [Overview](#overview)
2. [Architecture](#architecture)
3. [Getting Started](#getting-started)
4. [Adding New Data Structures](#adding-new-data-structures)
5. [Running Tests](#running-tests)
6. [Nomenclature and Conventions](#nomenclature-and-conventions)
7. [Troubleshooting](#troubleshooting)
8. [Agent-Friendly Instructions](#agent-friendly-instructions)

---

## Non-uniform Items and TODOs

This section documents differences between the documentation baseline and the current
implementation in `include/` and `src/`. Items are split into two categories:
- **TODO (can be unified)**: changes that should be made to code/docs to make behavior uniform.
- **Exceptions (must differ)**: implemented differences that are required for correctness.

Non-uniform items discovered:

- **`init()` signatures**: most data structures expose `ds_<name>_init(head)` but the
  `vyukhov` implementation requires `ds_vyukhov_init(head, capacity)`.  
  - Exception: bounded queues require an explicit capacity parameter — this cannot be
    safely inferred.  
  - TODO: Standardize `init()` to accept a `struct ds_options` pointer to handle optional parameters like capacity uniformly.

- **`delete` / `pop` signatures**: `ds_api.h` declares both `ds_<name>_delete(head, key)`
  and `ds_<name>_pop(head, struct ds_kv *out)`, but current implementations vary:
  - `ds_msqueue` and `ds_vyukhov` only expose `pop()`.
  - `ds_mpsc_delete()` consumes via `struct ds_kv *out` (pop semantics) and `ds_mpsc_pop()` returns 1/0.
  - `ds_bintree_delete()` accepts a `struct ds_kv` instead of a key.
  - TODO: Align delete/pop signatures or provide consistent wrappers.

- **Statistics Implementation**: `ds_api.h` defines `struct ds_stats`, but current data
  structures do not implement `get_stats`/`reset_stats`. Skeletons only track
  `total_kernel_ops` and `total_kernel_failures`, plus per-structure counters (e.g. list `head->count`).
  - TODO: Either implement `ds_stats` across all data structures or remove it from the API.

- **LSM hook wording**: All skeletons attach to `lsm.s/inode_create`, but a couple of
  userspace comments still mention `execve`.
  - TODO: Update comments to consistently refer to `inode_create`.

Exceptions (must remain different):

- **Bounded queues require capacity**: `ds_vyukhov_init()` must accept capacity.
- **Algorithm-specific semantics**: MS-Queue requires a dummy node; Vyukhov requires sequence numbers. These internal differences are expected.

Use this list to track real code/doc mismatches and resolve them as the API evolves.

## Implemented Data Structures

The framework currently includes the following data structure implementations:

| Data Structure | Header | Skeleton | Description |
|----------------|--------|----------|-------------|
| **Doubly-Linked List** | `ds_list.h` | `skeleton` | Reference implementation with lock-free operations. |
| **Michael-Scott Queue** | `ds_msqueue.h` | `skeleton_msqueue` | Standard lock-free FIFO queue. |
| **Binary Search Tree** | `ds_bst.h` | `skeleton_bst` | Standard BST implementation. |
| **Ellen's Binary Tree** | `ds_bintree.h` | `skeleton_bintree` | Lock-free binary search tree. |
| **MPSC Queue** | `ds_mpsc.h` | `skeleton_mpsc` | Multi-producer single-consumer queue. |
| **Vyukhov MPSC Queue** | `ds_vyukhov.h` | `skeleton_vyukhov` | Optimized MPSC queue by Dmitry Vyukhov. |
| **Folly SPSC Queue** | `ds_folly_spsc.h` | `skeleton_folly_spsc` | Single-producer single-consumer ring buffer queue (Folly implementation). |

## Overview

This framework provides a testing infrastructure for **concurrent data structures** that operate across both **kernel space** (BPF programs) and **user space** (direct arena access). It uses **BPF arena** (`BPF_MAP_TYPE_ARENA`) - a sparse shared memory region that allows both kernel and userspace to access the same memory using real C pointers.

### Key Features

- **Kernel-driven population**: `lsm.s/inode_create` hooks automatically insert data (PID and timestamp) when files are created.
- **Zero-copy access**: Userspace directly accesses arena memory without syscalls.
- **Arena Atomic API**: Built-in `arena_atomic_*` wrappers for GCC `__atomic` built-ins, providing C11-style memory ordering (Acquire/Release).
- **Verification support**: Data structure integrity checking.
- **Modular design**: Easy to add new data structures using the `ds_api.h` template.
- **Continuous Polling**: Userspace readers typically poll and dequeue elements in real-time.

### Use Cases

- Testing concurrent data structure implementations.
- Benchmarking lock-free algorithms using `arena_atomic` primitives.
- Validating correctness under concurrent access from multiple contexts.
- Performance comparison between kernel and userspace operations.

---

## Architecture

### Component Overview

```
┌─────────────────────────────────────────────────────┐
│                     USER SPACE                      │
├─────────────────────────────────────────────────────┤
│  skeleton.c (Continuous Reader)                     │
│  ┌───────────────────────────────────────────────┐  │
│  │  while (true) {                               │  │
│  │    data = ds_pop(head);                       │  │
│  │    if (data) print(data);                     │  │
│  │  }                                            │  │
│  └───────────────────────┬───────────────────────┘  │
│                          │                          │
│                    Direct Access                    │
│                     (Zero Copy)                     │
│                          ↓                          │
├─────────────────────────────────────────────────────┤
│                   BPF ARENA MAP                     │
│            (Shared Memory Backing Store)            │
├─────────────────────────────────────────────────────┤
│                          ↑                          │
│                    Direct Access                    │
│                     (Zero Copy)                     │
│                          │                          │
├─────────────────────────────────────────────────────┤
│                    KERNEL SPACE                     │
├─────────────────────────────────────────────────────┤
│  skeleton.bpf.c (LSM Hook)                          │
│  ┌───────────────────────────────────────────────┐  │
│  │  SEC("lsm.s/inode_create")                    │  │
│  │  int hook(...) {                              │  │
│  │    ds_insert(head, pid, ts);                  │  │
│  │  }                                            │  │
│  └───────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────┘
```

### Memory Model: BPF Arena & Atomics

The project relies on `BPF_MAP_TYPE_ARENA` as the backing store. This map type allows for a large, sparse virtual memory area that is shared between the BPF program and userspace.

#### The `arena_atomic` API

To ensure correctness in concurrent environments, we use the `arena_atomic_*` API defined in `include/libarena_ds.h`. These macros wrap GCC's `__atomic` built-ins and are compatible with BPF arena pointers.

**Key atomic primitives:**
- `arena_atomic_cmpxchg(ptr, old, new, success_mo, failure_mo)` - Compare-and-swap operation
- `arena_atomic_exchange(ptr, val, mo)` - Atomic swap
- `arena_atomic_add(ptr, val, mo)` - Fetch-and-add
- `arena_atomic_sub(ptr, val, mo)` - Fetch-and-subtract
- `arena_atomic_and(ptr, val, mo)` - Fetch-and-AND
- `arena_atomic_or(ptr, val, mo)` - Fetch-and-OR

**⚠️ IMPORTANT: Do NOT use `arena_atomic_load()` or `arena_atomic_store()`**

These primitives trigger BPF verifier issues. Instead, use the alternatives below based on the memory ordering you need.

**Convenience wrappers:**
- `arena_atomic_inc(ptr)` - Increment (RELAXED)
- `arena_atomic_dec(ptr)` - Decrement (RELAXED)
- `arena_memory_barrier()` - Full memory fence

**Compiler barriers for acquire/release semantics (use these instead of arena_atomic_load/store):**
- `smp_load_acquire(ptr)` - Load with acquire semantics (use for ACQUIRE ordering)
- `smp_store_release(ptr, val)` - Store with release semantics (use for RELEASE ordering)
- `READ_ONCE(x)` - Volatile read, prevents compiler reordering (use for RELAXED loads)
- `WRITE_ONCE(x, val)` - Volatile write, prevents compiler reordering (use for RELAXED stores)

**Memory ordering mapping:**
- Need ACQUIRE load? → Use `smp_load_acquire(ptr)`
- Need RELEASE store? → Use `smp_store_release(ptr, val)`
- Need RELAXED load? → Use `READ_ONCE(var)`
- Need RELAXED store? → Use `WRITE_ONCE(var, val)`
- Need SEQ_CST? → Use `arena_memory_barrier()` with READ_ONCE/WRITE_ONCE

**Supported memory orderings:**
- `ARENA_RELAXED` - No synchronization or ordering constraints
- `ARENA_ACQUIRE` - Load-acquire (prevents subsequent loads/stores from moving before this operation)
- `ARENA_RELEASE` - Store-release (prevents prior loads/stores from moving after this operation)
- `ARENA_ACQ_REL` - Both acquire and release semantics
- `ARENA_SEQ_CST` - Sequentially consistent (strongest guarantee)

**Usage examples:**
```c
// Lock-free insertion with CAS
retry:
    old_head = READ_ONCE(head->first);  // RELAXED load
    new_node->next = old_head;
    if (arena_atomic_cmpxchg(&head->first, old_head, new_node, 
                             ARENA_ACQ_REL, ARENA_ACQUIRE) != old_head)
        goto retry;

// Producer-consumer synchronization (SPSC queue)
// Producer: write data then release the write index
node->key = key;
node->value = value;
smp_store_release(&head->write_idx, next_idx);  // RELEASE store

// Consumer: acquire the write index then read data
current_write = smp_load_acquire(&head->write_idx);  // ACQUIRE load
if (current_read != current_write) {
    data = node->value;  // Safe to read
}

// Linked list traversal
current = smp_load_acquire(&node->next);  // ACQUIRE load to see updates

// Initialize node
WRITE_ONCE(node->value, initial_val);  // RELAXED store
```

### Algorithm-Specific Memory Ordering Patterns

Different concurrent data structures require different synchronization patterns:

#### Lock-Free Trees (Ellen's Binary Tree)
- **CAS on internal node update fields**: `ARENA_ACQ_REL` / `ARENA_ACQUIRE`
- **Child pointer updates**: `ARENA_RELEASE` to publish, `ARENA_ACQUIRE` to read
- **Helping protocol**: Operations may complete work started by others
- **Tagged pointers**: Low bits encode operation state (CLEAN, IFLAG, DFLAG, MARK)

#### SPSC Queues (Folly Implementation)
- **Producer writes data → RELEASE write index**: Ensures data is visible before index
- **Consumer ACQUIRE write index → reads data**: Ensures index update is visible first
- **Consumer reads data → RELEASE read index**: Signals slot is free
- **Producer ACQUIRE read index → checks space**: Sees consumer's completion
- **Key insight**: Single producer/consumer eliminates need for CAS operations

#### MPSC Queues (Multi-Producer Single-Consumer)
- **Multiple producers use CAS**: `arena_atomic_cmpxchg` for head updates
- **Consumer uses relaxed reads**: No contention on dequeue path
- **Memory barriers**: Separate cache lines prevent false sharing

### The Pipeline: `inode_create` -> Polling

1. **Trigger**: A file is created on the system (e.g., `touch /tmp/test`).
2. **Kernel Action**: The `lsm.s/inode_create` hook triggers. It retrieves the current PID and timestamp, then calls the data structure's `insert` operation.
3. **Shared State**: The data is placed directly into the BPF Arena.
4. **Userspace Action**: The userspace program (`skeleton.c`) runs a continuous loop, polling the data structure (e.g., via `pop` or `iterate`) and printing results.

### Memory Layout

```
Arena Virtual Address Space (up to 4GB)
┌────────────────────────────────────┐
│ Page 0: Data structure head        │ ← Initialized by BPF
│         (ds_list_head, etc.)       │
├────────────────────────────────────┤
│ Page 1-N: Dynamically allocated    │ ← bpf_arena_alloc()
│           nodes and data           │   allocates on demand
│                                    │
│  ┌─────────────┐  ┌─────────────┐  │
│  │ Node 1      │  │ Node 2      │  │
│  │ key: PID    │  │ key: PID    │  │
│  │ value: TS   │  │ value: TS   │  │
│  │ next: ───────→ │ next: NULL  │  │
│  └─────────────┘  └─────────────┘  │
│                                    │
└────────────────────────────────────┘
      ↑                    ↑
      │                    │
  Accessible          Accessible
  from BPF            from userspace
  (kernel)            (mmap'd)
```

### Execution Flow

1. **Initialization**: 
   - Userspace loads BPF program via skeleton.
   - Arena is automatically mmap'd into userspace.
   - Data structure is lazily initialized in the arena on the first kernel trigger.

2. **Kernel Operations**:
   - The `lsm.s/inode_create` hook triggers on file creation (e.g., `touch`).
   - It inserts the current PID and timestamp into the data structure using `arena_atomic` primitives.

3. **Userspace Operations**:
   - The userspace program runs a continuous polling loop.
   - It dequeues elements directly from the arena (zero-copy) and prints them.

4. **Verification & Stats**:
   - On exit (Ctrl+C), userspace can optionally verify the data structure's integrity.
   - Final statistics (total ops, failures) are printed from the BPF BSS.

---

## Getting Started

### Prerequisites

**Required**:
- Linux kernel 6.10+ with `CONFIG_BPF_ARENA=y`
- LLVM/Clang 15+ with BPF support
- libbpf 1.0+
- libelf, zlib

**Check your kernel**:
```bash
# Check kernel version
uname -r

# Check if arena is enabled
zgrep CONFIG_BPF_ARENA /proc/config.gz

# Check clang version
clang --version
```

### Building

```bash
cd bpf-arena-data-structures/

# Initialize third-party dependencies (first time only)
git submodule update --init --recursive

# Build all programs
make

# Or build specific programs
make skeleton      # Test framework
make arena_list    # Example program

# Verbose build
make V=1
```

### Quick Test

```bash
# Run the skeleton test (Ctrl+C to stop)
sudo ./skeleton

# Run with verification on exit
sudo ./skeleton -v
```

---

## Adding New Data Structures

This section walks through adding a new data structure step-by-step.

### Step 1: Create Data Structure Header

Create a new file `include/ds_<name>.h` (e.g., `include/ds_tree.h` for a tree):

```c
/* include/ds_tree.h - Example tree implementation */
#pragma once

#include "ds_api.h"

/* ========================================================================
 * DATA STRUCTURES
 * ======================================================================== */

struct ds_tree_node {
    struct ds_tree_node __arena *left;
    struct ds_tree_node __arena *right;
    struct ds_tree_node __arena *parent;
    __u64 key;
    __u64 value;
    int height;  /* For AVL balancing */
};

struct ds_tree_head {
    struct ds_tree_node __arena *root;
    __u64 count;
};

/* ========================================================================
 * API IMPLEMENTATION
 * ======================================================================== */

static inline int ds_tree_init(struct ds_tree_head *head)
{
    if (!head)
        return DS_ERROR_INVALID;
    
    head->root = NULL;
    head->count = 0;

    return DS_SUCCESS;
}

static inline int ds_tree_insert(struct ds_tree_head *head, __u64 key, __u64 value)
{
    /* Implement tree insertion with arena allocation */
    struct ds_tree_node __arena *new_node = bpf_arena_alloc(sizeof(*new_node));
    if (!new_node)
        return DS_ERROR_NOMEM;
    
    /* ... rest of insertion logic ... */
    
    head->count++;
    return DS_SUCCESS;
}

static inline int ds_tree_delete(struct ds_tree_head *head, __u64 key)
{
    /* Implement deletion */
    /* Use bpf_arena_free() to free nodes */
    return DS_SUCCESS;
}

static inline int ds_tree_pop(struct ds_tree_head *head, struct ds_kv *data)
{
    /* Optional: implement a dequeue-style removal (e.g., min element) */
    return DS_ERROR_NOT_FOUND;
}

static inline int ds_tree_search(struct ds_tree_head *head, __u64 key)
{
    /* Implement search */
    return DS_SUCCESS;
}

static inline int ds_tree_verify(struct ds_tree_head *head)
{
    /* Verify tree properties (balance, ordering, etc.) */
    return DS_SUCCESS;
}

static inline const struct ds_metadata* ds_tree_get_metadata(void)
{
    static const struct ds_metadata metadata = {
        .name = "tree",
        .description = "AVL tree",
        .node_size = sizeof(struct ds_tree_node),
        .requires_locking = 0,
    };
    return &metadata;
}
```

### Step 2: Modify Kernel Skeleton

Edit `src/skeleton.bpf.c` and add your data structure:

**1. Include the header** (line ~30):
```c
/* ========================================================================
 * DS_API_INSERT: Include your data structure headers here
 * ======================================================================== */
#include "ds_list.h"
#include "ds_tree.h"  // ADD THIS
```

**2. Declare the head** (line ~40):
```c
/* DS_API_INSERT: Declare your data structure head here */
struct ds_list_head __arena global_ds_head;
struct ds_tree_head __arena global_tree_head;  // ADD THIS
```

**3. Add dispatch cases** in `handle_operation()` (line ~70):
```c
/* If you want to switch between different data structures,
   add a configuration variable and dispatch accordingly */

switch (op->type) {
case DS_OP_INIT:
    result = ds_tree_init(&global_tree_head);  // Use tree instead
    initialized = true;
    break;
    
case DS_OP_INSERT:
    result = ds_tree_insert(&global_tree_head, op->kv.key, op->kv.value);
    break;
    
/* ... etc ... */
}
```

### Step 3: Modify Userspace Skeleton

Edit `src/skeleton.c` to use your data structure:

**1. Include the header** (line ~20):
```c
#include "ds_api.h"
#include "ds_list.h"
#include "ds_tree.h"  // ADD THIS
```

**2. Update the polling function** (line ~70):
```c
static void poll_and_dequeue()
{
    struct ds_tree_head *head = skel->bss->ds_head;  // Change to your type
    struct ds_kv data;
    int result;
    
    printf("Starting continuous polling (Ctrl+C to stop)...\n\n");
    
    while (!stop_test) {
        result = ds_tree_pop(head, &data);  // Use your pop/dequeue API
        
        if (result == DS_SUCCESS) {
            printf("Dequeued: key=%llu, value=%llu\n", data.key, data.value);
        } else if (result == DS_ERROR_NOT_FOUND) {
            continue;  // Keep polling
        }
    }
}
```

Note: return conventions differ by implementation (e.g., `ds_mpsc_pop()` returns 1/0 for
success/empty, while `ds_list_pop()` and `ds_msqueue_pop()` return `DS_SUCCESS`/`DS_ERROR_NOT_FOUND`).
Check the target `include/ds_*.h` file and match its return codes.

**3. Update verification function** (if implementing verify):
```c
static int verify_data_structure(void)
{
    struct ds_tree_head *head = skel->bss->ds_head;  // Change type
    
    printf("Verifying data structure from userspace...\n");
    
    int result = ds_tree_verify(head);  // Use your verify function
    if (result == DS_SUCCESS) {
        printf("✓ Data structure verification PASSED\n");
    } else {
        printf("✗ Data structure verification FAILED (error %d)\n", result);
    }
        
    return result;
}
```

### Step 4: Build and Test

```bash
# Rebuild
make clean && make

# Test your new data structure (kernel will populate via LSM)
sudo ./skeleton

# Verify integrity on exit
sudo ./skeleton -v
```

---

## Running Tests

### Basic Testing

```bash
# Run and poll for data (Ctrl+C to stop)
sudo ./skeleton

# Run with verification on exit
sudo ./skeleton -v

# Run with statistics output (enabled by default)
sudo ./skeleton -s
```

### Command-Line Options

```
Usage: ./skeleton [OPTIONS]
Usage: ./skeleton_msqueue [OPTIONS]

DESIGN:
  Kernel:    LSM hook on inode_create inserts items (triggers on file creation)
  Userspace: Continuously polls and dequeues elements as they arrive

OPTIONS:
  -v      Verify data structure integrity on exit
  -s      Print statistics on exit (default: enabled)
  -h      Show help

NOTE: Kernel inserts trigger automatically when files are created on the system.
      Userspace continuously dequeues and prints elements.
```

### Automated Tests

**Note:** The current test scripts are templates designed for a multi-threaded implementation.
They reference command-line options (`-t`, `-o`, `-w`) that the current simple implementation doesn't use.

```bash
# These are templates - may need adjustment for current simple design:
# make test
# make test-stress
# make test-verify
```

### Understanding Test Output

```
Starting continuous polling (Ctrl+C to stop)...

Dequeued element 0: pid=1234, ts=1234567890
Dequeued element 1: pid=1235, ts=1234567891
Dequeued element 2: pid=1236, ts=1234567892
...
Total dequeued: 342

============================================================
                    STATISTICS
============================================================

Kernel-Side Operations (inode_create LSM hook inserts):
  Total inserts:    342
  Insert failures:  0

Data Structure State:
  Elements in list: 342
```

**Key Metrics**:
- **pid**: Process ID that triggered the LSM hook (from file creation)
- **last_ts**: Timestamp when the insertion occurred
- **Total inserts**: Number of times the LSM hook fired and inserted data
- **Insert failures**: Should be 0 (indicates memory allocation issues if non-zero)
- **Elements in list**: Final count from walking the data structure (should match total inserts)

---

## Nomenclature and Conventions

### File Naming

| Type | Pattern | Example |
|------|---------|---------|
| Data structure header | `include/ds_<name>.h` | `include/ds_list.h`, `include/ds_tree.h` |
| BPF kernel program | `src/<name>.bpf.c` | `src/skeleton.bpf.c` |
| Userspace program | `src/<name>.c` | `src/skeleton.c` |
| Generated skeleton | `<name>.skel.h` | `skeleton.skel.h` |

### Function Naming

All data structure operations follow this pattern:

```
ds_<name>_<operation>
```

Examples:
- `ds_list_insert()` - Insert into list
- `ds_tree_search()` - Search in tree
- `ds_queue_pop()` - Dequeue from queue

### Required Operations

Current implementations provide a common core but do not share identical signatures.

| Operation | Purpose | Return Value |
|-----------|---------|--------------|
| `ds_<name>_init()` | Initialize empty structure | `DS_SUCCESS` |
| `ds_<name>_insert()` | Add key-value pair | `DS_SUCCESS` or `DS_ERROR_*` |
| `ds_<name>_search()` | Find key | `DS_SUCCESS` or `DS_ERROR_NOT_FOUND` |
| `ds_<name>_verify()` | Check integrity | `DS_SUCCESS` or `DS_ERROR_CORRUPT` |
| `ds_<name>_get_metadata()` | Get metadata | pointer to metadata |
| `ds_<name>_pop()` | Dequeue/remove next element | `DS_SUCCESS`/`DS_ERROR_*` (see per-impl notes) |
| `ds_<name>_delete()` | Remove by key | Implemented by list/bst/bintree; signatures vary |
| `ds_<name>_get_stats()` | Retrieve statistics | Not implemented in current structures |
| `ds_<name>_reset_stats()` | Clear statistics | Not implemented in current structures |

Note: current data structures do not use `DS_API_DECLARE`, so signatures are
hand-written per header. Check the specific `include/ds_*.h` file you target.

### Type Naming

```c
// Node type
struct ds_<name>_node { ... };

// Head/root type
struct ds_<name>_head { ... };

// Optional: typedef for convenience
typedef struct ds_<name>_head ds_<name>_head_t;
typedef struct ds_<name>_node ds_<name>_node_t;
```

### BPF Program Sections

```c
// LSM Hooks (for automatic triggers on file creation)
SEC("lsm.s/inode_create")
int BPF_PROG(lsm_inode_create, struct inode *dir, struct dentry *dentry, umode_t mode)
```

Common LSM hooks:
- `inode_create` - Triggered on file creation (e.g., `touch`)
Other LSM hooks are possible, but the current repository only uses `inode_create`.

### Customization Markers

Look for these comments in code to find insertion points:

```c
/* DS_API_INSERT: <description> */
```

These markers indicate where to add your data structure code.

---

## Troubleshooting

### Compilation Issues

**Error: `vmlinux.h not found`**
```bash
# Generate vmlinux.h from your running kernel
bpftool btf dump file /sys/kernel/btf/vmlinux format c > vmlinux.h

# Or use the one from the repository
ls ../../third_party/vmlinux/$(uname -m)/
```

**Error: `bpf_arena_alloc_pages not found`**
- Your kernel doesn't support BPF arena (need 6.10+)
- Check: `zgrep CONFIG_BPF_ARENA /proc/config.gz`

**Error: `address space cast not supported`**
- Compiler doesn't support `__arena` attribute
- Update to Clang 15+ or use fallback mode (see `bpf_arena_common.h`)

### Runtime Issues

**Error: `Failed to load BPF program`**
```bash
# Check verifier log
sudo bpftool prog load skeleton.bpf.o /sys/fs/bpf/skel 2>&1 | less

# Common issues:
# - Loop bounds: Add `&& can_loop` to while/for conditions
# - Null pointer: Add null checks before dereferencing
# - Unbounded memory access: Add explicit bounds checks
```

**Error: `Permission denied`**
- Need root for BPF operations
- Run with `sudo`

**Segmentation fault in userspace**
- Arena not properly mmap'd
- Check skeleton loading succeeded
- Verify arena map definition in BPF program

**Data structure verification fails**
- Race condition in concurrent access
- Missing memory barriers
- Use atomic operations (see `libarena_ds.h`)

### Performance Issues

**Low throughput**
- Reduce key range for more contention
- Increase operations per thread
- Check if verification is enabled (slower)

**High failure rate**
- Expected for delete operations (keys may not exist)
- Check for memory allocation failures
- Increase arena size in map definition

### Debugging Tips

**Enable verbose output**:
```bash
make V=1 skeleton
```

**Add debug prints**:
```c
// In BPF code
bpf_printk("Debug: key=%llu, value=%llu", key, value);

// View output
sudo cat /sys/kernel/debug/tracing/trace_pipe
```

**Check skeleton statistics**:
```c
printf("Total inserts: %llu\n", skel->bss->total_kernel_ops);
printf("Insert failures: %llu\n", skel->bss->total_kernel_failures);
```

---

## Agent-Friendly Instructions

This section provides structured information for automated agents.

### JSON Schema for Data Structure Definition

This schema is illustrative; current headers do not wire `struct ds_stats` into
the skeletons, and queue-like structures typically include a `pop()` operation.

```json
{
  "data_structure": {
    "name": "tree",
    "type": "avl_tree",
    "description": "Self-balancing AVL tree",
    "files": {
      "header": "include/ds_tree.h",
      "test": "test_tree.c"
    },
    "node": {
      "struct_name": "ds_tree_node",
      "fields": [
        {"name": "left", "type": "struct ds_tree_node __arena *"},
        {"name": "right", "type": "struct ds_tree_node __arena *"},
        {"name": "parent", "type": "struct ds_tree_node __arena *"},
        {"name": "key", "type": "__u64"},
        {"name": "value", "type": "__u64"},
        {"name": "height", "type": "int"}
      ]
    },
    "head": {
      "struct_name": "ds_tree_head",
      "fields": [
        {"name": "root", "type": "struct ds_tree_node __arena *"},
        {"name": "count", "type": "__u64"}
      ]
    },
    "operations": {
      "init": "ds_tree_init",
      "insert": "ds_tree_insert",
      "delete": "ds_tree_delete",
      "pop": "ds_tree_pop",
      "search": "ds_tree_search",
      "verify": "ds_tree_verify"
    },
    "properties": {
      "lock_free": true,
      "ordered": true,
      "allows_duplicates": false,
      "average_complexity": {
        "insert": "O(log n)",
        "delete": "O(log n)",
        "search": "O(log n)"
      }
    }
  }
}
```

### File Structure Template

```
bpf-arena-data-structures/
├── include/               # Headers (ds_api.h, ds_*.h, atomics)
├── src/                   # Skeletons and BPF programs
├── docs/                  # Documentation (this file lives here)
├── scripts/               # Test/benchmark scripts
├── third_party/           # libbpf, bpftool, vmlinux
├── Makefile               # Build system
├── README.md
├── QUICKSTART.md
└── INDEX.md
```

### Modification Points

1. **include/ds_<name>.h**: Implement init/insert/search/verify and the correct delete/pop variants
2. **src/skeleton.bpf.c**:
   - Add your header at the `DS_API_INSERT` include marker
   - Declare the head in BSS (see the existing `global_ds_head`)
   - Update `handle_operation()` to dispatch to your API
3. **src/skeleton.c**:
   - Include your header
   - Update `poll_and_dequeue()` and verification to call your API

### Testing Checklist

```yaml
tests:
  - name: "Compilation"
    command: "make clean && make"
    expected: "Build succeeds, no errors"
  
  - name: "Basic functionality"
    command: "sudo ./skeleton"
    expected: "Completes without errors"
  
  - name: "Verification"
    command: "sudo ./skeleton -v"
    expected: "Verification passes"
```

### Common Patterns

**Lock-free insertion**:
```c
retry:
    old_head = head->first;
    new_node->next = old_head;
    if (arena_atomic_cmpxchg(&head->first, old_head, new_node) != old_head)
        goto retry;
```

**Safe iteration**:
```c
n = head->first;
while (n && can_loop) {
    cast_kern(n);
    // Process n
    n = n->next;
}
```

---

## Additional Resources

- **Linux kernel BPF arena documentation**: `Documentation/bpf/arena.rst`
- **libbpf documentation**: https://libbpf.readthedocs.io/
- **BPF examples**: `samples/bpf/` in kernel source
- **Original tutorial**: See `README.md` for the arena_list example

For questions or issues, refer to the BPF mailing list or Linux kernel documentation.
