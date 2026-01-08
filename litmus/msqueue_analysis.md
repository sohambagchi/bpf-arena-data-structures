# Michael-Scott Queue Analysis (`ds_msqueue.h`)

## 1. Insert Operation (`ds_msqueue_insert`)

### Minimal Memory-Op Version

```c
// Pre-condition: new_node initialized (next=NULL)

retry:
    // Loop
    m0: tail = Load_Acquire(&Q->tail)
    m1: next = Load_Acquire(&tail->next)
    
    // Consistency check (not strictly a memory op on shared state, but logic)
    // if tail != Q->tail goto retry

    if (next != NULL) {
        // Tail lagging
        m2: CAS_Release(&Q->tail, tail, next_ptr) // Failure allowed
        goto retry
    }

    // Try to link new node
    m3: success = CAS_Release(&tail->next, NULL, new_node)
    if (!success) goto retry

    // Success path
    m4: CAS_Release(&Q->tail, tail, new_node) // Failure allowed
```

### Success Ordering
`m0` -> `m1` -> `m3` (Success) -> `m4` (Tail update)

### Failure Orderings
1.  **CAS Fail (tail->next)**: `m0` -> `m1` -> `m3` (Fail) -> Retry
2.  **Tail Lagging**: `m0` -> `m1` -> `m2` -> Retry

### Permitted Reorderings & Dependencies
-   **m0 -> m1**: Address dependency (m1 depends on m0). Preserved.
-   **m1 -> m3**: Control dependency (m3 conditional on m1 == NULL).
-   **m3 (Release)**: Prevents reordering of stores to `new_node` (initialization) past `m3`.
-   **m4 (Release)**: Prevents reordering of `m3` past `m4`? Not strictly, but `m4` depends on `m3` logic. `m4` updates `Q->tail`, `m3` updates `tail->next`.

## 2. Pop Operation (`ds_msqueue_pop`)

### Minimal Memory-Op Version

```c
retry:
    m0: head = Load_Acquire(&Q->head)
    m1: tail = Load_Acquire(&Q->tail)
    m2: next = Load_Acquire(&head->next)

    // Consistency check
    // if head != Q->head goto retry

    if (next == NULL) {
        return EMPTY
    }

    if (head == tail) {
        // Queue looks empty or tail lagging
        m3: CAS_Release(&Q->tail, tail, next)
        goto retry
    }

    // Read data (address dependent on m2)
    m4: data = Load(next->data)

    // Try to swing head
    m5: success = CAS_Acquire(&Q->head, head, next)
    if (!success) goto retry
```

### Success Ordering
`m0` -> `m1` -> `m2` -> `m4` -> `m5`

### Permitted Reorderings
-   **m0, m1**: Independent loads. Could be reordered.
-   **m2**: Dependent on `m0` (address). Must follow `m0`.
-   **m5 (Acquire)**: Prevents reordering of subsequent ops (outside this function) before `m5`.

## 3. Interleaving Analysis

### Case 1: Two Concurrent Inserts
**Thread A** executes `m0`..`m3` (success). `Q->tail->next` is now `NodeA`. `Q->tail` is still `OldTail`.
**Thread B** executes `m0`. Reads `OldTail`.
**Thread B** executes `m1`. Reads `NodeA` (from A's `m3`).
**Thread B** sees `next != NULL`. Enters helper path.
**Thread B** executes `m2`. CAS `Q->tail` from `OldTail` to `NodeA`.
**Thread A** executes `m4`. CAS `Q->tail` from `OldTail` to `NodeA`. One succeeds, one fails.
**Result**: Correct. Tail is advanced. Thread B retries and inserts after NodeA.

### Case 2: Insert vs Pop (Empty -> 1 Element)
**Initial**: Head = Tail = Dummy. Dummy->next = NULL.
**Thread I (Insert)**: `m3` (Links NodeI). Dummy->next = NodeI.
**Thread P (Pop)**: `m0` (Head=Dummy), `m1` (Tail=Dummy), `m2` (next=NodeI).
**Thread P**: `head == tail` is true. `next != NULL`.
**Thread P**: executes `m3` (Advance Tail). `Q->tail` = NodeI.
**Thread I**: executes `m4` (Advance Tail). Fails (already NodeI).
**Thread P**: Retries. `m0` (Head=Dummy), `m1` (Tail=NodeI), `m2` (next=NodeI).
**Thread P**: `head != tail`. `next != NULL`.
**Thread P**: `m5` (CAS Head). Head = NodeI. Returns data from NodeI.
**Result**: Correct.

### Problematic Cases
-   **ABA on Head**: The standard MS Queue is susceptible to ABA on `head` if nodes are freed and reused immediately without generation counters or safe memory reclamation (like hazard pointers or RCU).
    -   *Implementation Check*: `ds_msqueue_pop` frees `head` (`bpf_arena_free(head)`).
    -   *Scenario*:
        1.  Thread A reads `head` (N1), `next` (N2). Stalls.
        2.  Thread B Pops N1 (frees N1). Pops N2.
        3.  Thread C Allocates N1 (reused address). Inserts N1.
        4.  Thread A resumes. `CAS(&Q->head, N1, N2)` succeeds!
        5.  **FATAL**: Head is set to N2 (which might be free or in valid). Queue structure corrupted.
    -   *Mitigation*: BPF Arena might delay reuse or user must ensure safe reclamation. The current implementation does direct `bpf_arena_free`. If `bpf_arena_alloc` reuses address immediately, **THIS IS BUGGY**.
    -   *Note*: The standard solution uses a modification counter on pointers (Stamped Pointers). This implementation uses raw pointers.

## 4. Conclusion
The implementation follows standard MS Queue logic.
**CRITICAL ISSUE**: Lack of ABA protection on `head` updates combined with immediate `bpf_arena_free` makes it unsafe for concurrent modification if memory reuse occurs rapidly. The `ds_msqueue_node` does not contain a version count.

**Recommendation**: Add a version counter to the head pointer (requiring double-width CAS or tagged pointers) or use Safe Memory Reclamation (SMR).
