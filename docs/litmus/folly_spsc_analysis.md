# Folly SPSC Queue Analysis (`ds_folly_spsc.h`)

## 1. Insert Operation (`ds_spsc_insert`)

### Minimal Memory-Op Version

```c
m0: current_write = Load_Relaxed(write_idx)
    next_record = current_write + 1
    if (next_record >= size) next_record = 0

m1: current_read = Load_Acquire(read_idx)

    if (next_record == current_read) return FULL

m2: Store_Relaxed(records[current_write], data)
m3: Store_Release(write_idx, next_record)
```

### Success Ordering
`m0` -> `m1` -> `m2` -> `m3`

### Failure Ordering
`m0` -> `m1` -> Fail (Full)

### Permitted Reorderings
-   `m0` and `m1`? `m1` is Acquire. `m0` cannot move after `m1`.
-   `m2` and `m3`? `m3` is Release. `m2` cannot move after `m3`.
-   `m1` and `m2`? `m1` (Acquire) ensures we see the consumer's update to `read_idx` (making space) before we write to that space (`m2`). Essential.

## 2. Pop/Delete Operation (`ds_spsc_delete`)

### Minimal Memory-Op Version

```c
m0: current_read = Load_Relaxed(read_idx)
m1: current_write = Load_Acquire(write_idx)

    if (current_read == current_write) return EMPTY

m2: data = Load_Relaxed(records[current_read])
    next_record = current_read + 1
    if (next_record >= size) next_record = 0

m3: Store_Release(read_idx, next_record)
```

### Success Ordering
`m0` -> `m1` -> `m2` -> `m3`

### Failure Ordering
`m0` -> `m1` -> Fail (Empty)

### Permitted Reorderings
-   `m1` (Acquire) ensures we see producer's write (`write_idx`) and thus the data associated with it before we read `m2`.
-   `m3` (Release) ensures we read the data `m2` before we mark the slot as free (`read_idx` update).

## 3. Interleaving Analysis

### Single Producer, Single Consumer
-   **Producer** updates `write_idx` (Release).
-   **Consumer** loads `write_idx` (Acquire). Synchronizes with Producer. Sees all data writes before `write_idx` update.
-   **Consumer** updates `read_idx` (Release).
-   **Producer** loads `read_idx` (Acquire). Synchronizes with Consumer. Sees that slot is free.

### Multi-Producer/Consumer (Violation of Contract)
-   The code does NOT use CAS or locks.
-   If two producers enter `ds_spsc_insert`:
    -   Both read same `current_write`.
    -   Both write to `records[current_write]`. Data Race on data slot.
    -   Both write same `next_record` to `write_idx`.
    -   **Result**: Data corruption, lost inserts.
-   **Constraint**: Must strictly enforce SPSC.

## 4. Conclusion
The implementation correctly implements SPSC semantics using Acquire/Release.
-   `write_idx` and `read_idx` are on separate cache lines (padded structs in `ds_spsc_queue_head`? No, the struct definition uses `__attribute__((aligned(CACHE_LINE_SIZE)))` on the *structs* wrapping the indices).
    -   `write_idx` struct aligned.
    -   `read_idx` struct aligned.
    -   This prevents false sharing.

**Correctness**: The memory ordering is correct for SPSC.
