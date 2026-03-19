# LKMM Optimizations Reference

**Data structures:** Michael-Scott Queue · Vyukov MPMC Queue · Folly SPSC Queue · CK FIFO SPSC · CK Ring SPSC · CK Stack UPMC  
**Files:** `include/ds_msqueue.h`, `include/ds_vyukhov.h`, `include/ds_folly_spsc.h`, `include/ds_ck_fifo_spsc.h`, `include/ds_ck_ring_spsc.h`, `include/ds_ck_stack_upmc.h`  
**Last updated:** 2026-03-19

---

## 1. Background

The Linux Kernel Memory Model (LKMM) is the axiomatic memory model that governs how the Linux kernel reasons about shared-memory concurrency. It is specified formally in `tools/memory-model/` as a set of cat files and herd7 litmus tests. The model defines ordering relations — `rf` (reads-from), `co` (coherence order), `fr` (from-reads), `addr` (address dependency), `ctrl` (control dependency), and several fence relations — and uses them to determine which executions are permitted on a given piece of code. Importantly, LKMM is not C11: it extends C11's model with ordering mechanisms that C11 does not recognise as first-class primitives.

Every data structure in this codebase provides two implementations of each operation. Functions suffixed `_c` use `arena_atomic_load`, `arena_atomic_store`, and `arena_atomic_cmpxchg` with explicit C11 memory ordering constants (`ARENA_ACQUIRE`, `ARENA_RELEASE`, `ARENA_RELAXED`). Functions suffixed `_lkmm` use the kernel primitives `READ_ONCE`, `WRITE_ONCE`, `smp_load_acquire`, and `smp_store_release`. The `_lkmm` variants are what the BPF programs compile against when `__BPF__` is defined; the `_c` variants serve as a portable userspace reference.

The two variants are not equivalent. C11 does not recognise address dependencies or data dependencies as ordering mechanisms; a C11 compiler is permitted to break a dependency chain by constant-folding, redundant-load elimination, or speculative value prediction. Because of this, C11 code must use `ACQUIRE` loads wherever LKMM code can rely on an address dependency from a `READ_ONCE` load. The practical consequence is that the `_c` variants use `ARENA_ACQUIRE` in several places where the `_lkmm` variants use `READ_ONCE` followed by a pointer dereference — the latter being free on all major architectures (x86, ARM64, RISC-V) because hardware respects address dependencies without emitting any barrier instruction.

Four ordering mechanisms are exploited across this codebase. First, `smp_store_release`/`smp_load_acquire` pairs establish the classical producer-consumer handoff: all writes before a release store are visible to a thread that observes the released value via an acquire load. Second, address dependencies provide ordering between a `READ_ONCE` load of a pointer and any subsequent dereference of that pointer; the hardware cannot speculate past a dependent memory access. Third, control dependencies to stores provide ordering between a `READ_ONCE` load, a conditional branch that uses the loaded value, and a store inside the taken branch; the compiler cannot hoist the store above the branch because the store's execution depends on the branch outcome. Fourth, `READ_ONCE` and `WRITE_ONCE` by themselves impose no hardware barrier but prevent the compiler from caching, tearing, merging, or reordering accesses to shared fields.

The general rule for choosing between these mechanisms: if a pointer dereference immediately follows a `READ_ONCE` load of that pointer, the address dependency orders the dereference with respect to the write that set the pointer, and no acquire load is needed. If a store to a shared location is conditional on a `READ_ONCE` load of another shared location, the control dependency orders the store, and no acquire load of the condition is needed. Acquire loads are reserved for cases where neither dependency exists — typically, reading an index that is subsequently used only for arithmetic, not as a pointer to be dereferenced.

---

## 2. Primitives Reference

| Primitive | Hardware barrier? | LKMM relation | Notes |
|---|---|---|---|
| `READ_ONCE(x)` | No | Prevents compiler caching/tearing | No ordering with other threads |
| `WRITE_ONCE(x, v)` | No | Prevents compiler splitting/tearing | No ordering with other threads |
| `smp_load_acquire(p)` | Load-acquire | `[Acquire]` fence after load | Pairs with store-release |
| `smp_store_release(p, v)` | Store-release | `[Release]` fence before store | Pairs with load-acquire |
| `arena_atomic_cmpxchg(..., RELEASE, RELAXED)` | Store-release on success | On success: release; on failure: relaxed | Standard CAS for publishing |
| `arena_atomic_cmpxchg(..., RELAXED, RELAXED)` | None | Purely atomic RMW | Ordering via address dep. if applicable |
| Address dependency | None | `addr` relation in LKMM cat file | `READ_ONCE(p)` → dereference `p` |
| Control dependency (to store) | No | `ctrl` relation in LKMM cat file | `READ_ONCE` → conditional → store |

---

## 3. Michael-Scott Queue (`ds_msqueue.h`)

### 3.1 Algorithm Sketch

