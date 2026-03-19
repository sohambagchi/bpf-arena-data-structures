# Converting Reference Data Structures to BPF Arena Implementations

This skill guides the conversion of concurrent data structure reference implementations (C/C++) into the BPF Arena framework with dual `_c` and `_lkmm` stubs, skeleton files, and build integration.

**Files involved:**
- `reference/` — original data structure implementations
- `include/ds_<name>.h` — arena-adapted header with `_c` and `_lkmm` implementations
- `src/skeleton_<name>.bpf.c` — BPF kernel-side program
- `src/skeleton_<name>.c` — userspace loader and relay thread
- `Makefile` — build integration (`BPF_APPS` variable)
- `scripts/runner.py` — integration test runner (`candidates` list)

---

## 1. Prerequisites and Context

### 1.1 Two-Lane Relay Architecture

Every skeleton program operates a two-lane relay between kernel and userspace:

1. **KU lane (kernel → userspace):** A kernel LSM hook on `inode_create` produces key-value pairs (PID, timestamp) into a data structure instance. A userspace relay thread consumes from this lane.
2. **UK lane (userspace → kernel):** The same relay thread re-inserts consumed items into a second data structure instance. A kernel uprobe, triggered by calling a noinline function, consumes from this lane.

The two lanes use separate instances of the same data structure type: `global_ds_head_ku` and `global_ds_head_uk`, both declared as `__arena` globals in the BPF skeleton.

### 1.2 Dual Implementation Requirement

Every data structure provides two implementations of each function:

- **`_c` suffix (C11 atomics):** Uses `arena_atomic_load`, `arena_atomic_store`, `arena_atomic_cmpxchg` with explicit C11 ordering constants (`ARENA_ACQUIRE`, `ARENA_RELEASE`, `ARENA_RELAXED`). Wrapped in `#ifndef __BPF__` / `#endif` — compiled only for userspace.
- **`_lkmm` suffix (Linux Kernel Memory Model):** Uses `READ_ONCE`, `WRITE_ONCE`, `smp_load_acquire`, `smp_store_release`, and `arena_atomic_cmpxchg`. NOT wrapped in any `#ifdef` — compiled for both BPF and userspace.

A router function with no suffix selects between them:

```c
static inline int ds_<name>_insert(struct ds_<name>_head *head, __u64 key, __u64 value) {
#ifdef __BPF__
    return ds_<name>_insert_lkmm(head, key, value);
#else
    return ds_<name>_insert_c(head, key, value);
#endif
}
```

### 1.3 Key Headers

| Header | Purpose |
|--------|---------|
| `bpf_arena_common.h` | `READ_ONCE`/`WRITE_ONCE`, `smp_load_acquire`/`smp_store_release`, `cast_kern`/`cast_user`, `__arena` annotation, `arena_container_of` |
| `libarena_ds.h` | Arena bump allocator (`bpf_arena_alloc`/`bpf_arena_free`), `arena_atomic_*` operations, `can_loop`, `ARENA_RELAXED`/`ARENA_ACQUIRE`/`ARENA_RELEASE`/`ARENA_ACQ_REL`/`ARENA_SEQ_CST` |
| `ds_api.h` | Return codes (`DS_SUCCESS`, `DS_ERROR_*`), `struct ds_kv` (key-value pair), `struct ds_metadata`, standard API macros |

> **Note**: `bpf_experimental.h` provides the `can_loop` macro and `bpf_addr_space_cast` (used internally by `cast_kern`/`cast_user`). It is included from the BPF skeleton file (`skeleton_<name>.bpf.c`), not from the data structure headers. The DS headers rely on it being included first.

### 1.4 BPF Verifier Constraints

The BPF verifier imposes constraints that affect every function:

- **`can_loop`:** Every loop condition must include `&& can_loop`. Without it, the verifier rejects the program as having unbounded loops.
- **`cast_kern(ptr)`:** Must be called before dereferencing any arena pointer (`ptr->field`). In BPF context, this converts a user-space arena address to a kernel-space arena address.
- **`cast_user(ptr)`:** Must be called before: (1) NULL checks on arena pointers, (2) storing a pointer value into an arena field, (3) passing a pointer as the new-value argument to CAS.
- **`__arena` annotation:** Every pointer field in an arena-resident struct that points to other arena memory must be annotated with `__arena`.

---

## 2. Step 1 — Analyze the Reference Implementation

Read the reference implementation thoroughly (located in `reference/`). Identify:

1. **Struct types:** Node structures, head/queue/stack control structures, auxiliary types.
2. **Function signatures:** Init, enqueue/push, dequeue/pop, search, verify. Map each to the standard API names: `ds_<name>_init`, `ds_<name>_insert`, `ds_<name>_pop`, `ds_<name>_delete`, `ds_<name>_search`, `ds_<name>_verify`.
3. **Synchronization primitives:** CAS operations (`ck_pr_cas_ptr`, `__sync_bool_compare_and_swap`), atomic loads/stores (`ck_pr_load_ptr`, `ck_pr_store_ptr`), fences (`ck_pr_fence_store`, `ck_pr_fence_load`), memory barriers.
4. **Memory management:** Intrusive nodes (caller allocates) vs. value-copy (data structure allocates). Allocation strategy (per-operation allocation, slab, ring buffer).
5. **Concurrency model:** SPSC (single-producer, single-consumer), MPMC (multi-producer, multi-consumer), UPMC (unbounded-producer, multi-consumer), etc.
6. **Shared vs. private variables:** Which fields are written by multiple threads (require atomics) vs. written by only one thread (can use relaxed/plain access).

**Example mapping for CK Stack UPMC:**

| Reference function | Arena API function |
|---|---|
| `ck_stack_push_upmc` | `ds_ck_stack_upmc_insert` (allocates + pushes) |
| `ck_stack_pop_upmc` | `ds_ck_stack_upmc_pop` (pops + copies data) |
| `ck_stack_trypush_upmc` | `ds_ck_stack_upmc_trypush_upmc` (single-attempt push) |
| N/A (new) | `ds_ck_stack_upmc_init` |
| N/A (new) | `ds_ck_stack_upmc_search` |
| N/A (new) | `ds_ck_stack_upmc_verify` |

---

## 3. Step 2 — Design Arena Data Structures

### 3.1 Struct Conversion Rules

Every pointer field in a node or head struct that points to arena memory must use the `__arena` annotation:

```c
struct ds_foo_node __arena *next;    /* arena pointer to next node */
```

Use `typedef` for arena-annotated node types for convenience:

```c
typedef struct ds_foo_node __arena ds_foo_node_t;
```

Embed `struct ds_kv` for key-value data storage:

```c
struct ds_kv data;    /* fields: __u64 key, __u64 value */
```

Add cache-line padding between producer and consumer fields to prevent false sharing (56 bytes of padding + 8-byte field = 64-byte cache line):

