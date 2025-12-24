# **Implementation Specification: Non-Blocking Binary Search Tree (Ellen et al. 2010\)**

## **1\. Introduction**

This document specifies the implementation of a **Non-Blocking Binary Search Tree (BST)** for the BPF Arena framework. The algorithm is based on the 2010 paper *"Non-blocking binary search trees"* by Ellen, Fatourou, Ruppert, and van Breugel.

### **Algorithm Overview**

This is a **leaf-oriented** BST, meaning:

* **Internal Nodes** contain only keys (for routing) and pointers to children.  
* **Leaf Nodes** contain keys and actual values.  
* The tree structure is maintained using single-word Compare-And-Swap (CAS) operations.  
* **Concurrency Control:** It uses a cooperative "helping" mechanism. Update operations "flag" or "mark" nodes to indicate intent. If a concurrent thread encounters a flagged node, it helps complete the pending operation before attempting its own.

### **Performance & Guarantees**

* **Progress:** Non-blocking (Lock-free).  
* **Consistency:** Linearizable.  
* **Disjoint Access Parallelism:** Updates to different parts of the tree run concurrently without interference.

## **2\. Data Structure Organization**

### **2.1 Arena Memory & Pointers**

All nodes and auxiliary records must be allocated using bpf\_arena\_alloc and referenced via \_\_arena pointers.

* **Alignment:** 64-bit alignment is assumed to allow using the lower 2 bits of pointers for state flags.  
* **Tagged Pointers:** The update field in Internal nodes is a single 64-bit word combining a pointer to an Info record and a 2-bit state.

### **2.2 Enums and Constants**

// State definitions for the Update field (stored in low 2 bits)  
enum ds\_bst\_state {  
    BST\_CLEAN \= 0,  
    BST\_DFLAG \= 1,  
    BST\_IFLAG \= 2,  
    BST\_MARK  \= 3  
};

// Node Types  
enum ds\_bst\_node\_type {  
    BST\_NODE\_INTERNAL \= 0,  
    BST\_NODE\_LEAF     \= 1  
};

### **2.3 Struct Definitions**

#### **Common Header**

All nodes share a common header to identify their type.

struct ds\_bst\_node\_header {  
    \_\_u32 type; // BST\_NODE\_INTERNAL or BST\_NODE\_LEAF  
};

#### **Leaf Node**

Stores the actual key-value pair.

struct ds\_bst\_leaf {  
    struct ds\_bst\_node\_header header;  
    \_\_u64 key;  
    \_\_u64 value;  
};

#### **Internal Node**

Routing node. Contains the synchronization update field.

struct ds\_bst\_internal {  
    struct ds\_bst\_node\_header header;  
    \_\_u64 key;  
      
    // Left and Right children (points to Leaf or Internal)  
    void \_\_arena \*left;  
    void \_\_arena \*right;

    // The Synchronization Point: Combines (Info\* | State)  
    // Must be accessed atomically.  
    \_\_u64 update;   
};

#### **Info Records (for Helping)**

These records store the "context" of an ongoing operation so helpers can finish it.

// Abstract base for Info records  
struct ds\_bst\_info {  
    \_\_u32 type; // 0 for IInfo, 1 for DInfo  
};

struct ds\_bst\_iinfo {  
    struct ds\_bst\_info header; // type \= 0  
    struct ds\_bst\_internal \_\_arena \*p;           // Parent  
    struct ds\_bst\_leaf \_\_arena \*l;               // Leaf to replace  
    struct ds\_bst\_internal \_\_arena \*new\_internal; // New internal node  
};

struct ds\_bst\_dinfo {  
    struct ds\_bst\_info header; // type \= 1  
    struct ds\_bst\_internal \_\_arena \*gp; // Grandparent  
    struct ds\_bst\_internal \_\_arena \*p;  // Parent  
    struct ds\_bst\_leaf \_\_arena \*l;      // Leaf to delete  
    \_\_u64 p\_update;                     // Expected value of p-\>update  
};

#### **Head Structure**

The entry point for the BPF map/skeleton.

struct ds\_bst\_head {  
    struct ds\_bst\_internal \_\_arena \*root;  
    \_\_u64 count; // Statistics  
};

## **3\. Algorithm Pseudo-Code (BPF/C Adaptation)**

### **3.1 Helpers for Tagged Pointers**

// Macros or inline functions  
\#define UPDATE\_MASK\_STATE 0x3UL  
\#define UPDATE\_MASK\_PTR   (\~0x3UL)

static inline \_\_u64 make\_update(void \_\_arena \*info, \_\_u64 state) {  
    return ((\_\_u64)info & UPDATE\_MASK\_PTR) | (state & UPDATE\_MASK\_STATE);  
}

