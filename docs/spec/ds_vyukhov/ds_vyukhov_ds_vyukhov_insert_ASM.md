## ds_vyukhov_insert (0x150b-0x1653)
MPMC enqueue for Vyukhov's bounded queue: claims a slot via CAS on enqueue_pos, writes payload, and advances the sequence.

## Memory Operations

### m00: Load head->buffer pointer for NULL check
**Location:** 0x152d
**C Code:** `if (!head || !head->buffer) return DS_ERROR_INVALID;`
**Assembly:**
```asm
1529: 48 8b 45 b8           mov    rax,QWORD PTR [rbp-0x48]
152d: 48 8b 80 88 00 00 00  mov    rax,QWORD PTR [rax+0x88]   # MEMORY LOAD
1534: 48 85 c0              test   rax,rax
```

### m01: Load enqueue_pos
**Location:** 0x1547
**C Code:** `pos = READ_ONCE(head->enqueue_pos);`
**Assembly:**
```asm
1543: 48 8b 45 b8           mov    rax,QWORD PTR [rbp-0x48]
1547: 48 8b 00              mov    rax,QWORD PTR [rax]        # MEMORY LOAD
154a: 48 89 45 f8           mov    QWORD PTR [rbp-0x8],rax
```

### m02: Load buffer_mask
**Location:** 0x1552
**C Code:** `mask = head->buffer_mask;`
**Assembly:**
```asm
154e: 48 8b 45 b8           mov    rax,QWORD PTR [rbp-0x48]
1552: 48 8b 80 80 00 00 00  mov    rax,QWORD PTR [rax+0x80]   # MEMORY LOAD
1559: 48 89 45 e8           mov    QWORD PTR [rbp-0x18],rax
```

### m03: Load buffer base pointer
**Location:** 0x1566
**C Code:** `cell = &head->buffer[pos & mask];`
**Assembly:**
```asm
1562: 48 8b 45 b8           mov    rax,QWORD PTR [rbp-0x48]
1566: 48 8b 88 88 00 00 00  mov    rcx,QWORD PTR [rax+0x88]   # MEMORY LOAD
```

### m04: Load cell->sequence (ACQUIRE)
**Location:** 0x1590
**C Code:** `__u64 seq = smp_load_acquire(&cell->sequence);`
**Assembly:**
```asm
158c: 48 8b 45 e0           mov    rax,QWORD PTR [rbp-0x20]
1590: 48 8b 00              mov    rax,QWORD PTR [rax]        # MEMORY LOAD (acquire)
1593: 48 89 45 d8           mov    QWORD PTR [rbp-0x28],rax
```

### m05: CAS enqueue_pos to claim slot
**Location:** 0x15c8
**C Code:** `old_pos = arena_atomic_cmpxchg(&head->enqueue_pos, pos, pos + 1, ...);`
**Assembly:**
```asm
15c1: 48 8d 55 c0           lea    rdx,[rbp-0x40]
15c5: 48 8b 02              mov    rax,QWORD PTR [rdx]
15c8: f0 48 0f b1 31        lock cmpxchg QWORD PTR [rcx],rsi  # MEMORY CAS (relaxed)
```

### m06: Store cell->data.key
**Location:** 0x15f1
**C Code:** `cell->data.key = key;`
**Assembly:**
```asm
15e9: 48 8b 45 e0           mov    rax,QWORD PTR [rbp-0x20]
15ed: 48 8b 55 b0           mov    rdx,QWORD PTR [rbp-0x50]
15f1: 48 89 50 08           mov    QWORD PTR [rax+0x8],rdx   # MEMORY STORE
```

### m07: Store cell->data.value
**Location:** 0x15fd
**C Code:** `cell->data.value = value;`
**Assembly:**
```asm
15f5: 48 8b 45 e0           mov    rax,QWORD PTR [rbp-0x20]
15f9: 48 8b 55 a8           mov    rdx,QWORD PTR [rbp-0x58]
15fd: 48 89 50 10           mov    QWORD PTR [rax+0x10],rdx  # MEMORY STORE
```

### m08: Publish cell->sequence (RELEASE)
**Location:** 0x160d
**C Code:** `smp_store_release(&cell->sequence, pos + 1);`
**Assembly:**
```asm
1601: 48 8b 45 f8           mov    rax,QWORD PTR [rbp-0x8]
1605: 48 8d 50 01           lea    rdx,[rax+0x1]
1609: 48 8b 45 e0           mov    rax,QWORD PTR [rbp-0x20]
160d: 48 89 10              mov    QWORD PTR [rax],rdx        # MEMORY STORE (release)
```

### m09: Atomic increment of head->count
**Location:** 0x161a
**C Code:** `arena_atomic_inc(&head->count);`
**Assembly:**
```asm
1610: 48 8b 45 b8           mov    rax,QWORD PTR [rbp-0x48]
1614: 48 05 90 00 00 00     add    rax,0x90
161a: f0 48 83 00 01        lock add QWORD PTR [rax],0x1      # MEMORY ATOMIC ADD
```

### m10: Reload enqueue_pos on retry
**Location:** 0x1638
**C Code:** `pos = READ_ONCE(head->enqueue_pos);`
**Assembly:**
```asm
1634: 48 8b 45 b8           mov    rax,QWORD PTR [rbp-0x48]
1638: 48 8b 00              mov    rax,QWORD PTR [rax]        # MEMORY LOAD
163b: 48 89 45 f8           mov    QWORD PTR [rbp-0x8],rax
```

## Control Flow Analysis
- Bounded retry loop (`retries < DS_VYUKHOV_MAX_RETRIES`).
- `dif == 0`: attempt to claim slot via CAS (m05). On success, write payload and publish sequence.
- `dif < 0`: queue full → return `DS_ERROR_NOMEM`.
- `dif > 0`: reload position and retry (m10).

## Return Paths
1. **Invalid input:** `head == NULL` or `head->buffer == NULL` → `DS_ERROR_INVALID`.
   - Trace: m00 -> RETURN_INVALID
2. **Queue full:** `dif < 0` → `DS_ERROR_NOMEM`.
   - Trace: m01 -> m02 -> m03 -> m04 -> RETURN_NOMEM
3. **Success:** CAS succeeds, payload stored, sequence released, count incremented → `DS_SUCCESS`.
   - Trace: m01 -> m02 -> m03 -> m04 -> m05 -> m06 -> m07 -> m08 -> m09 -> RETURN_SUCCESS
4. **Max retries exceeded:** loop bound hit → `DS_ERROR_BUSY`.
   - Trace: (m01 -> m02 -> m03 -> m04 -> m05? -> m10 -> RETRY)* -> RETURN_BUSY

## Unified Expression
`m00? -> m01 -> m02 -> m03 -> m04 -> (m05 -> m06 -> m07 -> m08 -> m09 -> RETURN_SUCCESS | RETURN_NOMEM | m10 -> RETRY)* -> RETURN_BUSY`

## Notes
- Acquire load of `sequence` and release store back provide producer/consumer ordering.
- CAS on `enqueue_pos` is relaxed; ordering is enforced by sequence release.
- `head->count` is approximate and updated with a relaxed atomic add.
