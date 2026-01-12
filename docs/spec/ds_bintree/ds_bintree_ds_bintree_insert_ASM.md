## ds_bintree_insert (0x175e-0x1e0b)
Insert entry point for the non-blocking bintree: performs a search, allocates replacement nodes, flags the parent, and helps complete the insert.

## Memory Operations

### m00: Load insert_failure_invalid_key
**Location:** 0x17c2
**C Code:** `head->stats.insert_failure_invalid_key++;`
**Assembly:**
```asm
17bb: 48 8b 85 18 ff ff ff  mov    rax,QWORD PTR [rbp-0xe8]
17c2: 8b 40 1c              mov    eax,DWORD PTR [rax+0x1c]   # MEMORY LOAD
17c5: 8d 50 01              lea    edx,[rax+0x1]
```

### m01: Store insert_failure_invalid_key
**Location:** 0x17cf
**C Code:** `head->stats.insert_failure_invalid_key++;`
**Assembly:**
```asm
17c8: 48 8b 85 18 ff ff ff  mov    rax,QWORD PTR [rbp-0xe8]
17cf: 89 50 1c              mov    DWORD PTR [rax+0x1c],edx   # MEMORY STORE
```

### m02: Load head->root
**Location:** 0x181c
**C Code:** `pLeaf = (struct ds_bintree_tree_node *)head->root;`
**Assembly:**
```asm
1818: 48 8b 45 b0           mov    rax,QWORD PTR [rbp-0x50]
181c: 48 8b 40 68           mov    rax,QWORD PTR [rax+0x68]   # MEMORY LOAD
1820: 48 89 45 80           mov    QWORD PTR [rbp-0x80],rax
```

### m03: Load pParent->m_pUpdate (ACQUIRE)
**Location:** 0x1861
**C Code:** `updParent = smp_load_acquire(&pParent->m_pUpdate);`
**Assembly:**
```asm
1859: 48 8b 45 88           mov    rax,QWORD PTR [rbp-0x78]
185d: 48 83 c0 20           add    rax,0x20
1861: 48 8b 00              mov    rax,QWORD PTR [rax]        # MEMORY LOAD (acquire)
1864: 48 89 85 78 ff ff ff  mov    QWORD PTR [rbp-0x88],rax
```

### m04: Load pParent->key.key
**Location:** 0x18a2
**C Code:** `if (key < pParent->key.key) { ... }`
**Assembly:**
```asm
189e: 48 8b 45 88           mov    rax,QWORD PTR [rbp-0x78]
18a2: 48 8b 40 08           mov    rax,QWORD PTR [rax+0x8]    # MEMORY LOAD
18a6: 48 39 45 a8           cmp    QWORD PTR [rbp-0x58],rax
```

### m05: Load leaf key for equality check
**Location:** 0x1912
**C Code:** `nCmp = (key == ((struct ds_bintree_leaf *)pLeaf)->kv.key);`
**Assembly:**
```asm
190e: 48 8b 45 80           mov    rax,QWORD PTR [rbp-0x80]
1912: 48 8b 40 08           mov    rax,QWORD PTR [rax+0x8]    # MEMORY LOAD
1916: 48 39 45 a8           cmp    QWORD PTR [rbp-0x58],rax
```

### m06: Store existing leaf value
**Location:** 0x1985
**C Code:** `res.pLeaf->kv.value = kv.value;`
**Assembly:**
```asm
1977: 48 8b 85 40 ff ff ff  mov    rax,QWORD PTR [rbp-0xc0]
197e: 48 8b 95 08 ff ff ff  mov    rdx,QWORD PTR [rbp-0xf8]
1985: 48 89 50 10           mov    QWORD PTR [rax+0x10],rdx  # MEMORY STORE
```

### m07: Load insert_into_updates
**Location:** 0x1990
**C Code:** `head->stats.insert_into_updates++;`
**Assembly:**
```asm
1989: 48 8b 85 18 ff ff ff  mov    rax,QWORD PTR [rbp-0xe8]
1990: 8b 40 40              mov    eax,DWORD PTR [rax+0x40]   # MEMORY LOAD
1993: 8d 50 01              lea    edx,[rax+0x1]
```

