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

## Overview

This framework provides a testing infrastructure for **concurrent data structures** that operate across both **kernel space** (BPF programs) and **user space** (direct arena access). It uses **BPF arena** - a sparse shared memory region that allows both kernel and userspace to access the same memory using real C pointers.

### Key Features

- **Dual-context execution**: Operations can be performed from both kernel BPF programs and userspace threads
- **Zero-copy access**: Userspace directly accesses arena memory without syscalls
- **Lock-free primitives**: Built-in atomic operations for concurrent access
- **Statistics tracking**: Comprehensive operation metrics from both contexts
- **Verification support**: Data structure integrity checking
- **Modular design**: Easy to add new data structures

### Use Cases

- Testing concurrent data structure implementations
- Benchmarking lock-free algorithms
- Validating correctness under concurrent access
- Performance comparison between kernel and userspace operations

---

## Architecture

### Component Overview

```
┌─────────────────────────────────────────────────────────────┐
│                     USER SPACE                               │
├─────────────────────────────────────────────────────────────┤
│  skeleton.c                                                  │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐        │
│  │  Thread 1   │  │  Thread 2   │  │  Thread N   │        │
│  │  (insert)   │  │  (search)   │  │  (delete)   │        │
│  └──────┬──────┘  └──────┬──────┘  └──────┬──────┘        │
│         │                │                │                 │
│         └────────────────┴────────────────┘                 │
│                          │                                   │
│                    Direct Access                            │
│                          │                                   │
├──────────────────────────┼───────────────────────────────────┤
│                    BPF ARENA                                 │
│              (Shared Memory Region)                          │
│                                                              │
│  ┌──────────────────────────────────────────────────┐      │
│  │  Data Structure (ds_list, ds_tree, etc.)         │      │
│  │  - Nodes allocated with bpf_arena_alloc()        │      │
│  │  - Accessible from both contexts                 │      │
│  └──────────────────────────────────────────────────┘      │
│                          │                                   │
├──────────────────────────┼───────────────────────────────────┤
│                   KERNEL SPACE                               │
├─────────────────────────────────────────────────────────────┤
│  skeleton.bpf.c                                             │
│  ┌──────────────────┐  ┌──────────────────┐               │
│  │ Tracepoint:      │  │ Manual Trigger:  │               │
│  │ sys_enter_execve │  │ manual_operation │               │
│  │ (insert ops)     │  │ batch_operations │               │
│  └──────────────────┘  └──────────────────┘               │
└─────────────────────────────────────────────────────────────┘
```

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
│  ┌─────────────┐  ┌─────────────┐ │
│  │ Node 1      │  │ Node 2      │ │
│  │ key: 100    │  │ key: 200    │ │
│  │ value: 200  │  │ value: 400  │ │
│  │ next: ───────→ │ next: NULL  │ │
│  └─────────────┘  └─────────────┘ │
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
   - Userspace loads BPF program via skeleton
   - Arena is automatically mmap'd into userspace
   - BPF program initializes data structure in arena

2. **Concurrent Operations**:
   - Userspace threads: Direct arena access (no syscalls)
   - Kernel threads: Via syscall tracepoints or manual triggers
   - Both use the same arena memory with atomic operations

3. **Verification**:
   - Userspace: Walk data structure directly
   - Kernel: Trigger verification via BPF program
   - Both check consistency and compute statistics

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
cd bpf_arena/

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
# Run the skeleton test with 4 threads, 1000 operations
sudo ./skeleton -t 4 -o 1000

# Run arena_list example
sudo ./arena_list 100

# Run all smoke tests
make test
```

---

## Adding New Data Structures

This section walks through adding a new data structure step-by-step.

### Step 1: Create Data Structure Header

Create a new file `ds_<name>.h` (e.g., `ds_tree.h` for a tree):

```c
/* ds_tree.h - Example tree implementation */
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

Edit `skeleton.bpf.c` and add your data structure:

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

Edit `skeleton.c` to use your data structure:

**1. Include the header** (line ~20):
```c
#include "ds_api.h"
#include "ds_list.h"
#include "ds_tree.h"  // ADD THIS
```

