## ds_vyukhov_pop (0x1654-0x17be)
MPMC dequeue for Vyukhov's bounded queue: claims a slot via CAS on dequeue_pos, reads payload, and advances the sequence for the next lap.

## Memory Operations

### m00: Load head->buffer pointer for NULL check
**Location:** 0x1672
**C Code:** `if (!head || !head->buffer || !data) return DS_ERROR_INVALID;`
**Assembly:**
```asm
166e: 48 8b 45 b8           mov    rax,QWORD PTR [rbp-0x48]
1672: 48 8b 80 88 00 00 00  mov    rax,QWORD PTR [rax+0x88]   # MEMORY LOAD
1679: 48 85 c0              test   rax,rax
```

### m01: Load dequeue_pos
**Location:** 0x1697
**C Code:** `pos = READ_ONCE(head->dequeue_pos);`
**Assembly:**
```asm
168f: 48 8b 45 b8           mov    rax,QWORD PTR [rbp-0x48]
1693: 48 83 c0 40           add    rax,0x40
1697: 48 8b 00              mov    rax,QWORD PTR [rax]        # MEMORY LOAD
169a: 48 89 45 f8           mov    QWORD PTR [rbp-0x8],rax
```

### m02: Load buffer_mask
**Location:** 0x16a2
**C Code:** `mask = head->buffer_mask;`
**Assembly:**
```asm
169e: 48 8b 45 b8           mov    rax,QWORD PTR [rbp-0x48]
16a2: 48 8b 80 80 00 00 00  mov    rax,QWORD PTR [rax+0x80]   # MEMORY LOAD
16a9: 48 89 45 e8           mov    QWORD PTR [rbp-0x18],rax
```

### m03: Load buffer base pointer
**Location:** 0x16b6
**C Code:** `cell = &head->buffer[pos & mask];`
**Assembly:**
```asm
16b2: 48 8b 45 b8           mov    rax,QWORD PTR [rbp-0x48]
16b6: 48 8b 88 88 00 00 00  mov    rcx,QWORD PTR [rax+0x88]   # MEMORY LOAD
```

### m04: Load cell->sequence (ACQUIRE)
**Location:** 0x16e0
**C Code:** `__u64 seq = smp_load_acquire(&cell->sequence);`
**Assembly:**
```asm
16dc: 48 8b 45 e0           mov    rax,QWORD PTR [rbp-0x20]
16e0: 48 8b 00              mov    rax,QWORD PTR [rax]        # MEMORY LOAD (acquire)
16e3: 48 89 45 d8           mov    QWORD PTR [rbp-0x28],rax
```

### m05: CAS dequeue_pos to claim slot
**Location:** 0x1721
**C Code:** `old_pos = arena_atomic_cmpxchg(&head->dequeue_pos, pos, pos + 1, ...);`
**Assembly:**
```asm
171a: 48 8d 55 c0           lea    rdx,[rbp-0x40]
171e: 48 8b 02              mov    rax,QWORD PTR [rdx]
1721: f0 48 0f b1 31        lock cmpxchg QWORD PTR [rcx],rsi  # MEMORY CAS (relaxed)
```

### m06: Load cell->data.key
**Location:** 0x1746
**C Code:** `data->key = cell->data.key;`
**Assembly:**
```asm
1742: 48 8b 45 e0           mov    rax,QWORD PTR [rbp-0x20]
1746: 48 8b 50 08           mov    rdx,QWORD PTR [rax+0x8]    # MEMORY LOAD
```

### m07: Store output key
**Location:** 0x174e
**C Code:** `data->key = cell->data.key;`
**Assembly:**
```asm
174a: 48 8b 45 b0           mov    rax,QWORD PTR [rbp-0x50]
174e: 48 89 10              mov    QWORD PTR [rax],rdx        # MEMORY STORE
```

### m08: Load cell->data.value
**Location:** 0x1755
**C Code:** `data->value = cell->data.value;`
**Assembly:**
```asm
1751: 48 8b 45 e0           mov    rax,QWORD PTR [rbp-0x20]
1755: 48 8b 50 10           mov    rdx,QWORD PTR [rax+0x10]   # MEMORY LOAD
```