### m08: Store insert_into_updates
**Location:** 0x199d
**C Code:** `head->stats.insert_into_updates++;`
**Assembly:**
```asm
1996: 48 8b 85 18 ff ff ff  mov    rax,QWORD PTR [rbp-0xe8]
199d: 89 50 40              mov    DWORD PTR [rax+0x40],edx   # MEMORY STORE
```

### m09: Load insert_failure_no_parent
**Location:** 0x19bd
**C Code:** `head->stats.insert_failure_no_parent++;`
**Assembly:**
```asm
19b6: 48 8b 85 18 ff ff ff  mov    rax,QWORD PTR [rbp-0xe8]
19bd: 8b 40 2c              mov    eax,DWORD PTR [rax+0x2c]   # MEMORY LOAD
19c0: 8d 50 01              lea    edx,[rax+0x1]
```

### m10: Store insert_failure_no_parent
**Location:** 0x19ca
**C Code:** `head->stats.insert_failure_no_parent++;`
**Assembly:**
```asm
19c3: 48 8b 85 18 ff ff ff  mov    rax,QWORD PTR [rbp-0xe8]
19ca: 89 50 2c              mov    DWORD PTR [rax+0x2c],edx   # MEMORY STORE
```

### m11: Load insert_failure_no_leaf
**Location:** 0x19ea
**C Code:** `head->stats.insert_failure_no_leaf++;`
**Assembly:**
```asm
19e3: 48 8b 85 18 ff ff ff  mov    rax,QWORD PTR [rbp-0xe8]
19ea: 8b 40 30              mov    eax,DWORD PTR [rax+0x30]   # MEMORY LOAD
19ed: 8d 50 01              lea    edx,[rax+0x1]
```

### m12: Store insert_failure_no_leaf
**Location:** 0x19f7
**C Code:** `head->stats.insert_failure_no_leaf++;`
**Assembly:**
```asm
19f0: 48 8b 85 18 ff ff ff  mov    rax,QWORD PTR [rbp-0xe8]
19f7: 89 50 30              mov    DWORD PTR [rax+0x30],edx   # MEMORY STORE
```

### m13: Load insert_retry_didnt_help
**Location:** 0x1a31
**C Code:** `head->stats.insert_retry_didnt_help++;`
**Assembly:**
```asm
1a2a: 48 8b 85 18 ff ff ff  mov    rax,QWORD PTR [rbp-0xe8]
1a31: 8b 40 3c              mov    eax,DWORD PTR [rax+0x3c]   # MEMORY LOAD
1a34: 8d 50 01              lea    edx,[rax+0x1]
```

### m14: Store insert_retry_didnt_help
**Location:** 0x1a3e
**C Code:** `head->stats.insert_retry_didnt_help++;`
**Assembly:**
```asm
1a37: 48 8b 85 18 ff ff ff  mov    rax,QWORD PTR [rbp-0xe8]
1a3e: 89 50 3c              mov    DWORD PTR [rax+0x3c],edx   # MEMORY STORE
```

### m15: Load insert_failure_nomem (new_leaf failure)
**Location:** 0x1a99
**C Code:** `head->stats.insert_failure_nomem++;`
**Assembly:**
```asm
1a92: 48 8b 85 18 ff ff ff  mov    rax,QWORD PTR [rbp-0xe8]
1a99: 8b 40 24              mov    eax,DWORD PTR [rax+0x24]   # MEMORY LOAD
1a9c: 8d 50 01              lea    edx,[rax+0x1]
```

### m16: Store insert_failure_nomem (new_leaf failure)
**Location:** 0x1aa6
**C Code:** `head->stats.insert_failure_nomem++;`
**Assembly:**
```asm
1a9f: 48 8b 85 18 ff ff ff  mov    rax,QWORD PTR [rbp-0xe8]
1aa6: 89 50 24              mov    DWORD PTR [rax+0x24],edx   # MEMORY STORE
```

### m17: Load insert_failure_nomem (new_internal failure)
**Location:** 0x1adb
**C Code:** `head->stats.insert_failure_nomem++;`
**Assembly:**
```asm
1ad4: 48 8b 85 18 ff ff ff  mov    rax,QWORD PTR [rbp-0xe8]
1adb: 8b 40 24              mov    eax,DWORD PTR [rax+0x24]   # MEMORY LOAD
1ade: 8d 50 01              lea    edx,[rax+0x1]
```

