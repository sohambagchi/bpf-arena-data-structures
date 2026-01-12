## ds_spsc_insert (0x1483-0x1510)
Fixed-size ring-buffer enqueue for the SPSC queue: loads producer/consumer indices, writes payload, and publishes the write index.

## Memory Operations

### m00: Load current write index (RELAXED)
**Location:** 0x1497
**C Code:** `__u32 current_write = READ_ONCE(head->write_idx.idx);`
**Assembly:**
```asm
1493: 48 8b 45 d8           mov    rax,QWORD PTR [rbp-0x28]
1497: 8b 00                 mov    eax,DWORD PTR [rax]        # MEMORY LOAD
1499: 89 45 f8              mov    DWORD PTR [rbp-0x8],eax
```

### m01: Load queue size
**Location:** 0x14a9
**C Code:** `if (next_record >= head->size) { ... }`
**Assembly:**
```asm
14a5: 48 8b 45 d8           mov    rax,QWORD PTR [rbp-0x28]
14a9: 8b 40 44              mov    eax,DWORD PTR [rax+0x44]    # MEMORY LOAD
14ac: 39 45 fc              cmp    DWORD PTR [rbp-0x4],eax
```

### m02: Load read index (ACQUIRE)
**Location:** 0x14c0
**C Code:** `__u32 current_read = smp_load_acquire(&head->read_idx.idx);`
**Assembly:**
```asm
14b8: 48 8b 45 d8           mov    rax,QWORD PTR [rbp-0x28]
14bc: 48 83 c0 40           add    rax,0x40
14c0: 8b 00                 mov    eax,DWORD PTR [rax]        # MEMORY LOAD (acquire)
14c2: 89 45 f4              mov    DWORD PTR [rbp-0xc],eax
```

### m03: Load records base pointer
**Location:** 0x14d1
**C Code:** `struct ds_kv *node = &head->records[current_write];`
**Assembly:**
```asm
14cd: 48 8b 45 d8           mov    rax,QWORD PTR [rbp-0x28]
14d1: 48 8b 40 48           mov    rax,QWORD PTR [rax+0x48]    # MEMORY LOAD
14d5: 8b 55 f8              mov    edx,DWORD PTR [rbp-0x8]
```

### m04: Store payload key
**Location:** 0x14eb
**C Code:** `node->key = key;`
**Assembly:**
```asm
14e3: 48 8b 45 e8           mov    rax,QWORD PTR [rbp-0x18]
14e7: 48 8b 55 d0           mov    rdx,QWORD PTR [rbp-0x30]
14eb: 48 89 10              mov    QWORD PTR [rax],rdx        # MEMORY STORE
```

### m05: Store payload value
**Location:** 0x14f6
**C Code:** `node->value = value;`
**Assembly:**
```asm
14ee: 48 8b 45 e8           mov    rax,QWORD PTR [rbp-0x18]
14f2: 48 8b 55 c8           mov    rdx,QWORD PTR [rbp-0x38]
14f6: 48 89 50 08           mov    QWORD PTR [rax+0x8],rdx    # MEMORY STORE
```

### m06: Publish write index (RELEASE)
**Location:** 0x1501
**C Code:** `smp_store_release(&head->write_idx.idx, next_record);`
**Assembly:**
```asm
14fa: 48 8b 45 d8           mov    rax,QWORD PTR [rbp-0x28]
14fe: 8b 55 fc              mov    edx,DWORD PTR [rbp-0x4]
1501: 89 10                 mov    DWORD PTR [rax],edx        # MEMORY STORE (release)
```

## Control Flow Analysis
- No loops.
- Conditional wrap: if `next_record >= head->size`, reset to 0.
- Conditional full check: if `next_record == current_read`, return `DS_ERROR_FULL`.

## Return Paths
1. **Queue full:** `next_record == current_read` → return `DS_ERROR_FULL` (`eax = 0xfffffff9`).
   - Trace: m00 -> m01 -> m02 -> RETURN
   - Expression: `m00 -> m01 -> m02 -> RETURN_FULL`
2. **Enqueue success:** space available, payload written, index published → return `DS_SUCCESS`.
   - Trace: m00 -> m01 -> m02 -> m03 -> m04 -> m05 -> m06 -> RETURN
   - Expression: `m00 -> m01 -> m02 -> m03 -> m04 -> m05 -> m06 -> RETURN_SUCCESS`

## Unified Expression
`m00 -> m01 -> m02 -> (m03 -> m04 -> m05 -> m06 -> RETURN_SUCCESS | RETURN_FULL)`

## Notes
- Acquire on `read_idx` pairs with consumer's release store to ensure payload visibility.
- Release store on `write_idx` publishes the written slot to the consumer.
- No explicit backoff or retry; producer returns `DS_ERROR_FULL` when full.
