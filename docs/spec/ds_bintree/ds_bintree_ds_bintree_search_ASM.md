## ds_bintree_search (0x692f-0x6b0e)
Search entry point: traverses the bintree to find the leaf that would contain the key and returns success if it matches.

## Memory Operations

### m00: Load head->root
**Location:** 0x69b4
**C Code:** `pLeaf = (struct ds_bintree_tree_node *)head->root;`
**Assembly:**
```asm
69b0: 48 8b 45 f8           mov    rax,QWORD PTR [rbp-0x8]
69b4: 48 8b 40 68           mov    rax,QWORD PTR [rax+0x68]   # MEMORY LOAD
69b8: 48 89 45 c8           mov    QWORD PTR [rbp-0x38],rax
```

### m01: Load pParent->m_pUpdate (ACQUIRE)
**Location:** 0x69ed
**C Code:** `updParent = smp_load_acquire(&pParent->m_pUpdate);`
**Assembly:**
```asm
69e5: 48 8b 45 d0           mov    rax,QWORD PTR [rbp-0x30]
69e9: 48 83 c0 20           add    rax,0x20
69ed: 48 8b 00              mov    rax,QWORD PTR [rax]        # MEMORY LOAD (acquire)
69f0: 48 89 45 c0           mov    QWORD PTR [rbp-0x40],rax
```

### m02: Load pParent->key.key
**Location:** 0x6a1f
**C Code:** `if (key < pParent->key.key) { ... }`
**Assembly:**
```asm
6a1b: 48 8b 45 d0           mov    rax,QWORD PTR [rbp-0x30]
6a1f: 48 8b 40 08           mov    rax,QWORD PTR [rax+0x8]    # MEMORY LOAD
6a23: 48 39 45 f0           cmp    QWORD PTR [rbp-0x10],rax
```

### m03: Load leaf key for equality check
**Location:** 0x6a89
**C Code:** `nCmp = (key == ((struct ds_bintree_leaf *)pLeaf)->kv.key);`
**Assembly:**
```asm
6a85: 48 8b 45 c8           mov    rax,QWORD PTR [rbp-0x38]
6a89: 48 8b 40 08           mov    rax,QWORD PTR [rax+0x8]    # MEMORY LOAD
6a8d: 48 39 45 f0           cmp    QWORD PTR [rbp-0x10],rax
```

### m04: Load res.pLeaf->kv.key for final match
**Location:** 0x6af1
**C Code:** `if (res.pLeaf->kv.key == kv.key) return DS_SUCCESS;`
**Assembly:**
```asm
6aed: 48 8b 45 80           mov    rax,QWORD PTR [rbp-0x80]
6af1: 48 8b 50 08           mov    rdx,QWORD PTR [rax+0x8]    # MEMORY LOAD
6af5: 48 8b 85 50 ff ff ff  mov    rax,QWORD PTR [rbp-0xb0]
6afc: 48 39 c2              cmp    rdx,rax
```

## Control Flow Analysis
- Traversal loop: while `bintree_is_internal(pLeaf)` and depth bound holds.
- Backoff: if `m_pUpdate` indicates DFLAG or MARK, break traversal.
- Branch on key comparison to follow left or right child (via `bintree_get_child`).
- After traversal, compare leaf key to target key.

## Return Paths
1. **Invalid head:** `head == NULL` → `DS_ERROR_INVALID`.
   - Expression: `RETURN_INVALID`
2. **Leaf missing (too deep):** `res.pLeaf == NULL` → `DS_ERROR_NOT_FOUND`.
   - Expression: `m00 -> ... -> RETURN_NOT_FOUND`
3. **Key found:** leaf key equals `kv.key` → `DS_SUCCESS`.
   - Expression: `m00 -> m01 -> m02 -> m03 -> m04 -> RETURN_SUCCESS`
4. **Key not found:** leaf key differs → `DS_ERROR_NOT_FOUND`.
   - Expression: `m00 -> m01 -> m02 -> m03 -> m04 -> RETURN_NOT_FOUND`

## Unified Expression
`m00 -> (m01 -> m02 -> (bintree_get_child -> loop))* -> m03 -> m04 -> (RETURN_SUCCESS | RETURN_NOT_FOUND)`

## Notes
- Calls to `bintree_get_child` and `bintree_is_internal` perform additional loads (see their reports).
- `bintree_get_bits` is used to detect flagged or marked updates without additional memory accesses.
