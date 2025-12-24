# Prompt

```
Implement [DATA_STRUCTURE_NAME] for this BPF arena testing framework.

1. Create `ds_[name].h` following the ds_api.h contract (study ds_list.h as reference).

2. Create NEW skeleton files `skeleton_[name].bpf.c` and `skeleton_[name].c` based on the existing skeletons - copy them and replace all `ds_list_*` calls with `ds_[name]_*` calls (look for /* DS_API_* */ comments as markers). Update #include to use your new header.

DO NOT modify the original skeleton.bpf.c or skeleton.c files.

Key requirements:
- All operations must update stats (count, failures, total_time_ns)
- Use __arena for arena pointers, cast_kern()/cast_user() appropriately  
- Implement verify() to check structural integrity
- Use can_loop in all loops
- Memory via bpf_arena_alloc()/bpf_arena_free()

See GUIDE.md for details. Ask questions if the specification is unclear.
```

## Critical Lessons Learned

### Compilation & Verifier Issues

1. **Arena Variables Not in BSS Section**
   - Variables declared with `__arena` (e.g., `struct foo __arena global_var;`) live in arena memory, NOT in the BPF program's BSS section
   - **Cannot access via skeleton:** `skel->bss->global_stats` will fail if `global_stats` is `__arena`
   - **Fix:** Remove arena variable access from userspace, or declare without `__arena` if userspace needs access

2. **Config Variables Must Be Writable from Userspace**
   - Do NOT use `volatile const` for variables userspace needs to write (they go to `.rodata` which is read-only)
   - **Correct:** `int config_value = 100;` (goes to `.bss`, writable)
   - **Wrong:** `volatile const int config_value = 100;` (goes to `.rodata`, read-only)
   - **Alternative:** Skip writing config from userspace, use BPF defaults

3. **BPF Verifier Requires cast_kern() for Arena Pointers** ⚠️ **CRITICAL**
   - When taking address of arena global: `ds_head = &global_ds_head;`
   - **MUST call `cast_kern(ds_head);` immediately after** to validate pointer for verifier
   - Without it, verifier sees a scalar and rejects memory access with "R1 invalid mem access 'scalar'"
   - **Applies to ALL initialization points:** Check every place where you assign `ds_head = &global_ds_head;`
   - Example from MS Queue: Three initialization sites in `skeleton_msqueue.bpf.c` all required this fix

4. **Dual-Context Headers Need Conditional Compilation**
   - Headers included by both BPF and userspace need `#ifdef __BPF__` guards
   - **can_loop macro:** Defined by `bpf_experimental.h` in BPF context, but undefined in userspace
   - **Fix in libarena_ds.h:**
     ```c
     #ifndef can_loop
     #define can_loop 1
     #endif
     ```

### Code Quality & API Conformance

5. **Initialize All Variables Used in Conditional Paths**
   - Compiler warning `-Wmaybe-uninitialized` often indicates real bugs
   - Example: `expected_pprev` in `ds_list_verify()` must be initialized to `&head->first`

6. **Mark Intentionally Unused CAS Results**
   - In lock-free algorithms, some CAS operations are "helping" moves where failure is acceptable
   - Cast to `(void)` to silence warnings: `(void)arena_atomic_cmpxchg(&head->tail, tail, node);`
   - Documents that ignoring the result is intentional, not a bug

7. **Unused Parameters in API Conformance**
   - Some data structures don't use all parameters from `ds_api.h` contract
   - Example: MS Queue `delete()` doesn't use `key` (FIFO, not keyed), but `ds_api.h` requires it
   - **Fix:** Add `__attribute__((unused))` to parameter declaration

8. **Add New Apps to Makefile**
   - After creating `skeleton_[name].bpf.c` and `skeleton_[name].c`
   - **Must add to:** `APPS = skeleton skeleton_[name]` in Makefile
   - Otherwise `make` won't build the new programs

### Memory Safety Considerations (Lock-Free Algorithms)

9. **Memory Reclamation Strategy**
   - Current framework uses immediate `bpf_arena_free()` after node removal
   - **Page-level reference counting:** Each page tracks number of allocated objects; page freed when count reaches zero
   - **Not hazard-pointer safe:** Individual objects on same page can be freed while other threads access different objects on that page
   - **Per-CPU allocation:** Reduces contention but doesn't prevent ABA on same-CPU sequential allocations
   - For production lock-free structures, consider epoch-based reclamation or hazard pointers
   - Document this limitation in your implementation specification