### m18: Store insert_failure_nomem (new_internal failure)
**Location:** 0x1ae8
**C Code:** `head->stats.insert_failure_nomem++;`
**Assembly:**
```asm
1ae1: 48 8b 85 18 ff ff ff  mov    rax,QWORD PTR [rbp-0xe8]
1ae8: 89 50 24              mov    DWORD PTR [rax+0x24],edx   # MEMORY STORE
```

### m19: Store new_leaf->header.type
**Location:** 0x1af9
**C Code:** `new_leaf->header.type = BINTREE_NODE_LEAF;`
**Assembly:**
```asm
1af5: 48 8b 45 f0           mov    rax,QWORD PTR [rbp-0x10]
1af9: c7 00 01 00 00 00     mov    DWORD PTR [rax],0x1        # MEMORY STORE
```

### m20: Store new_leaf->kv.key
**Location:** 0x1b0a
**C Code:** `new_leaf->kv.key = kv.key;`
**Assembly:**
```asm
1aff: 48 8b 95 00 ff ff ff  mov    rdx,QWORD PTR [rbp-0x100]
1b06: 48 8b 45 f0           mov    rax,QWORD PTR [rbp-0x10]
1b0a: 48 89 50 08           mov    QWORD PTR [rax+0x8],rdx    # MEMORY STORE
```

### m21: Store new_leaf->kv.value
**Location:** 0x1b19
**C Code:** `new_leaf->kv.value = kv.value;`
**Assembly:**
```asm
1b0e: 48 8b 95 08 ff ff ff  mov    rdx,QWORD PTR [rbp-0xf8]
1b15: 48 8b 45 f0           mov    rax,QWORD PTR [rbp-0x10]
1b19: 48 89 50 10           mov    QWORD PTR [rax+0x10],rdx   # MEMORY STORE
```

### m22: Store new_internal->header.type
**Location:** 0x1b21
**C Code:** `new_internal->header.type = BINTREE_NODE_INTERNAL;`
**Assembly:**
```asm
1b1d: 48 8b 45 e8           mov    rax,QWORD PTR [rbp-0x18]
1b21: c7 00 00 00 00 00     mov    DWORD PTR [rax],0x0        # MEMORY STORE
```

### m23: Store new_internal->m_pUpdate
**Location:** 0x1b3a
**C Code:** `new_internal->m_pUpdate = bintree_make_update(NULL, BINTREE_CLEAN);`
**Assembly:**
```asm
1b36: 48 8b 55 e8           mov    rdx,QWORD PTR [rbp-0x18]
1b3a: 48 89 42 20           mov    QWORD PTR [rdx+0x20],rax   # MEMORY STORE
```

### m24: Load res.pLeaf->kv.key
**Location:** 0x1b4c
**C Code:** `bool bNewKeyIsLess = (kv.key < res.pLeaf->kv.key);`
**Assembly:**
```asm
1b45: 48 8b 85 40 ff ff ff  mov    rax,QWORD PTR [rbp-0xc0]
1b4c: 48 8b 40 08           mov    rax,QWORD PTR [rax+0x8]    # MEMORY LOAD
1b50: 48 39 c2              cmp    rdx,rax
```

### m25: Store new_internal->key.key (less-than case)
**Location:** 0x1b8b
**C Code:** `new_internal->key.key = res.pLeaf->kv.key;`
**Assembly:**
```asm
1b7c: 48 8b 85 40 ff ff ff  mov    rax,QWORD PTR [rbp-0xc0]
1b83: 48 8b 50 08           mov    rdx,QWORD PTR [rax+0x8]
1b87: 48 8b 45 e8           mov    rax,QWORD PTR [rbp-0x18]
1b8b: 48 89 50 08           mov    QWORD PTR [rax+0x8],rdx    # MEMORY STORE
```

### m26: Store new_internal->pLeft (less-than case)
**Location:** 0x1baa
**C Code:** `WRITE_ONCE(new_internal->pLeft, &new_leaf->header);`
**Assembly:**
```asm
1ba2: 48 8b 45 e8           mov    rax,QWORD PTR [rbp-0x18]
1ba6: 48 8d 50 10           lea    rdx,[rax+0x10]
1baa: 48 8b 45 f0           mov    rax,QWORD PTR [rbp-0x10]
1bae: 48 89 02              mov    QWORD PTR [rdx],rax        # MEMORY STORE
```

