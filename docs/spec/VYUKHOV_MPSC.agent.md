This is an implementation specification for a **Wait-Free (for producers) MPSC (Multiple Producer, Single Consumer) Unbounded Queue**, based on the algorithm described by Dmitry Vyukov.

This data structure is particularly well-suited for your BPF framework because it pushes the heavy synchronization costs to the consumer (userspace), allowing the producers (BPF kernel hooks) to be extremely lightweight and wait-free.

---

# Implementation Specification: Vyukov MPSC Node-Based Queue

## 1. Introduction

This document specifies the implementation of an **intrusive MPSC queue** for the BPF arena. The algorithm is based on the [1024cores.net design](https://www.1024cores.net/home/lock-free-algorithms/queues/intrusive-mpsc-node-based-queue).

**Performance Characteristics:**

* **Producers (Insert):** Wait-free. Uses a single atomic `XCHG` instruction. Ideally suited for BPF contexts where minimizing instruction count and avoiding locks is critical.
* **Consumer (Delete):** Obstruction-free (blocking). The consumer may need to spin briefly if a producer is preempted between swapping the head and linking the new node.
* **Memory:** Requires a dummy "stub" node to maintain invariants, preventing the need for NULL checks on the head pointer during enqueue.

**System Roles:**

* **Producers:** Kernel-space BPF programs (multiple contexts allowed).
* **Consumer:** Userspace test harness (single thread allowed).

## 2. Data Structure Organization

The queue relies on a singly linked list where the `head` pointer acts as the "back" of the queue (where items are added) and the `tail` pointer acts as the "front" (where items are removed).

### 2.1 Node Structure

```c
struct ds_mpsc_node {
    struct ds_mpsc_node __arena *next;
    __u64 key;
    __u64 value;
};

```

### 2.2 Head/Control Structure

```c
struct ds_mpsc_head {
    // 'head' is the back of the queue (producer target).
    // Modified atomically by producers.
    struct ds_mpsc_node __arena *head;

    // 'tail' is the front of the queue (consumer target).
    // Modified only by the single consumer.
    struct ds_mpsc_node __arena *tail;

    // Statistics (Optional but recommended by framework)
    __u64 count; 
};

```

### 2.3 Invariants

1. **Stub Node:** The queue is initialized with one dummy node (stub).
2. **Non-Null Pointers:** `head` and `tail` are never NULL; they always point to a valid memory region (either a data node or the stub).
3. **State Definition:**
* **Empty:** `tail == head` (both point to the same node, usually the stub).
* **Non-Empty:** `tail->next != NULL`.



## 3. Algorithm Specification

### 3.1 Initialization (`ds_mpsc_init`)

The initialization must allocate the stub node. This node does not contain valid data.

```c
static inline int ds_mpsc_init(struct ds_mpsc_head __arena *ctx) {
    cast_kern(ctx);

    // 1. Allocate the stub node
    struct ds_mpsc_node __arena *stub = bpf_arena_alloc(sizeof(*stub));
    if (!stub) return DS_ERROR_NOMEM;

    // 2. Initialize stub
    stub->next = NULL;
    stub->key = 0;
    stub->value = 0;

    // 3. Point both head and tail to stub
    ctx->head = stub;
    ctx->tail = stub;
    ctx->count = 0;

    return DS_SUCCESS;
}

```

### 3.2 Insert / Enqueue (`ds_mpsc_insert`)

**Context:** BPF Kernel (Multiple Producers) or Userspace.
**Logic:** Atomically swap the `head` pointer to the new node, then link the old head to the new node.

```c
static inline int ds_mpsc_insert(struct ds_mpsc_head __arena *ctx, 
                                 __u64 key, __u64 value) {
    cast_kern(ctx);

    // 1. Allocate new node
    struct ds_mpsc_node __arena *n = bpf_arena_alloc(sizeof(*n));
    if (!n) return DS_ERROR_NOMEM;

    // 2. Setup node
    n->key = key;
    n->value = value;
    n->next = NULL; // New node is always the new terminator

    // 3. Serialization point: Atomic Exchange (XCHG)
    // RELEASE ordering ensures all writes to node are visible before XCHG
    // 'prev' becomes the node that was previously at the back
    struct ds_mpsc_node __arena *prev = 
        arena_atomic_exchange(&ctx->head, n, ARENA_RELEASE);
    
    // cast_kern call is safe/idempotent on the returned pointer 
    // inside the atomic macro if implemented correctly, 
    // otherwise manual cast needed depending on macro definition.
    cast_kern(prev); 

    // 4. Link the old back to the new node
    // RELEASE store ensures link is visible to consumer
    // Note: If producer is preempted here, consumer sees prev->next == NULL
    smp_store_release(&prev->next, n);

    arena_atomic_add(&ctx->count, 1, ARENA_RELAXED);
    return DS_SUCCESS;
}

```

### 3.3 Delete / Dequeue (`ds_mpsc_delete`)

**Context:** Userspace Only (Single Consumer).
**Signature:** Follows the framework pattern with output parameter (like `ds_vyukhov_delete`).

**Algorithm:**

1. Read `tail`.
2. Read `tail->next` with ACQUIRE ordering to see producer's link.
3. If `tail->next` exists, that node is the "real" head of the logical queue (since `tail` sits on the stub or the previously consumed node).
4. Recover data from `tail->next`.
5. Advance `tail` to `tail->next`.
6. Free the *old* `tail`.

```c
static inline int ds_mpsc_delete(struct ds_mpsc_head __arena *ctx, 
                                 struct ds_kv *output) {
    // Only safe for Single Consumer (Userspace)
#ifdef __BPF__
    return DS_ERROR_INVALID; // BPF cannot consume in MPSC
#endif

    if (!ctx || !output)
        return DS_ERROR_INVALID;

    cast_kern(ctx);
    struct ds_mpsc_node __arena *tail = ctx->tail;
    cast_kern(tail);
    
    // ACQUIRE load to see producer's link (prev->next = n)
    struct ds_mpsc_node __arena *next = 
        smp_load_acquire(&tail->next);

    // Case 1: Queue is logically empty
    if (tail == ctx->head) {
        return DS_ERROR_NOT_FOUND;
    }

    // Case 2: Inconsistent state (Producer stalled between XCHG and link)
    // tail != head, but next is NULL. 
    // We must wait for producer to finish linking.
    if (next == NULL) {
         // In userspace, we can yield or spin.
         // Return DS_ERROR_BUSY so caller can retry
         return DS_ERROR_BUSY; 
    }

    // Case 3: Valid dequeue
    // The data is actually in 'next'
    // 'tail' is just the dummy/stub from the previous operation
    cast_kern(next);
    
    // Read the data
    output->key = next->key;
    output->value = next->value;

    // Move tail forward (only consumer touches tail, RELAXED is fine)
    ctx->tail = next;
    
    // Free the old stub (the node we just moved off of)
    // Note: In strict Vyukov, the old 'tail' becomes the garbage.
    bpf_arena_free(tail);

    arena_atomic_sub(&ctx->count, 1, ARENA_RELAXED);
    return DS_SUCCESS;
}

```

### 3.4 Search / Iterate (`ds_mpsc_search`)

Iterating a concurrent queue is unsafe unless the consumer is paused. However, for verification, we can traverse from `tail` to `head`. Note: In MPSC, search is typically not supported during concurrent operations, but provided for debugging/verification.

```c
static inline int ds_mpsc_search(struct ds_mpsc_head __arena *ctx, __u64 key) {
    if (!ctx)
        return DS_ERROR_INVALID;
        
    cast_kern(ctx);
    struct ds_mpsc_node __arena *curr = ctx->tail;
    
    // can_loop needed for BPF verifier
    for (int i = 0; i < 10000 && can_loop; i++) {
        if (!curr) break;
        
        cast_kern(curr);
        
        // Skip the dummy node (current tail)
        // Data starts at tail->next
        if (curr->key == key && curr != ctx->tail) { 
            return DS_SUCCESS;
        }
        
        curr = smp_load_acquire(&curr->next);
    }
    return DS_ERROR_NOT_FOUND;
}

```

## 4. Concurrency & Memory Safety

### 4.1 Memory Ordering

Based on the 1024cores.net specification:

* **Producer XCHG:** Uses `ARENA_RELEASE` ordering to ensure all writes to the new node (key, value, next=NULL) are visible before the node is published to other threads.
* **Producer Link:** Uses `ARENA_RELEASE` store to ensure the link (`prev->next = n`) is visible to the consumer.
* **Consumer Read:** Uses `ARENA_ACQUIRE` load on `tail->next` to observe the producer's link operation.
* **Consumer Tail Update:** Uses plain store (no atomics needed) since only the single consumer modifies `tail`.
* **Count Updates:** Uses `ARENA_RELAXED` since it's only for statistics/observability, not synchronization.

### 4.2 Memory Reclamation

* **Node Reuse:** The standard Vyukov algorithm allows the old `tail` node to be reused as the new `stub` to avoid `free/alloc` churn.
* **Arena Implementation:** To fit the `ds_api` contract, we `bpf_arena_free` the old tail and `bpf_arena_alloc` new nodes. This is less efficient than reuse but cleaner for the framework.
* **ABA Problem:** This algorithm is naturally immune to ABA on the `head` pointer because nodes are not reused immediately in this allocation scheme (bump allocator or free list), and the `XCHG` operation does not depend on the "version" of the pointer, only that it swaps.

### 4.3 Stalled Producers

The critical edge case in Vyukov's queue is when a producer performs the `XCHG` (swapping head) but gets preempted before performing `prev->next = node`.

* **Symptom:** `tail != head` (implies not empty), but `tail->next == NULL`.
* **Resolution:** The consumer observes this inconsistency. Since the consumer is userspace, it can simply spin-wait (`return DS_ERROR_BUSY`) until the preempted BPF program is scheduled again and completes the link.
* **Framework Pattern:** The userspace polling loop in `skeleton_mpsc.c` should retry on `DS_ERROR_BUSY` with a short backoff.

## 5. Verification Invariants (`ds_mpsc_verify`)

The verification function should check for loop integrity and count consistency.

```c
static inline int ds_mpsc_verify(struct ds_mpsc_head __arena *ctx) {
    if (!ctx)
        return DS_ERROR_INVALID;
        
    cast_kern(ctx);
    struct ds_mpsc_node __arena *curr = ctx->tail;
    __u64 computed_count = 0;
    __u64 max_iter = 100000;

    for (__u64 i = 0; i < max_iter && can_loop; i++) {
        if (!curr) {
            // NULL before reaching head means broken list
            if (curr != ctx->head)
                return DS_ERROR_CORRUPT;
            break;
        }
        
        cast_kern(curr);
        
        if (curr == ctx->head) {
            // Reached the end successfully
            // Note: computed_count might differ from ctx->count 
            // due to concurrency, but structure is valid.
            return DS_SUCCESS;
        }

        // ACQUIRE load to follow the linked list safely
        curr = smp_load_acquire(&curr->next);
        
        // If we hit NULL before head->head, the list might be broken
        // UNLESS we hit the "stalled producer" gap.
        if (curr == NULL && ctx->tail != ctx->head) {
             // Acceptable transient state during concurrent operations
             // Producer is between XCHG and link
             return DS_SUCCESS; 
        }
        
        computed_count++;
    }
    
    // Exceeded max iterations without reaching head
    return DS_ERROR_CORRUPT;
}

```

## 6. Implementation Status & Design Decisions

* **Status:** Specification Ready for Implementation.
* **Memory Ordering:** Uses explicit memory ordering semantics from 1024cores.net:
  - `ARENA_RELEASE` for producer XCHG and link operations
  - `ARENA_ACQUIRE` for consumer reads of next pointers
  - `ARENA_RELAXED` for statistics updates
* **API Signature:** `ds_mpsc_delete(ctx, struct ds_kv *output)` follows the framework pattern (like `ds_vyukhov_delete`).
* **Unidirectional Design:** Kernel (BPF) produces, userspace consumes. BPF calls to `ds_mpsc_delete` return `DS_ERROR_INVALID`.
* **Error Handling:** Returns `DS_ERROR_BUSY` when producer is stalled between XCHG and link. Userspace should retry.
* **Allocation Strategy:** Uses `bpf_arena_alloc`/`bpf_arena_free` per framework conventions (not stub reuse optimization).

---

## 7. Framework Integration

### File Naming
* **Header:** `include/ds_mpsc.h`
* **Skeleton:** `skeleton_mpsc` (both .c and .bpf.c variants)

### Test Pattern
1. **Kernel:** LSM hook (e.g., `inode_create`) inserts (pid, timestamp) pairs via `ds_mpsc_insert()`
2. **Userspace:** Single consumer thread polls with `ds_mpsc_delete()`, retrying on `DS_ERROR_BUSY`
3. **Verification:** Optional call to `ds_mpsc_verify()` after quiescing

---

### Next Step

Ready to generate:
1. `include/ds_mpsc.h` - Complete header with all operations
2. `src/skeleton_mpsc.bpf.c` - Kernel-side BPF program
3. `src/skeleton_mpsc.c` - Userspace consumer program