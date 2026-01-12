# BinTree Search Assembly Analysis

## Function: `ds_bintree_search` (0x692f - 0x6b0e)

## Memory Operations

### m00: Load head->root
**Location:** 0x69b4  
**C Code:** `pLeaf = (struct ds_bintree_tree_node __arena *) head->root;`  
**Assembly:**
```asm
69b0:  mov    rax,QWORD PTR [rbp-0x8]     # Load head pointer
69b4:  mov    rax,QWORD PTR [rax+0x68]    # Load head->root (MEMORY LOAD)
69b8:  mov    QWORD PTR [rbp-0x38],rax    # Store to pLeaf
```

### m01: Load pParent->m_pUpdate (acquire)
**Location:** 0x69ed  
**C Code:** `updParent = (struct ds_bintree_update_desc __arena *) smp_load_acquire(&pParent->m_pUpdate);`  
**Assembly:**
```asm
69e5:  mov    rax,QWORD PTR [rbp-0x30]    # Load pParent pointer
69e9:  add    rax,0x20                     # Add offset to m_pUpdate field
69ed:  mov    rax,QWORD PTR [rax]         # ACQUIRE LOAD of m_pUpdate (MEMORY LOAD)
69f0:  mov    QWORD PTR [rbp-0x40],rax    # Store to updParent
```

### m02: Load pParent->key.key (comparison)
**Location:** 0x6a1f  
**C Code:** `if (key < pParent->key.key)`  
**Assembly:**
```asm
6a1b:  mov    rax,QWORD PTR [rbp-0x30]    # Load pParent pointer
6a1f:  mov    rax,QWORD PTR [rax+0x8]     # Load pParent->key.key (MEMORY LOAD)
6a23:  cmp    QWORD PTR [rbp-0x10],rax    # Compare with search key
```

### m03: Load child (left branch, acquire)
**Location:** Inside `bintree_get_child` called at 0x6a3a  
**C Code:** `pLeaf = bintree_get_child(pParent, false, BINTREE_ACQUIRE);`  
**Assembly:**
```asm
6a29:  mov    rax,QWORD PTR [rbp-0x30]    # Load pParent
6a2d:  mov    edx,0x1                      # BINTREE_ACQUIRE flag
6a32:  mov    esi,0x0                      # false (left child)
6a37:  mov    rdi,rax
6a3a:  call   68ca <bintree_get_child>    # MEMORY LOAD inside function
6a3f:  mov    QWORD PTR [rbp-0x38],rax    # Store result to pLeaf
```

### m04: Load child (right branch, acquire)
**Location:** Inside `bintree_get_child` called at 0x6a5a  
**C Code:** `pLeaf = bintree_get_child(pParent, true, BINTREE_ACQUIRE);`  
**Assembly:**
```asm
6a49:  mov    rax,QWORD PTR [rbp-0x30]    # Load pParent
6a4d:  mov    edx,0x1                      # BINTREE_ACQUIRE flag
6a52:  mov    esi,0x1                      # true (right child)
6a57:  mov    rdi,rax
6a5a:  call   68ca <bintree_get_child>    # MEMORY LOAD inside function
6a5f:  mov    QWORD PTR [rbp-0x38],rax    # Store result to pLeaf
```

### m05: Load pLeaf->kv.key (final comparison)
**Location:** 0x6a89  
**C Code:** `nCmp = (key == ((struct ds_bintree_leaf __arena *)pLeaf)->kv.key);`  
**Assembly:**
```asm
6a85:  mov    rax,QWORD PTR [rbp-0x38]    # Load pLeaf pointer
6a89:  mov    rax,QWORD PTR [rax+0x8]     # Load pLeaf->kv.key (MEMORY LOAD)
6a8d:  cmp    QWORD PTR [rbp-0x10],rax    # Compare with search key
```

### m06: Store to res->pGrandParent
**Location:** 0x6a9f  
**C Code:** `res->pGrandParent = pGrandParent;`  
**Assembly:**
```asm
6a97:  mov    rax,QWORD PTR [rbp-0x18]    # Load res pointer
6a9b:  mov    rdx,QWORD PTR [rbp-0x20]    # Load pGrandParent
6a9f:  mov    QWORD PTR [rax],rdx         # Store to res->pGrandParent (MEMORY STORE)
```

