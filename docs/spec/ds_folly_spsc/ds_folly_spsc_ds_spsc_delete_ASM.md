## ds_spsc_delete (0x1511-0x15ac)
FIFO dequeue for the SPSC queue: checks for empty, copies payload, and publishes the new read index.

## Memory Operations

### m00: Load current read index (RELAXED)
**Location:** 0x1525
**C Code:** `__u32 current_read = READ_ONCE(head->read_idx.idx);`
**Assembly:**
```asm
151d: 48 8b 45 d8           mov    rax,QWORD PTR [rbp-0x28]
1521: 48 83 c0 40           add    rax,0x40
1525: 8b 00                 mov    eax,DWORD PTR [rax]        # MEMORY LOAD
1527: 89 45 f8              mov    DWORD PTR [rbp-0x8],eax
```

### m01: Load current write index (ACQUIRE)
**Location:** 0x152e
**C Code:** `__u32 current_write = smp_load_acquire(&head->write_idx.idx);`
**Assembly:**
```asm
152a: 48 8b 45 d8           mov    rax,QWORD PTR [rbp-0x28]
152e: 8b 00                 mov    eax,DWORD PTR [rax]        # MEMORY LOAD (acquire)
1530: 89 45 f4              mov    DWORD PTR [rbp-0xc],eax
```

### m02: Load records base pointer
**Location:** 0x1546
**C Code:** `struct ds_kv *node = &head->records[current_read];`
**Assembly:**
```asm
1542: 48 8b 45 d8           mov    rax,QWORD PTR [rbp-0x28]
1546: 48 8b 40 48           mov    rax,QWORD PTR [rax+0x48]    # MEMORY LOAD
154a: 8b 55 f8              mov    edx,DWORD PTR [rbp-0x8]
```

### m03: Load payload key
**Location:** 0x1563
**C Code:** `data->key = node->key;`
**Assembly:**
```asm
155f: 48 8b 45 e8           mov    rax,QWORD PTR [rbp-0x18]
1563: 48 8b 10              mov    rdx,QWORD PTR [rax]        # MEMORY LOAD
1566: 48 8b 45 d0           mov    rax,QWORD PTR [rbp-0x30]
```

### m04: Store output key
**Location:** 0x156a
**C Code:** `data->key = node->key;`
**Assembly:**
```asm
1566: 48 8b 45 d0           mov    rax,QWORD PTR [rbp-0x30]
156a: 48 89 10              mov    QWORD PTR [rax],rdx        # MEMORY STORE
```

### m05: Load payload value
**Location:** 0x1571
**C Code:** `data->value = node->value;`
**Assembly:**
```asm
156d: 48 8b 45 e8           mov    rax,QWORD PTR [rbp-0x18]
1571: 48 8b 50 08           mov    rdx,QWORD PTR [rax+0x8]    # MEMORY LOAD
1575: 48 8b 45 d0           mov    rax,QWORD PTR [rbp-0x30]
```

### m06: Store output value
**Location:** 0x1579
**C Code:** `data->value = node->value;`
**Assembly:**
```asm
1575: 48 8b 45 d0           mov    rax,QWORD PTR [rbp-0x30]
1579: 48 89 50 08           mov    QWORD PTR [rax+0x8],rdx    # MEMORY STORE
```

### m07: Load queue size
**Location:** 0x158a
**C Code:** `if (next_record >= head->size) { ... }`
**Assembly:**
```asm
1586: 48 8b 45 d8           mov    rax,QWORD PTR [rbp-0x28]
158a: 8b 40 44              mov    eax,DWORD PTR [rax+0x44]    # MEMORY LOAD
158d: 39 45 fc              cmp    DWORD PTR [rbp-0x4],eax
```

### m08: Publish read index (RELEASE)
**Location:** 0x15a4
**C Code:** `smp_store_release(&head->read_idx.idx, next_record);`
**Assembly:**
```asm
1599: 48 8b 45 d8           mov    rax,QWORD PTR [rbp-0x28]
159d: 48 8d 50 40           lea    rdx,[rax+0x40]
15a1: 8b 45 fc              mov    eax,DWORD PTR [rbp-0x4]
15a4: 89 02                 mov    DWORD PTR [rdx],eax        # MEMORY STORE (release)
```

## Control Flow Analysis
- No loops.
- Empty check: if `current_read == current_write`, return `DS_ERROR_NOT_FOUND`.
- Optional data copy when `data != NULL`.
- Conditional wrap for `next_record` when hitting `head->size`.

## Return Paths
1. **Queue empty:** `current_read == current_write` → return `DS_ERROR_NOT_FOUND` (`eax = 0xffffffff`).
   - Trace: m00 -> m01 -> RETURN
   - Expression: `m00 -> m01 -> RETURN_EMPTY`
2. **Dequeue success:** payload copied (if requested), read index updated → return `DS_SUCCESS`.
   - Trace: m00 -> m01 -> m02 -> (m03 -> m04 -> m05 -> m06)? -> m07 -> m08 -> RETURN
   - Expression: `m00 -> m01 -> m02 -> (m03 -> m04 -> m05 -> m06)? -> m07 -> m08 -> RETURN_SUCCESS`

## Unified Expression
`m00 -> m01 -> (RETURN_EMPTY | m02 -> (m03 -> m04 -> m05 -> m06)? -> m07 -> m08 -> RETURN_SUCCESS)`

## Notes
- Acquire on `write_idx` pairs with producer's release store to ensure payload visibility.
- Release store on `read_idx` signals slot availability to the producer.
- Data copy is conditional on `data != NULL`.
