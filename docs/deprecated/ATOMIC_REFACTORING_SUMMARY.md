# Atomic Operations Refactoring Summary

## Overview
Successfully migrated the BPF Arena data structures codebase from legacy GCC `__sync` built-ins to modern C11 `__atomic` primitives with fine-grained memory ordering control.

## Changes Made

### 1. Header File (libarena_ds.h)

#### New Atomic API
Replaced the old synchronization primitives with C11 atomics:

```c
/* Memory ordering constants */
#define ARENA_RELAXED __ATOMIC_RELAXED
#define ARENA_ACQUIRE __ATOMIC_ACQUIRE
#define ARENA_RELEASE __ATOMIC_RELEASE
#define ARENA_ACQ_REL __ATOMIC_ACQ_REL
#define ARENA_SEQ_CST __ATOMIC_SEQ_CST
```

#### Updated Macros
- **arena_atomic_cmpxchg**: Now takes explicit success/failure memory orders
  - Old: `arena_atomic_cmpxchg(ptr, old, new)`
  - New: `arena_atomic_cmpxchg(ptr, old, new, success_mo, failure_mo)`

- **arena_atomic_exchange**: Now takes explicit memory order
  - Old: `arena_atomic_exchange(ptr, new)`
  - New: `arena_atomic_exchange(ptr, new, mo)`

- **arena_atomic_add/sub/and/or**: Now take explicit memory order
  - Old: `arena_atomic_add(ptr, val)`
  - New: `arena_atomic_add(ptr, val, mo)`

- **New operations**: `arena_atomic_load` and `arena_atomic_store`

- **Convenience wrappers**: `arena_atomic_inc` and `arena_atomic_dec` use `ARENA_RELAXED` for statistics

#### Bump Allocator
- Updated to use `arena_atomic_add(&alloc_offset, size, ARENA_RELAXED)` for offset bump
- Memory order: `RELAXED` (no synchronization needed for bump allocation)

### 2. Vyukhov MPMC Queue (ds_vyukhov.h)

#### Enqueue Operation
- **Position CAS**: `ARENA_ACQUIRE` on success, `ARENA_RELAXED` on failure
  - Rationale: Need to acquire ownership of the slot and see previous operations
- **Sequence Exchange**: `ARENA_RELEASE`
  - Rationale: Release data to consumers
- **Count Update**: Uses `arena_atomic_inc` (RELAXED)

#### Dequeue Operation
- **Position CAS**: `ARENA_ACQUIRE` on success, `ARENA_RELAXED` on failure
  - Rationale: Need to acquire data written by producer
- **Sequence Exchange**: `ARENA_RELEASE`
  - Rationale: Release slot back to producers for next lap
- **Count Update**: Uses `arena_atomic_dec` (RELAXED)

### 3. Michael-Scott Queue (ds_msqueue.h)

#### Enqueue Operation
- **Next Pointer CAS**: `ARENA_ACQUIRE` on success, `ARENA_RELAXED` on failure
  - Rationale: Acquire tail state to ensure we see the correct next pointer
- **Tail Advancement (helping)**: `ARENA_RELEASE` on success, `ARENA_RELAXED` on failure
  - Rationale: Release new tail pointer so other threads see the update
- **Final Tail Swing**: `ARENA_RELEASE` on success, `ARENA_RELAXED` on failure
  - Rationale: Publish the newly linked node as the new tail
- **Count Update**: Uses `arena_atomic_inc` (RELAXED)

#### Dequeue Operation
- **Tail Helping**: `ARENA_RELEASE` on success, `ARENA_RELAXED` on failure
  - Rationale: Help advance tail to maintain queue invariants
- **Head CAS**: `ARENA_ACQUIRE` on success, `ARENA_RELAXED` on failure
  - Rationale: Acquire data from the dequeued node
- **Count Update**: Uses `arena_atomic_dec` (RELAXED)

### 4. Doubly-Linked List (ds_list.h)

No changes required - this data structure doesn't use atomic operations in the current implementation (uses `WRITE_ONCE` for list manipulation).

## Memory Ordering Rationale

### Design Principles Applied

1. **Lock Acquisition Pattern** (CAS loops):
   - Success: `ARENA_ACQUIRE` - ensures we see data protected by the "lock"
   - Failure: `ARENA_RELAXED` - no ordering needed if we fail, just retry

2. **Lock Release Pattern** (publishing data/pointers):
   - `ARENA_RELEASE` - ensures all previous writes are visible before the release

3. **Statistics/Counters**:
   - `ARENA_RELAXED` - exact global visibility order rarely matters for approximate counts

4. **Reference Counting** (if needed in the future):
   - Increment: `ARENA_RELAXED`
   - Decrement: `ARENA_ACQ_REL` (in case we need to destroy on zero)

### Benefits

1. **Performance**: Relaxed operations allow more aggressive compiler optimizations
2. **Correctness**: Explicit orderings make synchronization guarantees clear
3. **Portability**: C11 atomics are more portable than GCC built-ins
4. **Maintainability**: Memory ordering intentions are documented in the code

## Testing Recommendations

1. **Functional Testing**: Verify all existing tests still pass
2. **Stress Testing**: Run under high contention to verify lock-free progress
3. **Memory Model Testing**: Use ThreadSanitizer or similar tools to detect races
4. **Performance Testing**: Benchmark to measure the impact of relaxed orderings

## Future Improvements

1. Consider using `ARENA_CONSUME` where appropriate (if supported)
2. Add more fine-grained memory barriers where needed
3. Audit any remaining `SEQ_CST` operations for potential relaxation
4. Document memory ordering assumptions in algorithm comments

## Compatibility

- **BPF side**: Uses C11 `__atomic_*` built-ins (supported by clang)
- **Userspace**: Uses same C11 atomics for consistency
- **Backward compatibility**: Old API is completely replaced (this is a breaking change)