### m27: Store new_internal->pRight (less-than case)
**Location:** 0x1bc0
**C Code:** `WRITE_ONCE(new_internal->pRight, &res.pLeaf->header);`
**Assembly:**
```asm
1bb1: 48 8b 95 40 ff ff ff  mov    rdx,QWORD PTR [rbp-0xc0]
1bb8: 48 8b 45 e8           mov    rax,QWORD PTR [rbp-0x18]
1bbc: 48 83 c0 18           add    rax,0x18
1bc0: 48 89 10              mov    QWORD PTR [rax],rdx        # MEMORY STORE
```

### m28: Store new_internal->key.key (greater-than case)
**Location:** 0x1be1
**C Code:** `new_internal->key.key = kv.key;`
**Assembly:**
```asm
1bd6: 48 8b 95 00 ff ff ff  mov    rdx,QWORD PTR [rbp-0x100]
1bdd: 48 8b 45 e8           mov    rax,QWORD PTR [rbp-0x18]
1be1: 48 89 50 08           mov    QWORD PTR [rax+0x8],rdx    # MEMORY STORE
```

### m29: Store new_internal->pLeft (greater-than case)
**Location:** 0x1bf4
**C Code:** `WRITE_ONCE(new_internal->pLeft, &res.pLeaf->header);`
**Assembly:**
```asm
1be5: 48 8b 95 40 ff ff ff  mov    rdx,QWORD PTR [rbp-0xc0]
1bec: 48 8b 45 e8           mov    rax,QWORD PTR [rbp-0x18]
1bf0: 48 83 c0 10           add    rax,0x10
1bf4: 48 89 10              mov    QWORD PTR [rax],rdx        # MEMORY STORE
```

### m30: Store new_internal->pRight (greater-than case)
**Location:** 0x1c03
**C Code:** `WRITE_ONCE(new_internal->pRight, &new_leaf->header);`
**Assembly:**
```asm
1bf7: 48 8b 45 e8           mov    rax,QWORD PTR [rbp-0x18]
1bfb: 48 8d 50 18           lea    rdx,[rax+0x18]
1bff: 48 8b 45 f0           mov    rax,QWORD PTR [rbp-0x10]
1c03: 48 89 02              mov    QWORD PTR [rdx],rax        # MEMORY STORE
```

### m31: Load insert_failure_nomem (pOp failure)
**Location:** 0x1c3a
**C Code:** `head->stats.insert_failure_nomem++;`
**Assembly:**
```asm
1c33: 48 8b 85 18 ff ff ff  mov    rax,QWORD PTR [rbp-0xe8]
1c3a: 8b 40 24              mov    eax,DWORD PTR [rax+0x24]   # MEMORY LOAD
1c3d: 8d 50 01              lea    edx,[rax+0x1]
```

### m32: Store insert_failure_nomem (pOp failure)
**Location:** 0x1c47
**C Code:** `head->stats.insert_failure_nomem++;`
**Assembly:**
```asm
1c40: 48 8b 85 18 ff ff ff  mov    rax,QWORD PTR [rbp-0xe8]
1c47: 89 50 24              mov    DWORD PTR [rax+0x24],edx   # MEMORY STORE
```

### m33: Load insert_failure_nomem (iinfo failure)
**Location:** 0x1c94
**C Code:** `head->stats.insert_failure_nomem++;`
**Assembly:**
```asm
1c8d: 48 8b 85 18 ff ff ff  mov    rax,QWORD PTR [rbp-0xe8]
1c94: 8b 40 24              mov    eax,DWORD PTR [rax+0x24]   # MEMORY LOAD
1c97: 8d 50 01              lea    edx,[rax+0x1]
```

### m34: Store insert_failure_nomem (iinfo failure)
**Location:** 0x1ca1
**C Code:** `head->stats.insert_failure_nomem++;`
**Assembly:**
```asm
1c9a: 48 8b 85 18 ff ff ff  mov    rax,QWORD PTR [rbp-0xe8]
1ca1: 89 50 24              mov    DWORD PTR [rax+0x24],edx   # MEMORY STORE
```