### m07: Store to res->pParent
**Location:** 0x6aaa  
**C Code:** `res->pParent = pParent;`  
**Assembly:**
```asm
6aa2:  mov    rax,QWORD PTR [rbp-0x18]    # Load res pointer
6aa6:  mov    rdx,QWORD PTR [rbp-0x30]    # Load pParent
6aaa:  mov    QWORD PTR [rax+0x8],rdx     # Store to res->pParent (MEMORY STORE)
```

### m08: Store to res->pLeaf
**Location:** 0x6ab6  
**C Code:** `res->pLeaf = (struct ds_bintree_leaf __arena *) pLeaf;`  
**Assembly:**
```asm
6aae:  mov    rax,QWORD PTR [rbp-0x18]    # Load res pointer
6ab2:  mov    rdx,QWORD PTR [rbp-0x38]    # Load pLeaf
6ab6:  mov    QWORD PTR [rax+0x10],rdx    # Store to res->pLeaf (MEMORY STORE)
```

### m09: Store to res->updParent
**Location:** 0x6ac2  
**C Code:** `res->updParent = (__u64)updParent;`  
**Assembly:**
```asm
6aba:  mov    rdx,QWORD PTR [rbp-0x40]    # Load updParent
6abe:  mov    rax,QWORD PTR [rbp-0x18]    # Load res pointer
6ac2:  mov    QWORD PTR [rax+0x18],rdx    # Store to res->updParent (MEMORY STORE)
```

### m10: Store to res->updGrandParent
**Location:** 0x6ace  
**C Code:** `res->updGrandParent = (__u64)updGrandParent;`  
**Assembly:**
```asm
6ac6:  mov    rdx,QWORD PTR [rbp-0x50]    # Load updGrandParent
6aca:  mov    rax,QWORD PTR [rbp-0x18]    # Load res pointer
6ace:  mov    QWORD PTR [rax+0x20],rdx    # Store to res->updGrandParent (MEMORY STORE)
```

### m11: Store to res->bRightLeaf
**Location:** 0x6ada  
**C Code:** `res->bRightLeaf = bRightLeaf;`  
**Assembly:**
```asm
6ad2:  mov    rax,QWORD PTR [rbp-0x18]    # Load res pointer
6ad6:  movzx  edx,BYTE PTR [rbp-0x41]     # Load bRightLeaf
6ada:  mov    BYTE PTR [rax+0x28],dl      # Store to res->bRightLeaf (MEMORY STORE)
```

### m12: Load res.pLeaf (null check)
**Location:** 0x6add  
**C Code:** `if (!res.pLeaf)`  
**Assembly:**
```asm
6add:  mov    rax,QWORD PTR [rbp-0x80]    # Load res.pLeaf (MEMORY LOAD)
6ae1:  test   rax,rax                      # Check if NULL
```

### m13: Load res.pLeaf->kv.key (final verification)
**Location:** 0x6af1  
**C Code:** `if (res.pLeaf->kv.key == kv.key)`  
**Assembly:**
```asm
6aed:  mov    rax,QWORD PTR [rbp-0x80]    # Load res.pLeaf pointer
6af1:  mov    rdx,QWORD PTR [rax+0x8]     # Load res.pLeaf->kv.key (MEMORY LOAD)
6af5:  mov    rax,QWORD PTR [rbp-0xb0]    # Load search key
6afc:  cmp    rdx,rax                      # Compare
```

---

## Control Flow Analysis

### Loop Structure
The main loop is located from **0x69cd** to **0x6a7f** with loop condition check at **0x6a6b**.

**Loop Entry:** 0x6a6b (jmp from 0x69c8)  
**Loop Body:** 0x69cd - 0x6a67  
**Loop Condition:** 0x6a6b - 0x6a7f  
**Loop Back:** 0x6a7f (jle to 0x69cd)  
**Loop Exit:** 0x6a85

### Return Paths

#### Path 1: Early return on NULL head
**Return:** DS_ERROR_INVALID (-4)  
**Path:** `ENTRY -> 0x6960 -> 0x6968 (jne not taken) -> 0x696a -> 0x696f (jmp) -> 0x6b0d (RETURN)`  
**Expression:** `ENTRY -> RETURN1`