The Michael-Scott queue is a lock-free MPMC linked list that maintains a persistent dummy node. Enqueue appends a new node at the tail by attempting a CAS on `tail->node.next`; if that CAS fails because the tail has lagged, the thread first helps advance `queue->tail` before retrying. Dequeue swings the head pointer forward via a CAS on `queue->head`, making the old dummy node unreachable; the new head becomes the dummy for subsequent dequeues. Tail lag is a routine condition handled cooperatively by all concurrent threads.

### 3.2 Function-by-Function Analysis

#### `ds_msqueue_init_lkmm`

All writes here target freshly-allocated private memory. `WRITE_ONCE(dummy->node.next, NULL)`, `WRITE_ONCE(dummy->data.key, 420)`, and `WRITE_ONCE(dummy->data.value, 69)` prevent the compiler from tearing or reordering the initialisation stores, but no synchronisation with other threads is needed: the allocation is not yet visible to any concurrent thread at this point. The subsequent plain stores to `queue->head` and `queue->tail` are safe for the same reason — the queue structure itself is not shared until the caller publishes it.

#### `__msqueue_add_node_lkmm` (enqueue loop)

`READ_ONCE(queue->tail)` loads the current tail with no ordering. A stale value is acceptable: the loop will detect the lag via `tail->node.next != NULL` and help advance the tail before retrying.

`READ_ONCE(tail->node.next)` is the key load. Because `tail` was obtained from a `READ_ONCE` of `queue->tail` and is now being dereferenced, an address dependency connects these two loads. The hardware cannot prefetch `tail->node.next` before determining the value of `tail`. This makes the `ARENA_ACQUIRE` load that `__msqueue_add_node_c` uses on `tail->node.next` unnecessary under LKMM: the address dependency provides the same guarantee that a load-acquire would, without emitting a barrier instruction.

The CAS `arena_atomic_cmpxchg(&tail->node.next, next, &new_node->node, ARENA_RELEASE, ARENA_RELAXED)` links the new node into the list. The `ARENA_RELEASE` success ordering ensures that all plain stores to `new_node->data.key` and `new_node->data.value`, which occur before the call to `__msqueue_add_node_lkmm`, are visible to any thread that subsequently observes the `tail->node.next` update. The failure ordering is `ARENA_RELAXED` because a failed CAS does not publish anything; the loop simply retries.

The CAS `arena_atomic_cmpxchg(&queue->tail, tail, new_node, ARENA_RELEASE, ARENA_RELAXED)` advances the tail pointer. The release ordering here ensures that a helper thread advancing the tail on behalf of a lagging enqueuer sees the entire linked list structure before seeing the new tail value.

Plain stores to `new_node->data` before the CAS are safe. No other thread can observe `new_node` until the release-CAS on `tail->node.next` links it into the list. The compiler cannot reorder those stores past the CAS because the CAS uses release semantics.

#### `ds_msqueue_pop_lkmm` (dequeue loop)

`READ_ONCE(queue->head)` and `READ_ONCE(queue->tail)` are relaxed loads. Consistency is verified by re-reading `queue->head` and checking it has not changed; a mismatch causes a retry.

`READ_ONCE(head->node.next)` creates an address dependency from `head` (obtained by `READ_ONCE(queue->head)`) through the pointer dereference. This is the beginning of a dependency chain: `READ_ONCE(queue->head)` → `READ_ONCE(head->node.next)` → computation of `next_elem` → reads of `next_elem->data.key` and `next_elem->data.value`. The `_c` version breaks this chain at the second step by using `ARENA_ACQUIRE` on `head->node.next` to explicitly order the data reads, because C11 does not recognise the address dependency.

The data reads `next_elem->data.key` and `next_elem->data.value` are plain loads in the `_lkmm` version. They are ordered by the transitive address dependency chain described above. A thread that reads a pointer published by a release store and then dereferences that pointer is guaranteed, under LKMM, to see all writes that the release store ordered.

The CAS `arena_atomic_cmpxchg(&queue->head, head, next_elem, ARENA_RELAXED, ARENA_RELAXED)` uses `ARENA_RELAXED` for both success and failure orderings. This is the principal LKMM optimisation in the pop path: `ds_msqueue_pop_c` uses `ARENA_ACQUIRE` on success here. Under LKMM the acquire is redundant because the address dependency chain has already provided visibility of `next_elem`'s data. The relaxed CAS is sufficient to atomically swing the head pointer.

#### `ds_msqueue_search_lkmm`

The loop traversal uses `READ_ONCE(node->node.next)` at each step. Each `READ_ONCE` of the `next` pointer creates an address dependency to the following dereference of that pointer, so each step in the chain is ordered with respect to the previous. The function provides snapshot semantics: it observes the list as it was at some point during execution, which is the correct contract for a search on a concurrently-modified structure.

#### `ds_msqueue_verify_lkmm`