### m09: Store output value
**Location:** 0x175d
**C Code:** `data->value = cell->data.value;`
**Assembly:**
```asm
1759: 48 8b 45 b0           mov    rax,QWORD PTR [rbp-0x50]
175d: 48 89 50 08           mov    QWORD PTR [rax+0x8],rdx    # MEMORY STORE
```

### m10: Publish cell->sequence for next lap (RELEASE)
**Location:** 0x1774
**C Code:** `smp_store_release(&cell->sequence, pos + mask + 1);`
**Assembly:**
```asm
1761: 48 8b 55 f8           mov    rdx,QWORD PTR [rbp-0x8]
1765: 48 8b 45 e8           mov    rax,QWORD PTR [rbp-0x18]
1769: 48 01 d0              add    rax,rdx
176c: 48 8d 50 01           lea    rdx,[rax+0x1]
1770: 48 8b 45 e0           mov    rax,QWORD PTR [rbp-0x20]
1774: 48 89 10              mov    QWORD PTR [rax],rdx        # MEMORY STORE (release)
```

### m11: Atomic decrement of head->count
**Location:** 0x1781
**C Code:** `arena_atomic_dec(&head->count);`
**Assembly:**
```asm
1777: 48 8b 45 b8           mov    rax,QWORD PTR [rbp-0x48]
177b: 48 05 90 00 00 00     add    rax,0x90
1781: f0 48 83 28 01        lock sub QWORD PTR [rax],0x1      # MEMORY ATOMIC SUB
```

### m12: Reload dequeue_pos on retry
**Location:** 0x17a3
**C Code:** `pos = READ_ONCE(head->dequeue_pos);`
**Assembly:**
```asm
179b: 48 8b 45 b8           mov    rax,QWORD PTR [rbp-0x48]
179f: 48 83 c0 40           add    rax,0x40
17a3: 48 8b 00              mov    rax,QWORD PTR [rax]        # MEMORY LOAD
17a6: 48 89 45 f8           mov    QWORD PTR [rbp-0x8],rax
```

## Control Flow Analysis
- Bounded retry loop (`retries < DS_VYUKHOV_MAX_RETRIES`).
- `dif == 0`: attempt to claim slot via CAS (m05). On success, read payload and publish sequence.
- `dif < 0`: queue empty → return `DS_ERROR_NOT_FOUND`.
- `dif > 0`: reload position and retry (m12).

## Return Paths
1. **Invalid input:** `head == NULL`, `head->buffer == NULL`, or `data == NULL` → `DS_ERROR_INVALID`.
   - Trace: m00 -> RETURN_INVALID
2. **Queue empty:** `dif < 0` → `DS_ERROR_NOT_FOUND`.
   - Trace: m01 -> m02 -> m03 -> m04 -> RETURN_EMPTY
3. **Success:** CAS succeeds, payload read, sequence released, count decremented → `DS_SUCCESS`.
   - Trace: m01 -> m02 -> m03 -> m04 -> m05 -> m06 -> m07 -> m08 -> m09 -> m10 -> m11 -> RETURN_SUCCESS
4. **Max retries exceeded:** loop bound hit → `DS_ERROR_BUSY`.
   - Trace: (m01 -> m02 -> m03 -> m04 -> m05? -> m12 -> RETRY)* -> RETURN_BUSY

## Unified Expression
`m00? -> m01 -> m02 -> m03 -> m04 -> (m05 -> m06 -> m07 -> m08 -> m09 -> m10 -> m11 -> RETURN_SUCCESS | RETURN_EMPTY | m12 -> RETRY)* -> RETURN_BUSY`

## Notes
- Acquire load of `sequence` and release store back provide producer/consumer ordering.
- CAS on `dequeue_pos` is relaxed; ordering is enforced by the sequence store.
- `head->count` is approximate and updated with a relaxed atomic sub.
