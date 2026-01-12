## ds_msqueue_insert (0x15d1-0x1662)
Enqueue entry point: allocates a node, initializes payload, and calls `__msqueue_add_node` to link it.

## Memory Operations

### m00: Store new_node->data.key
**Location:** 0x1617
**C Code:** `new_node->data.key = key;`
**Assembly:**
```asm
160f: 48 8b 45 f8           mov    rax,QWORD PTR [rbp-0x8]
1613: 48 8b 55 e0           mov    rdx,QWORD PTR [rbp-0x20]
1617: 48 89 50 08           mov    QWORD PTR [rax+0x8],rdx   # MEMORY STORE
```

### m01: Store new_node->data.value
**Location:** 0x1623
**C Code:** `new_node->data.value = value;`
**Assembly:**
```asm
161b: 48 8b 45 f8           mov    rax,QWORD PTR [rbp-0x8]
161f: 48 8b 55 d8           mov    rdx,QWORD PTR [rbp-0x28]
1623: 48 89 50 10           mov    QWORD PTR [rax+0x10],rdx  # MEMORY STORE
```

### m02: Store new_node->node.next = NULL
**Location:** 0x162b
**C Code:** `new_node->node.next = NULL;`
**Assembly:**
```asm
1627: 48 8b 45 f8           mov    rax,QWORD PTR [rbp-0x8]
162b: 48 c7 00 00 00 00 00  mov    QWORD PTR [rax],0x0       # MEMORY STORE
```

## Control Flow Analysis
- No loops.
- Early returns for NULL `queue` and allocation failures.
- Calls `__msqueue_add_node` to perform the CAS loop (see separate report).

## Return Paths
1. **Invalid queue:** `queue == NULL` → return `DS_ERROR_INVALID`.
   - Trace: RETURN_INVALID
2. **Allocation failure:** `new_node == NULL` → return `DS_ERROR_NOMEM`.
   - Trace: RETURN_NOMEM
3. **Success:** node initialized, `__msqueue_add_node` succeeds → return `DS_SUCCESS`.
   - Trace: m00 -> m01 -> m02 -> CALL(__msqueue_add_node) -> RETURN_SUCCESS
4. **Add-node failure:** `__msqueue_add_node` fails → free node → return `DS_ERROR_INVALID`.
   - Trace: m00 -> m01 -> m02 -> CALL(__msqueue_add_node) -> RETURN_INVALID

## Unified Expression
`(RETURN_INVALID | RETURN_NOMEM | m00 -> m01 -> m02 -> CALL(__msqueue_add_node) -> (RETURN_SUCCESS | RETURN_INVALID))`

## Notes
- Allocation and free operations occur via calls (`usertest_arena_alloc/free`) and may hide additional memory effects.
- Linking logic and atomics are implemented in `__msqueue_add_node`.
