# Doubly-Linked List Analysis (`ds_list.h`)

## 1. Overview
A standard doubly-linked list implementation using `WRITE_ONCE` / `READ_ONCE`.
Metadata claims `requires_locking = 0`, implying it is intended to be lock-free.

## 2. Insert Operation (`ds_list_insert`)

### Logic
Calls `__list_add_tail`.
1.  Iterates `list_for_each_entry` to find the last node.
2.  Writes `new_node->next = NULL`.
3.  Writes `new_node->pprev = &last->next`.
4.  Writes `last->next = new_node`.

### Analysis
-   **Iteration**: Not atomic. By the time `last` is found, it might have been deleted.
-   **Appending**: `WRITE_ONCE(last->node.next, &elem->node)`.
    -   This is a plain store.
    -   **Race**: Two threads A and B find the same `last`.
    -   A writes `last->next = A`.
    -   B writes `last->next = B`.
    -   A is overwritten and lost. **Memory Leak + Data Loss**.
-   **Back-pointers**: `WRITE_ONCE` on `pprev` is also racy.

## 3. Delete Operation (`ds_list_delete`)

### Logic
1.  Iterates to find key.
2.  Calls `__list_del`.
    -   `*pprev = next`
    -   `next->pprev = pprev`
3.  Frees node.

### Analysis
-   **Concurrent Delete**:
    -   If two threads try to delete adjacent nodes, they update shared pointers (`next`, `pprev`) without synchronization (CAS/Locks).
    -   Standard double-linked list deletion is **NOT** lock-free safe without complex helping schemes (e.g., Sundell-Tsigas).

## 4. Conclusion
**This implementation is NOT THREAD SAFE.**
It is a standard sequential linked list using `volatile` accesses. It offers no concurrency protection.
-   Concurrent Inserts: Lost updates.
-   Concurrent Deletes: Corrupted list structure.
-   Insert vs Delete: Segfaults/Corruption.

**Verdict**: The metadata claiming `requires_locking = 0` is incorrect unless it implies "the *caller* must provide locking", but `0` usually means "implementation handles it". Given the context of other "lock-free" data structures here, this file is misleading/buggy for concurrent use.
