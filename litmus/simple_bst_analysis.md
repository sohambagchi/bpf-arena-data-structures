# Simple BST Analysis (`ds_bst.h`)

## 1. Overview
This implementation claims to be "Ellen et al." but lacks the `Info` records and `Update` fields (flags) required for the cooperative lock-free algorithm. It uses simple CAS on child pointers.

## 2. Insert Operation (`ds_bst_insert`)

### Minimal Memory-Op Version

```c
    // Search
    bst_search(...) -> (parent, leaf, ...)

    // Alloc
    new_internal = Alloc()
    new_leaf = Alloc()

    // Link
    m0: Store(new_internal->left, ...)
    m1: Store(new_internal->right, ...)

    // CAS Child (Linearization Point)
    m2: success = CAS_Release(&parent->child, leaf, new_internal)
```

## 3. Delete Operation (`ds_bst_delete`)

### Minimal Memory-Op Version

```c
    // Search
    bst_search(...) -> (gp, p, l, ...)

    // Read Sibling
    m0: sibling = Load_Acquire(&p->sibling_child)

    // CAS Grandparent Child (Linearization Point)
    // Removes 'p' and 'l' from tree, replaces 'p' with 'sibling'
    m1: success = CAS_Release(&gp->child, p, sibling)

    if (success) {
        // Free nodes
        Free(l)
        Free(p)
    }
```

## 4. Critical Flaws & Interleaving Analysis

### The "Free-While-Insert" Bug (Use-After-Free)
**Scenario**:
1.  **Thread I (Insert)**: Searches and lands at `p`. Wants to insert child under `p`.
2.  **Thread I**: Pauses just before `m2` (CAS on `p->child`). It holds a pointer to `p`.
3.  **Thread D (Delete)**: Searches and finds `key` at `leaf` (child of `p`).
4.  **Thread D**: Executing `ds_bst_delete`. GP is `p`'s parent.
5.  **Thread D**: Executes `m1` (CAS `gp->child`). Replaces `p` with `p->sibling`.
6.  **Thread D**: **FREES `p`** (`bpf_arena_free(result.parent)`).
7.  **Thread I**: Resumes. Accesses `p` to perform CAS.
    -   **Result**: Accessing freed memory.
    -   If memory was unmapped: Crash.
    -   If memory reused: Corruption (modifying a node now belonging to another structure).

### The "Lost Update" / Tree Corruption
Even without memory freeing issues:
1.  **Thread I**: Wants to insert under `p`. Reads `p->child` (old_leaf).
2.  **Thread D**: Deletes `p`. `p` is now detached from tree (reachable only via stale pointers).
3.  **Thread I**: CAS `p->child` succeeds! (Because `p`'s content hasn't changed, only reachability to `p` changed).
4.  **Result**: Thread I attached a new node to `p`, but `p` is garbage. The new node is **LOST**.
    -   Memory Leak.
    -   Data Loss (Insert reported success but data is gone).

## 5. Conclusion
**This implementation is BROKEN and UNSAFE for concurrent use.**
It attempts a lock-free strategy without the necessary coordination (flags/marking) to handle the interference between node modification (Insert) and node removal (Delete).

The correct Ellen algorithm (implemented in `ds_bintree.h`) solves this by:
1.  Marking `p` as "being deleted".
2.  Insert sees the mark and helps/backs-off, preventing the CAS on `p`.

**Recommendation**: Do not use `ds_bst.h`. Use `ds_bintree.h`.
