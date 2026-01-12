## bintree_help_insert (0x1515-0x1614)
Completes an insert by swinging the parent child pointer and clearing the parent's update flag.

## Memory Operations

### m00: Load op->insert->pLeaf
**Location:** 0x1528
**C Code:** `pLeaf = (struct ds_bintree_tree_node *)op->insert->pLeaf;`
**Assembly:**
```asm
1521: 48 8b 45 c8           mov    rax,QWORD PTR [rbp-0x38]
1525: 48 8b 00              mov    rax,QWORD PTR [rax]        # load op->insert
1528: 48 8b 40 18           mov    rax,QWORD PTR [rax+0x18]   # MEMORY LOAD
```

### m01: Load op->insert->bRightLeaf
**Location:** 0x1537
**C Code:** `if (op->insert->bRightLeaf) { ... }`
**Assembly:**
```asm
1530: 48 8b 45 c8           mov    rax,QWORD PTR [rbp-0x38]
1534: 48 8b 00              mov    rax,QWORD PTR [rax]        # load op->insert
1537: 0f b6 40 20           movzx  eax,BYTE PTR [rax+0x20]   # MEMORY LOAD
```

### m02: CAS parent->pRight to new internal (RELEASE)
**Location:** 0x156b
**C Code:** `arena_atomic_cmpxchg(&op->insert->pParent->pRight, pLeaf, op->insert->pNew, ...);`
**Assembly:**
```asm
1564: 48 8d 55 e0           lea    rdx,[rbp-0x20]
1568: 48 8b 02              mov    rax,QWORD PTR [rdx]
156b: f0 48 0f b1 31        lock cmpxchg QWORD PTR [rcx],rsi  # MEMORY CAS (release)
```

### m03: CAS parent->pLeft to new internal (RELEASE)
**Location:** 0x15a8
**C Code:** `arena_atomic_cmpxchg(&op->insert->pParent->pLeft, pLeaf, op->insert->pNew, ...);`
**Assembly:**
```asm
15a1: 48 8d 55 d8           lea    rdx,[rbp-0x28]
15a5: 48 8b 02              mov    rax,QWORD PTR [rdx]
15a8: f0 48 0f b1 31        lock cmpxchg QWORD PTR [rcx],rsi  # MEMORY CAS (release)
```

### m04: CAS parent->m_pUpdate to CLEAN (RELEASE)
**Location:** 0x1603
**C Code:** `arena_atomic_cmpxchg(&op->insert->pParent->m_pUpdate, expected, clean, ...);`
**Assembly:**
```asm
15f8: 48 8d 55 d0           lea    rdx,[rbp-0x30]
15fc: 48 8b 02              mov    rax,QWORD PTR [rdx]
1603: f0 48 0f b1 0e        lock cmpxchg QWORD PTR [rsi],rcx  # MEMORY CAS (release)
```

## Control Flow Analysis
- Branch on `bRightLeaf` to choose which child pointer to CAS (m02 vs m03).
- After child update attempt, unflag parent update field via CAS (m04).

## Return Paths
1. **Right-child path:** `bRightLeaf == true` → CAS right child, then clear update.
   - Expression: `m00 -> m01 -> m02 -> m04 -> RETURN`
2. **Left-child path:** `bRightLeaf == false` → CAS left child, then clear update.
   - Expression: `m00 -> m01 -> m03 -> m04 -> RETURN`

## Unified Expression
`m00 -> m01 -> (m02 | m03) -> m04 -> RETURN`

## Notes
- CAS operations are release-ordered to publish the new subtree before clearing the update flag.
- This helper reads multiple fields from `op->insert` to assemble CAS arguments (pointer loads omitted for brevity).