```c
struct {
    __u32 idx;
} write_idx __attribute__((aligned(64)));
char pad[56];
struct {
    __u32 idx;
} read_idx __attribute__((aligned(64)));
```

### 3.2 Linked-List Node Template

For linked-list data structures (stacks, queues, FIFOs):

```c
struct ds_<name>_node;
typedef struct ds_<name>_node __arena ds_<name>_node_t;

struct ds_<name>_node {
    ds_<name>_node_t *next;     /* arena pointer to next node */
    struct ds_kv data;          /* embedded key-value pair */
};

struct ds_<name>_head {
    ds_<name>_node_t *head;     /* head pointer (or top, front, etc.) */
    ds_<name>_node_t *tail;     /* tail pointer (if applicable) */
    __u64 count;                /* element count (advisory, relaxed) */
};

typedef struct ds_<name>_head __arena ds_<name>_head_t;
```

See `include/ds_ck_stack_upmc.h` for a minimal linked-list example, or `include/ds_msqueue.h` for a queue with separate node/elem wrappers.

> **Note on naming flexibility**: The naming pattern `ds_<name>_head` / `ds_<name>_node` is the common convention, but implementations may deviate. For example, the Michael-Scott Queue uses `struct ds_msqueue` (no `_head` suffix) with `struct ds_msqueue_elem` and `struct ds_msqueue_node` as separate types. The Folly SPSC uses the prefix `ds_spsc_` rather than `ds_folly_spsc_`. Choose names that are clear and consistent within your implementation.

### 3.3 Ring-Buffer Node Template

For bounded ring-buffer data structures:

```c
struct ds_<name>_node {
    struct ds_kv data;          /* key-value pair */
    __u64 sequence;             /* sequence number (if Vyukov-style) */
};

struct ds_<name>_head {
    struct {
        __u32 idx;
    } write_idx __attribute__((aligned(64)));

    struct {
        __u32 idx;
    } read_idx __attribute__((aligned(64)));

    __u32 size;                 /* total slots (capacity + 1 for SPSC) */
    __u32 mask;                 /* size - 1 (for power-of-two indexing) */
    struct ds_<name>_node __arena *buffer;   /* arena-allocated array */
};
```

See `include/ds_folly_spsc.h` for a ring-buffer SPSC example, or `include/ds_vyukhov.h` for a bounded MPMC queue with sequence numbers.

---

## 4. Step 3 — Write Initialization Functions

### 4.1 LKMM Init (Linked-List DS with Dummy Node)

```c
static inline int ds_<name>_init_lkmm(struct ds_<name>_head __arena *head)
{
    struct ds_<name>_node __arena *dummy;

    if (!head)
        return DS_ERROR_INVALID;
    // cast_kern(head) only if head is an arena pointer; 
    // if head is stack/BSS-allocated, no cast needed
    cast_kern(head);

    /* Allocate dummy/sentinel node */
    dummy = bpf_arena_alloc(sizeof(*dummy));
    if (!dummy)
        return DS_ERROR_NOMEM;

    /* Initialize dummy — cast_kern before writing fields */
    cast_kern(dummy);
    WRITE_ONCE(dummy->next, NULL);
    WRITE_ONCE(dummy->data.key, 0);
    WRITE_ONCE(dummy->data.value, 0);

    /* cast_user before storing into arena field */
    cast_user(dummy);
    head->head = dummy;
    head->tail = dummy;     /* if applicable */
    head->count = 0;

    return DS_SUCCESS;
}
```

### 4.2 LKMM Init (Ring-Buffer DS)

```c
static inline int ds_<name>_init_lkmm(struct ds_<name>_head __arena *head, __u32 capacity)
{
    struct ds_<name>_node __arena *buffer;

    cast_kern(head);

    head->size = capacity;
    head->mask = capacity - 1;
    WRITE_ONCE(head->write_idx.idx, 0);
    WRITE_ONCE(head->read_idx.idx, 0);

    buffer = bpf_arena_alloc(capacity * sizeof(struct ds_<name>_node));
    if (!buffer)
        return DS_ERROR_NOMEM;

    cast_kern(buffer);
    for (__u32 i = 0; i < capacity && can_loop; i++) {
        struct ds_<name>_node __arena *cell = &buffer[i];
        cast_kern(cell);
        WRITE_ONCE(cell->sequence, i);   /* if using sequence numbers */
    }

    /* cast_user before storing pointer into arena field */
    cast_user(buffer);
    head->buffer = buffer;

    return DS_SUCCESS;
}
```

### 4.3 C11 Init Variant

The `_c` init is structurally identical but uses `arena_atomic_store` instead of `WRITE_ONCE`:

```c
#ifndef __BPF__
static inline int ds_<name>_init_c(struct ds_<name>_head __arena *head)
{
    struct ds_<name>_node __arena *dummy;

    if (!head)
        return DS_ERROR_INVALID;
    // cast_kern(head) only if head is an arena pointer; 
    // if head is stack/BSS-allocated, no cast needed
    cast_kern(head);

    dummy = bpf_arena_alloc(sizeof(*dummy));
    if (!dummy)
        return DS_ERROR_NOMEM;

    cast_kern(dummy);
    arena_atomic_store(&dummy->next, NULL, ARENA_RELAXED);
    arena_atomic_store(&dummy->data.key, 0, ARENA_RELAXED);
    arena_atomic_store(&dummy->data.value, 0, ARENA_RELAXED);

    cast_user(dummy);
    head->head = dummy;
    head->tail = dummy;
    head->count = 0;

    return DS_SUCCESS;
}
#endif
```

> **Note on `head` parameter in init functions**: The `head` parameter in init functions is typically NOT an arena pointer that needs `cast_kern` — it's usually a BSS global (`&global_ds_head_ku`) or stack-allocated, not a dynamically allocated arena object. In those cases, `cast_kern` is not needed at all. However, if your init function receives a dynamically allocated arena pointer, the NULL check MUST come before `cast_kern` (since `cast_kern` on NULL is undefined behavior in BPF).

---

## 5. Step 4 — Translate to `_c` (C11 Atomic) Version

This is the first implementation pass. Convert the reference implementation function by function.

### 5.1 Conversion Rules for `_c` Functions

| Reference pattern | Arena `_c` equivalent |
|---|---|
| Read of shared field | `arena_atomic_load(&field, ARENA_<ordering>)` |
| Write to shared field | `arena_atomic_store(&field, value, ARENA_<ordering>)` |
| CAS operation | `arena_atomic_cmpxchg(&field, expected, desired, success_mo, failure_mo)` |
| Memory fence (`ck_pr_fence_store`) | Absorbed into `ARENA_RELEASE` on CAS; or `arena_memory_barrier()` for seq_cst |
| Loop condition | Add `&& can_loop` to every loop |
| Pointer dereference | Precede with `cast_kern(ptr)` |
| NULL check on arena pointer | Precede with `cast_user(ptr)` |
| Store pointer into arena field | Precede with `cast_user(ptr)` |

