Here is the implementation specification for the **Bounded MPMC Queue**, adapted from the 1024cores algorithm for the BPF Arena framework.

---

#BPF Arena Data Structure Specification: Bounded MPMC Queue##1. IntroductionThis document specifies the implementation of a **Bounded Multi-Producer Multi-Consumer (MPMC) Queue** for the BPF Arena framework. The algorithm is based on [Dmitry Vyukov's Bounded MPMC Queue](https://www.1024cores.net/home/lock-free-algorithms/queues/bounded-mpmc-queue).

###Characteristics* **Throughput:** Extremely high; operations typically cost one CAS (Compare-And-Swap) in the fast path.
* **Concurrency:** Lock-free implementation. Producers and consumers are disjoint; they only contend on shared cache lines when the queue is full or empty.
* **Bounded:** The queue has a fixed capacity (power of 2) determined at initialization.
* **Mechanism:** Uses a circular buffer with "sequence" counters in each cell to coordinate access and solve the ABA problem naturally.

###Guarantees* **Safety:** No data races on elements; sequence counters ensure a cell is written only when empty and read only when full.
* **Liveness:** Lock-free (some thread always makes progress).
* **FIFO:** Strict First-In-First-Out ordering.

---

##2. Data Structure OrganizationTo minimize false sharing, the structure relies heavily on padding between the producer index (`enqueue_pos`) and consumer index (`dequeue_pos`).

###Node Structure (Cell)Represents a single slot in the ring buffer.

```c
struct ds_mpmc_node {
    __u64 sequence;      // Coordination counter
    __u64 key;           // User data
    __u64 value;         // User data
};

```

###Head StructureThe control structure residing in shared memory.

```c
struct ds_mpmc_head {
    // Producer State
    __u64 enqueue_pos;           // Atomic counter
    char pad1[56];               // Cache line padding (assuming 64-byte line)

    // Consumer State
    __u64 dequeue_pos;           // Atomic counter
    char pad2[56];               // Cache line padding

    // Constants & Memory
    __u64 buffer_mask;           // Capacity - 1
    struct ds_mpmc_node __arena *buffer; // Pointer to array
    
    // Statistics (Optional/Debug)
    __u64 count;                 // Optional: purely for observability
};

```

###Arena Mapping* **Head:** Allocated via `bpf_arena_alloc` or strictly cast from map value.
* **Buffer:** A single contiguous allocation of `(mask + 1) * sizeof(node)` allocated via `bpf_arena_alloc` during initialization.

---

##3. Algorithm Pseudo-CodeAll pointers must be cast using `cast_kern()` or `cast_user()` depending on context.

###3.1 Initialization (`ds_mpmc_init`)Initializes the sequence counters so that the first lap (generation 0) is ready to accept writes.

```c
static inline int ds_mpmc_init(struct ds_mpmc_head __arena *head, __u32 capacity) {
    cast_kern(head);
    
    // Capacity must be power of 2 (e.g., 1024)
    if (capacity < 2 || (capacity & (capacity - 1)) != 0) 
        return DS_ERROR_INVALID;

    head->buffer_mask = capacity - 1;
    head->enqueue_pos = 0;
    head->dequeue_pos = 0;

    // Allocate the contiguous array
    // Note: In bump allocator, we alloc once.
    head->buffer = bpf_arena_alloc(capacity * sizeof(struct ds_mpmc_node));
    if (!head->buffer) return DS_ERROR_NOMEM;
    cast_kern(head->buffer);

    // Initialize sequence counters
    // Cell 0 gets seq 0, Cell 1 gets seq 1...
    for (__u32 i = 0; i < capacity && can_loop; i++) {
        head->buffer[i].sequence = i;
        head->buffer[i].key = 0;
        head->buffer[i].value = 0;
    }

    return DS_SUCCESS;
}

```

###3.2 Insert / Enqueue (`ds_mpmc_insert`)Producers compete to reserve a slot. A slot is available if `cell->sequence == enqueue_pos`.

