# Concurrency Analysis: Ellen Binary Search Tree

**Target File:** `include/ds_bst.h`  
**Type:** Leaf-Oriented Lock-Free Binary Search Tree  
**Status:** **UNSAFE** (Susceptible to Lost Updates)

## 1. Data Structure Overview

The `ds_bst.h` implements a leaf-oriented Binary Search Tree (BST) where:
*   **Internal Nodes** (`bst_internal_node`) contain only routing keys and pointers to children.
*   **Leaf Nodes** (`bst_leaf_node`) contain the actual key-value pairs.
*   **Updates** are performed via `arena_atomic_cmpxchg` (CAS) on child pointers.

The implementation relies on `BPF_MAP_TYPE_ARENA` for shared memory and uses C11-style atomic built-ins (`arena_atomic_*`) for synchronization.

## 2. Invariants

1.  **BST Property:** For any internal node with routing key $K$, all keys in the left subtree are $< K$, and all keys in the right subtree are $\ge K$.
2.  **Leaf-Oriented:** All data is stored in leaves. Internal nodes are immutable routing structures (once initialized).
3.  **Connectivity:** The tree is strictly strictly binary (every internal node has exactly two children).

## 3. Critical Functions & Synchronization

### 3.1. Search (`bst_search`)
*   **Operation:** Traverses from root to leaf using `smp_load_acquire` to read child pointers.
*   **Memory Ordering:** Uses `ACQUIRE` semantics to ensure that if a node pointer is observed, the node's initialization (payload) is also visible.
*   **Correctness Note:** The traversal has a hard loop limit (`BST_MAX_RETRIES`). If this limit is reached, the function may incorrectly return an internal node as a leaf (due to missing `is_leaf` check on the fallback path), leading to potential type confusion in callers.

### 3.2. Insert (`ds_bst_insert`)
*   **Operation:** Replaces an existing leaf $L$ with a new internal node $I$ that points to $L$ and a new leaf $New$.
*   **Synchronization:**
    ```c
    prev = arena_atomic_cmpxchg(&parent->child, old_leaf, new_internal,
                                ARENA_RELEASE, ARENA_RELAXED);
    ```
*   **Memory Ordering:**
    *   **Success (`ARENA_RELEASE`):** Synchronizes with `smp_load_acquire` in `bst_search`. Ensures `new_internal` and `new_leaf` are fully initialized before they become visible.
    *   **Failure (`ARENA_RELAXED`):** Acceptable as the operation simply retries the search.

### 3.3. Delete (`ds_bst_delete`)
*   **Operation:** Removes a leaf $L$ (child of $P$) by swinging the pointer of the grandparent $GP$ to point directly to $L$'s sibling $S$. Effectively removes $P$ and $L$.
*   **Synchronization:**
    ```c
    prev = arena_atomic_cmpxchg(&grandparent->child, parent, sibling,
                                ARENA_RELEASE, ARENA_RELAXED);
    ```
*   **Memory Ordering:** `ARENA_RELEASE` ensures the structural change is visible.

## 4. Concurrency Safety Analysis

### 4.1. The "Lost Update" Race Condition
The implementation is **incorrect** for concurrent Insert and Delete operations. The original algorithm by Ellen et al. requires nodes to be "flagged" (marked) before modification to prevent concurrent changes to a node that is being removed. This implementation lacks flags.

**Litmus Test (Lost Insert):**
Consider a tree segment: $GP \to P \to \{L1, L2\}$.
*   **Thread A (Delete L1):** Wants to remove $L1$. It reads $GP$, $P$, and observes $L2$ as the sibling of $L1$. It prepares to CAS $GP \to child$ from $P$ to $L2$.
*   **Thread B (Insert near L2):** Wants to insert a new key near $L2$. It reads $P$ and $L2$. It prepares to CAS $P \to child$ (where $L2$ is) to a new internal node $I(L2, New)$.

**Execution Trace:**
1.  **Thread A** reads state: $Sibling = L2$.
2.  **Thread B** executes CAS on $P$: Successfully changes $P \to child$ from $L2$ to $I(L2, New)$.
    *   *Note:* The tree now logically contains the new key.
3.  **Thread A** executes CAS on $GP$: Successfully changes $GP \to child$ from $P$ to $Sibling$ (which is $L2$).
    *   *Note:* Thread A's CAS succeeds because $GP \to child$ is still $P$.
    *   *Result:* $P$ is disconnected from the tree. The subtree rooted at $I(L2, New)$ (created by Thread B) is discarded. The insertion by Thread B is **lost**.

### 4.2. Memory Ordering Primitives
*   **`arena_atomic_cmpxchg`**: The use of `ARENA_RELEASE` is correct for publishing updates.
*   **`smp_load_acquire`**: Defined in `bpf_arena_common.h` as `READ_ONCE` + `barrier()`.
    *   **x86 (TSO):** This is sufficient as loads are implicitly acquire.
    *   **ARM64 (Weak):** This is **insufficient**. It compiles to a plain `LDR` which allows reordering of subsequent loads (e.g., reading the node contents before the pointer is fully validated). It should be `__atomic_load_n(..., __ATOMIC_ACQUIRE)` (compiling to `LDAR`).

## 5. Conclusion

The `ds_bst.h` implementation is **unsafe for general concurrent use**.

1.  **Algorithmic Flaw:** It suffers from the "Lost Update" problem when an Insert competes with a Delete on a sibling node. It is only safe for:
    *   Read-only workloads.
    *   Insert-only workloads (no deletions).
    *   Single-writer workloads.
2.  **Implementation Flaw:** The `bst_search` function has a bug where exceeding the retry limit can cause it to return an internal node as a leaf, leading to potential type confusion in callers.
3.  **Portability Issue:** `smp_load_acquire` is not strictly correct for weak memory models in this codebase.

**Recommendation:** Do not use this data structure for the `skeleton_bst` test case which mixes Kernel Inserts and Userspace Deletes (`pop`), as it will likely trigger the race condition described above. Implement the full Ellen et al. algorithm with flagging, or switch to a simpler lock-based approach if lock-freedom is not strictly required.