All `_c` functions must be wrapped in `#ifndef __BPF__` / `#endif`.

### 5.2 Memory Ordering Selection

| Access pattern | Ordering |
|---|---|
| Publication store (making data visible) | `ARENA_RELEASE` |
| Consumption load (reading published data) | `ARENA_ACQUIRE` |
| Private field (only one thread writes) | `ARENA_RELAXED` |
| CAS for claiming a slot (sync handled elsewhere) | `ARENA_RELAXED, ARENA_RELAXED` |
| CAS for publishing a pointer | `ARENA_RELEASE, ARENA_RELAXED` |
| Advisory counter (statistics) | `ARENA_RELAXED` |

### 5.3 Concrete Example: Treiber Stack Push

```c
/* Reference (ck_stack.h):
 *   stack = ck_pr_load_ptr(&target->head);
 *   entry->next = stack;
 *   ck_pr_fence_store();
 *   while (ck_pr_cas_ptr_value(&target->head, stack, entry, &stack) == false) {
 *       entry->next = stack;
 *       ck_pr_fence_store();
 *   }
 */

/* Converted _c version: */
#ifndef __BPF__
static inline void ds_ck_stack_upmc_push_upmc_c(ds_ck_stack_upmc_head_t *stack,
                                                 ds_ck_stack_upmc_entry_t *entry,
                                                 __u64 key, __u64 value)
{
    ds_ck_stack_upmc_entry_t *head;
    ds_ck_stack_upmc_entry_t *observed;
    bool pushed = false;

    if (!stack || !entry)
        return;

    cast_kern(stack);
    cast_kern(entry);

    entry->data.key = key;
    entry->data.value = value;
    head = arena_atomic_load(&stack->head, ARENA_RELAXED);

    do {
        arena_atomic_store(&entry->next, head, ARENA_RELAXED);
        cast_user(entry);       /* before using entry as CAS new_val */
        observed = arena_atomic_cmpxchg(&stack->head, head, entry,
                                        ARENA_RELEASE, ARENA_RELAXED);
        if (observed == head) {
            pushed = true;
            break;
        }
        head = observed;
    } while (can_loop);

    if (pushed)
        arena_atomic_add(&stack->count, 1, ARENA_RELAXED);
}
#endif
```

Key observations:
- `ck_pr_load_ptr` → `arena_atomic_load(..., ARENA_RELAXED)` (head is read in a CAS loop; stale value is retried)
- `ck_pr_fence_store()` + `ck_pr_cas_ptr` → single `arena_atomic_cmpxchg(..., ARENA_RELEASE, ARENA_RELAXED)` (the release on CAS success absorbs the store fence)
- `cast_user(entry)` before the CAS because `entry` is the new value being stored into an arena field

---

## 6. Step 5 — Create `_lkmm` Version

Duplicate each `_c` function, rename with `_lkmm` suffix, and apply LKMM conversions. Remove the `#ifndef __BPF__` guard — `_lkmm` functions compile for both BPF and userspace.

### 6.1 Conversion Table: `_c` to `_lkmm`

| `_c` operation | `_lkmm` equivalent |
|---|---|
| `arena_atomic_load(&field, ARENA_RELAXED)` | `READ_ONCE(field)` |
| `arena_atomic_store(&field, value, ARENA_RELAXED)` | `WRITE_ONCE(field, value)` |
| `arena_atomic_load(&field, ARENA_ACQUIRE)` | `smp_load_acquire(&field)` |
| `arena_atomic_store(&field, value, ARENA_RELEASE)` | `smp_store_release(&field, value)` |
| `arena_atomic_cmpxchg(...)` | `arena_atomic_cmpxchg(...)` (unchanged) |
| `arena_atomic_add(&field, val, mo)` | `arena_atomic_add(&field, val, mo)` (unchanged) |
| `arena_atomic_sub(&field, val, mo)` | `arena_atomic_sub(&field, val, mo)` (unchanged) |
| `arena_atomic_inc(&field)` | `arena_atomic_inc(&field)` (unchanged) |
| `arena_atomic_dec(&field)` | `arena_atomic_dec(&field)` (unchanged) |
| `arena_memory_barrier()` | Usually removable (see LKMM optimizations) |

> **When to use plain stores vs WRITE_ONCE**: Use `WRITE_ONCE` for stores to shared fields. Plain stores are acceptable ONLY for fields that are demonstrably private to the current thread and will not be read by another thread until after a subsequent release operation publishes them (e.g., `entry->next = head` in a push operation, where `entry` is private until the release-CAS publishes it).

### 6.2 Concrete Example: Treiber Stack Push

```c
/* _lkmm version — no #ifdef guard */
static inline void ds_ck_stack_upmc_push_upmc_lkmm(ds_ck_stack_upmc_head_t *stack,
                                                    ds_ck_stack_upmc_entry_t *entry,
                                                    __u64 key, __u64 value)
{
    ds_ck_stack_upmc_entry_t *head;
    ds_ck_stack_upmc_entry_t *observed;
    bool pushed = false;

    if (!stack || !entry)
        return;

    cast_kern(stack);
    cast_kern(entry);

    entry->data.key = key;
    entry->data.value = value;
    head = READ_ONCE(stack->head);   /* was: arena_atomic_load(..., ARENA_RELAXED) */

    do {
        entry->next = head;          /* was: arena_atomic_store(..., ARENA_RELAXED) */
        cast_user(entry);
        observed = arena_atomic_cmpxchg(&stack->head, head, entry,
                                        ARENA_RELEASE, ARENA_RELAXED);
        if (observed == head) {
            pushed = true;
            break;
        }
        head = observed;
    } while (can_loop);

    if (pushed)
        arena_atomic_add(&stack->count, 1, ARENA_RELAXED);
}
```

---

## 7. Step 6 — Apply LKMM Dependency-Enforced Optimizations

This is the critical optimization step. The `_lkmm` version can exploit ordering relationships that C11 does not recognize, reducing barrier overhead on weakly-ordered architectures.

### 7.1 Rule 1: Address Dependency (Pointer Chase)

**Pattern:** `READ_ONCE(shared_ptr)` followed by dereferencing that pointer value.

**Effect:** The dereference is ordered after the read that obtained the pointer — no acquire needed on the pointer load.

**Decision:** If `_c` uses `ARENA_ACQUIRE` to load a pointer, and the `_lkmm` code immediately dereferences that pointer, replace the acquire with `READ_ONCE`.

**Example — MS Queue pop:**

```c
/* _c version: */
head = arena_atomic_load(&queue->head, ARENA_ACQUIRE);
cast_kern(head);
next = arena_atomic_load(&head->node.next, ARENA_ACQUIRE);

/* _lkmm version: */
head = READ_ONCE(queue->head);
/* LKMM: address dependency from head to head->node.next provides ordering */
cast_kern(head);
next = READ_ONCE(head->node.next);    /* ordered by address dependency */
```