```c
static inline int ds_mpmc_insert(struct ds_mpmc_head __arena *head, 
                                 __u64 key, __u64 value) {
    struct ds_mpmc_node __arena *cell;
    __u64 pos = READ_ONCE(head->enqueue_pos);
    __u64 mask = head->buffer_mask;
    int retries = 0;

    // Bounded loop for verifier
    for (; retries < 10000 && can_loop; retries++) {
        cell = &head->buffer[pos & mask];
        cast_kern(cell); // Verify pointer access
        
        __u64 seq = READ_ONCE(cell->sequence);
        __s64 dif = (__s64)seq - (__s64)pos;

        if (dif == 0) {
            // Cell is ready to be written. Try to increment enqueue_pos.
            __u64 old_pos = arena_atomic_cmpxchg(&head->enqueue_pos, pos, pos + 1);
            
            if (old_pos == pos) {
                // Success: We claimed this slot.
                cell->key = key;
                cell->value = value;
                
                // Release: Mark cell as ready for consumer (seq = pos + 1)
                // Use atomic exchange or write primitive with barrier if available
                arena_atomic_exchange(&cell->sequence, pos + 1); 
                return DS_SUCCESS;
            }
            // CAS failed: reload and retry
        } 
        else if (dif < 0) {
            // Sequence < pos: Queue is full.
            // Wait or return error. For this framework, we return error.
            return DS_ERROR_NOMEM; // "Full"
        }
        else {
            // Sequence > pos: Rare race, reload pos
        }
        
        pos = READ_ONCE(head->enqueue_pos);
    }
    
    return DS_ERROR_BUSY;
}

```

###3.3 Delete / Dequeue (`ds_mpmc_delete`)**Note:** In a Queue context, `key` is ignored. This function acts as `pop_front`.

Consumers compete for data. Data is available if `cell->sequence == dequeue_pos + 1`.

```c
static inline int ds_mpmc_delete(struct ds_mpmc_head __arena *head, __u64 key) {
    struct ds_mpmc_node __arena *cell;
    __u64 pos = READ_ONCE(head->dequeue_pos);
    __u64 mask = head->buffer_mask;
    int retries = 0;

    // Use unused parameter macro to avoid compiler warnings
    (void)key; 

    for (; retries < 10000 && can_loop; retries++) {
        cell = &head->buffer[pos & mask];
        cast_kern(cell);
        
        __u64 seq = READ_ONCE(cell->sequence);
        __s64 dif = (__s64)seq - (__s64)(pos + 1);

        if (dif == 0) {
            // Cell has data. Try to increment dequeue_pos.
            __u64 old_pos = arena_atomic_cmpxchg(&head->dequeue_pos, pos, pos + 1);
            
            if (old_pos == pos) {
                // Success: We claimed the data.
                __u64 val = cell->value; // Read data
                
                // Release: Mark cell as ready for producer (seq = pos + mask + 1)
                // This "wraps" the sequence for the next lap.
                arena_atomic_exchange(&cell->sequence, pos + mask + 1);
                return DS_SUCCESS;
            }
        }
        else if (dif < 0) {
            // Queue is empty
            return DS_ERROR_NOT_FOUND;
        }
        
        pos = READ_ONCE(head->dequeue_pos);
    }

    return DS_ERROR_BUSY;
}

```

###3.4 Search (`ds_mpmc_search`)**Constraint:** Queues do not support efficient random access search.
**Implementation:** Linear scan (snapshot). Note that this is not atomic relative to the whole queue state; it may miss items moving during the scan.

```c
static inline int ds_mpmc_search(struct ds_mpmc_head __arena *head, __u64 key) {
    __u64 start = READ_ONCE(head->dequeue_pos);
    __u64 end = READ_ONCE(head->enqueue_pos);
    __u64 mask = head->buffer_mask;
    
    for (__u64 i = start; i < end && can_loop; i++) {
        struct ds_mpmc_node __arena *cell = &head->buffer[i & mask];
        cast_kern(cell);
        if (cell->key == key) return DS_SUCCESS;
    }
    return DS_ERROR_NOT_FOUND;
}

```