`ds_msqueue_verify_lkmm` is documented as requiring exclusive access during a quiescent period. Under that assumption, `READ_ONCE` on `head->node.next` and `node->node.next` at each step is sufficient: it prevents the compiler from reading each field more than once, and no hardware ordering is needed because there is no concurrent writer. The address dependency from each `READ_ONCE` of a `next` pointer to the subsequent dereference provides ordering as a side effect.

#### `ds_msqueue_iterate_lkmm`

The iteration follows the same pattern as verify: `READ_ONCE(node->node.next)` at each step, with an address dependency to the dereference that follows. The iteration is documented as unsafe during concurrent modification; `READ_ONCE` is used purely for compiler discipline to prevent the compiler from hoisting or merging the pointer reads.

---

## 4. Vyukov MPMC Queue (`ds_vyukhov.h`)

### 4.1 Algorithm Sketch

The Vyukov queue is a bounded MPMC ring buffer in which each slot (`struct ds_vyukhov_node`) holds a `sequence` counter alongside the data fields `key` and `value`. The sequence counter serves simultaneously as ABA protection and as the synchronisation medium between producers and consumers. A producer may write to slot `pos & mask` when `cell->sequence == pos`; after writing, it stores `pos + 1` into the sequence with release semantics to hand the slot to consumers. A consumer may read slot `pos & mask` when `cell->sequence == pos + 1`; after reading, it stores `pos + mask + 1` into the sequence with release semantics to return the slot to producers for the next lap of the ring.

### 4.2 Function-by-Function Analysis

#### `ds_vyukhov_init_lkmm`

`WRITE_ONCE(head->enqueue_pos, 0)`, `WRITE_ONCE(head->dequeue_pos, 0)`, and `WRITE_ONCE(head->count, 0)` initialise the control scalars without hardware barriers; single-threaded context makes this correct. The inner loop stores `WRITE_ONCE(cell->sequence, i)` for each slot. Using `WRITE_ONCE` rather than a plain assignment prevents the compiler from optimising away or coalescing these stores (some compilers will merge a loop of stores to an array into a `memset` if the values are uniform, which would be incorrect here since each index gets its own value).

#### `ds_vyukhov_insert_lkmm` (enqueue)

`READ_ONCE(head->enqueue_pos)` provides the initial position for the CAS loop. A stale value is safe: the comparison with `cell->sequence` will detect the mismatch and the loop will retry with an updated position.

`smp_load_acquire(&cell->sequence)` is the central synchronisation point for the producer. It acquires the sequence number, which was last written by a consumer's `smp_store_release(&cell->sequence, pos + mask + 1)` when that consumer returned the slot for reuse. The acquire-release pair guarantees that the producer sees the slot as completely consumed and available before writing new data. Without the acquire, a producer on a weakly-ordered architecture could observe the old data in the slot even after the consumer has logically freed it.

`arena_atomic_cmpxchg(&head->enqueue_pos, pos, pos + 1, ARENA_RELAXED, ARENA_RELAXED)` claims the slot for this producer. Both orderings are relaxed because `enqueue_pos` is only a claim ticket: the actual data synchronisation is handled entirely by the sequence number's release-acquire pair. Two producers racing on the same `pos` value will have exactly one succeed; the loser observes an updated `pos` from the CAS return and retries.

Plain stores to `cell->data.key` and `cell->data.value` after a successful CAS are safe. The slot is exclusively owned by this producer between the CAS success and the release store of the sequence; no other thread can enter this slot until the producer publishes it.

`smp_store_release(&cell->sequence, pos + 1)` publishes the data to consumers. The release fence before this store ensures `cell->data.key` and `cell->data.value` are visible to any consumer that subsequently observes `sequence == pos + 1` via its `smp_load_acquire`.

#### `ds_vyukhov_pop_lkmm` (dequeue)

`READ_ONCE(head->dequeue_pos)` loads the initial position. Same reasoning as the enqueue side: stale value causes a retry.

`smp_load_acquire(&cell->sequence)` pairs with the producer's `smp_store_release(&cell->sequence, pos + 1)`. The consumer that observes `seq == pos + 1` via this acquire load is guaranteed to see all stores the producer made before releasing the sequence, including `cell->data.key` and `cell->data.value`.

`arena_atomic_cmpxchg(&head->dequeue_pos, pos, pos + 1, ARENA_RELAXED, ARENA_RELAXED)` claims the data slot for this consumer. Same reasoning as the enqueue side: `dequeue_pos` is a claim ticket; the sequence provides the synchronisation.

Data reads `cell->data.key` and `cell->data.value` after the CAS are ordered by the acquire on `cell->sequence`. The load-acquire was issued before the CAS, and the CAS is a sequentially-consistent operation that cannot be reordered before the preceding acquire under LKMM.

`smp_store_release(&cell->sequence, pos + mask + 1)` releases the slot back to producers. The release fence orders all preceding reads (the data reads) before this store, ensuring producers on the next lap see the slot as fully consumed.