10. **ABA Problem Mitigation**
    - BPF arena provides natural ABA protection through memory isolation and page metadata
    - Arena pointers implicitly include version-like information
    - **Explicit version counters per-pointer are NOT required** for this framework
    - Note: If porting to general-purpose C, you WOULD need explicit version counters as described in academic papers

### Toolchain Requirements

- **clang-20 or newer** required for full BPF arena support with atomic operations
- clang-11 crashes on atomic operations on arena memory (LLVM backend bug)
- Update in Makefile: `CLANG ?= clang-20`

---

## Framework Overview for Algorithm Specification Authors

**Purpose:** This guide helps AI agents produce detailed implementation specification documents for concurrent data structures in our BPF arena framework.

### What is This Framework?

A **concurrent data structure testing framework** enabling simultaneous access from:
- **Linux kernel space** (BPF programs attached to LSM hooks)
- **User space** (direct memory access to shared arena)

### Core Architecture Components

#### 1. **BPF Arena - Shared Memory Foundation**
- Sparse, mmap-able memory region (up to 4GB) shared between kernel BPF and userspace
- Both sides use real C pointers (not BPF maps)
- Address space 1 (`__arena` attribute) with automatic LLVM cast handling
- `bpf_arena_alloc()` - Per-CPU page fragment allocator for node allocation
- `bpf_arena_free()` - Memory deallocation with page-level reference counting (no-op in userspace)

#### 2. **ds_api.h Contract - Required Operations**
Every data structure MUST implement:
- `ds_<name>_init(head)` - Initialize empty structure
- `ds_<name>_insert(head, key, value)` - Add element
- `ds_<name>_delete(head, key)` - Remove element  
- `ds_<name>_search(head, key)` - Find element
- `ds_<name>_verify(head)` - Check structural integrity
- `ds_<name>_get_stats(head, stats)` - Get operation statistics
- `ds_<name>_reset_stats(head)` - Reset statistics counters
- `ds_<name>_get_metadata()` - Get data structure metadata

**Optional operations:**
- `ds_<name>_iterate(head, callback, ctx)` - Traverse elements (implemented in reference implementations)

**All operations must:**
- Return `DS_SUCCESS`, `DS_ERROR_NOMEM`, `DS_ERROR_NOT_FOUND`, `DS_ERROR_INVALID`, etc.
- Use `__arena` pointers and `bpf_arena_alloc()`/`bpf_arena_free()`
- Work in both BPF and userspace contexts

#### 3. **Skeleton Pattern - Dual-Context Programs**

**Kernel side** (`skeleton_<name>.bpf.c`):
- LSM hook attachment (`lsm.s/inode_create`) triggers on file creation
- Populates data structure with (pid, timestamp) pairs
- **CRITICAL:** Call `cast_kern(ds_head)` after taking address of arena globals

**Userspace side** (`skeleton_<name>.c`):
- Multi-threaded test harness with direct arena mmap access
- Workload generators and verification
- No syscalls needed for read operations

#### 4. **Atomic Primitives (C11 with Explicit Memory Ordering)**
- `arena_atomic_cmpxchg(ptr, old_val, new_val, success_mo, failure_mo)` - Compare-and-swap
  - Memory orders: `ARENA_RELAXED`, `ARENA_ACQUIRE`, `ARENA_RELEASE`, `ARENA_ACQ_REL`, `ARENA_SEQ_CST`
- `arena_atomic_exchange(ptr, val, mo)` - Atomic exchange
- `arena_atomic_add(ptr, val, mo)` - Atomic fetch-and-add
- `arena_atomic_sub(ptr, val, mo)` - Atomic fetch-and-subtract
- `arena_atomic_load(ptr, mo)` / `arena_atomic_store(ptr, val, mo)` - Explicit atomic access
- Convenience: `arena_atomic_inc(ptr)`, `arena_atomic_dec(ptr)` - Relaxed increment/decrement
- `WRITE_ONCE() / READ_ONCE()` - Volatile access (alternative to atomic load/store)

