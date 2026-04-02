# Why LKMM Functions are Faster

## Summary

Every data structure in this project has two variants: `_lkmm` (compiled for BPF) and `_c` (compiled for userspace). The `_lkmm` variants are faster. This document explains why, with evidence from the compiled assembly in `build/*.S`.

The short version: on x86-TSO, `memory_order_acquire` and `memory_order_release` on plain loads and stores compile to the same `MOV` instructions as `memory_order_relaxed`. The hardware's Total Store Order already provides acquire/release semantics for free. So the `_c` variants don't actually pay for their acquire/release annotations on x86 — the cost difference between `_lkmm` and `_c` on this architecture comes down to exactly one case: `ds_ck_ring_spsc_delete_c` emits a redundant full memory barrier (`lock or QWORD PTR [rsp],0x0`). Everything else compiles identically.

The real performance gap is structural: `_lkmm` functions run inside the BPF VM (kernel context, triggered by `inode_create` and uprobe), while `_c` functions run in userspace pthread context. The BPF execution path has lower overhead per operation because it's already in kernel space — no syscall boundary, no scheduler interference from the relay loop, and `smp_load_acquire`/`smp_store_release` in BPF expand to nothing more than `READ_ONCE`/`WRITE_ONCE` plus a compiler barrier (`asm volatile("" ::: "memory")`).

## What the assembly actually shows

The `.S` files in `build/` contain the compiled x86-64 assembly for both BPF skeleton programs (`skeleton_*.S`, ~80K lines, mostly embedded BPF ELF blobs) and userspace test programs (`usertest_*.S`, ~1K lines, readable x86 assembly of the `_c` variants).

### Every acquire load is a plain MOV

From `usertest_folly_spsc.S`, the `ARENA_ACQUIRE` load of `read_idx`:

```asm
# arena_atomic_load(&head->read_idx.idx, ARENA_ACQUIRE)
14c0: 48 8b 45 d8          mov    rax,QWORD PTR [rbp-0x28]
14c4: 48 83 c0 40          add    rax,0x40
14c8: 8b 00                mov    eax,DWORD PTR [rax]        # plain MOV
```

From `usertest_ck_ring_spsc.S`, the `ARENA_ACQUIRE` load of `c_head`:

```asm
# arena_atomic_load(&head->c_head, ARENA_ACQUIRE)
1503: 48 8b 45 d8          mov    rax,QWORD PTR [rbp-0x28]
1507: 48 83 c0 08          add    rax,0x8
150b: 8b 00                mov    eax,DWORD PTR [rax]        # plain MOV
```

From `usertest_vyukhov.S`, the `ARENA_ACQUIRE` load of `cell->sequence`:

```asm
# arena_atomic_load(&cell->sequence, ARENA_ACQUIRE)
158f: 48 8b 45 e0          mov    rax,QWORD PTR [rbp-0x20]
1593: 48 8b 00             mov    rax,QWORD PTR [rax]        # plain MOV
```

From `usertest_ck_stack_upmc.S`, the `ARENA_ACQUIRE` load of `stack->head`:

```asm
# arena_atomic_load(&stack->head, ARENA_ACQUIRE)
1454: 48 8b 45 d8          mov    rax,QWORD PTR [rbp-0x28]
1458: 48 8b 00             mov    rax,QWORD PTR [rax]        # plain MOV
```

### Every release store is a plain MOV

From `usertest_folly_spsc.S`:

```asm
# arena_atomic_store(&head->write_idx.idx, next_record, ARENA_RELEASE)
1505: 48 8b 45 d8          mov    rax,QWORD PTR [rbp-0x28]
1509: 8b 55 fc             mov    edx,DWORD PTR [rbp-0x4]
150c: 89 10                mov    DWORD PTR [rax],edx        # plain MOV
```

From `usertest_vyukhov.S`:

```asm
# arena_atomic_store(&cell->sequence, pos + 1, ARENA_RELEASE)
1604: 48 8b 45 f8          mov    rax,QWORD PTR [rbp-0x8]
1608: 48 8d 50 01          lea    rdx,[rax+0x1]
160c: 48 8b 45 e0          mov    rax,QWORD PTR [rbp-0x20]
1610: 48 89 10             mov    QWORD PTR [rax],rdx        # plain MOV
```

### CAS is `lock cmpxchg` regardless of ordering

From `usertest_ck_stack_upmc.S`, the `ARENA_ACQUIRE` CAS (used in `_c` pop):

```asm
# arena_atomic_cmpxchg(&stack->head, head, next, ARENA_ACQUIRE, ARENA_RELAXED)
1483: f0 48 0f b1 31       lock cmpxchg QWORD PTR [rcx],rsi  # same lock cmpxchg
```

From `usertest_msqueue.S`, the `ARENA_RELEASE` CAS:

```asm
# arena_atomic_cmpxchg(&tail->node.next, next, &new_node->node, ARENA_RELEASE, ARENA_RELAXED)
1546: f0 48 0f b1 31       lock cmpxchg QWORD PTR [rcx],rsi  # same lock cmpxchg
```

On x86, `lock cmpxchg` is always a full memory barrier. The ARENA_ACQUIRE vs ARENA_RELAXED distinction in the source code has no effect on the generated instruction.

### The one exception: `arena_memory_barrier()`

`ds_ck_ring_spsc_delete_c` has this between the acquire load and the slot reads:

```asm
# arena_memory_barrier()
15dc: f0 48 83 0c 24 00    lock or QWORD PTR [rsp],0x0      # full SEQ_CST fence
```

This compiles from `__atomic_thread_fence(memory_order_seq_cst)`. It's redundant — the preceding acquire load already prevents reordering with subsequent reads on x86. The `_lkmm` variant omits it. This is the only instruction-level difference between `_lkmm` and `_c` that the assembly reveals on this architecture.

## Where the speed difference actually comes from

If the x86 assembly is nearly identical, why are LKMM operations faster?

### Execution context, not instruction selection

The benchmark measures four categories:
- **LKMM producer**: BPF program in `inode_create` hook (kernel context)
- **User consumer**: Userspace relay thread polling the KU queue
- **User producer**: Userspace relay thread pushing to the UK queue
- **LKMM consumer**: BPF program in uprobe handler (kernel context)

The LKMM producer and consumer run in kernel BPF context. The user producer and consumer run in a userspace pthread. The speed difference is between these execution contexts, not between the memory ordering primitives.

In BPF context:
- `smp_load_acquire` = `READ_ONCE` + compiler barrier (`asm volatile("" ::: "memory")`)
- `smp_store_release` = compiler barrier + `WRITE_ONCE`
- `READ_ONCE` = `*(volatile typeof(x) *)&(x)` — a volatile access with no hardware barrier

```c
// BPF side (include/bpf_arena_common.h)
#define READ_ONCE(x) (*(volatile typeof(x) *)&(x))
#define WRITE_ONCE(x, val) ((*(volatile typeof(x) *) &(x)) = (val))
#define barrier() asm volatile("" ::: "memory")
#define smp_load_acquire(p) ({ uintptr_t __p = (uintptr_t)READ_ONCE(*p); barrier(); (typeof(*p))__p; })
#define smp_store_release(p, v) do { barrier(); WRITE_ONCE(*p, v); } while (0)
```

In userspace context:

```c
// Userspace side (include/bpf_arena_common.h)
#define smp_store_release(p, v) __atomic_store_n((p), (v), memory_order_release)
#define smp_load_acquire(p) __atomic_load_n((p), memory_order_acquire)
```

On x86, both expand to the same `MOV` instruction. But the BPF program runs inside a kernel hook with no scheduling overhead, while the userspace thread contends with the relay loop's busy-polling and kernel-user transitions.

### On a weakly-ordered architecture (ARM64, RISC-V), the picture changes

The assembly-level equivalence is an x86-specific property. On ARM64:
- `memory_order_acquire` emits `LDAR` (load-acquire) instead of `LDR`
- `memory_order_release` emits `STLR` (store-release) instead of `STR`
- A `lock or` fence equivalent becomes a `DMB` (data memory barrier)

On those architectures, the `_lkmm` variants' use of address/control dependencies instead of acquire loads would produce genuinely different (and cheaper) instruction sequences.

## The LKMM advantage: portability of zero-cost ordering

Even though the x86 assembly is identical today, the `_lkmm` source code encodes weaker ordering requirements. This matters for:

1. **Correctness documentation**: The `_lkmm` variants make explicit which ordering is provided by data flow (address dependency) vs which requires a barrier. The `_c` variants use ARENA_ACQUIRE as a blunt instrument that is correct but hides the actual dependency structure.

2. **Portability to weak architectures**: If this code runs on ARM64 BPF, the `_lkmm` variants will use fewer barrier instructions than the `_c` variants. Address dependencies are free on ARM64 too — `LDR` through a dependent pointer is ordered without `LDAR`.

3. **Compiler optimization latitude**: C11 does not recognize address or control dependencies. A C11 compiler is allowed to break a dependency chain through constant folding, redundant load elimination, or value prediction. `READ_ONCE` prevents this. By encoding the dependency explicitly with `READ_ONCE` instead of relying on `ARENA_ACQUIRE`, the `_lkmm` code is robust against compiler transformations that would silently break the `_c` code's unstated dependency assumptions.

## Per-data-structure LKMM techniques

### Address dependency (MSQueue, CK FIFO SPSC, CK Stack UPMC)

These linked-list structures rely on pointer chasing. The `_lkmm` variants use `READ_ONCE(ptr)` → dereference `ptr` chains where the `_c` variants use `ARENA_ACQUIRE` loads:

| Function | LKMM | C11 |
|---|---|---|
| MSQueue pop | `READ_ONCE(head->node.next)` (address dep from head) | `arena_atomic_load(&head->node.next, ARENA_ACQUIRE)` |
| MSQueue enqueue | `READ_ONCE(tail->node.next)` (address dep from tail) | `arena_atomic_load(&tail->node.next, ARENA_ACQUIRE)` |
| CK FIFO dequeue | `READ_ONCE(head->next)` (address dep from head) | `arena_atomic_load(&head->next, ARENA_ACQUIRE)` |
| CK Stack pop CAS | `ARENA_RELAXED` (address dep provides visibility) | `ARENA_ACQUIRE` (CAS provides visibility) |

On x86, both columns compile to `MOV`. On ARM64, the LKMM column would be `LDR` and the C11 column would be `LDAR`.

### Control dependency (Folly SPSC, CK Ring SPSC, IO_URING)

These ring buffers read the "other side's" index to check full/empty, then store data inside a conditional branch. The branch creates a control dependency that orders the stores:

| Function | LKMM | C11 |
|---|---|---|
| Folly SPSC insert | `READ_ONCE(head->read_idx.idx)` → branch → store | `arena_atomic_load(&head->read_idx.idx, ARENA_ACQUIRE)` → store |
| CK Ring SPSC insert | `READ_ONCE(head->c_head)` → branch → store | `arena_atomic_load(&head->c_head, ARENA_ACQUIRE)` → store |
| IO_URING insert | Same pattern as CK Ring SPSC | Same pattern |

Again, identical on x86. Different on ARM64.

### Redundant barrier elimination (CK Ring SPSC delete)

`ds_ck_ring_spsc_delete_c` calls `arena_memory_barrier()` — which is `__atomic_thread_fence(memory_order_seq_cst)` — between the acquire load of `p_tail` and the slot reads. This compiles to `lock or QWORD PTR [rsp],0x0` on x86. The fence is redundant because the acquire load already prevents subsequent loads from being reordered above it. The `_lkmm` variant omits it.

This is the only place in the entire codebase where the `_c` variant generates a different (and more expensive) instruction sequence on x86.

### KCOV

KCOV uses no acquire/release on its hot path. Only `READ_ONCE`/`WRITE_ONCE` + `barrier()`. This is safe because the buffer has a single-writer guarantee: the BPF producer is the only writer to the write index, and the BPF consumer is the only writer to the read index.

### Vyukhov

Both `_lkmm` and `_c` use acquire/release on the sequence counter. No dependency optimization is possible because the sequence value is compared arithmetically, not dereferenced as a pointer. On x86, the compiled code is identical.

## Benchmark data

4-core machine, 1 touch worker, ~10s runs, ~890 events each. All latencies are Avg-OK in nanoseconds.

### Run 0

| Structure | LKMM Prod (ns) | LKMM Cons (ns) |
|---|---:|---:|
| CK Ring SPSC | 37 | 24 |
| KCOV | 46 | 22 |
| IO_URING | 56 | 22 |
| CK FIFO SPSC | 63 | 24 |
| Folly SPSC | 77 | 22 |
| MSQueue | 122 | 118 |
| Vyukhov | 125 | 32 |
| CK Stack UPMC | 181 | 36 |

### Run 1

| Structure | LKMM Prod (ns) | LKMM Cons (ns) |
|---|---:|---:|
| CK FIFO SPSC | 45 | 23 |
| CK Ring SPSC | 53 | 23 |
| Folly SPSC | 60 | 22 |
| IO_URING | 84 | 21 |
| MSQueue | 122 | 203 |
| Vyukhov | 131 | 29 |
| KCOV | 134 | 22 |
| CK Stack UPMC | 193 | 37 |

### Run 2

| Structure | LKMM Prod (ns) | LKMM Cons (ns) |
|---|---:|---:|
| CK FIFO SPSC | 50 | 23 |
| CK Ring SPSC | 73 | 24 |
| Folly SPSC | 81 | 21 |
| KCOV | 99 | 24 |
| MSQueue | 144 | 129 |
| CK Stack UPMC | 186 | 36 |
| Vyukhov | 195 | 28 |
| IO_URING | 218 | 21 |

## Summary table

| Structure | LKMM Technique | x86 Instruction Difference | ARM64 Difference (expected) |
|---|---|---|---|
| MSQueue | Address dependency | None | LDR vs LDAR |
| CK FIFO SPSC | Address dependency | None | LDR vs LDAR |
| CK Stack UPMC | Address dep + relaxed CAS | None (`lock cmpxchg` either way) | LDR vs LDAR |
| Folly SPSC | Control dependency | None | LDR vs LDAR |
| CK Ring SPSC | Control dep + barrier removal | `lock or` removed from delete | LDR vs LDAR + DMB removed |
| IO_URING | Control dependency | None | LDR vs LDAR |
| KCOV | No barriers on hot path | None | No barrier vs barrier |
| Vyukhov | Same acquire/release | None | None |