#### `ds_vyukhov_search_lkmm`

The function takes snapshot loads of `head->dequeue_pos` and `head->enqueue_pos` using `READ_ONCE`, then walks the buffer between those bounds. Neither the snapshot nor the per-cell access is atomic with ongoing modifications. This is the correct contract for a snapshot scan: it may miss elements that were enqueued after the snapshot or include elements that were dequeued before the scan reaches them.

#### `ds_vyukhov_verify_lkmm`

`READ_ONCE(head->enqueue_pos)` and `READ_ONCE(head->dequeue_pos)` snapshot the positions for the bounds check `deq > enq`. Relaxed is correct for a verification pass that only needs a consistent snapshot of the positions (not a consistent view of all data in the buffer). The check `size > capacity` catches the case where positions have wrapped unevenly, which would indicate corruption.

#### `ds_vyukhov_iterate_lkmm`

Position snapshot using `READ_ONCE` on both `dequeue_pos` and `enqueue_pos`, then a linear walk. The pattern is identical to search; no additional ordering is required because the iteration is a best-effort snapshot.

---

## 5. Folly SPSC Queue (`ds_folly_spsc.h`)

### 5.1 Algorithm Sketch

The Folly SPSC queue is a fixed-size ring buffer with a single producer and single consumer. The producer owns `write_idx` (the only writer) and reads `read_idx` to determine whether the queue is full. The consumer owns `read_idx` (the only writer) and acquires `write_idx` to determine whether data is available. Because only one thread ever writes each index, no CAS is needed; a simple store suffices. The producer releases `write_idx` after writing data; the consumer acquires `write_idx` before reading data. The consumer releases `read_idx` after reading; the producer reads `read_idx` to check for available space.

### 5.2 Function-by-Function Analysis

#### `ds_spsc_init_lkmm`

`WRITE_ONCE(head->read_idx.idx, 0)` and `WRITE_ONCE(head->write_idx.idx, 0)` initialise both indices. The single-threaded initialisation context makes hardware ordering unnecessary; `WRITE_ONCE` is present to prevent the compiler from omitting or reordering the zero-initialisation stores.

#### `ds_spsc_insert_lkmm` (producer)

`READ_ONCE(head->write_idx.idx)` loads the producer's own index. No other thread writes `write_idx`, so a plain load would be technically sufficient, but `READ_ONCE` is used for consistency and to prevent the compiler from caching the value across the function.

`READ_ONCE(head->read_idx.idx)` is the LKMM optimisation point. The `_c` variant (`ds_spsc_insert_c`) uses `ARENA_ACQUIRE` here. Under LKMM, a **control dependency** from this load to the subsequent stores provides sufficient ordering: the value of `current_read` feeds directly into the condition `if (next_record != current_read)`, and all stores inside the taken branch (`node->key = key`, `node->value = value`, and `smp_store_release(&head->write_idx.idx, next_record)`) are ordered after the branch by the control dependency. The compiler cannot hoist any of those stores above the branch because doing so would change the observable behaviour of the conditional. A stale read of `read_idx` causes a spurious "queue full" return (`DS_ERROR_FULL`), which is safe: the producer will retry on the next call.

Plain stores `node->key = key` and `node->value = value` occur after the full-check and before the release store. They are ordered by both the control dependency (they are inside the taken branch) and the release fence that `smp_store_release` inserts before the `write_idx` store.

`smp_store_release(&head->write_idx.idx, next_record)` is the publication handoff. All preceding stores in this invocation, including the data stores, are ordered before this release store. The consumer's `smp_load_acquire(&head->write_idx.idx)` pairs with this release.

#### `ds_spsc_delete_lkmm` (consumer)

`READ_ONCE(head->read_idx.idx)` loads the consumer's own index. No other thread writes `read_idx`.

`smp_load_acquire(&head->write_idx.idx)` pairs with the producer's `smp_store_release`. The consumer acquires visibility of all stores the producer made before releasing `write_idx`, which includes `node->key` and `node->value` at `records[current_read]`.

Data reads `node->key` and `node->value` follow the acquire load of `write_idx`. They are ordered by the acquire-release pair.

`smp_store_release(&head->read_idx.idx, next_record)` signals to the producer that the slot at `current_read` is free. The release fence before this store ensures that the data reads have completed before the slot is made available for reuse.

#### `ds_spsc_verify_lkmm`, `ds_spsc_size_lkmm`, `ds_spsc_is_empty_lkmm`, `ds_spsc_is_full_lkmm`

All four functions use `READ_ONCE` on both `read_idx.idx` and `write_idx.idx`. They provide snapshot semantics: the two index reads are not atomic with respect to each other, so the result reflects the state at two slightly different points in time. This is the correct contract for auxiliary queries on a concurrently-active SPSC queue.

---

## 6. CK FIFO SPSC (`ds_ck_fifo_spsc.h`)

### 6.1 Algorithm Sketch