#### Path 2: Tree too deep (pLeaf is NULL after loop)
**Return:** DS_ERROR_NOT_FOUND (-1)  
**Path:** `ENTRY -> 0x6968 (jne taken) -> m00 -> LOOP_ENTRY -> (m01 -> m02 -> (m03|m04))* -> LOOP_EXIT -> m05 -> m06 -> m07 -> m08 -> m09 -> m10 -> m11 -> m12 -> 0x6ae4 (jne not taken) -> 0x6ae6 -> 0x6aeb (jmp) -> 0x6b0d (RETURN)`  
**Expression:** `ENTRY -> m00 -> (m01 -> m02 -> (m03|m04))* -> m05 -> m06 -> m07 -> m08 -> m09 -> m10 -> m11 -> m12 -> RETURN2`

#### Path 3: Key found
**Return:** DS_SUCCESS (0)  
**Path:** `ENTRY -> 0x6968 (jne taken) -> m00 -> LOOP_ENTRY -> (m01 -> m02 -> (m03|m04))* -> LOOP_EXIT -> m05 -> m06 -> m07 -> m08 -> m09 -> m10 -> m11 -> m12 -> 0x6ae4 (jne taken) -> m13 -> 0x6aff (jne taken) -> 0x6b01 -> 0x6b06 (jmp) -> 0x6b0d (RETURN)`  
**Expression:** `ENTRY -> m00 -> (m01 -> m02 -> (m03|m04))* -> m05 -> m06 -> m07 -> m08 -> m09 -> m10 -> m11 -> m12 -> m13 -> RETURN3`

#### Path 4: Key not found
**Return:** DS_ERROR_NOT_FOUND (-1)  
**Path:** `ENTRY -> 0x6968 (jne taken) -> m00 -> LOOP_ENTRY -> (m01 -> m02 -> (m03|m04))* -> LOOP_EXIT -> m05 -> m06 -> m07 -> m08 -> m09 -> m10 -> m11 -> m12 -> 0x6ae4 (jne taken) -> m13 -> 0x6aff (jne not taken) -> 0x6b08 -> 0x6b0d (RETURN)`  
**Expression:** `ENTRY -> m00 -> (m01 -> m02 -> (m03|m04))* -> m05 -> m06 -> m07 -> m08 -> m09 -> m10 -> m11 -> m12 -> m13 -> RETURN4`

---

## Unified Control Flow Expression

**All Paths with Memory Operations:**

```
Path1:  ENTRY -> RETURN1  (early NULL check)

Path2:  ENTRY -> m00 -> (m01 -> m02 -> (m03|m04))* -> m05 -> m06 -> m07 -> m08 -> m09 -> m10 -> m11 -> m12 -> RETURN2

Path3:  ENTRY -> m00 -> (m01 -> m02 -> (m03|m04))* -> m05 -> m06 -> m07 -> m08 -> m09 -> m10 -> m11 -> m12 -> m13 -> RETURN3

Path4:  ENTRY -> m00 -> (m01 -> m02 -> (m03|m04))* -> m05 -> m06 -> m07 -> m08 -> m09 -> m10 -> m11 -> m12 -> m13 -> RETURN4
```

---

## Loop Details

### Main Traversal Loop
The loop executes while:
1. `bintree_is_internal(pLeaf)` is true (checked at 0x6a72-0x6a79)
2. `iterations < BINTREE_MAX_DEPTH` (checked at 0x6a7b-0x6a7f, max depth = 3)

**Loop Body Operations:**
- **m01**: Load update descriptor (acquire semantics)
- **State check**: Check if DFLAG or MARK bit is set (0x6a07-0x6a13)
  - If set, break from loop (backoff)
- **m02**: Load parent key for comparison
- **Conditional branching:**
  - If `key < parent->key`: **m03** (load left child)
  - Otherwise: **m04** (load right child)
- Increment iteration counter

**Loop Exit Conditions:**
1. Node is not internal (leaf reached)
2. Iteration count reaches max depth (4 iterations)
3. Update descriptor has DFLAG or MARK set (backoff for concurrent modification)

### Early Break Path (Backoff)
When DFLAG or MARK detected:
```
m01 -> (state_check: DFLAG|MARK) -> increment_counter -> BREAK -> m05 -> ...
```

---

## Notes

- **Acquire semantics:** m01 and m03/m04 use acquire memory ordering to ensure visibility of concurrent modifications
- **No CAS operations:** This is a read-only search operation with no compare-and-swap
- **Backoff mechanism:** The search backs off when it detects ongoing updates (DFLAG or MARK bits)
- **Maximum depth:** Limited to 4 iterations (BINTREE_MAX_DEPTH = 3, checked with `<=`)
- **Memory ordering:** Critical loads use acquire semantics to coordinate with concurrent writers