**Also applies to CAS:** If a CAS returns a pointer that is then dereferenced, the CAS success ordering can be `ARENA_RELAXED` instead of `ARENA_ACQUIRE`. The MS Queue pop CAS uses `ARENA_RELAXED, ARENA_RELAXED` in the `_lkmm` version because the address dependency chain `READ_ONCE(queue->head)` → `READ_ONCE(head->node.next)` → `next_elem->data` already provides visibility. See `ds_msqueue_pop_lkmm` line 421.

**CRITICAL:** The loaded pointer MUST be read with `READ_ONCE()` and the dereference of the obtained value MUST also use `READ_ONCE()`. Plain loads break the dependency chain because the compiler can optimize them away (register caching, load elimination, speculative value prediction).

### 7.2 Rule 2: Control Dependency (Branch + Store)

**Pattern:** `READ_ONCE(shared_index)` → conditional branch using that value → stores inside the taken branch.

**Effect:** The stores cannot be hoisted above the branch — no acquire on the index needed.

**Decision:** If `_c` uses `ARENA_ACQUIRE` to load an index that is then used in an if-condition, and all dependent operations are stores (not loads), replace with `READ_ONCE`.

**Example — Folly SPSC insert:**

```c
/* _c version: */
current_read = arena_atomic_load(&head->read_idx.idx, ARENA_ACQUIRE);
if (next_record != current_read) {
    arena_atomic_store(&node->key, key, ARENA_RELAXED);
    arena_atomic_store(&node->value, value, ARENA_RELAXED);
    arena_atomic_store(&head->write_idx.idx, next_record, ARENA_RELEASE);
}

/* _lkmm version: */
/* LKMM: control dependency from read_idx load to subsequent stores
 * provides sufficient ordering; acquire not needed */
current_read = READ_ONCE(head->read_idx.idx);
if (next_record != current_read) {
    node->key = key;       /* store inside branch — ordered by ctrl dep */
    node->value = value;   /* store inside branch — ordered by ctrl dep */
    smp_store_release(&head->write_idx.idx, next_record);
}
```