static inline int get\_state(\_\_u64 update) {  
    return (int)(update & UPDATE\_MASK\_STATE);  
}

static inline void \_\_arena \*get\_info(\_\_u64 update) {  
    return (void \_\_arena \*)(update & UPDATE\_MASK\_PTR);  
}

### **3.2 Search**

The search function is used by Insert, Delete, and Find. It must handle cast\_kern on every dereference.

**Memory Order Note:** We use ARENA\_ACQUIRE when loading update and children. This ensures that if we see a non-null pointer or a flagged state, the data pointed to (the child node or the Info record) is fully visible.

struct search\_result {  
    struct ds\_bst\_internal \_\_arena \*gp;  
    struct ds\_bst\_internal \_\_arena \*p;  
    struct ds\_bst\_leaf \_\_arena \*l;  
    \_\_u64 p\_update;  
    \_\_u64 gp\_update;  
};

static inline void search(struct ds\_bst\_head \_\_arena \*head, \_\_u64 key, struct search\_result \*res) {  
    struct ds\_bst\_internal \_\_arena \*p \= head-\>root;  
    struct ds\_bst\_internal \_\_arena \*gp \= NULL;  
    void \_\_arena \*l\_ptr \= NULL;  
      
    // cast\_kern is required when reading pointers from arena  
    cast\_kern(p); 

    // Initial dummy nodes handling required (see Init section)  
      
    while (can\_loop) {  
        // p is Internal here.  
          
        // ACQUIRE required to safely dereference info if flagged  
        \_\_u64 p\_up \= arena\_atomic\_load(\&p-\>update, ARENA\_ACQUIRE);  
          
        // Decide direction  
        // ACQUIRE required to safely dereference l\_ptr  
        if (key \< p-\>key) {  
            l\_ptr \= arena\_atomic\_load(\&p-\>left, ARENA\_ACQUIRE);   
        } else {  
            l\_ptr \= arena\_atomic\_load(\&p-\>right, ARENA\_ACQUIRE);  
        }  
        cast\_kern(l\_ptr);  
          
        // Check if child is leaf  
        struct ds\_bst\_node\_header \_\_arena \*h \= l\_ptr;  
        if (h-\>type \== BST\_NODE\_LEAF) {  
            // Found leaf  
            res-\>gp \= gp;  
            res-\>p \= p;  
            res-\>l \= (struct ds\_bst\_leaf \_\_arena \*)l\_ptr;  
            res-\>p\_update \= p\_up;  
            // res-\>gp\_update must be maintained from previous iteration  
            break;  
        } else {  
            // Step down  
            res-\>gp\_update \= p\_up;  
            gp \= p;  
            p \= (struct ds\_bst\_internal \_\_arena \*)l\_ptr;  
        }  
    }  
}

### **3.3 Insertion**

**Logic:**

1. Search for key. If found, return error.  
2. Check p-\>update. If state is not CLEAN, help() it and retry.  
3. Allocate NewLeaf, NewInternal, IInfo.  
4. CAS p-\>update from (CLEAN, old\_info) to (IFLAG, new\_IInfo).  
5. If successful, call help\_insert (perform child CAS and unflag).

**Memory Order Note (Critical):** \* **Success (ACQ\_REL):** RELEASE ensures the initialized iinfo is visible to helpers. ACQUIRE ensures we synchronize with previous updates to this node.

* **Failure (ACQUIRE):** If CAS fails, we get the current value of p-\>update. We immediately pass this to help(), which dereferences the info pointer. **We MUST use ARENA\_ACQUIRE on failure** to ensure the Info object is visible. RELAXED is unsafe here.

static inline int ds\_bst\_insert(struct ds\_bst\_head \_\_arena \*head, \_\_u64 key, \_\_u64 value) {  
    while (can\_loop) {  
        struct search\_result res;  
        search(head, key, \&res);  
          
        if (res.l-\>key \== key) return DS\_ERROR\_EXISTS;  
          
        if (get\_state(res.p\_update) \!= BST\_CLEAN) {  
            help(res.p\_update);  
            continue; // Retry  
        }  
          
        // 1\. Allocate nodes (NewLeaf, NewInternal, IInfo)  
        // Check DS\_ERROR\_NOMEM  
          
        // 2\. Setup NewInternal  
        // Determine order of res.l and NewLeaf  
          
        // 3\. Setup IInfo  
        // iinfo-\>p \= res.p; iinfo-\>l \= res.l; iinfo-\>new\_internal \= new\_int;  
          
        // 4\. Try to Flag Parent  
        \_\_u64 new\_update \= make\_update(iinfo, BST\_IFLAG);  
          
        // FAILURE MO MUST BE ACQUIRE to safe deref result in help()  
        \_\_u64 result \= arena\_atomic\_cmpxchg(\&res.p-\>update, res.p\_update, new\_update,   
                                            ARENA\_ACQ\_REL, ARENA\_ACQUIRE);  
                                              
        if (result \== res.p\_update) {  
            // Success\! Complete the work  
            help\_insert(iinfo);  
            arena\_atomic\_inc(\&head-\>count);  
            return DS\_SUCCESS;  
        } else {  
            // CAS Failed. Help the winner if needed  
            help(result);  
            // Free allocated nodes (Manual cleanup required in C)  
        }  
    }  
    return DS\_ERROR\_TIMEOUT;  
}