**2. Update worker functions** (line ~150):
```c
static void workload_mixed(struct ds_tree_head *head, int thread_id,
                            struct thread_stats *stats)
{
    for (int i = 0; i < config.ops_per_thread && !stop_test; i++) {
        __u64 key = generate_random_key(thread_id, i);
        __u64 value = key * 2;
        int op_type = i % 10;
        int result;
        
        if (op_type < 5) {
            result = ds_tree_insert(head, key, value);  // Use tree API
            stats->inserts++;
        } else if (op_type < 8) {
            result = ds_tree_search(head, key, &value);
            stats->searches++;
        } else {
            result = ds_tree_delete(head, key);
            stats->deletes++;
        }
        
        stats->operations++;
        if (result != DS_SUCCESS)
            stats->failures++;
    }
}
```

**3. Update worker context type**:
```c
struct worker_context {
    int thread_id;
    struct ds_tree_head *ds_head;  // Change type
    struct thread_stats stats;
};
```

### Step 4: Build and Test

```bash
# Rebuild
make clean && make

# Test your new data structure
sudo ./skeleton -t 4 -o 1000 -w mixed

# Verify integrity
sudo ./skeleton -t 4 -o 1000 -v
```

---

## Running Tests

### Basic Testing

```bash
# Simple test: 4 threads, 1000 operations each
sudo ./skeleton -t 4 -o 1000

# Insert-only workload
sudo ./skeleton -t 8 -o 5000 -w insert

# Mixed workload with large key space
sudo ./skeleton -t 4 -o 2000 -k 100000 -w mixed

# With verification
sudo ./skeleton -t 4 -o 1000 -v
```

### Command-Line Options

```
Usage: ./skeleton [OPTIONS]

OPTIONS:
  -t N    Number of userspace threads (default: 4)
  -o N    Operations per thread (default: 1000)
  -k N    Key range for operations (default: 10000)
  -d N    Test duration in seconds (default: 10)
  -w TYPE Workload type: insert, search, delete, mixed (default: mixed)
  -v      Verify data structure integrity
  -s      Print statistics (default: enabled)
  -K      Trigger kernel operations via syscalls
  -h      Show help
```

### Automated Tests

```bash
# Run all smoke tests
make test

# Run stress tests (takes longer)
make test-stress

# Run verification tests
make test-verify
```

### Understanding Test Output

```
============================================================
                    TEST STATISTICS                         
============================================================

Per-Thread Results:
Thread   Operations    Inserts    Deletes   Searches  Failures      Ops/sec
--------------------------------------------------------------------
0              1000        500        200        300         5       45231
1              1000        498        202        300         8       44892
2              1000        502        198        300         3       45567
3              1000        499        201        300         6       45123
--------------------------------------------------------------------
TOTAL          4000       1999        801       1200        22      180813

Kernel-Side Operations:
  Total operations: 145
  Total failures:   3

Arena Memory Statistics:
  Allocations:      2144
  Frees:            801
  Current allocs:   1343
  Failed allocs:    0

Data Structure State:
  Elements in list: 1343

Test Duration: 0.09 seconds
============================================================
```

**Key Metrics**:
- **Ops/sec**: Throughput per thread and total
- **Failures**: Operations that didn't complete (e.g., key not found on delete)
- **Kernel-Side Operations**: Count from BPF tracepoints
- **Current allocs**: Memory still in use (should match element count)

---

## Nomenclature and Conventions

### File Naming

| Type | Pattern | Example |
|------|---------|---------|
| Data structure header | `ds_<name>.h` | `ds_list.h`, `ds_tree.h` |
| BPF kernel program | `<name>.bpf.c` | `skeleton.bpf.c` |
| Userspace program | `<name>.c` | `skeleton.c` |
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
// Syscall tracepoints (for automatic triggers)
SEC("tp/syscalls/sys_enter_<syscall>")
int trace_<syscall>(struct trace_event_raw_sys_enter *ctx)

// Manual triggers (called via bpf_prog_test_run)
SEC("syscall")
int <operation_name>(void *ctx)
```

Common syscall tracepoints:
- `sys_enter_execve` - Program execution (frequent)
- `sys_enter_exit_group` - Process exit
- `sys_enter_openat` - File opening
- `sys_enter_write` - Write operations

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
      "header": "ds_tree.h",
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
    command: "sudo ./skeleton -t 1 -o 100 -w insert"
    expected: "Completes without errors"
  
  - name: "Concurrent access"
    command: "sudo ./skeleton -t 8 -o 1000 -w mixed"
    expected: "All threads complete, low failure rate"
  
  - name: "Verification"
    command: "sudo ./skeleton -t 4 -o 500 -v"
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