### m35: Store iinfo->pParent
**Location:** 0x1cb9
**C Code:** `iinfo->pParent = res.pParent;`
**Assembly:**
```asm
1cae: 48 8b 95 38 ff ff ff  mov    rdx,QWORD PTR [rbp-0xc8]
1cb5: 48 8b 45 d0           mov    rax,QWORD PTR [rbp-0x30]
1cb9: 48 89 50 08           mov    QWORD PTR [rax+0x8],rdx    # MEMORY STORE
```

### m36: Store iinfo->pNew
**Location:** 0x1cc5
**C Code:** `iinfo->pNew = new_internal;`
**Assembly:**
```asm
1cbd: 48 8b 45 d0           mov    rax,QWORD PTR [rbp-0x30]
1cc1: 48 8b 55 e8           mov    rdx,QWORD PTR [rbp-0x18]
1cc5: 48 89 50 10           mov    QWORD PTR [rax+0x10],rdx   # MEMORY STORE
```

### m37: Store iinfo->pLeaf
**Location:** 0x1cd4
**C Code:** `iinfo->pLeaf = res.pLeaf;`
**Assembly:**
```asm
1cc9: 48 8b 95 40 ff ff ff  mov    rdx,QWORD PTR [rbp-0xc0]
1cd0: 48 8b 45 d0           mov    rax,QWORD PTR [rbp-0x30]
1cd4: 48 89 50 18           mov    QWORD PTR [rax+0x18],rdx   # MEMORY STORE
```

### m38: Store iinfo->bRightLeaf
**Location:** 0x1ce3
**C Code:** `iinfo->bRightLeaf = res.bRightLeaf;`
**Assembly:**
```asm
1cd8: 0f b6 95 58 ff ff ff  movzx  edx,BYTE PTR [rbp-0xa8]
1cdf: 48 8b 45 d0           mov    rax,QWORD PTR [rbp-0x30]
1ce3: 88 50 20              mov    BYTE PTR [rax+0x20],dl     # MEMORY STORE
```

### m39: Store pOp->insert
**Location:** 0x1cee
**C Code:** `pOp->insert = iinfo;`
**Assembly:**
```asm
1ce6: 48 8b 45 d8           mov    rax,QWORD PTR [rbp-0x28]
1cea: 48 8b 55 d0           mov    rdx,QWORD PTR [rbp-0x30]
1cee: 48 89 10              mov    QWORD PTR [rax],rdx        # MEMORY STORE
```

### m40: CAS res.pParent->m_pUpdate (ACQ_REL)
**Location:** 0x1d3d
**C Code:** `arena_atomic_cmpxchg(&res.pParent->m_pUpdate, expected, desired, ARENA_ACQ_REL, ARENA_ACQUIRE);`
**Assembly:**
```asm
1d2b: 48 8d 70 20           lea    rsi,[rax+0x20]
1d36: 48 8b 02              mov    rax,QWORD PTR [rdx]
1d3d: f0 48 0f b1 0e        lock cmpxchg QWORD PTR [rsi],rcx  # MEMORY CAS (acq_rel)
```

### m41: Atomic increment of head->count
**Location:** 0x1d78
**C Code:** `arena_atomic_inc(&head->count);`
**Assembly:**
```asm
1d6d: 48 8b 85 18 ff ff ff  mov    rax,QWORD PTR [rbp-0xe8]
1d74: 48 83 e8 80           sub    rax,0xffffffffffffff80
1d78: f0 48 83 00 01        lock add QWORD PTR [rax],0x1      # MEMORY ATOMIC ADD
```

### m42: Load total_inserts
**Location:** 0x1d84
**C Code:** `head->stats.total_inserts++;`
**Assembly:**
```asm
1d7d: 48 8b 85 18 ff ff ff  mov    rax,QWORD PTR [rbp-0xe8]
1d84: 8b 00                 mov    eax,DWORD PTR [rax]        # MEMORY LOAD
1d86: 8d 50 01              lea    edx,[rax+0x1]
```

### m43: Store total_inserts
**Location:** 0x1d90
**C Code:** `head->stats.total_inserts++;`
**Assembly:**
```asm
1d89: 48 8b 85 18 ff ff ff  mov    rax,QWORD PTR [rbp-0xe8]
1d90: 89 10                 mov    DWORD PTR [rax],edx        # MEMORY STORE
```