---

##4. Concurrency & Memory Safety###Required Atomics* **`arena_atomic_cmpxchg`**: Essential for advancing `enqueue_pos` and `dequeue_pos`.
* **`arena_atomic_exchange`** (or `store_release`): Used to update `sequence` to signal completion of read/write.
* **`READ_ONCE`**: Essential for reading sequences and positions to prevent compiler reordering.

###ABA ProtectionThis algorithm is immune to ABA problems on the indices because the `sequence` check acts as a version counter.

* A producer only writes if `seq == pos`.
* A consumer only reads if `seq == pos + 1`.
Even if `pos` wraps (64-bit overflow), the `sequence` wraps with it, maintaining the invariant.

###Progress Guarantees* **Lock-Free:** If one thread sleeps or crashes, others can still make progress (unless the crashed thread successfully claimed a slot via CAS but failed to update the Sequence; in that specific crash scenario, that specific slot halts, effectively blocking the queue eventually. This is a known property of "blocking" lock-free queues where the data write is not atomic with the index reservation).

---

##5. Implementation Considerations###API Mapping* **`delete(key)`**: The `key` parameter is **ignored**. The operation is always `dequeue`.
* **`iterate`**: Iterates from `dequeue_pos` to `enqueue_pos`. Note that `enqueue_pos` might advance during iteration, so we must capture the "end" snapshot at the start of the loop to ensure termination for the verifier.

###Memory Layout* The `buffer` allocation is large. Ensure `bpf_arena_alloc` has sufficient space.
* **False Sharing:** The padding in `ds_mpmc_head` is critical for performance on multicore systems (userspace) to prevent cache thrashing between producers and consumers.

###Verifier Constraints* **`can_loop`**: Must be included in the CAS retry loops.
* **Bounds:** The verifier fails if it cannot prove loops terminate. We limit CAS retries to 10,000.
* **Recursion:** None used.

---

##6. Example Concurrent Scenario**Scenario:** Queue capacity 4. `enqueue_pos` = 0, `dequeue_pos` = 0.

1. **Producer A** reads `enqueue_pos` (0). Calculates index `0 & 3 = 0`. Reads cell[0].seq (0).
2. **Producer B** reads `enqueue_pos` (0). Calculates index `0`. Reads cell[0].seq (0).
3. **Producer A** CAS(`enqueue_pos`, 0 -> 1). **Success**.
* A writes data to cell[0].
* A writes cell[0].seq = 1.


4. **Producer B** CAS(`enqueue_pos`, 0 -> 1). **Fail**.
* B reloads `enqueue_pos` (now 1).
* B reads cell[1].seq (1). Matches pos (1).
* B CAS(`enqueue_pos`, 1 -> 2). **Success**.
* B writes data to cell[1].
* B writes cell[1].seq = 2.


5. **Consumer C** reads `dequeue_pos` (0). Reads cell[0].seq (1).
* Diff `1 - (0 + 1) == 0`. Data ready.
* C CAS(`dequeue_pos`, 0 -> 1). **Success**.
* C reads data.
* C writes cell[0].seq = `0 + 3 + 1 = 4`. (Ready for Producer generation 2).



---

##7. Implementation Status| Feature | Status | Notes |
| --- | --- | --- |
| **Arena Init** | Ready | Requires power-of-2 size |
| **Insert (Enqueue)** | Ready | Uses CAS loop with backoff (verifier limit) |
| **Delete (Dequeue)** | Ready | Acts as `pop`, ignores key |
| **Search** | Partial | O(N) scan, snapshot consistency only |
| **Verify** | Ready | Checks `dequeue <= enqueue` |
| **Iterate** | Ready | Standard traversal |

###Known Limitations* **Crash Safety:** If a thread crashes *after* incrementing `pos` but *before* updating `sequence`, the queue will stall at that index. This is an inherent trade-off for this specific algorithm's speed.
* **Fixed Size:** Cannot resize dynamically. Must allocate max required capacity at init.