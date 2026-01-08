# Ellen Binary Search Tree Analysis (`ds_bintree.h`)

## 1. Overview
This implementation follows the algorithm by Ellen, Fatourou, Ruppert, and van Breugel (2010). It uses a cooperative mechanism with `Info` records (via `m_pUpdate` tagged pointers) to coordinate updates, ensuring that operations help each other rather than conflicting destructively.

## 2. Insert Operation (`ds_bintree_insert`)

### Minimal Memory-Op Version

```c
retry:
    // 1. Search
    //    Traverses using Load_Acquire on child pointers.
    //    Reads m_pUpdate (Acquire) to check for flags (Backoff).
    bst_search(...) -> (gp, p, l, updP, updGP)

    // 2. Consistency Checks
    if (updP is flagged || updGP is flagged) goto retry
    if (p->child != l) goto retry

    // 3. Allocation (local)
    new_leaf = Alloc()
    new_internal = Alloc()
    op = Alloc() // Update Descriptor

    // 4. Flag Parent (Linearization Point for Insert attempt)
    //    Changes p->update from Clean to IFLAG | op
    m_cas: success = CAS_AcqRel(&p->m_pUpdate, updP, (op | IFLAG))
    
    if (success) {
        // 5. Help Insert (Child Update)
        //    Changes p->child from l to new_internal
        m_child: CAS_Release(&p->child, l, new_internal)
        
        // 6. Unflag Parent
        //    Changes p->update from IFLAG to Clean
        m_unflag: CAS_Release(&p->m_pUpdate, (op | IFLAG), (op | CLEAN))
        
        return SUCCESS
    }
    
    // Cleanup and Retry
    Free(op, new_internal, new_leaf)
    goto retry
```

### Success Ordering
`Search` -> `m_cas` -> `m_child` -> `m_unflag`

### Permitted Reorderings
-   **m_cas (AcqRel)**: Synchronizes with any previous operation on `p`. Ensures we have exclusive rights to modify `p`'s child.
-   **m_child (Release)**: Publishes the new subtree. Must happen after `m_cas` (logic dependency).
-   **m_unflag (Release)**: Releases the "lock" on `p`. Must happen after `m_child`.

## 3. Delete Operation (`ds_bintree_delete`)

### Minimal Memory-Op Version

```c
retry:
    // 1. Search
    bst_search(...) -> (gp, p, l, updP, updGP)

    // 2. Consistency Checks
    if (updP flagged || updGP flagged) goto retry

    // 3. Alloc Op
    op = Alloc()

    // 4. Flag Grandparent (Linearization Point 1)
    //    Changes gp->update from Clean to DFLAG | op
    m_cas_gp: success = CAS_AcqRel(&gp->m_pUpdate, updGP, (op | DFLAG))
    
    if (success) {
        // 5. Help Delete
        //    Attempt to Mark Parent
        m_mark_p: success_mark = CAS_AcqRel(&p->m_pUpdate, updP, (op | MARK))
        
        if (success_mark) {
            // 6. Physical Delete (Child Update)
            //    Swing gp->child from p to sibling
            m_child_gp: CAS_Release(&gp->child, p, sibling)
            
            // 7. Unflag Grandparent
            m_unflag_gp: CAS_Release(&gp->m_pUpdate, (op | DFLAG), (op | CLEAN))
            
            return SUCCESS
        } else {
            // Backtrack (Unflag GP)
            m_backtrack: CAS_Release(&gp->m_pUpdate, (op | DFLAG), (op | CLEAN))
            // Retry
        }
    }
    goto retry
```

## 4. Interleaving Analysis

### Cooperative Helping
The key feature is that if a thread encounters a node with `IFLAG` or `DFLAG`, it can help complete that operation using the information in the `Info` record pointed to by `m_pUpdate`.
-   **Current Implementation**: `bst_search` implements **Backoff** instead of Helping.
    -   `if (state == BST_DFLAG || state == BST_MARK) break; // Backoff`
    -   This is a valid strategy (Obstruction-Free) but not Wait-Free. The paper describes helping to achieve Lock-Freedom.
    -   The `insert` and `delete` functions *check* for flags and retry, but they don't explicitly call `bst_help` on the *encountered* node during search. They only help if they successfully flag the node themselves?
    -   Wait, `ds_bintree_insert` calls `bst_search`. If `bst_search` backs off, `res.pParent` might be invalid or NULL. `ds_bintree_insert` handles this: `if (!res.pParent) return BUSY`.
    -   So this implementation is **Lock-Free** (because some thread makes progress? No, if everyone backs off, it's Livelock prone? No, the thread holding the flag *will* complete its operation).
    -   Since the thread that placed the flag proceeds to finish the operation, progress is guaranteed for *that* thread. Other threads wait (backoff). So it is effectively Lock-Free (system wide progress).

### Concurrency Correctness
-   **Insert vs Delete**:
    -   Insert locks `p`.
    -   Delete locks `gp` then tries to mark `p`.
    -   If Insert holds `p` (IFLAG), Delete's CAS on `p` (to MARK) will fail (Expected `CLEAN`, found `IFLAG`).
    -   Delete backtracks. Correct.
-   **Delete vs Delete (Adjacent)**:
    -   Delete1 (locks GP, P).
    -   Delete2 (locks P, Child).
    -   Delete2 will fail to lock P if Delete1 already marked it?
    -   Or Delete1 fails to lock GP if Delete2 flagged it (as its P)?
    -   The hierarchy of locking (GP then P) prevents deadlock.

## 5. Conclusion
`ds_bintree.h` is a robust implementation of the Ellen et al. algorithm.
-   **Memory Reclamation**: The code calls `bpf_arena_free` on deleted nodes.
    -   **Issue**: Is it safe?
    -   If a thread is sleeping in `bst_search` holding a pointer to `p`, and `p` is deleted and freed...
    -   BPF Arena pointers are just integers. `bpf_arena_free` might return pages to the pool.
    -   If pages are unmapped -> Fault.
    -   If pages are reused -> Use-After-Free (ABA, Corruption).
    -   **The implementation lacks explicit SMR (Safe Memory Reclamation)** like Hazard Pointers or RCU.
    -   However, in BPF, if we use `bpf_rcu_read_lock` (not present here) or if the BPF verifier ensures safety...
    -   **Verdict**: Theoretically unsafe regarding memory reclamation in a pre-emptive environment without RCU/HP. But the logic of the BST itself is correct.