This optimization is also used in `ds_ck_ring_spsc_insert_lkmm` where the producer reads `c_head` (the consumer's index) and branches on whether the ring is full.

**WARNING:** Control dependencies order stores but NOT loads. If the branch body contains loads of shared data, those loads are NOT ordered by the control dependency. Use `smp_load_acquire` for loads that must see published data.

### 7.3 Rule 3: Release-Acquire Pairs (Unchanged)

**Pattern:** One thread does `smp_store_release(&flag, value)`, another does `smp_load_acquire(&flag)`.

Both `_c` and `_lkmm` use this pattern — it cannot be further optimized. This is the fundamental handoff primitive.

**Examples:**
- Vyukov queue: `cell->sequence` uses release-acquire for producer↔consumer handoff
- Folly SPSC: `write_idx` published with release, consumed with acquire
- CK Ring SPSC: `p_tail` published with release, consumed with acquire

### 7.4 Rule 4: READ_ONCE/WRITE_ONCE for Compiler Discipline

Always use `READ_ONCE` for shared fields even when no hardware ordering is needed. Always use `WRITE_ONCE` for shared fields during initialization and for writes that don't need release semantics.

**Prevents:**
- Register caching (compiler reuses a stale value from a register)
- Load/store tearing (compiler splits a 64-bit access into two 32-bit accesses)
- Store merging (compiler combines multiple stores into one)
- Speculative reads (compiler reads a field before a conditional that guards it)

### 7.5 SPSC Single-Writer Relaxation

**SPSC Single-Writer Relaxation**

In SPSC (single-producer single-consumer) data structures, fields that are exclusively written by one side (e.g., `fifo->head` is consumer-only, `fifo->tail` is producer-only) do not need release semantics when updated. The `_c` version may conservatively use `ARENA_RELEASE` for these writes, but the `_lkmm` version can safely use `WRITE_ONCE` because no other thread contends on the write. See `ds_ck_fifo_spsc_dequeue_lkmm` where `WRITE_ONCE(fifo->head, entry)` replaces `arena_atomic_store(&fifo->head, entry, ARENA_RELEASE)` from the `_c` version.

### 7.6 Comment Convention

Every LKMM optimization site must have an inline comment explaining the dependency:

```c
/* LKMM: address dependency from head to head->next provides ordering */
/* LKMM: control dependency from this load to the subsequent stores provides sufficient ordering */
/* LKMM: consumer-only field in SPSC; no cross-thread sync needed */
/* LKMM: address dependency chain (head → head->next → data) ensures data visibility; relax CAS to RELAXED */
```

### 7.7 Decision Flowchart

When encountering an `ARENA_ACQUIRE` load in a `_c` function, ask:

1. **Is the loaded value a pointer that is immediately dereferenced?**
   - YES → Address dependency. Replace with `READ_ONCE`. Ensure the dereference also uses `READ_ONCE`.
   - NO → Continue to question 2.

2. **Is the loaded value used in a conditional branch, and are all dependent operations inside the branch stores (not loads)?**
   - YES → Control dependency to stores. Replace with `READ_ONCE`.
   - NO → Continue to question 3.

3. **Is the load part of a release-acquire pair that cannot be decomposed?**
   - YES → Keep as `smp_load_acquire`. No optimization possible.
   - NO → Analyze the specific pattern; it may be a composite case.

---

## 8. Step 7 — Create Router Functions

Every public API function needs a router that selects between `_c` and `_lkmm`:

```c
static inline int ds_<name>_init(struct ds_<name>_head __arena *head)
{
#ifdef __BPF__
    return ds_<name>_init_lkmm(head);
#else
    return ds_<name>_init_c(head);
#endif
}
```

### 8.1 Complete File Layout for `include/ds_<name>.h`

```
#pragma once

#include "ds_api.h"

/* ================================================================
 * DATA STRUCTURES
 * ================================================================ */

struct ds_<name>_node;
typedef struct ds_<name>_node __arena ds_<name>_node_t;

struct ds_<name>_node {
    ds_<name>_node_t *next;
    struct ds_kv data;
};

struct ds_<name>_head {
    ds_<name>_node_t *head;
    __u64 count;
};

typedef struct ds_<name>_head __arena ds_<name>_head_t;

/* ================================================================
 * LKMM IMPLEMENTATION (_lkmm suffix)
 * ================================================================ */
/* No #ifdef guard — compiles for both BPF and userspace */

static inline int ds_<name>_init_lkmm(ds_<name>_head_t *head) { ... }
static inline int ds_<name>_insert_lkmm(ds_<name>_head_t *head, __u64 key, __u64 value) { ... }
static inline int ds_<name>_pop_lkmm(ds_<name>_head_t *head, struct ds_kv *out) { ... }
static inline int ds_<name>_search_lkmm(ds_<name>_head_t *head, __u64 key) { ... }
static inline int ds_<name>_verify_lkmm(ds_<name>_head_t *head) { ... }

/* ================================================================
 * C11 ATOMIC IMPLEMENTATION (_c suffix)
 * ================================================================ */

#ifndef __BPF__

static inline int ds_<name>_init_c(ds_<name>_head_t *head) { ... }
static inline int ds_<name>_insert_c(ds_<name>_head_t *head, __u64 key, __u64 value) { ... }
static inline int ds_<name>_pop_c(ds_<name>_head_t *head, struct ds_kv *out) { ... }
static inline int ds_<name>_search_c(ds_<name>_head_t *head, __u64 key) { ... }
static inline int ds_<name>_verify_c(ds_<name>_head_t *head) { ... }

#endif /* !__BPF__ */

/* ================================================================
 * API ROUTERS
 * ================================================================ */

static inline int ds_<name>_init(ds_<name>_head_t *head) {
#ifdef __BPF__
    return ds_<name>_init_lkmm(head);
#else
    return ds_<name>_init_c(head);
#endif
}

static inline int ds_<name>_insert(ds_<name>_head_t *head, __u64 key, __u64 value) {
#ifdef __BPF__
    return ds_<name>_insert_lkmm(head, key, value);
#else
    return ds_<name>_insert_c(head, key, value);
#endif
}

/* ... routers for pop, search, verify, etc. ... */
```

---

## 9. Step 8 — Create BPF Skeleton (`src/skeleton_<name>.bpf.c`)

Full template:

```c
// SPDX-License-Identifier: GPL-2.0

/* skeleton_<name>.bpf.c - BPF Arena <Name> Data Structure
 *
 * Two-lane relay architecture:
 *   KU lane: kernel produces (LSM hook) → userspace consumes (relay thread)
 *   UK lane: userspace produces (relay thread) → kernel consumes (uprobe)
 */
#define BPF_NO_KFUNC_PROTOTYPES
#include <vmlinux.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>
#include "bpf_experimental.h"

/* ================================================================
 * ARENA MAP DEFINITION
 * ================================================================ */
struct {
    __uint(type, BPF_MAP_TYPE_ARENA);
    __uint(map_flags, BPF_F_MMAPABLE);
    __uint(max_entries, 1000);
#ifdef __TARGET_ARCH_arm64
    __ulong(map_extra, 0x1ull << 32);
#else
    __ulong(map_extra, 0x1ull << 44);
#endif
} arena SEC(".maps");

/* ================================================================
 * LIBRARY AND DATA STRUCTURE INCLUDES
 * (MUST come AFTER arena map definition)
 * ================================================================ */
#include "libarena_ds.h"
#include "ds_api.h"
#include "ds_<name>.h"

/* ================================================================
 * GLOBAL STATE (arena-resident)
 * ================================================================ */
struct ds_<name>_head __arena global_ds_head_ku;
struct ds_<name>_head __arena global_ds_head_uk;

/* ================================================================
 * STATISTICS (BSS, accessible from userspace via skel->bss)
 * ================================================================ */
__u64 total_kernel_prod_ops = 0;
__u64 total_kernel_prod_failures = 0;
__u64 total_kernel_consume_ops = 0;
__u64 total_kernel_consume_failures = 0;
__u64 total_kernel_consumed = 0;
bool initialized_ku = false;
/* UK initialization is handled from userspace (relay_worker); no BSS flag needed */

/* ================================================================
 * KERNEL-SIDE PRODUCER (LSM Hook)
 * ================================================================ */
SEC("lsm.s/inode_create")
int BPF_PROG(lsm_inode_create, struct inode *dir, struct dentry *dentry, umode_t mode)
{
    struct ds_<name>_head __arena *head = &global_ds_head_ku;
    __u64 pid;
    __u64 ts;
    int result;

    (void)dir;
    (void)dentry;
    (void)mode;

    if (!initialized_ku) {
        result = ds_<name>_init_lkmm(head);   /* add capacity arg if bounded */
        if (result != DS_SUCCESS)
            return 0;
        initialized_ku = true;
    }

    pid = bpf_get_current_pid_tgid() >> 32;
    ts = bpf_ktime_get_ns();
    result = ds_<name>_insert_lkmm(head, pid, ts);

    total_kernel_prod_ops++;
    if (result != DS_SUCCESS)
        total_kernel_prod_failures++;

    return 0;
}

/* ================================================================
 * KERNEL-SIDE CONSUMER (Uprobe)
 * ================================================================ */
SEC("uprobe.s")
int bpf_<name>_consume(struct pt_regs *ctx)
{
    struct ds_<name>_head __arena *head = &global_ds_head_uk;
    struct ds_kv out = {};
    int ret;

    (void)ctx;

    /* UK lane not yet initialized by userspace — check DS internal state */
    cast_kern(head);
    if (!head->head)  /* adjust sentinel for your DS type */
        return DS_ERROR_INVALID;

    ret = ds_<name>_pop_lkmm(head, &out);
    total_kernel_consume_ops++;
    if (ret == DS_SUCCESS) {
        total_kernel_consumed++;
        bpf_printk("<name> consume key=%llu value=%llu\n", out.key, out.value);
    } else {
        total_kernel_consume_failures++;
    }

    return ret;
}

char _license[] SEC("license") = "GPL";
```

**Critical notes:**
- Includes MUST be ordered: `libarena_ds.h` → `ds_api.h` → `ds_<name>.h`, and MUST come AFTER the arena map definition. The allocator in `libarena_ds.h` references the `arena` map symbol.
- The LSM hook calls `_lkmm` functions directly (not the router), because this code only compiles under `__BPF__`.
- The uprobe calls `_lkmm` functions directly for the same reason.
- The `bpf_printk` format string must contain `"<name> consume key=%llu value=%llu\n"` — the runner.py trace validation looks for this pattern.
- Use `lsm.s/inode_create` (sleepable LSM) and `uprobe.s` (sleepable uprobe) SEC types.

---

## 10. Step 9 — Create Userspace Loader (`src/skeleton_<name>.c`)

Full template:

```c
// SPDX-License-Identifier: GPL-2.0

/* skeleton_<name>.c - Userspace loader for <Name> data structure
 *
 * Two-lane relay:
 *   1. Kernel LSM hook produces into KU lane
 *   2. This program's relay thread consumes from KU, produces into UK
 *   3. Kernel uprobe consumes from UK lane
 */
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>

#include "ds_api.h"
#include "ds_<name>.h"
#include "skeleton_<name>.skel.h"

/* ================================================================
 * CONFIGURATION
 * ================================================================ */
struct test_config {
    bool verify;
    bool print_stats;
};

static struct test_config config = {
    .verify = false,
    .print_stats = true,
};

static struct skeleton_<name>_bpf *skel;
static volatile sig_atomic_t stop_test;
static pthread_t relay_thread;
static bool relay_thread_started;
static __u64 ku_dequeued_count;
static __u64 uk_enqueued_count;

/* ================================================================
 * UPROBE TRIGGER
 * ================================================================ */
__attribute__((noinline)) void <name>_kernel_consume_trigger(void)
{
    asm volatile("" ::: "memory");
}

/* ================================================================
 * SIGNAL HANDLER
 * ================================================================ */
static void signal_handler(int sig)
{
    (void)sig;
    stop_test = 1;
}

/* ================================================================
 * USERSPACE ALLOCATOR SETUP
 * ================================================================ */
static int setup_userspace_allocator(void)
{
    size_t arena_bytes;
    size_t alloc_bytes;
    void *alloc_base;
    long page_size;

    page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0)
        return -1;

    arena_bytes = (size_t)bpf_map__max_entries(skel->maps.arena) * (size_t)page_size;
    if (arena_bytes <= (size_t)page_size)
        return -1;

    alloc_base = (void *)((char *)skel->arena + (size_t)page_size);
    alloc_bytes = arena_bytes - (size_t)page_size;
    bpf_arena_userspace_set_range(alloc_base, alloc_bytes);

    printf("Arena alloc range: base=%p size=%zu KB\n", alloc_base, alloc_bytes / 1024);
    return 0;
}

/* ================================================================
 * BPF PROGRAM ATTACHMENT
 * ================================================================ */
static int attach_programs(void)
{
    struct bpf_link *lsm_link;
    struct bpf_link *consume_link;
    struct bpf_uprobe_opts uprobe_opts = {
        .sz = sizeof(uprobe_opts),
        .func_name = "<name>_kernel_consume_trigger",
    };
    int err;

    lsm_link = bpf_program__attach_lsm(skel->progs.lsm_inode_create);
    err = libbpf_get_error(lsm_link);
    if (err)
        return err;
    skel->links.lsm_inode_create = lsm_link;

    consume_link = bpf_program__attach_uprobe_opts(
        skel->progs.bpf_<name>_consume,
        getpid(),
        "/proc/self/exe",
        0,
        &uprobe_opts);
    err = libbpf_get_error(consume_link);
    if (err)
        return err;
    skel->links.bpf_<name>_consume = consume_link;

    return 0;
}

/* ================================================================
 * RELAY WORKER THREAD
 * ================================================================ */
static void *relay_worker(void *arg)
{
    struct ds_<name>_head *head_ku = &skel->arena->global_ds_head_ku;
    struct ds_<name>_head *head_uk = &skel->arena->global_ds_head_uk;
    struct ds_kv data;
    bool uk_initialized = false;
    int ret;

    (void)arg;

    printf("UserThread: waiting for KU lane initialization...\n");
    // Wait for KU lane initialization by kernel
    // Check the DS's internal state (e.g., head pointer, buffer, slots)
    while (!stop_test) {
        // Adjust sentinel check for your DS type:
        //   Linked-list DS: head_ku->head != NULL
        //   Ring buffer DS: head_ku->buffer != NULL (or head_ku->slots)
        //   FIFO DS: head_ku->fifo.head && head_ku->fifo.tail
        if (head_ku->head != NULL)  
            break;
        usleep(1000);
    }
    if (stop_test)
        return NULL;

    printf("UserThread: relay loop started (KU -> UK)\n");

    while (!stop_test) {
        if (!uk_initialized) {
            int ret = ds_<name>_init_c(head_uk);  // add capacity arg if bounded
            if (ret == DS_SUCCESS)
                uk_initialized = true;
            else
                continue;
        }

        ret = ds_<name>_pop_c(head_ku, &data);
        if (ret == DS_SUCCESS) {
            int ins_ret;

            ku_dequeued_count++;
            ins_ret = ds_<name>_insert_c(head_uk, data.key, data.value);
            if (ins_ret == DS_SUCCESS)
                uk_enqueued_count++;
            continue;
        }

        if (ret == DS_ERROR_NOT_FOUND || ret == DS_ERROR_INVALID)
            continue;
    }

    return NULL;
}

/* ================================================================
 * KERNEL CONSUMER DRAIN
 * ================================================================ */
static void trigger_kernel_consumer_on_exit(void)
{
    __u64 initial_consumed;
    __u64 target_consumed;
    __u64 attempts = 0;
    __u64 max_attempts;

    initial_consumed = skel->bss->total_kernel_consumed;
    target_consumed = initial_consumed + uk_enqueued_count;
    max_attempts = uk_enqueued_count + 1024;

    printf("MainThread: triggering kernel consumer uprobe...\n");

    if (uk_enqueued_count == 0) {
        <name>_kernel_consume_trigger();
        return;
    }

    while (attempts < max_attempts &&
           skel->bss->total_kernel_consumed < target_consumed) {
        <name>_kernel_consume_trigger();
        attempts++;
    }

    printf("MainThread: consume triggers=%llu consumed=%llu target=%llu\n",
           (unsigned long long)attempts,
           (unsigned long long)skel->bss->total_kernel_consumed,
           (unsigned long long)target_consumed);
}

/* ================================================================
 * VERIFICATION
 * ================================================================ */
static int verify_data_structure(void)
{
    struct ds_<name>_head *head_ku = &skel->arena->global_ds_head_ku;
    struct ds_<name>_head *head_uk = &skel->arena->global_ds_head_uk;
    int ku_result = DS_SUCCESS;
    int uk_result = DS_SUCCESS;

    printf("Verifying <name> lanes from userspace...\n");

    // Guard: only verify if the lane was initialized
    // Adjust sentinel check for your DS type
    if (head_ku->head)  // or head_ku->buffer, head_ku->slots, etc.
        ku_result = ds_<name>_verify_c(head_ku);
    if (head_uk->head)  // adjust sentinel
        uk_result = ds_<name>_verify_c(head_uk);

    if (ku_result == DS_SUCCESS && uk_result == DS_SUCCESS) {
        printf("Verification PASSED (KU=%d UK=%d)\n", ku_result, uk_result);
        return DS_SUCCESS;
    }

    printf("Verification FAILED (KU=%d UK=%d)\n", ku_result, uk_result);
    return DS_ERROR_INVALID;
}

/* ================================================================
 * STATISTICS
 * ================================================================ */
static void print_statistics(void)
{
    printf("\n============================================================\n");
    printf("                <NAME> RELAY STATISTICS                     \n");
    printf("============================================================\n");
    printf("Kernel producer (inode_create -> KU):\n");
    printf("  ops=%llu failures=%llu\n",
           (unsigned long long)skel->bss->total_kernel_prod_ops,
           (unsigned long long)skel->bss->total_kernel_prod_failures);

    printf("Kernel consumer (uprobe pop from UK):\n");
    printf("  ops=%llu failures=%llu consumed=%llu\n",
           (unsigned long long)skel->bss->total_kernel_consume_ops,
           (unsigned long long)skel->bss->total_kernel_consume_failures,
           (unsigned long long)skel->bss->total_kernel_consumed);

    printf("Userspace relay:\n");
    printf("  KU popped=%llu UK pushed=%llu\n",
           (unsigned long long)ku_dequeued_count,
           (unsigned long long)uk_enqueued_count);
    printf("============================================================\n\n");
}

/* ================================================================
 * ARGUMENT PARSING
 * ================================================================ */
static void print_usage(const char *prog)
{
    printf("Usage: %s [OPTIONS]\n\n", prog);
    printf("<Name> relay test (kernel->user->kernel lanes)\n\n");
    printf("OPTIONS:\n");
    printf("  -v      Verify both lanes on exit\n");
    printf("  -s      Print statistics on exit (default: enabled)\n");
    printf("  -h      Show this help\n");
}

static int parse_args(int argc, char **argv)
{
    int opt;

    while ((opt = getopt(argc, argv, "vsh")) != -1) {
        switch (opt) {
        case 'v':
            config.verify = true;
            break;
        case 's':
            config.print_stats = true;
            break;
        case 'h':
            print_usage(argv[0]);
            exit(0);
        default:
            print_usage(argv[0]);
            return -1;
        }
    }

    return 0;
}

/* ================================================================
 * MAIN
 * ================================================================ */
int main(int argc, char **argv)
{
    int err;

    if (parse_args(argc, argv) < 0)
        return 1;

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    printf("Loading BPF program for <name> relay...\n");
    skel = skeleton_<name>_bpf__open_and_load();
    if (!skel) {
        fprintf(stderr, "Failed to open and load BPF skeleton\n");
        return 1;
    }

    err = setup_userspace_allocator();
    if (err) {
        fprintf(stderr, "Failed to set userspace arena allocator range\n");
        goto cleanup;
    }

    err = attach_programs();
    if (err) {
        fprintf(stderr, "Failed to attach BPF programs: %d\n", err);
        goto cleanup;
    }

    err = pthread_create(&relay_thread, NULL, relay_worker, NULL);
    if (err) {
        fprintf(stderr, "Failed to create relay thread: %s\n", strerror(err));
        err = -1;
        goto cleanup;
    }
    relay_thread_started = true;

    printf("MainThread: attached. Trigger inode_create events in another shell.\n");
    printf("Press Ctrl+C to stop and invoke kernel consumer trigger.\n");

    while (!stop_test)
        pause();

    if (relay_thread_started)
        pthread_join(relay_thread, NULL);

    trigger_kernel_consumer_on_exit();

    if (config.verify)
        verify_data_structure();
    if (config.print_stats)
        print_statistics();

    err = 0;

cleanup:
    skeleton_<name>_bpf__destroy(skel);
    return err;
}
```

**Key details:**
- The uprobe trigger function name must match between `skeleton_<name>.c` (the `__attribute__((noinline))` function) and `skeleton_<name>.bpf.c` (the `uprobe_opts.func_name`).
- The relay worker calls `_c` functions (userspace C11 atomics), NOT the router functions and NOT `_lkmm` functions.
- The relay worker waits for KU lane initialization by polling the data structure's internal state (e.g., `head_ku->head != NULL`), not by checking a BSS flag. The UK lane is initialized with `ds_<name>_init_c` using a local `uk_initialized` boolean — no BSS flag is used.
- Links are stored in `skel->links.*` for automatic cleanup by `skeleton_<name>_bpf__destroy()`.

---

## 11. Step 10 — Build Integration

### 11.1 Makefile Changes

Append `skeleton_<name>` to the `BPF_APPS` variable (line ~103 of `Makefile`):

```makefile
# Before:
BPF_APPS = skeleton_msqueue skeleton_vyukhov skeleton_folly_spsc skeleton_ck_fifo_spsc skeleton_ck_ring_spsc skeleton_ck_stack_upmc

# After:
BPF_APPS = skeleton_msqueue skeleton_vyukhov skeleton_folly_spsc skeleton_ck_fifo_spsc skeleton_ck_ring_spsc skeleton_ck_stack_upmc skeleton_<name>
```

If you also create a usertest (pure-userspace pthread test without BPF), add it to `USERTEST_APPS` on the next line. This guide does not cover usertest creation.

### 11.2 scripts/runner.py Changes

Append `'skeleton_<name>'` to the `candidates` list in `find_executables()` (line ~25):

```python
# Before:
candidates = ['skeleton_msqueue', 'skeleton_vyukhov', 'skeleton_folly_spsc', 'skeleton_ck_fifo_spsc', 'skeleton_ck_ring_spsc', 'skeleton_ck_stack_upmc']

# After:
candidates = ['skeleton_msqueue', 'skeleton_vyukhov', 'skeleton_folly_spsc', 'skeleton_ck_fifo_spsc', 'skeleton_ck_ring_spsc', 'skeleton_ck_stack_upmc', 'skeleton_<name>']
```

---

## 12. Step 11 — Update Documentation

After the implementation compiles and passes basic testing:

1. **`docs/LKMM_OPTIMIZATIONS.md`:** Add a new section with function-by-function analysis of the new data structure's `_lkmm` functions, explaining every LKMM optimization applied. Follow the format of the existing sections (algorithm sketch → function-by-function analysis).

2. **`INDEX.md`:** Add entries for the new files in the Source Code and Test Framework sections.

3. **`docs/GUIDE.md`:** Add a row to the "Implemented Data Structures" table:

```markdown
| **<Name>** | `ds_<name>.h` | `skeleton_<name>` | <Description>. |
```

---

## 13. cast_kern / cast_user Rules Reference

A concise reference card for pointer casting in arena code:

| Situation | Call |
|---|---|
| Before dereferencing an arena pointer (`ptr->field`) | `cast_kern(ptr)` |
| Before NULL check on arena pointer (`if (!ptr)`) | `cast_user(ptr)` |
| Before storing ptr into an arena field (`head->next = ptr`) | `cast_user(ptr)` |
| Before using ptr as new_val in CAS | `cast_user(ptr)` |
| After `bpf_arena_alloc()` | `cast_kern(ptr)` to initialize fields |
| After initializing fields, before storing into arena | `cast_user(ptr)` |

**Common pattern for CAS loops:**

```c
ptr = READ_ONCE(head->top);       /* ptr is user-space form after READ_ONCE */
while (ptr != NULL && can_loop) {
    cast_kern(ptr);                /* dereference: read ptr->next */
    next = READ_ONCE(ptr->next);
    cast_user(ptr);                /* before CAS that stores ptr */
    observed = arena_atomic_cmpxchg(&head->top, ptr, next,
                                    ARENA_RELEASE, ARENA_RELAXED);
    if (observed == ptr) break;
    ptr = observed;
}
```

**Common pattern for allocation:**

```c
node = bpf_arena_alloc(sizeof(*node));
if (!node) return DS_ERROR_NOMEM;
cast_kern(node);                   /* write fields */
node->data.key = key;
node->data.value = value;
node->next = NULL;
cast_user(node);                   /* before storing into arena */
head->top = node;
```

**Note:** In userspace, `cast_kern` and `cast_user` are no-ops (macros that expand to nothing). They only have effect when compiled under `__BPF__`. They must still be present in all code for correctness when the same header is compiled for BPF.

---

## 14. arena_atomic_* Quick Reference

| Operation | `_c` version | `_lkmm` equivalent |
|---|---|---|
| Relaxed load | `arena_atomic_load(&x, ARENA_RELAXED)` | `READ_ONCE(x)` |
| Relaxed store | `arena_atomic_store(&x, v, ARENA_RELAXED)` | `WRITE_ONCE(x, v)` |
| Acquire load | `arena_atomic_load(&x, ARENA_ACQUIRE)` | `smp_load_acquire(&x)` |
| Release store | `arena_atomic_store(&x, v, ARENA_RELEASE)` | `smp_store_release(&x, v)` |
| CAS | `arena_atomic_cmpxchg(&x, old, new, s_mo, f_mo)` | `arena_atomic_cmpxchg(&x, old, new, s_mo, f_mo)` (same) |
| Fetch-add | `arena_atomic_add(&x, v, mo)` | `arena_atomic_add(&x, v, mo)` (same) |
| Fetch-sub | `arena_atomic_sub(&x, v, mo)` | `arena_atomic_sub(&x, v, mo)` (same) |
| Increment | `arena_atomic_inc(&x)` | `arena_atomic_inc(&x)` (same) |
| Decrement | `arena_atomic_dec(&x)` | `arena_atomic_dec(&x)` (same) |
| Bitwise AND | `arena_atomic_and(&x, v, mo)` | `arena_atomic_and(&x, v, mo)` (same) |
| Bitwise OR | `arena_atomic_or(&x, v, mo)` | `arena_atomic_or(&x, v, mo)` (same) |
| Exchange | `arena_atomic_exchange(&x, v, mo)` | `arena_atomic_exchange(&x, v, mo)` (same) |
| Full fence | `arena_memory_barrier()` | Usually removable in `_lkmm` |

**Key insight:** Only loads and stores differ between `_c` and `_lkmm`. All read-modify-write operations (CAS, fetch-add, fetch-sub, exchange) use the same `arena_atomic_*` macros in both variants because they map to `__atomic` builtins that work identically in BPF and userspace.

---

## 15. Verification Checklist

Run through this checklist after completing the implementation:

1. [ ] All arena pointer fields annotated with `__arena`
2. [ ] All loops contain `&& can_loop`
3. [ ] Every arena pointer dereference preceded by `cast_kern()`
4. [ ] Every NULL check on arena pointer preceded by `cast_user()`
5. [ ] Every pointer stored into arena field is in user-space form (`cast_user()` called before store)
6. [ ] Every `_lkmm` function has a corresponding `_c` function
7. [ ] Every public API function has a router with `#ifdef __BPF__`
8. [ ] `_c` functions wrapped in `#ifndef __BPF__` / `#endif`
9. [ ] `_lkmm` functions NOT wrapped in any `#ifdef`
10. [ ] LKMM optimization sites have inline `/* LKMM: ... */` comments
11. [ ] BPF skeleton has both `lsm.s/inode_create` and `uprobe.s` entry points
12. [ ] `bpf_printk` in uprobe consumer uses format: `"<name> consume key=%llu value=%llu\n"`
13. [ ] Arena map definition uses correct architecture-specific `map_extra` (arm64: `0x1ull << 32`, others: `0x1ull << 44`)
14. [ ] Includes in `.bpf.c` are ordered: `libarena_ds.h` → `ds_api.h` → `ds_<name>.h` (AFTER arena map)
15. [ ] `Makefile` updated with `skeleton_<name>` in `BPF_APPS`
16. [ ] `scripts/runner.py` updated with `'skeleton_<name>'` in candidates list
17. [ ] Build succeeds: `make skeleton_<name>`
18. [ ] Run test: `sudo ./build/skeleton_<name> -v -s`
19. [ ] Integration test: `sudo python3 scripts/runner.py skeleton_<name>`

---

## 16. Common Pitfalls

### 16.1 Forgetting `cast_kern()` Before Dereference

**Symptom:** BPF verifier rejects the program with an error about accessing arena memory without proper address space cast.

**Fix:** Add `cast_kern(ptr)` before every `ptr->field` access.

### 16.2 Forgetting `cast_user()` Before NULL Check

**Symptom:** BPF verifier rejects the program. The NULL check operates on a kernel-space address, which the verifier cannot validate.

**Fix:** Add `cast_user(ptr)` before `if (!ptr)` or `if (ptr == NULL)`.

### 16.3 Using Plain Loads Instead of `READ_ONCE()` on Shared Fields in `_lkmm`

**Symptom:** Data races or incorrect behavior on weakly-ordered architectures. The compiler may cache the value in a register, read it multiple times and get different results, or optimize away the dependency chain entirely.

**Fix:** Use `READ_ONCE()` for every read of a shared field in `_lkmm` code. This is especially critical for address dependency chains — a plain `head->next` breaks the dependency that `READ_ONCE(head->next)` preserves.

### 16.4 Including `ds_<name>.h` Before the Arena Map Definition in `.bpf.c`

**Symptom:** Compilation error. The allocator in `libarena_ds.h` references the `arena` symbol, which must be defined first.

**Fix:** Always define the arena map struct BEFORE the `#include "libarena_ds.h"` line.

### 16.5 Missing `can_loop` in Loops

**Symptom:** BPF verifier rejects the program with "back-edge" or "infinite loop" errors.

**Fix:** Add `&& can_loop` to every `while` and `for` loop condition. For `do { ... } while (cond)` loops, use `do { ... } while (cond && can_loop)` or convert to a while loop with `can_loop`.

### 16.6 Using `arena_atomic_load`/`arena_atomic_store` in `_lkmm` Functions

**Symptom:** May cause verifier issues or miss LKMM optimization opportunities. The `_lkmm` functions should use `READ_ONCE`/`WRITE_ONCE`/`smp_load_acquire`/`smp_store_release` for loads and stores.

**Fix:** Use the LKMM primitives in `_lkmm` functions. Reserve `arena_atomic_load`/`arena_atomic_store` for `_c` functions only. CAS and fetch-add/sub operations (`arena_atomic_cmpxchg`, `arena_atomic_add`, `arena_atomic_sub`) are the same in both variants.

### 16.7 Not Using `ARENA_RELEASE` on a CAS That Publishes Data

**Symptom:** Data race on weakly-ordered architectures (ARM64, RISC-V). The consuming thread may see the new pointer but not the data fields that were written before the CAS.

**Fix:** Use `ARENA_RELEASE` as the success ordering on any CAS that links a new node into a shared data structure (makes the node visible to other threads).

### 16.8 Assuming Control Dependencies Order Loads

**Symptom:** Data race. A load inside a branch that depends on a `READ_ONCE` value is NOT ordered by the control dependency. Only stores are ordered.

**Fix:** If the branch body contains loads of shared data that must be ordered, use `smp_load_acquire` on the condition variable instead of relying on the control dependency.
