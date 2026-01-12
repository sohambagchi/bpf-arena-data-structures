## bintree_set_infinite_key (0x14c9-0x1514)
Updates the node's infinite-key flag bits with release semantics.

## Memory Operations

### m00: Load current node->type
**Location:** 0x14d8
**C Code:** `unsigned int nFlags = READ_ONCE(node->type);`
**Assembly:**
```asm
14d4: 48 8b 45 e8           mov    rax,QWORD PTR [rbp-0x18]
14d8: 8b 00                 mov    eax,DWORD PTR [rax]        # MEMORY LOAD
14da: 89 45 fc              mov    DWORD PTR [rbp-0x4],eax
```

### m01: Store updated node->type (RELEASE)
**Location:** 0x1510
**C Code:** `smp_store_release(&node->type, nFlags);`
**Assembly:**
```asm
1509: 48 8b 45 e8           mov    rax,QWORD PTR [rbp-0x18]
150d: 8b 55 fc              mov    edx,DWORD PTR [rbp-0x4]
1510: 89 10                 mov    DWORD PTR [rax],edx        # MEMORY STORE (release)
```

## Control Flow Analysis
- Switch over `nInf` to set or clear infinite-key flags.
- No loops; single final release store.

## Return Paths
1. **Return void:** always completes after updating flags.
   - Expression: `m00 -> m01 -> RETURN`

## Unified Expression
`m00 -> m01 -> RETURN`

## Notes
- Release store ensures updated type/flag bits become visible before subsequent operations that depend on them.
