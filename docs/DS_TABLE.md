# Synchronization Annotation Comparison: C11 vs LKMM

Each data structure ships two implementations in the same header, selected
at compile time by `#ifdef __BPF__`:

| Variant | Guard | Atomic primitives |
|---------|-------|-------------------|
| **C** (`_c`) | `#ifndef __BPF__` | `arena_atomic_load/store/cmpxchg` with explicit `ARENA_ACQUIRE`, `ARENA_RELEASE`, `ARENA_RELAXED` |
| **LKMM** (`_lkmm`) | `#ifdef __BPF__` | `READ_ONCE`, `WRITE_ONCE`, `smp_load_acquire`, `smp_store_release`, `barrier()` |

The LKMM column drops explicit memory-ordering annotations wherever LKMM's
**preserved program order (ppo)** -- address, data, or control dependencies --
provides the required ordering natively on the BPF/kernel side.

---

## Comparison Table

> Legend for the annotation columns:
> - `acq` = acquire load (`arena_atomic_load(..., ARENA_ACQUIRE)` or `smp_load_acquire`)
> - `rel` = release store/CAS (`arena_atomic_store(..., ARENA_RELEASE)` or `smp_store_release`)
> - `rlx` = relaxed (`ARENA_RELAXED`, `READ_ONCE`, `WRITE_ONCE`)
> - `rel-CAS` = release-success CAS
> - `acq-CAS` = acquire-success CAS
> - `rlx-CAS` = relaxed-success CAS
> - `mb` = `arena_memory_barrier()` (full fence)
> - `barrier()` = compiler-only fence

| Data Structure | Prod C | Cons C | Prod LKMM | Cons LKMM | ppo used |
|---|---|---|---|---|---|
| **MS Queue** (MPMC linked-list) `ds_msqueue.h` | `acq` tail, `acq` next, `rel-CAS` next/tail | `acq` head, `acq` tail, `acq` next, `acq-CAS` head, `rel-CAS` tail | `rlx` tail, `rlx` next, `rel-CAS` next/tail | `rlx` head, `rlx` tail, `rlx` next, `rlx-CAS` head, `rel-CAS` tail | **addr** (head->next->data, tail->next), **ctrl** (CAS retry loop) |
| **Vyukov MPMC** (bounded array ring) `ds_vyukhov.h` | `rlx` enqueue_pos, `acq` seq, `rlx-CAS` pos, `rel` seq | `rlx` dequeue_pos, `acq` seq, `rlx-CAS` pos, `rel` seq | `rlx` enqueue_pos, `acq` seq, `rlx-CAS` pos, `rel` seq | `rlx` dequeue_pos, `acq` seq, `rlx-CAS` pos, `rel` seq | **none** -- sequence-number protocol requires explicit acq/rel; no pointer-chasing to exploit |
| **Folly SPSC** (fixed-size ring) `ds_folly_spsc.h` | `rlx` write_idx, `acq` read_idx, `rel` write_idx | `rlx` read_idx, `acq` write_idx, `rel` read_idx | `rlx` write_idx, `rlx` read_idx, `rel` write_idx | `rlx` read_idx, `acq` write_idx, `rel` read_idx | **ctrl** (producer branch `next_record != current_read` gates data writes) |
| **CK FIFO SPSC** (linked-list stub) `ds_ck_fifo_spsc.h` | `rlx` tail, `rel` tail->next | `rlx` head, `acq` head->next, `rel` head | `rlx` tail, `rel` tail->next | `rlx` head, `rlx` head->next, `rlx` head (WRITE_ONCE) | **addr** (head->next->entry->value), **data** (kv writes before release of next) |
| **CK Ring SPSC** (power-of-2 ring) `ds_ck_ring_spsc.h` | `acq` c_head, `rlx` p_tail, `rel` p_tail | `rlx` c_head, `acq` p_tail, `mb`, `rel` c_head | `rlx` c_head, `rlx` p_tail, `rel` p_tail | `rlx` c_head, `acq` p_tail, `rel` c_head | **ctrl** (producer full-check branch gates writes), consumer `mb` dropped (acq on p_tail suffices) |
| **CK Stack UPMC** (linked-list stack) `ds_ck_stack_upmc.h` | `rlx` head, `rel-CAS` head | `acq` head, `rlx` next, `acq-CAS` head | `rlx` head, `rel-CAS` head | `rlx` head, `rlx` next, `rlx-CAS` head | **addr** (head->data), **ctrl** (CAS retry loop) |
| **io_uring Ring** (SPSC ring) `ds_io_uring.h` | `rlx` prod.tail, `acq` cons.head, `rel` prod.tail | `rlx` cons.head, `acq` prod.tail, `rel` cons.head | `rlx` prod.tail, `acq` cons.head, `rel` prod.tail | `rlx` cons.head, `acq` prod.tail, `rel` cons.head | **none** -- faithful io_uring barrier model; all acq/rel retained |
| **kcov Buffer** (flat append array) `ds_kcov.h` | `rlx` area[0], `rlx` data, `barrier()` | `rlx` area[0], `rlx` data, `barrier()` | `rlx` area[0], `rlx` data, `barrier()` | `rlx` area[0], `rlx` data, `barrier()` | **none** -- compiler barriers only; identical annotations on both sides |

---

## Reading the Table

