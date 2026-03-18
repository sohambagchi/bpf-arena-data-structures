# Userspace Allocator Analysis Across Six Skeleton Programs

## Scope

This report covers the six skeleton userspace launchers in `src/`:

- `skeleton_ck_fifo_spsc.c`
- `skeleton_ck_ring_spsc.c`
- `skeleton_ck_stack_upmc.c`
- `skeleton_folly_spsc.c`
- `skeleton_msqueue.c`
- `skeleton_vyukhov.c`

and their corresponding BPF programs plus allocator/data-structure headers:

- `include/libarena_ds.h`
- `include/bpf_arena_common.h`
- `include/ds_ck_fifo_spsc.h`
- `include/ds_ck_ring_spsc.h`
- `include/ds_ck_stack_upmc.h`
- `include/ds_folly_spsc.h`
- `include/ds_msqueue.h`
- `include/ds_vyukhov.h`

## Common Initialization Pattern in All Six Skeletons

All six userspace skeleton launchers do the same allocator bootstrap:

1. Load BPF skeleton (`*_bpf__open_and_load()`), which includes an arena map (`BPF_MAP_TYPE_ARENA`, `BPF_F_MMAPABLE`, `max_entries=1000` pages in each `*.bpf.c`).
2. Call `setup_userspace_allocator()`.
3. Inside setup:
   - Read page size (`sysconf(_SC_PAGESIZE)`).
   - Compute arena byte size from map metadata:
     `arena_bytes = bpf_map__max_entries(skel->maps.arena) * page_size`.
   - Reserve first page:
     `alloc_base = (char *)skel->arena + page_size`.
   - Use remaining bytes:
     `alloc_bytes = arena_bytes - page_size`.
   - Initialize userspace allocator range:
     `bpf_arena_userspace_set_range(alloc_base, alloc_bytes)`.

In short: userspace allocates from the mmap'ed arena region, but intentionally skips page 0.

## Design and Architecture

### Unified API, split implementation

`bpf_arena_alloc()` / `bpf_arena_free()` are a single API surface, but they have two different implementations selected at compile time in `include/libarena_ds.h`:

- `__BPF__` path (kernel/BPF code):
  - Uses `bpf_arena_alloc_pages(&arena, ...)` and `bpf_arena_free_pages(&arena, ...)`.
  - Maintains per-CPU page-fragment state (`page_frag_cur_page[cpu]`, `page_frag_cur_offset[cpu]`).
- non-`__BPF__` path (userspace code):
  - Uses process-local allocator state (`bpf_arena_userspace_*` globals).
  - Allocates from the range set by `bpf_arena_userspace_set_range()`.

So the API is unified, but allocator state/logic is not.

### Shared backing store

Both sides operate on the same shared arena map memory (same virtual arena region visible to BPF and userspace). The userspace side obtains this via `skel->arena` mapping.

### Not one coherent allocator state

There is no single shared allocator metadata structure between kernel and userspace allocators in this repo. Userspace does not call `bpf_arena_alloc_pages()`, and kernel allocator internals are not synchronized with userspace allocator internals.

Practical effect: this is best described as two allocators over one shared arena address space, with a common object layout convention.

## Direct Answers

### 1) Unified allocator or separate allocators?

They are **separate allocators** with a **unified API name**.

So, to your exact framing: **yes, this is two separate allocators operating on a shared arena memory region** (with userspace explicitly configured to skip only the first page).

- Unified API: both sides call `bpf_arena_alloc` / `bpf_arena_free` from DS code.
- Separate implementation/state: kernel and userspace follow different code paths and keep different allocator state.
- Shared memory region: yes, both allocate objects that reside in the same arena map address space.

So this is not one globally coordinated allocator instance; it is two allocator implementations operating on shared arena memory.

### 2) Allocation/free/reuse mechanisms

### (a) How memory is obtained from the system

There are two layers:

1. **Arena backing memory creation**
   - Created as a BPF arena map during skeleton load.
   - Userspace sees it through mmap-able map memory (`skel->arena`).

2. **Page sourcing inside allocators**
   - **Kernel allocator path:** asks kernel arena subsystem for pages via `bpf_arena_alloc_pages()`.
   - **Userspace allocator path:** does not request fresh system pages per allocation; it slices from already mapped arena range set by `bpf_arena_userspace_set_range()`.

### (b) How memory is allocated/freed/reclaimed for application objects

### Allocator-level behavior

Both implementations use a similar page-fragment layout:

- Allocation size rounded to 8 bytes.
- Objects carved from a current page by moving an offset downward.
- Last 8 bytes of each page store an object counter (`obj_cnt`).

#### Kernel (`__BPF__`) allocator behavior

- On refill, allocates a new arena page (`bpf_arena_alloc_pages`).
- On `bpf_arena_free(addr)`:
  - Align `addr` down to page base.
  - Decrement page `obj_cnt`.
  - If `obj_cnt == 0`, return page with `bpf_arena_free_pages`.

This gives page-level reclamation back to the arena subsystem.

#### Userspace allocator behavior

- On refill, advances `bpf_arena_userspace_next_page_off` within configured range.
- Uses one process-global spinlock for allocation path (`atomic_flag`).
- On `bpf_arena_free(addr)`:
  - Align to page base.
  - Decrement page `obj_cnt` if positive.
  - **Does not call `bpf_arena_free_pages` and does not return/recycle pages in allocator state.**

So userspace free updates counters but does not reclaim pages back to allocator pool.

### Data-structure-level reuse/reclamation differences

Allocator behavior is only part of the story. Each DS has its own reuse policy:

| Data structure | Allocation pattern | Free/reuse behavior |
|---|---|---|
| CK FIFO SPSC | Stub at init; allocate node when recycle pool empty | Internal recycle mechanism (`head_snapshot`/`garbage`) reuses dequeued nodes; no `bpf_arena_free` |
| CK Ring SPSC | Allocate slot array once at init | No per-op alloc/free; ring slots reused by index wrap |
| CK Stack UPMC | Allocate one node per insert | Pop does not free node; no allocator-level reclamation in DS code |
| Folly SPSC | Allocate ring array once at init | No per-op alloc/free; fixed ring slots reused |
| Vyukhov MPMC | Allocate ring cell array once at init | No per-op alloc/free; sequence numbers recycle cells |
| MSQueue | Allocate dummy at init and node per insert | `bpf_arena_free` on failed insert rollback and on pop of old dummy head |

For these six skeletons, **MSQueue is the only DS that explicitly calls `bpf_arena_free` in its DS implementation**.

## Net Takeaways

1. API is common, allocator engines are separate.
2. Backing memory is shared arena map memory.
3. Kernel side has page-return reclamation (`bpf_arena_free_pages` on zero page refcount).
4. Userspace side is mostly bump-style over configured range; free decrements counters but does not return/recycle pages in userspace allocator state.
5. Effective reuse is heavily DS-specific (ring/circular structures reuse slots; CK FIFO recycles nodes; CK Stack currently does not free popped nodes; MSQueue does explicit frees).
