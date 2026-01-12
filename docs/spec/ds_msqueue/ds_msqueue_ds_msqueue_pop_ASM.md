## ds_msqueue_pop (0x1663-0x17cc)
Dequeue entry point: reads head/tail/next, handles lagging tail, and swings head to remove the dummy node.

## Memory Operations

### m00: Load queue->head (ACQUIRE)
**Location:** 0x16a1
**C Code:** `head = smp_load_acquire(&queue->head);`
**Assembly:**
```asm
169d: 48 8b 45 a8           mov    rax,QWORD PTR [rbp-0x58]
16a1: 48 8b 00              mov    rax,QWORD PTR [rax]        # MEMORY LOAD (acquire)
16a4: 48 89 45 f0           mov    QWORD PTR [rbp-0x10],rax
```

### m01: Load queue->tail (ACQUIRE)
**Location:** 0x16b0
**C Code:** `tail = smp_load_acquire(&queue->tail);`
**Assembly:**
```asm
16a8: 48 8b 45 a8           mov    rax,QWORD PTR [rbp-0x58]
16ac: 48 83 c0 08           add    rax,0x8
16b0: 48 8b 00              mov    rax,QWORD PTR [rax]        # MEMORY LOAD (acquire)
```

### m02: Load head->node.next (ACQUIRE)
**Location:** 0x16bb
**C Code:** `next = smp_load_acquire(&head->node.next);`
**Assembly:**
```asm
16b7: 48 8b 45 f0           mov    rax,QWORD PTR [rbp-0x10]
16bb: 48 8b 00              mov    rax,QWORD PTR [rax]        # MEMORY LOAD (acquire)
16be: 48 89 45 e0           mov    QWORD PTR [rbp-0x20],rax
```

### m03: Reload queue->head for consistency check
**Location:** 0x16c6
**C Code:** `if (smp_load_acquire(&queue->head) != head) { ... }`
**Assembly:**
```asm
16c2: 48 8b 45 a8           mov    rax,QWORD PTR [rbp-0x58]
16c6: 48 8b 00              mov    rax,QWORD PTR [rax]        # MEMORY LOAD (acquire)
16c9: 48 39 45 f0           cmp    QWORD PTR [rbp-0x10],rax
```

### m04: CAS queue->tail when tail is lagging
**Location:** 0x171e
**C Code:** `arena_atomic_cmpxchg(&queue->tail, tail, next_elem_tail, ARENA_RELEASE, ARENA_RELAXED);`
**Assembly:**
```asm
1713: 48 8d 70 08           lea    rsi,[rax+0x8]
171b: 48 8b 02              mov    rax,QWORD PTR [rdx]
171e: f0 48 0f b1 0e        lock cmpxchg QWORD PTR [rsi],rcx  # MEMORY CAS (release)
```

### m05: Load next element key
**Location:** 0x174a
**C Code:** `data->key = next_elem->data.key;`
**Assembly:**
```asm
1746: 48 8b 45 d0           mov    rax,QWORD PTR [rbp-0x30]
174a: 48 8b 50 08           mov    rdx,QWORD PTR [rax+0x8]    # MEMORY LOAD
```

### m06: Store output key
**Location:** 0x1752
**C Code:** `data->key = next_elem->data.key;`
**Assembly:**
```asm
174e: 48 8b 45 a0           mov    rax,QWORD PTR [rbp-0x60]
1752: 48 89 10              mov    QWORD PTR [rax],rdx        # MEMORY STORE
```

### m07: Load next element value
**Location:** 0x1759
**C Code:** `data->value = next_elem->data.value;`
**Assembly:**
```asm
1755: 48 8b 45 d0           mov    rax,QWORD PTR [rbp-0x30]
1759: 48 8b 50 10           mov    rdx,QWORD PTR [rax+0x10]   # MEMORY LOAD
```

### m08: Store output value
**Location:** 0x1761
**C Code:** `data->value = next_elem->data.value;`
**Assembly:**
```asm
175d: 48 8b 45 a0           mov    rax,QWORD PTR [rbp-0x60]
1761: 48 89 50 08           mov    QWORD PTR [rax+0x8],rdx    # MEMORY STORE
```

### m09: CAS queue->head to advance dummy
**Location:** 0x177c
**C Code:** `arena_atomic_cmpxchg(&queue->head, head, next_elem, ARENA_ACQUIRE, ARENA_RELAXED);`
**Assembly:**
```asm
1779: 48 8b 02              mov    rax,QWORD PTR [rdx]
177c: f0 48 0f b1 31        lock cmpxchg QWORD PTR [rcx],rsi  # MEMORY CAS (acquire)
```

### m10: Atomic decrement of queue->count
**Location:** 0x17a9
**C Code:** `arena_atomic_dec(&queue->count);`
**Assembly:**
```asm
17a1: 48 8b 45 a8           mov    rax,QWORD PTR [rbp-0x58]
17a5: 48 83 c0 10           add    rax,0x10
17a9: f0 48 83 28 01        lock sub QWORD PTR [rax],0x1      # MEMORY ATOMIC SUB
```

## Control Flow Analysis
- Loop with bounded retries (`retry_count < max_retries`).
- If head changes mid-iteration, retry.
- If `next == NULL`, return empty.
- If `head == tail`, help advance tail with CAS (m04) and retry.
- Otherwise, read payload, CAS head to next (m09), free old head, decrement count.

## Return Paths
1. **Invalid args:** `queue == NULL || data == NULL` → return `DS_ERROR_INVALID`.
   - Trace: RETURN_INVALID
2. **Queue empty:** `next == NULL` → return `DS_ERROR_NOT_FOUND` (`eax = 0xffffffff`).
   - Trace: m00 -> m01 -> m02 -> RETURN_EMPTY
3. **Successful dequeue:** head advanced via CAS → return `DS_SUCCESS`.
   - Trace: m00 -> m01 -> m02 -> m03 -> (m04 -> RETRY)* -> m05 -> m06 -> m07 -> m08 -> m09 -> m10 -> RETURN_SUCCESS
4. **Max retries exceeded:** loop exhausted → return `DS_ERROR_INVALID`.
   - Trace: (m00 -> m01 -> m02 -> m03 -> (m04 | m09-fail) -> RETRY)* -> RETURN_INVALID

## Unified Expression
`(RETURN_INVALID | m00 -> m01 -> m02 -> (RETURN_EMPTY | (m03 -> (m04 -> RETRY)*) -> m05 -> m06 -> m07 -> m08 -> m09 -> m10 -> RETURN_SUCCESS | RETRY*) -> RETURN_INVALID)`

## Notes
- Acquire loads on head/tail/next enforce ordering before payload reads.
- Tail-help CAS uses release semantics; head-advance CAS uses acquire semantics.
- Old head node is freed after successful CAS (call to `usertest_arena_free`).