### **3.4 Deletion**

**Logic:**

1. Search for key. If not found, error.  
2. Check gp-\>update and p-\>update. If dirty, help() and retry.  
3. Alloc DInfo.  
4. CAS gp-\>update to (DFLAG, DInfo).  
5. If success, call help\_delete.

static inline int ds\_bst\_delete(struct ds\_bst\_head \_\_arena \*head, \_\_u64 key) {  
    while (can\_loop) {  
        struct search\_result res;  
        search(head, key, \&res);  
          
        if (res.l-\>key \!= key) return DS\_ERROR\_NOT\_FOUND;  
          
        if (get\_state(res.gp\_update) \!= BST\_CLEAN) {  
            help(res.gp\_update); continue;  
        }  
        if (get\_state(res.p\_update) \!= BST\_CLEAN) {  
            help(res.p\_update); continue;  
        }  
          
        // Alloc DInfo  
        // dinfo-\>gp \= res.gp; dinfo-\>p \= res.p; dinfo-\>l \= res.l;   
        // dinfo-\>p\_update \= res.p\_update;  
          
        \_\_u64 new\_up \= make\_update(dinfo, BST\_DFLAG);  
          
        // FAILURE MO MUST BE ACQUIRE to safe deref result in help()  
        \_\_u64 res\_cas \= arena\_atomic\_cmpxchg(\&res.gp-\>update, res.gp\_update, new\_up,  
                                             ARENA\_ACQ\_REL, ARENA\_ACQUIRE);  
          
        if (res\_cas \== res.gp\_update) {  
            if (help\_delete(dinfo)) {  
                arena\_atomic\_dec(\&head-\>count);  
                return DS\_SUCCESS;  
            }  
            // If help\_delete returns false, it backtracked. Retry.  
        } else {  
            help(res\_cas);  
            // Free dinfo  
        }  
    }  
}

### **3.5 Helping Logic & Child CAS**

static inline void help(\_\_u64 update) {  
    void \_\_arena \*info \= get\_info(update);  
    int state \= get\_state(update);  
    cast\_kern(info); // Dereference safety relies on ACQUIRE loads of 'update'  
      
    if (state \== BST\_IFLAG) {  
        help\_insert((struct ds\_bst\_iinfo \_\_arena \*)info);  
    } else if (state \== BST\_MARK) {  
        help\_marked((struct ds\_bst\_dinfo \_\_arena \*)info);  
    } else if (state \== BST\_DFLAG) {  
        help\_delete((struct ds\_bst\_dinfo \_\_arena \*)info);  
    }  
}

// CAS\_CHILD implementation  
static inline void cas\_child(struct ds\_bst\_internal \_\_arena \*parent,  
                             void \_\_arena \*old\_node,   
                             void \_\_arena \*new\_node) {  
    // Determine which child to change based on keys.  
    // Note: Paper uses keys to decide left/right.  
    if (new\_node-\>key \< parent-\>key) {  
        arena\_atomic\_cmpxchg(\&parent-\>left, (\_\_u64)old\_node, (\_\_u64)new\_node,   
                             ARENA\_ACQ\_REL, ARENA\_ACQUIRE);  
    } else {  
        arena\_atomic\_cmpxchg(\&parent-\>right, (\_\_u64)old\_node, (\_\_u64)new\_node,   
                             ARENA\_ACQ\_REL, ARENA\_ACQUIRE);  
    }  
}

// Help Insert:  
// 1\. CAS child pointer of op-\>p to point to op-\>new\_internal  
// 2\. CAS op-\>p-\>update from IFLAG to CLEAN  
static inline void help\_insert(struct ds\_bst\_iinfo \_\_arena \*op) {  
    // Perform Child CAS  
    cas\_child(op-\>p, op-\>l, op-\>new\_internal);  
      
    // Unflag  
    // We use 'op' as the info pointer even for CLEAN state to ensure the 64-bit value   
    // is unique (ABA protection), though it won't be dereferenced when CLEAN.  
    \_\_u64 expected \= make\_update(op, BST\_IFLAG);  
    \_\_u64 clean \= make\_update(op, BST\_CLEAN);   
    arena\_atomic\_cmpxchg(\&op-\>p-\>update, expected, clean, ARENA\_RELEASE, ARENA\_RELAXED);  
}

