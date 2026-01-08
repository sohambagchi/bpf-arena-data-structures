# Vyukhov MPSC Queue Analysis (`ds_mpsc.h`)

## 1. Insert Operation (`ds_mpsc_insert`)

### Minimal Memory-Op Version

```c
    // m0: Allocation (Not shared yet)
    // m1, m2: Initialize Node (next=NULL, data=...)

    // Atomic Exchange (Linearization Point)
    m3: prev = Atomic_Exchange_Release(&ctx->head, n)
    
    // Link prev to new (m3 established prev)
    m4: Store_Release(&prev->next, n)
```

### Success Ordering
`m3` -> `m4`

### Permitted Reorderings
-   **m3 (Release)**: Ensures node initialization (`m1`, `m2`) is visible to any thread that sees `n` (which would be via `head`? No, consumer sees `n` via `prev->next`).
    -   Actually, `m3` release ensures that if anyone sees the *new* head, they see the data? Not quite.
    -   The consumer follows `next` pointers.
    -   `m3` is the serialization point for producers (determines the order).
-   **m4 (Release)**: This is critical. It publishes `n` to the consumer (who is at `tail` = `prev` or earlier).
    -   Ensures `n`'s initialization is visible to consumer before consumer sees `prev->next == n`.

## 2. Pop Operation (`ds_mpsc_delete`)

### Minimal Memory-Op Version

```c
    m0: tail = ctx->tail // Local load (single consumer)
    
    // Check for Producer Link
    m1: next = Load_Acquire(&tail->next)

    if (tail == ctx->head) {
        return EMPTY // (Logical empty check)
    }

    if (next == NULL) {
        return BUSY // (Producer stalled state)
    }

    // Data Read
    m2: data = Load(next->data)

    // Advance Tail
    m3: ctx->tail = next
    
    // Free old tail
    Free(tail)
```

### Success Ordering
`m0` -> `m1` -> `m2` -> `m3`

### Failure Orderings
1.  **Empty**: `m0` -> `m1` -> Return Empty
2.  **Busy (Stall)**: `m0` -> `m1` -> Return Busy

### Permitted Reorderings
-   **m1 (Acquire)**: Synchronizes with Producer's `m4`. Ensures we see `next->data`.
-   **m2**: Dependent on `m1`.

## 3. Interleaving Analysis

### The "Stalled Producer" State
This is the defining characteristic of this algorithm.
1.  Producer A executes `m3` (XCHG). `head` is now `NodeA`. `prev` is `Stub`.
2.  Producer A is preempted before `m4`. `Stub->next` is still `NULL`.
3.  Consumer reads `tail` (`Stub`).
4.  Consumer reads `tail->next` (`NULL`).
5.  Consumer checks `tail == head`. `Stub != NodeA`. Queue is **NOT** empty.
6.  Consumer detects inconsistency: Not empty, but `next` is NULL.
7.  Consumer returns `BUSY`. Caller must retry.

### Concurrency
-   **Producers**: Serialized by `Atomic_Exchange` on `head`. Wait-free (always make progress).
-   **Consumer**: Obstruction-free. If a producer stalls at step 2, consumer stalls (returns BUSY).

## 4. Conclusion
The implementation is correct for MPSC.
-   **Release** on `m4` matches **Acquire** on `m1`.
-   **Release** on `m3` is possibly stronger than needed?
    -   Producer-to-Producer ordering is handled by the atomic hardware lock on the cache line of `head`.
    -   Producer-to-Consumer ordering relies on `m4`.
    -   `m3` Release might be ensuring that `prev`'s data is visible? No, `prev` was already published.
    -   Actually `m3` Release ensures that the *node initialization* of `n` is ordered before `n` becomes reachable via `head`? But `head` is not used for reaching data, `next` pointers are.
    -   Wait, if another producer B comes along:
        -   B does XCHG `head`. Gets `n`.
        -   B does `n->next = NodeB`.
        -   B needs to be sure `n` is valid.
        -   So `m3` Release protects the initialization of `n` against subsequent producers. Correct.

**Verdict**: Implementation seems correct.
