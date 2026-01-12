## __msqueue_add_node (0x14a7-0x15d0)
Internal enqueue helper for the Michael-Scott queue: links a new node and advances the tail with CAS, retrying under contention.

## Memory Operations

### m00: Load queue->tail (ACQUIRE)
**Location:** 0x14ce
**C Code:** `tail = smp_load_acquire(&queue->tail);`
**Assembly:**
```asm
14c6: 48 8b 45 b0           mov    rax,QWORD PTR [rbp-0x50]
14ca: 48 83 c0 08           add    rax,0x8
14ce: 48 8b 00              mov    rax,QWORD PTR [rax]        # MEMORY LOAD (acquire)
```

### m01: Load tail->node.next (ACQUIRE)
**Location:** 0x14d9
**C Code:** `next = smp_load_acquire(&tail->node.next);`
**Assembly:**
```asm
14d5: 48 8b 45 f8           mov    rax,QWORD PTR [rbp-0x8]
14d9: 48 8b 00              mov    rax,QWORD PTR [rax]        # MEMORY LOAD (acquire)
14dc: 48 89 45 e8           mov    QWORD PTR [rbp-0x18],rax
```

### m02: CAS queue->tail to help advance lagging tail
**Location:** 0x1512
**C Code:** `arena_atomic_cmpxchg(&queue->tail, tail, next_elem, ARENA_RELEASE, ARENA_RELAXED);`
**Assembly:**
```asm
1507: 48 8d 70 08           lea    rsi,[rax+0x8]
150f: 48 8b 02              mov    rax,QWORD PTR [rdx]
1512: f0 48 0f b1 0e        lock cmpxchg QWORD PTR [rsi],rcx  # MEMORY CAS (release)
```

### m03: CAS tail->node.next to link new node
**Location:** 0x1546
**C Code:** `arena_atomic_cmpxchg(&tail->node.next, next, &new_node->node, ARENA_RELEASE, ARENA_RELAXED);`
**Assembly:**
```asm
1543: 48 8b 02              mov    rax,QWORD PTR [rdx]
1546: f0 48 0f b1 31        lock cmpxchg QWORD PTR [rcx],rsi  # MEMORY CAS (release)
```

### m04: Atomic increment of queue->count
**Location:** 0x158a
**C Code:** `arena_atomic_inc(&queue->count);`
**Assembly:**
```asm
1582: 48 8b 45 b0           mov    rax,QWORD PTR [rbp-0x50]
1586: 48 83 c0 10           add    rax,0x10
158a: f0 48 83 00 01        lock add QWORD PTR [rax],0x1      # MEMORY ATOMIC ADD
```

### m05: CAS queue->tail to new node after link
**Location:** 0x15aa
**C Code:** `arena_atomic_cmpxchg(&queue->tail, tail, new_node, ARENA_RELEASE, ARENA_RELAXED);`
**Assembly:**
```asm
159f: 48 8d 70 08           lea    rsi,[rax+0x8]
15a7: 48 8b 02              mov    rax,QWORD PTR [rdx]
15aa: f0 48 0f b1 0e        lock cmpxchg QWORD PTR [rsi],rcx  # MEMORY CAS (release)
```

## Control Flow Analysis
- Loop with bounded retries (`retry_count < max_retries`).
- If `next != NULL`, help advance tail (m02) and retry.
- If `next == NULL`, attempt to link new node (m03); on failure, retry.
- After successful link, increment count (m04) and attempt to swing tail (m05).

## Return Paths
1. **Max retries exceeded:** `retry_count >= max_retries` → return `DS_ERROR_INVALID` (`eax = 0xfffffffc`).
   - Trace: `(m00 -> m01 -> (m02 | m03) -> RETRY)* -> RETURN_INVALID`
2. **Success:** node linked, count incremented, tail update attempted → return `DS_SUCCESS`.
   - Trace: `(m00 -> m01 -> (m02 -> RETRY)* -> m03) -> m04 -> m05 -> RETURN_SUCCESS`

## Unified Expression
`(m00 -> m01 -> (m02 -> RETRY | m03 -> (RETRY | BREAK)))* -> (RETURN_INVALID | m04 -> m05 -> RETURN_SUCCESS)`

## Notes
- Both tail-advance and link operations use CAS with release semantics.
- The count update is purely statistical and uses a relaxed atomic add.
- Calls to `arena_atomic_cmpxchg` encapsulate CAS; failure paths retry without helping deeper recursion.