The CK FIFO SPSC is a linked-list SPSC queue with a persistent stub (dummy) node, closely following the original ConcurrencyKit design. The producer appends new entries by performing a release store into `tail->next`, then updating `fifo->tail` (which is producer-private). The consumer reads `head->next` to find the next entry, then updates `fifo->head` (which is consumer-private). Because only one thread ever writes each pointer, no CAS is required. The recycler (`ds_ck_fifo_spsc_recycle_lkmm`) is producer-side and reuses nodes that the consumer has finished with by snapshotting `fifo->head` to detect which nodes are safe to reclaim.

### 6.2 Function-by-Function Analysis

#### `ds_ck_fifo_spsc_fifo_init_lkmm`

Plain stores to `stub->next`, `fifo->head`, `fifo->tail`, `fifo->head_snapshot`, and `fifo->garbage` during single-threaded initialisation. No synchronisation needed; no `READ_ONCE` or `WRITE_ONCE` required here because no concurrent thread observes these stores until after the caller publishes the queue.

#### `ds_ck_fifo_spsc_init_lkmm`

Allocates the stub node, uses `WRITE_ONCE(stub->kv.key, 0)` and `WRITE_ONCE(stub->kv.value, 0)` to zero the key-value fields without compiler interference, then delegates to `ds_ck_fifo_spsc_fifo_init_lkmm`. Single-threaded context; the `WRITE_ONCE` calls prevent the compiler from omitting the initialisation stores.

#### `ds_ck_fifo_spsc_enqueue_lkmm`

`READ_ONCE(fifo->tail)` loads the producer-private tail pointer. Because `tail` is exclusively written by the producer, this is primarily a compiler fence: it prevents the compiler from caching an older value of `fifo->tail` from a prior iteration.

Plain stores to `entry->value` and `entry->next = NULL` before the publication. These stores are private to the producer until the release store makes the entry visible.

`smp_store_release(&tail->next, entry)` is the publication point. The release fence before this store orders all preceding writes — `entry->value`, `entry->next = NULL`, and any data written into the entry by the caller — so they are visible to the consumer before `tail->next` is updated. This is the sole synchronisation operation in the enqueue path.

`fifo->tail = entry` is a plain store updating the producer-private tail. No barrier is needed because `fifo->tail` is not observed by the consumer.

#### `ds_ck_fifo_spsc_dequeue_lkmm`

`READ_ONCE(fifo->head)` loads the consumer-private head pointer. Like `fifo->tail` on the producer side, `fifo->head` is exclusively written by the consumer; the `READ_ONCE` is a compiler fence.

`READ_ONCE(head->next)` is the LKMM address dependency. `head` was obtained by `READ_ONCE(fifo->head)` and is now dereferenced. The hardware cannot speculatively load `head->next` before knowing the value of `head`. This address dependency pairs with the producer's `smp_store_release(&tail->next, entry)`: a consumer that observes a non-NULL value for `head->next` through this address-dependent load is guaranteed to see all stores the producer made before the release. The `_c` variant (`ds_ck_fifo_spsc_dequeue_c`) uses `ARENA_ACQUIRE` on `head->next` to achieve the same effect without relying on the address dependency.

`READ_ONCE(entry->value)` extends the address dependency chain. `entry` was loaded from `head->next` via a `READ_ONCE`-dependent dereference; dereferencing `entry->value` continues the chain, ordering this load after the producer's writes to `entry->value`.

`WRITE_ONCE(fifo->head, entry)` updates the consumer-private head pointer. `WRITE_ONCE` rather than a plain store prevents the compiler from reordering or coalescing the store, but no hardware release fence is emitted because `fifo->head` is not used for cross-thread synchronisation.

#### `ds_ck_fifo_spsc_recycle_lkmm`

Reads `fifo->head` with `READ_ONCE` to snapshot the current consumer position. The snapshot may lag behind the actual consumer position (the consumer may have advanced `fifo->head` after the snapshot), but it can never lead it: a node that `head_snapshot` considers unreclaimed will either already be reclaimed or still in flight. This makes the recycle operation conservative — it may fail to reclaim a recently-consumed node — but it never recycles a node that is still live.

#### `ds_ck_fifo_spsc_insert_lkmm` and `ds_ck_fifo_spsc_delete_lkmm`

These are thin wrappers around `ds_ck_fifo_spsc_enqueue_lkmm` and `ds_ck_fifo_spsc_dequeue_lkmm` respectively, with an additional layer for key-value encoding. The ordering analysis for the enqueue and dequeue primitives applies directly.

#### `ds_ck_fifo_spsc_isempty_lkmm`

`READ_ONCE(fifo->head)` followed by `READ_ONCE(head->next)`. The address dependency from the first load to the second ensures the compiler cannot cache a stale `head->next`. The result is a snapshot check: `head->next == NULL` implies the queue appears empty at this moment.

#### `ds_ck_fifo_spsc_verify_lkmm`

