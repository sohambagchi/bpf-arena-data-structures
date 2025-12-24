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

- **Userspace collection pattern**: most `src/skeleton_*` binaries implement a continuous
  poll-and-dequeue loop (prints elements as they arrive). Some older docs
  describe a sleep-then-read pattern.  
  - TODO: Standardize on the continuous polling pattern as the default.

- **`init()` signatures**: most data structures expose `ds_<name>_init(head)` but the
  `vyukhov` implementation requires `ds_vyukhov_init(head, capacity)`.  
  - Exception: bounded queues require an explicit capacity parameter — this cannot be
    safely inferred.  
  - TODO: Standardize `init()` to accept a `struct ds_options` pointer to handle optional parameters like capacity uniformly.

- **`delete` / `pop` signatures**: `ds_api.h` describes `ds_<name>_delete(head, key)`;
  several implementations (MS Queue, Vyukhov) expose `ds_<name>_delete(head, struct ds_kv *out)`
  which is effectively a `pop()` operation.
  - TODO: Formally introduce `pop()` into `ds_api.h` and distinguish it from `delete(key)`.

- **Statistics Implementation**: `ds_api.h` defines a complex `ds_stats` structure, but the current simplified implementation only tracks `total_kernel_ops` and `total_kernel_failures` in the BPF skeleton's BSS.
  - TODO: Either fully implement `ds_stats` across all data structures or simplify `ds_api.h` to match the current minimal tracking.

- **LSM Hook Consistency**: All skeletons currently use `lsm.s/inode_create`. Some comments still refer to `path_mkdir` or `file_open`.
  - TODO: Update all comments to consistently refer to `inode_create`.

Exceptions (must remain different):

- **Bounded queues require capacity**: `ds_vyukhov_init()` must accept capacity.
- **Algorithm-specific semantics**: MS-Queue requires a dummy node; Vyukhov requires sequence numbers. These internal differences are expected.

How this was applied to docs:
- Updated `README.md`, `QUICKSTART.md`, and `INDEX.md` to reflect continuous polling
  behavior and fixed incorrect paths that pointed to `.agent/*` (now `docs/*`).
- Added this section to `docs/GUIDE.md` so future contributors can prioritize
  the TODOs above when unifying APIs and behaviors.
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

Key primitives include:
- `arena_atomic_cmpxchg(ptr, old, new, success_mo, failure_mo)`
- `arena_atomic_exchange(ptr, val, mo)`
- `arena_atomic_add(ptr, val, mo)`
- `arena_atomic_load(ptr, mo)`
- `arena_atomic_store(ptr, val, mo)`

Supported memory orderings:
- `ARENA_RELAXED`
- `ARENA_ACQUIRE`
- `ARENA_RELEASE`
- `ARENA_ACQ_REL`
- `ARENA_SEQ_CST`

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
    struct ds_stats stats;
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
    
    /* Initialize stats... */
    
    return DS_SUCCESS;
}

static inline int ds_tree_insert(struct ds_tree_head *head, __u64 key, __u64 value)
{
    /* Implement tree insertion with arena allocation */
    struct ds_tree_node __arena *new_node = bpf_arena_alloc(sizeof(*new_node));
    if (!new_node)
        return DS_ERROR_NOMEM;
    
    /* ... rest of insertion logic ... */
    
    head->stats.ops[DS_OP_INSERT].count++;
    return DS_SUCCESS;
}

static inline int ds_tree_delete(struct ds_tree_head *head, __u64 key)
{
    /* Implement deletion */
    /* Use bpf_arena_free() to free nodes */
    return DS_SUCCESS;
}

static inline int ds_tree_search(struct ds_tree_head *head, __u64 key, __u64 *value)
{
    /* Implement search */
    return DS_SUCCESS;
}

static inline int ds_tree_verify(struct ds_tree_head *head)
{
    /* Verify tree properties (balance, ordering, etc.) */
    return DS_SUCCESS;
}

static inline void ds_tree_get_stats(struct ds_tree_head *head, struct ds_stats *stats)
{
    /* Copy statistics */
}

static inline void ds_tree_reset_stats(struct ds_tree_head *head)
{
    /* Reset statistics */
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
    result = ds_tree_insert(&global_tree_head, op->key, op->value);
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
- `ds_queue_delete()` - Delete from queue

### Required Operations

Every data structure **MUST** implement:

| Operation | Purpose | Return Value |
|-----------|---------|--------------|
| `ds_<name>_init()` | Initialize empty structure | `DS_SUCCESS` |
| `ds_<name>_insert()` | Add key-value pair | `DS_SUCCESS` or `DS_ERROR_*` |
| `ds_<name>_delete()` | Remove key | `DS_SUCCESS` or `DS_ERROR_NOT_FOUND` |
| `ds_<name>_search()` | Find key, return value | `DS_SUCCESS` or `DS_ERROR_NOT_FOUND` |
| `ds_<name>_verify()` | Check integrity | `DS_SUCCESS` or `DS_ERROR_CORRUPT` |
| `ds_<name>_get_stats()` | Retrieve statistics | void |
| `ds_<name>_reset_stats()` | Clear statistics | void |
| `ds_<name>_get_metadata()` | Get metadata | pointer to metadata |

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

// Manual triggers (optional, called via bpf_prog_test_run)
SEC("syscall")
int handle_operation(struct ds_operation *op)
```

Common LSM hooks:
- `inode_create` - Triggered on file creation (e.g., `touch`)
- `file_open` - Triggered on file open
- `socket_create` - Triggered on socket creation

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

**Check arena statistics**:
```c
// Access via skeleton
printf("Current allocations: %llu\n", 
       skel->bss->global_stats.current_allocations);
```

---

## Agent-Friendly Instructions

This section provides structured information for automated agents.

### JSON Schema for Data Structure Definition

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
        {"name": "count", "type": "__u64"},
        {"name": "stats", "type": "struct ds_stats"}
      ]
    },
    "operations": {
      "init": "ds_tree_init",
      "insert": "ds_tree_insert",
      "delete": "ds_tree_delete",
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
bpf_arena/
├── libarena_ds.h          # Arena allocation library
├── ds_api.h               # API definitions and macros
├── bpf_arena_common.h     # Common definitions
├── ds_<name>.h            # Your data structure (IMPLEMENT THIS)
├── skeleton.bpf.c         # Kernel-side test driver
├── skeleton.c             # Userspace test driver
├── Makefile.new           # Build system
└── GUIDE.md              # This file
```

### Modification Points

1. **ds_<name>.h**: Implement all required operations
2. **skeleton.bpf.c**: 
   - Line 30: Include header
   - Line 40: Declare head
   - Line 70-100: Update `handle_operation()` dispatch
3. **skeleton.c**:
   - Line 20: Include header
   - Line 100: Update worker context type
   - Line 150: Update workload functions

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
  
  - name: "Memory cleanup"
    expected: "current_allocations ≈ final element count"
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

**Statistics update**:
```c
head->stats.ops[DS_OP_INSERT].count++;
head->stats.current_elements++;
if (head->stats.current_elements > head->stats.max_elements)
    head->stats.max_elements = head->stats.current_elements;
```

---

## Additional Resources

- **Linux kernel BPF arena documentation**: `Documentation/bpf/arena.rst`
- **libbpf documentation**: https://libbpf.readthedocs.io/
- **BPF examples**: `samples/bpf/` in kernel source
- **Original tutorial**: See `README.md` for the arena_list example

For questions or issues, refer to the BPF mailing list or Linux kernel documentation.