// Help Delete:  
// 1\. CAS op-\>p-\>update to MARK (using op-\>p\_update as expected)  
// 2\. If Mark succeeds (or is already marked by us), call help\_marked  
// 3\. If Mark fails, Backtrack (CAS op-\>gp-\>update from DFLAG to CLEAN)  
static inline bool help\_delete(struct ds\_bst\_dinfo \_\_arena \*op) {  
    \_\_u64 expected \= op-\>p\_update;  
    \_\_u64 marked\_val \= make\_update(op, BST\_MARK);  
      
    // FAILURE MO MUST BE ACQUIRE because we call help(res)  
    \_\_u64 res \= arena\_atomic\_cmpxchg(\&op-\>p-\>update, expected, marked\_val,   
                                     ARENA\_ACQ\_REL, ARENA\_ACQUIRE);  
                                       
    if (res \== expected || res \== marked\_val) {  
        help\_marked(op);  
        return true;  
    } else {  
        // Failed to mark. Backtrack GP.  
        help(res); // Help the operation that blocked our mark  
        \_\_u64 gp\_expected \= make\_update(op, BST\_DFLAG);  
        \_\_u64 gp\_clean \= make\_update(op, BST\_CLEAN);  
        arena\_atomic\_cmpxchg(\&op-\>gp-\>update, gp\_expected, gp\_clean,   
                             ARENA\_RELEASE, ARENA\_RELAXED);  
        return false;  
    }  
}

// Help Marked:  
// 1\. Determine sibling of op-\>l  
// 2\. Splice out op-\>p by swinging op-\>gp's child pointer to sibling  
// 3\. Unflag op-\>gp  
static inline void help\_marked(struct ds\_bst\_dinfo \_\_arena \*op) {  
    struct ds\_bst\_internal \_\_arena \*p \= op-\>p;  
    cast\_kern(p);  
      
    void \_\_arena \*sibling;  
    // We need to read p's children to find the sibling of op-\>l.  
    // Since p is MARKED, its children are immutable now.  
    // However, we still use ACQUIRE to see the latest.  
    void \_\_arena \*right \= arena\_atomic\_load(\&p-\>right, ARENA\_ACQUIRE);  
      
    // Note: Comparing pointers directly requires they are valid arena pointers  
    if (right \== op-\>l) {  
        sibling \= arena\_atomic\_load(\&p-\>left, ARENA\_ACQUIRE);  
    } else {  
        sibling \= right;  
    }  
      
    // Physical deletion  
    cas\_child(op-\>gp, op-\>p, sibling);  
      
    // Unflag GP  
    \_\_u64 expected \= make\_update(op, BST\_DFLAG);  
    \_\_u64 clean \= make\_update(op, BST\_CLEAN);  
    arena\_atomic\_cmpxchg(\&op-\>gp-\>update, expected, clean, ARENA\_RELEASE, ARENA\_RELAXED);  
}

## **4\. Initialization & Edge Cases**

### **4.1 Initialization (ds\_bst\_init)**

The tree must be initialized with **two infinite key sentinel leaves** and one internal root to avoid edge cases with empty trees or deleting the last node.

* Leaf1: Key \= UINT64\_MAX \- 1  
* Leaf2: Key \= UINT64\_MAX  
* Root: Internal Node, Key \= UINT64\_MAX, Left \-\> Leaf1, Right \-\> Leaf2.  
* Users insert keys \< UINT64\_MAX \- 1\.

### **4.2 Constraints & Limits**

* **Max Iterations:** All loops (while(can\_loop)) must be bounded to satisfy the BPF verifier.  
* **Recursion:** No recursion allowed. search is iterative.

## **5\. Verification (ds\_bst\_verify)**

The verification function runs in userspace (usually) to validate integrity.

1. Traverse the tree (BFS or DFS).  
2. Verify **BST Property**: Left \< Parent \<= Right.  
3. Verify **Type Consistency**: header.type matches pointer expectations.  
4. Count leaf nodes and match against head-\>count.

## **6\. Implementation Status Checklist**

* $$ $$  
  Struct layouts (Internal, Leaf, IInfo, DInfo)  
* $$ $$  
  Tagged Pointer macros  
* $$ $$  
  ds\_bst\_init with sentinels  
* $$ $$  
  search helper (returning context)  
* $$ $$  
  help, help\_insert, help\_delete, help\_marked  
* $$ $$  
  ds\_bst\_insert  
* $$ $$  
  ds\_bst\_delete  
* $$ $$  
  ds\_bst\_verify