Structural walk using `ds_ck_fifo_spsc_ptr_load_lkmm`, which internally calls `READ_ONCE(*ptr)`. Each step in the walk creates an address dependency to the next. Verify is documented as requiring no concurrent modifications; `READ_ONCE` is present for compiler discipline.

---

## 7. CK Ring SPSC (`ds_ck_ring_spsc.h`)

### 7.1 Algorithm Sketch

The CK Ring SPSC is a power-of-two ring buffer SPSC. The producer owns `p_tail` and reads `c_head` to detect a full buffer. The consumer owns `c_head` and acquires `p_tail` to detect an available slot. The structure is symmetric with the Folly SPSC queue but uses a `mask` for index arithmetic (`index & mask`) rather than modular division, and leaves exactly one slot empty to distinguish full from empty. The `_c` variant (`ds_ck_ring_spsc_delete_c`) contains a redundant `arena_memory_barrier()` between the acquire load of `p_tail` and the slot reads; the `_lkmm` variant correctly removes it.

### 7.2 Function-by-Function Analysis

#### `ds_ck_ring_spsc_init_lkmm`

Plain stores to `head->capacity`, `head->mask`, `head->c_head`, and `head->p_tail` during single-threaded initialisation. `WRITE_ONCE(head->slots, slots)` uses `WRITE_ONCE` for the pointer assignment to prevent the compiler from reordering the store of the slots pointer with respect to any subsequent use of `head->slots`.

#### `ds_ck_ring_spsc_insert_lkmm` (producer)

`READ_ONCE(head->c_head)` loads the consumer's index. This is the LKMM optimisation point: `ds_ck_ring_spsc_insert_c` uses `ARENA_ACQUIRE` here. Under LKMM, a **control dependency** from this load to the subsequent stores provides the required ordering. The value of `consumer` feeds into `if (next == consumer)`, and the stores `slot->key = key`, `slot->value = value`, and `smp_store_release(&head->p_tail, next)` all reside inside the branch taken when the queue is not full. The compiler cannot hoist those stores above the conditional. A stale `consumer` value causes a spurious `DS_ERROR_FULL` return, which is safe.

`READ_ONCE(head->p_tail)` loads the producer's own index. Relaxed is correct; the producer is the exclusive writer.

Plain stores `slot->key = key` and `slot->value = value` are inside the branch taken when space is available, ordered by the control dependency and fenced by the subsequent `smp_store_release`.

`smp_store_release(&head->p_tail, next)` publishes the written slot. All preceding stores in this invocation, including the slot data stores, are ordered before this release. The consumer's `smp_load_acquire(&head->p_tail)` pairs with this release.

#### `ds_ck_ring_spsc_delete_lkmm` (consumer)

`READ_ONCE(head->c_head)` loads the consumer's own index. Relaxed is correct; the consumer is the exclusive writer of `c_head`.

`smp_load_acquire(&head->p_tail)` pairs with the producer's `smp_store_release`. The consumer acquires visibility of all slot data the producer stored before releasing `p_tail`.

Data reads `slot->key` and `slot->value` follow the acquire load and are therefore ordered. No further barrier is needed between the acquire and the data reads — the acquire itself prohibits any load or store from being reordered above it in program order.

**Note on the `_c` variant:** `ds_ck_ring_spsc_delete_c` calls `arena_memory_barrier()` — a full `SEQ_CST` fence — between `arena_atomic_load(&head->p_tail, ARENA_ACQUIRE)` and the slot reads. This fence is redundant. `ARENA_ACQUIRE` already prevents any subsequent load or store from being reordered above the acquire load. Adding a full fence on top of an acquire load provides no additional ordering guarantee in any execution; it is a harmless but wasteful instruction. `ds_ck_ring_spsc_delete_lkmm` omits it correctly.

`smp_store_release(&head->c_head, next)` signals to the producer that the slot is free for reuse.

#### `ds_ck_ring_spsc_pop_lkmm`

Delegates directly to `ds_ck_ring_spsc_delete_lkmm`. The ordering analysis is identical.

#### `ds_ck_ring_spsc_size_lkmm`, `ds_ck_ring_spsc_is_empty_lkmm`, `ds_ck_ring_spsc_is_full_lkmm`

All three functions use `READ_ONCE` on both `c_head` and `p_tail`. The two reads are not atomic with each other, so the result is a snapshot that may reflect indices from slightly different points in time. This is correct for auxiliary queries; neither the producer nor the consumer makes correctness decisions based on the result of these functions.

#### `ds_ck_ring_spsc_verify_lkmm`

`READ_ONCE(head->c_head)` and `READ_ONCE(head->p_tail)` snapshot both indices for the structural checks. The verification checks that `capacity` is a valid power of two, that `mask == capacity - 1`, that both indices are within bounds, and that the computed size does not exceed `mask`. Single-threaded quiescent context assumed.

---

## 8. CK Stack UPMC (`ds_ck_stack_upmc.h`)

