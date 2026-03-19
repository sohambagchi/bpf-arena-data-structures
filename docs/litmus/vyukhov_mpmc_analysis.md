# Vyukhov MPMC Queue Analysis (`ds_vyukhov.h`)

## 1. Insert Operation (`ds_vyukhov_insert`)

### Minimal Memory-Op Version

```c
retry:
    m0: pos = Load_Relaxed(enqueue_pos)
    
    // Calculate cell address
    cell = &buffer[pos & mask]
    
    m1: seq = Load_Acquire(&cell->sequence)
    dif = seq - pos

    if (dif == 0) {
        // Slot available
        m2: success = CAS_Relaxed(&enqueue_pos, pos, pos + 1)
        if (success) {
            // Write Data
            m3: Store(cell->data, data)
            
            // Commit
            m4: Store_Release(&cell->sequence, pos + 1)
            return SUCCESS
        }
        goto retry
    }
    
    if (dif < 0) {
        return FULL
    }
    
    goto retry
```

### Success Ordering
`m0` -> `m1` -> `m2` -> `m3` -> `m4`

### Failure Orderings
1.  **Full**: `m0` -> `m1` -> Return Full
2.  **CAS Fail**: `m0` -> `m1` -> `m2` (Fail) -> Retry

### Permitted Reorderings
-   **m1 (Acquire)**: Ensures we see the consumer's update to `sequence` (indicating the slot is free and data has been read) before we overwrite it.
-   **m2 (CAS Relaxed)**: Updates the global `enqueue_pos`. Being relaxed is fine because `m1` establishes the necessary order for the *slot*. The CAS just claims the index.
-   **m3** (Data write) cannot pass `m4`.
-   **m4 (Release)**: Publishes the data. Consumer will synchronize with this.

## 2. Pop Operation (`ds_vyukhov_pop`)

### Minimal Memory-Op Version

```c
retry:
    m0: pos = Load_Relaxed(dequeue_pos)
    cell = &buffer[pos & mask]
    
    m1: seq = Load_Acquire(&cell->sequence)
    dif = seq - (pos + 1)

    if (dif == 0) {
        // Data available
        m2: success = CAS_Relaxed(&dequeue_pos, pos, pos + 1)
        if (success) {
            // Read Data
            m3: data = Load(cell->data)
            
            // Release slot
            m4: Store_Release(&cell->sequence, pos + mask + 1)
            return SUCCESS
        }
        goto retry
    }
    
    if (dif < 0) {
        return EMPTY
    }
    
    goto retry
```

### Success Ordering
`m0` -> `m1` -> `m2` -> `m3` -> `m4`

### Failure Orderings
1.  **Empty**: `m0` -> `m1` -> Return Empty
2.  **CAS Fail**: `m0` -> `m1` -> `m2` (Fail) -> Retry

### Permitted Reorderings
-   **m1 (Acquire)**: Synchronizes with Producer's `m4`. Ensures we see the data written by Producer.
-   **m3** (Data read) must happen before `m4`.
-   **m4 (Release)**: Signals to producers that the slot is free.

## 3. Interleaving Analysis

### ABA Protection
The `sequence` field acts as a version counter.
-   **Producer** sets `sequence = pos + 1`.
-   **Consumer** sets `sequence = pos + mask + 1`.
-   Each lap, the sequence increments by `capacity`.
-   `pos` also increments.
-   `dif` calculation `seq - pos` remains invariant for valid states.
-   This inherently protects against ABA on the slot index because `pos` is strictly monotonic (until wrap-around at 2^64, which is effectively infinite).

### False Sharing
-   `enqueue_pos` and `dequeue_pos` are padded to 64 bytes (cache line).
-   This minimizes cache thrashing between producers and consumers.

### Correctness
The algorithm relies on the property that `sequence` is only updated by the thread that "owns" the slot (via `pos` check or CAS claim).
-   Producer owns slot `i` if `seq[i] == pos`.
-   Consumer owns slot `i` if `seq[i] == pos + 1`.
-   Ownership transfer is atomic via `Store_Release`.

## 4. Conclusion
The implementation correctly follows Vyukhov's MPMC bounded queue design.
-   **Relaxed CAS** on positions is correct because the `sequence` provides the data synchronization.
-   **Acquire/Release** on `sequence` is crucial and correctly implemented.
