## bintree_is_internal (0x14b4-0x14c8)
Checks whether a node is an internal routing node by reading its type field.

## Memory Operations

### m00: Load node->type
**Location:** 0x14c0
**C Code:** `return node->type == BINTREE_NODE_INTERNAL;`
**Assembly:**
```asm
14bc: 48 8b 45 f8           mov    rax,QWORD PTR [rbp-0x8]
14c0: 8b 00                 mov    eax,DWORD PTR [rax]        # MEMORY LOAD
14c2: 85 c0                 test   eax,eax
```

## Control Flow Analysis
- Straight-line code, no branches other than the implicit condition flag for the return value.

## Return Paths
1. **Return type check:** returns `true` if type is `BINTREE_NODE_INTERNAL`, else `false`.
   - Expression: `m00 -> RETURN(bool)`

## Unified Expression
`m00 -> RETURN(bool)`

## Notes
- This helper is used in tree traversal loops to distinguish internal nodes from leaves.
