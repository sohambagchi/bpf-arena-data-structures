## bintree_get_child (0x1464-0x14b3)
Returns either left or right child pointer with relaxed or acquire semantics based on `mo` and `bRight`.

## Memory Operations

### m00: Load node->pRight (RELAXED path)
**Location:** 0x1484
**C Code:** `return bRight ? READ_ONCE(node->pRight) : READ_ONCE(node->pLeft);`
**Assembly:**
```asm
1480: 48 8b 45 f8           mov    rax,QWORD PTR [rbp-0x8]
1484: 48 8b 40 18           mov    rax,QWORD PTR [rax+0x18]   # MEMORY LOAD (relaxed)
```

### m01: Load node->pLeft (RELAXED path)
**Location:** 0x148e
**C Code:** `return bRight ? READ_ONCE(node->pRight) : READ_ONCE(node->pLeft);`
**Assembly:**
```asm
148a: 48 8b 45 f8           mov    rax,QWORD PTR [rbp-0x8]
148e: 48 8b 40 10           mov    rax,QWORD PTR [rax+0x10]   # MEMORY LOAD (relaxed)
```

### m02: Load node->pRight (ACQUIRE path)
**Location:** 0x14a2
**C Code:** `return bRight ? smp_load_acquire(&node->pRight) : smp_load_acquire(&node->pLeft);`
**Assembly:**
```asm
149a: 48 8b 45 f8           mov    rax,QWORD PTR [rbp-0x8]
149e: 48 83 c0 18           add    rax,0x18
14a2: 48 8b 00              mov    rax,QWORD PTR [rax]        # MEMORY LOAD (acquire)
```

### m03: Load node->pLeft (ACQUIRE path)
**Location:** 0x14af
**C Code:** `return bRight ? smp_load_acquire(&node->pRight) : smp_load_acquire(&node->pLeft);`
**Assembly:**
```asm
14a7: 48 8b 45 f8           mov    rax,QWORD PTR [rbp-0x8]
14ab: 48 83 c0 10           add    rax,0x10
14af: 48 8b 00              mov    rax,QWORD PTR [rax]        # MEMORY LOAD (acquire)
```

## Control Flow Analysis
- Branch on `mo == BINTREE_RELAXED` to choose relaxed vs acquire load.
- Branch on `bRight` to choose left vs right child.

## Return Paths
1. **Relaxed right child:** `mo == RELAXED && bRight` → return `pRight`.
   - Expression: `m00 -> RETURN`
2. **Relaxed left child:** `mo == RELAXED && !bRight` → return `pLeft`.
   - Expression: `m01 -> RETURN`
3. **Acquire right child:** `mo != RELAXED && bRight` → return `pRight`.
   - Expression: `m02 -> RETURN`
4. **Acquire left child:** `mo != RELAXED && !bRight` → return `pLeft`.
   - Expression: `m03 -> RETURN`

## Unified Expression
`(m00 | m01 | m02 | m03) -> RETURN`

## Notes
- Acquire loads are used when traversing under concurrent updates.
- Relaxed loads are used for locally validated paths where ordering is not required.