**ABA mitigation:** Framework provides natural protection via arena page metadata. Explicit version counters NOT required.

#### 5. **Verification & Testing**
- `verify()` checks data structure invariants
- Statistics track: operation counts, failures, timing
- Userspace validates structure after kernel populates

### Specification Document Structure

Your document should contain these sections:

1. **Introduction** - Algorithm overview, performance characteristics, safety/liveness guarantees
2. **Data Structure Organization** - Node/head structures, invariants, arena pointer mapping
3. **Algorithm Pseudo-Code** - Init, insert, delete, search with BPF annotations (`can_loop`, `cast_kern()`)
4. **Concurrency & Memory Safety** - Required atomics, memory reclamation, progress guarantees
5. **Implementation Considerations** - API mapping, edge cases, verification approach
6. **Example Concurrent Scenario** - Race condition walkthrough, CAS conflict resolution
7. **Implementation Status** - Completed features, known limitations

### Critical Framework Constraints

**Arena Pointer Discipline:**
- Declare all pointers with `__arena` attribute
- Call `cast_kern()` immediately after taking address of arena globals
- Use `cast_user()` for userspace-visible contexts

**Verifier Compatibility:**
- All loops MUST have `can_loop` guard
- Bound iterations in verify() (max iteration limit)
- No unbounded recursion

**Dual-Context Headers:**
- Must compile in both BPF (`__BPF__` defined) and userspace
- Conditionally define missing macros: `#ifndef can_loop #define can_loop 1 #endif`

### Reference Implementation Pattern

```c
#pragma once
#include "ds_api.h"

struct ds_<name>_node {
    struct ds_<name>_node __arena *next;  // Arena pointer
    __u64 key;
    __u64 value;
};

struct ds_<name>_head {
    struct ds_<name>_node __arena *root;
    __u64 count;  // NOT __arena - accessed via skel->bss
};

static inline int ds_<name>_init(struct ds_<name>_head __arena *head) {
    cast_kern(head);  // CRITICAL
    head->root = NULL;
    head->count = 0;
    return DS_SUCCESS;
}

static inline int ds_<name>_insert(struct ds_<name>_head __arena *head, 
                                    __u64 key, __u64 value) {
    struct ds_<name>_node __arena *node = bpf_arena_alloc(sizeof(*node));
    if (!node) return DS_ERROR_NOMEM;
    
    // Algorithm implementation with arena_atomic_* as needed
    // Example: arena_atomic_cmpxchg(&ptr, old, new, ARENA_ACQ_REL, ARENA_RELAXED)
    
    arena_atomic_inc(&head->count);  // Relaxed atomic increment
    return DS_SUCCESS;
}

static inline int ds_<name>_verify(struct ds_<name>_head __arena *head) {
    cast_kern(head);
    __u64 visited = 0;
    __u64 max_iter = 10000;  // Bound iteration for verifier
    
    // Check invariants with can_loop guards
    for (... ; visited < max_iter && can_loop; visited++) {
        // Verify logic
    }
    return DS_SUCCESS;
}
```

### Essential Questions Your Specification Must Answer

1. **Core data structures?** → Define with `__arena` pointers
2. **Algorithmic steps?** → Pseudo-code with BPF annotations
3. **Required atomic operations?** → Map to `arena_atomic_*`
4. **Memory allocation/deallocation?** → `bpf_arena_alloc/free()` placement
5. **Verification invariants?** → Structural and semantic properties
6. **Concurrency edge cases?** → Race conditions, helping mechanisms
7. **API parameter mapping?** → Handle unused params with `__attribute__((unused))`

---

## Files to Include

1. `ds_api.h` - API contract
2. `libarena_ds.h` - Per-CPU page fragment allocator
3. `ds_list.h` - Reference implementation (doubly-linked list with locking)
4. `ds_msqueue.h` - Reference implementation (Michael-Scott lock-free FIFO queue)
5. `ds_mpsc.h` - Reference implementation (Vyukhov's MPSC unbounded queue)
6. `ds_vyukhov.h` - Reference implementation (Vyukhov's bounded MPMC queue)
7. `skeleton.bpf.c` - Kernel skeleton template
8. `skeleton.c` - Userspace skeleton template
9. `GUIDE.md` - Framework guide
10. [Your data structure specification document - the output you're producing]