### 8.1 Algorithm Sketch

The CK Stack UPMC is a Treiber stack (lock-free LIFO) with an "unbounded producer, multiple consumer" usage model. Push performs a CAS on `stack->head` with `ARENA_RELEASE` to atomically link a new node and publish its data. Pop performs a CAS on `stack->head` with `ARENA_RELAXED`, relying on the address dependency from `READ_ONCE(stack->head)` through `READ_ONCE(head->next)` to provide visibility of the popped node's data without requiring an acquire barrier on the CAS itself.

### 8.2 Function-by-Function Analysis

#### `ds_ck_stack_upmc_init_lkmm`

`WRITE_ONCE(stack->head, NULL)` and `WRITE_ONCE(stack->count, 0)` initialise the stack fields with compiler discipline but no hardware ordering. Single-threaded context; no concurrent thread observes these stores.

#### `ds_ck_stack_upmc_isempty_lkmm`

`READ_ONCE(stack->head)` provides a snapshot check. A stale NULL indicates the stack appeared empty at some point during or before this call; a non-NULL value indicates at least one element was present. In either case the result is a hint, not a linearisable query, and callers must tolerate the race.

#### `ds_ck_stack_upmc_push_upmc_lkmm`

`READ_ONCE(stack->head)` loads the current head for the CAS loop. Plain stores to `entry->data.key` and `entry->data.value` are private to the pusher until the CAS publishes the entry.

`entry->next = head` is a plain store. `entry` is not yet visible to other threads, so no barrier is needed.

`arena_atomic_cmpxchg(&stack->head, head, entry, ARENA_RELEASE, ARENA_RELAXED)` is the publication CAS. On success, the release fence orders `entry->data.key`, `entry->data.value`, and `entry->next` before the update to `stack->head`. Any thread that subsequently reads `stack->head` and observes `entry` is guaranteed to see those fields correctly initialised. On failure, `head` is updated from the CAS return value, and the loop retries.

`arena_atomic_add(&stack->count, 1, ARENA_RELAXED)` updates the statistics counter with relaxed ordering. Count is advisory; no other thread synchronises on it.

#### `ds_ck_stack_upmc_trypush_upmc_lkmm`

A single-attempt variant of push. The ordering analysis is identical to `push_upmc_lkmm`: `READ_ONCE(stack->head)`, plain store to `entry->next`, release CAS on `stack->head`.

#### `ds_ck_stack_upmc_pop_upmc_lkmm`

`READ_ONCE(stack->head)` begins the address dependency chain.

`READ_ONCE(head->next)` is the critical load. `head` was obtained from `READ_ONCE(stack->head)`; dereferencing `head->next` through that pointer value creates an address dependency. This dependency pairs with the pusher's `ARENA_RELEASE` CAS: under LKMM, a thread that observes a pointer value written by a release store and then dereferences that pointer to reach additional data is guaranteed to see all writes that the release store ordered, including `entry->data.key`, `entry->data.value`, and `entry->next`. `READ_ONCE` is required here (as opposed to a plain `head->next` load) to prevent the compiler from reading `head->next` speculatively or from a cached register, either of which would break the address dependency chain. **This was one of the two changes made in this codebase**: the original `ds_ck_stack_upmc_pop_upmc_lkmm` used `head->next` without `READ_ONCE`.

`arena_atomic_cmpxchg(&stack->head, head, next, ARENA_RELAXED, ARENA_RELAXED)` is the LKMM optimisation. Both success and failure orderings are relaxed. `ds_ck_stack_upmc_pop_upmc_c` uses `ARENA_ACQUIRE` on success; under C11, the acquire is necessary because C11 does not recognise the address dependency from `READ_ONCE(stack->head)` to the `head->next` load. Under LKMM, the address dependency chain already provides visibility, so the CAS itself needs no acquire ordering.

After a successful CAS, reads of `head->data.key` and `head->data.value` in `ds_ck_stack_upmc_pop_lkmm` are ordered by the address dependency chain from `READ_ONCE(stack->head)` through `READ_ONCE(head->next)` to the returned `head` pointer.

#### `ds_ck_stack_upmc_trypop_upmc_lkmm`

Single-attempt pop variant. Uses `READ_ONCE(stack->head)` and `READ_ONCE(head->next)`. The second `READ_ONCE` here is the same change as in `pop_upmc_lkmm`: the original code used `head->next` without `READ_ONCE`, which would allow the compiler to eliminate the address dependency. The relaxed CAS follows for the same reason as in `pop_upmc_lkmm`.

#### `ds_ck_stack_upmc_search_lkmm`

`READ_ONCE(stack->head)` begins the traversal. Inside the loop, `READ_ONCE(cursor->next)` advances the cursor. Each `READ_ONCE` of a `next` pointer creates an address dependency to the following iteration's dereference of that pointer, chaining transitively through the list. The search is a snapshot scan and may miss concurrently-pushed nodes or traverse nodes that are concurrently being popped.