**Where Prod/Cons LKMM differs from Prod/Cons C, a ppo mechanism is covering
the dropped annotation.** Three patterns appear:

### Address dependency (addr)
Pointer-chasing reads create a hardware-enforced ordering chain.  When
the CPU must dereference pointer A to obtain pointer B, it cannot
speculate the load of B before A completes.  This covers:
- MS Queue: `head -> head->next -> next->data`, `tail -> tail->next`
- CK FIFO SPSC: `head -> head->next -> entry -> entry->value`
- CK Stack UPMC: `head -> head->data`

### Control dependency (ctrl)
A conditional branch whose outcome depends on a prior read prevents
subsequent **stores** from being reordered above the branch.  This covers:
- Folly SPSC producer: `if (next_record != current_read)` gates payload writes
- CK Ring SPSC producer: `if (next == consumer)` full-check gates slot writes
- MS Queue / CK Stack UPMC: CAS retry loops (branch on CAS result)

### Data dependency (data)
When a store's **value** is computed from a prior load, hardware preserves
the ordering.  This covers:
- CK FIFO SPSC: key/value writes flow into the node that is published
  via `smp_store_release(&tail->next, entry)`

---

## Structures Where C == LKMM (no ppo exploitation)

| Data Structure | Reason |
|---|---|
| Vyukov MPMC | Sequence-number protocol inherently requires acq/rel on the sequence field; array indexing has no pointer-chasing chain to exploit |
| io_uring Ring | Faithful port of Linux io_uring's barrier model; deliberately retains all `smp_load_acquire` / `smp_store_release` |
| kcov Buffer | No hardware fences on either side -- single-writer model uses only `READ_ONCE`/`WRITE_ONCE` + `barrier()` everywhere |

---

## Annotation Counts

Static count of every annotation site in the producer (insert/enqueue) and
consumer (pop/dequeue) hot path of each data structure.  Naked loads and
stores through arena pointers (e.g. `slot->key = key`) are counted as
**rlx** -- they carry no hardware fence but are still shared-memory
accesses.  Zeros are omitted.

| Data Structure | Path | acq | rel | rlx | rel-CAS | acq-CAS | rlx-CAS | mb | barrier |
|---|---|---|---|---|---|---|---|---|---|
| MS Queue | Prod C | 2 | | 4 | 3 | | | | |
| | Prod LKMM | | | 6 | 3 | | | | |
| | Cons C | 4 | | 3 | 1 | 1 | | | |
| | Cons LKMM | | | 7 | 1 | | 1 | | |
| Vyukov MPMC | Prod C | 1 | 1 | 5 | | | 1 | | |
| | Prod LKMM | 1 | 1 | 5 | | | 1 | | |
| | Cons C | 1 | 1 | 5 | | | 1 | | |
| | Cons LKMM | 1 | 1 | 5 | | | 1 | | |
| Folly SPSC | Prod C | 1 | 1 | 3 | | | | | |
| | Prod LKMM | | 1 | 4 | | | | | |
| | Cons C | 1 | 1 | 3 | | | | | |
| | Cons LKMM | 1 | 1 | 3 | | | | | |
| CK FIFO SPSC | Prod C | | 1 | 6 | | | | | |
| | Prod LKMM | | 1 | 6 | | | | | |
| | Cons C | 1 | 1 | 4 | | | | | |
| | Cons LKMM | | | 6 | | | | | |
| CK Ring SPSC | Prod C | 1 | 1 | 3 | | | | | |
| | Prod LKMM | | 1 | 4 | | | | | |
| | Cons C | 1 | 1 | 3 | | | | 1 | |
| | Cons LKMM | 1 | 1 | 3 | | | | | |
| CK Stack UPMC | Prod C | | | 5 | 1 | | | | |
| | Prod LKMM | | | 5 | 1 | | | | |
| | Cons C | 1 | | 4 | | 1 | | | |
| | Cons LKMM | | | 5 | | | 1 | | |
| io_uring Ring | Prod C | 1 | 1 | 5 | | | | | |
| | Prod LKMM | 1 | 1 | 5 | | | | | |
| | Cons C | 1 | 1 | 3 | | | | | |
| | Cons LKMM | 1 | 1 | 3 | | | | | |
| kcov Buffer | Prod C | | | 6 | | | | | 1 |
| | Prod LKMM | | | 6 | | | | | 1 |
| | Cons C | | | 5 | | | | | 1 |
| | Cons LKMM | | | 5 | | | | | 1 |

### Totals

| | acq | rel | rlx | rel-CAS | acq-CAS | rlx-CAS | mb | barrier | **total** |
|---|---|---|---|---|---|---|---|---|---|
| **All C** | 16 | 10 | 67 | 5 | 2 | 2 | 1 | 2 | **105** |
| **All LKMM** | 6 | 9 | 78 | 5 | | 4 | | 2 | **104** |
| **delta (C − LKMM)** | **+10** | **+1** | **−11** | | **+2** | **−2** | **+1** | | **+1** |

The LKMM side eliminates **10 acquire loads**, **2 acquire-CAS**,
**1 release store**, and **1 full memory barrier** across the eight
structures -- **14 hardware fences** in total.  Thirteen become relaxed
operations (11 extra `rlx` + 2 extra `rlx-CAS`); the CK Ring SPSC
memory barrier is simply dropped (redundant with the acquire on `p_tail`).