### m44: Load insert_retry_didnt_help (CAS failure)
**Location:** 0x1dd0
**C Code:** `head->stats.insert_retry_didnt_help++;`
**Assembly:**
```asm
1dc9: 48 8b 85 18 ff ff ff  mov    rax,QWORD PTR [rbp-0xe8]
1dd0: 8b 40 3c              mov    eax,DWORD PTR [rax+0x3c]   # MEMORY LOAD
1dd3: 8d 50 01              lea    edx,[rax+0x1]
```

### m45: Store insert_retry_didnt_help (CAS failure)
**Location:** 0x1ddd
**C Code:** `head->stats.insert_retry_didnt_help++;`
**Assembly:**
```asm
1dd6: 48 8b 85 18 ff ff ff  mov    rax,QWORD PTR [rbp-0xe8]
1ddd: 89 50 3c              mov    DWORD PTR [rax+0x3c],edx   # MEMORY STORE
```

### m46: Load insert_failure_busy
**Location:** 0x1df5
**C Code:** `head->stats.insert_failure_busy++;`
**Assembly:**
```asm
1dee: 48 8b 85 18 ff ff ff  mov    rax,QWORD PTR [rbp-0xe8]
1df5: 8b 40 28              mov    eax,DWORD PTR [rax+0x28]   # MEMORY LOAD
1df8: 8d 50 01              lea    edx,[rax+0x1]
```

### m47: Store insert_failure_busy
**Location:** 0x1e02
**C Code:** `head->stats.insert_failure_busy++;`
**Assembly:**
```asm
1dfb: 48 8b 85 18 ff ff ff  mov    rax,QWORD PTR [rbp-0xe8]
1e02: 89 50 28              mov    DWORD PTR [rax+0x28],edx   # MEMORY STORE
```

## Control Flow Analysis
- Outer retry loop: `iterations < BINTREE_MAX_DEPTH` bounds the insert attempt.
- Inner traversal loop (search): follows child pointers while node is internal and depth bound holds.
- Early-exit conditions: invalid key, missing parent/leaf, key exists, allocation failure, CAS failure, or busy retries.
- Success path: allocate nodes, build new internal node, flag parent with CAS, help complete insert, update counters.

## Return Paths
1. **Invalid head:** `head == NULL` → `DS_ERROR_INVALID`.
   - Expression: `RETURN_INVALID`
2. **Invalid key:** `kv.key >= BINTREE_SENTINEL_KEY1` → increment stat (m00/m01) → `DS_ERROR_INVALID`.
   - Expression: `m00 -> m01 -> RETURN_INVALID`
3. **Key exists:** update leaf value (m06), update stats (m07/m08) → `DS_SUCCESS`.
   - Expression: `m02 -> m03 -> m04 -> m05 -> m06 -> m07 -> m08 -> RETURN_SUCCESS`
4. **Missing parent/leaf:** stats update (m09/m10 or m11/m12) → `DS_ERROR_BUSY`.
   - Expression: `m09 -> m10 -> RETURN_BUSY | m11 -> m12 -> RETURN_BUSY`
5. **Allocation failure:** stats update (m15/m16, m17/m18, m31/m32, m33/m34) → `DS_ERROR_NOMEM`.
   - Expression: `(m15 -> m16 | m17 -> m18 | m31 -> m32 | m33 -> m34) -> RETURN_NOMEM`
6. **CAS success:** build nodes (m19..m39), CAS parent (m40), help insert, update counts (m41..m43) → `DS_SUCCESS`.
   - Expression: `m19 -> ... -> m40 -> m41 -> m42 -> m43 -> RETURN_SUCCESS`
7. **Retries exhausted:** update busy stat (m46/m47) → `DS_ERROR_BUSY`.
   - Expression: `m46 -> m47 -> RETURN_BUSY`

## Unified Expression
`(m02 -> (search_loop -> m05)* -> (m06 -> m07 -> m08 -> RETURN_SUCCESS | m09..m12 -> RETURN_BUSY | alloc_path(m19..m39) -> m40 -> (RETURN_SUCCESS | retry) | retry))* -> (m46 -> m47 -> RETURN_BUSY)`

## Notes
- Calls to `bintree_get_child` and `bintree_set_infinite_key` hide additional memory operations (see their reports).
- CAS on `m_pUpdate` uses acquire/release semantics to publish the new internal node before helping completes.
- The traversal loop bounds depth with `BINTREE_MAX_DEPTH` to satisfy verifier constraints.