#### `ds_ck_stack_upmc_verify_lkmm`

Floyd's cycle detection using `slow` and `fast` pointers. All pointer loads — `READ_ONCE(stack->head)`, `READ_ONCE(fast->next)`, `READ_ONCE(fast_next->next)`, and `READ_ONCE(slow->next)` — use `READ_ONCE` to prevent the compiler from reading any pointer field more than once per loop step. The address dependency from each `READ_ONCE` of a pointer to the subsequent dereference chains through the walk. Verify is intended for use during quiescent periods; concurrent modifications would invalidate the cycle-detection logic.

#### `ds_ck_stack_upmc_stats_lkmm`

`READ_ONCE(stack->count)` reads the relaxed statistics counter. No ordering is required; the count is advisory and can be transiently stale.

---

## 9. Summary of Changes Made

| File | Function | Change | Justification |
|---|---|---|---|
| `ds_ck_stack_upmc.h` | `ds_ck_stack_upmc_pop_upmc_lkmm` | `head->next` → `READ_ONCE(head->next)` | Prevent compiler tearing of a shared field; preserve address dependency chain |
| `ds_ck_stack_upmc.h` | `ds_ck_stack_upmc_trypop_upmc_lkmm` | `head->next` → `READ_ONCE(head->next)` | Same as above |

All other `_lkmm` functions across the five remaining data structures were already correct. No changes were made to `ds_msqueue.h`, `ds_vyukhov.h`, `ds_folly_spsc.h`, `ds_ck_fifo_spsc.h`, or `ds_ck_ring_spsc.h`.

---

## 10. Key Patterns Reference

### 10.1 Release-Acquire Pair (Cross-Thread Publication)

```c
/* Producer */
data->key = key;
data->value = value;
smp_store_release(&shared->index, new_idx);  /* fence before store */

/* Consumer */
idx = smp_load_acquire(&shared->index);      /* fence after load */
/* data->key and data->value now visible */
```

The producer stores data, then releases a flag or index. The consumer acquires that flag or index, and all writes the producer made before the release are then visible. This is the fundamental handoff primitive in every data structure here: `smp_store_release` / `smp_load_acquire` on `write_idx` in the Folly SPSC, on `p_tail` / `c_head` in the CK Ring SPSC, and on `cell->sequence` in the Vyukov MPMC.

### 10.2 Address Dependency (Pointer-Chased Reads, LKMM Only)

```c
/* Under LKMM: no barrier needed between these two loads */
ptr = READ_ONCE(shared->head);   /* addr dep begins here */
val = ptr->data;                 /* hardware cannot reorder before ptr is known */
```

A `READ_ONCE` load of a pointer, followed by a dereference of that pointer's value, is ordered with respect to the write that stored the pointer — at zero hardware cost on all architectures. This enables `ARENA_RELAXED` CAS operations in pop paths across the Michael-Scott queue and the CK Stack UPMC, because the address dependency from `READ_ONCE(stack->head)` through `READ_ONCE(head->next)` already provides the visibility that a load-acquire CAS would provide in the C11 variant. C11 does not recognise this relation; the `_c` variants must use `ARENA_ACQUIRE` at the CAS site instead.

### 10.3 Control Dependency to Store (LKMM Only)

```c
/* Under LKMM: acquire on read_idx is not needed */
consumer = READ_ONCE(shared->read_idx);   /* ctrl dep begins here */
if (next != consumer) {                   /* branch uses the value */
    slot->key = key;                      /* store inside the branch */
    slot->value = value;                  /* compiler cannot hoist above branch */
    smp_store_release(&shared->write_idx, next);
}
```

A `READ_ONCE` load of a shared index, a conditional branch that consumes the loaded value, and stores inside the taken branch form a control dependency. The compiler cannot hoist the stores above the branch because doing so would change the observable behaviour of the conditional. This is the ordering mechanism used in `ds_spsc_insert_lkmm` and `ds_ck_ring_spsc_insert_lkmm` to avoid the acquire load of the "other side's" index that the corresponding `_c` variants require.

### 10.4 `READ_ONCE`/`WRITE_ONCE` for Compiler Discipline

```c
/* Shared field that needs no hardware ordering but must not be cached */
val = READ_ONCE(shared->counter);  /* not: val = shared->counter */
WRITE_ONCE(shared->ptr, new_ptr);  /* not: shared->ptr = new_ptr */
```

`READ_ONCE` and `WRITE_ONCE` by themselves emit no hardware barrier instruction. Their sole function is to tell the compiler that the access is to a shared location: the compiler must not cache the value in a register across a loop iteration, must not read the field more than once and assume the two reads return the same value, must not merge two stores into one, and must not split a single store into multiple narrower stores. Every plain read of a shared field that feeds into an ordering decision or a dependency chain in this codebase uses `READ_ONCE` for exactly this reason.
