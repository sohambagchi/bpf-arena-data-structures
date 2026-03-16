# Two-Lane Relay Architecture Report (MSQueue)

## Purpose

This report documents the integrated two-lane relay architecture now used by `src/skeleton_msqueue.c` and `src/skeleton_msqueue.bpf.c`.

It also gives a concrete, file-oriented blueprint to apply the same pattern to:

- `skeleton_vyukhov`
- `skeleton_folly_spsc`
- `skeleton_ck_fifo_spsc`
- `skeleton_ck_ring_spsc`
- `skeleton_ck_stack_upmc`

The write-up assumes the reader has only seen `src/` and `include/`.

---

## Architecture Summary

The program is split into two independent queue instances of the same data structure type:

- `*KU` lane: kernel producer -> userspace consumer
- `*UK` lane: userspace producer -> kernel consumer

For MSQueue this is:

- `global_ds_queue_ku`
- `global_ds_queue_uk`

Both are declared in BPF global arena state in `src/skeleton_msqueue.bpf.c`.

### End-to-end flow

1. Main thread loads BPF object and manually attaches programs.
2. Main thread spawns one relay thread.
3. `inode_create` runs in kernel and enqueues into the KU queue.
4. Relay thread busy-polls KU, pops one element, and immediately enqueues it into UK.
5. On Ctrl+C, main thread triggers a userspace symbol that has a BPF uprobe attached; the uprobe pops from UK in kernel context.

---

## What Is Implemented for MSQueue

### BPF side (`src/skeleton_msqueue.bpf.c`)

- Two queue instances exist in arena globals (`*_ku`, `*_uk`).
- `lsm.s/inode_create` only produces to KU.
- A new `SEC("uprobe.s")` program (`bpf_msq_consume`) consumes from UK.
- Producer and consumer counters are split:
  - `total_kernel_prod_ops`, `total_kernel_prod_failures`
  - `total_kernel_consume_ops`, `total_kernel_consume_failures`, `total_kernel_consumed`

### Userspace side (`src/skeleton_msqueue.c`)

- A dedicated trigger symbol exists:
  - `msq_kernel_consume_trigger()`
- Main thread manually attaches:
  - LSM program (`bpf_program__attach_lsm`)
  - uprobe program (`bpf_program__attach_uprobe_opts`)
- Main thread starts one relay worker thread.
- Relay worker:
  - waits for KU initialization
  - initializes UK from userspace if needed
  - loops `pop(KU) -> insert(UK)`
- Ctrl+C path:
  - stop relay thread
  - repeatedly call `msq_kernel_consume_trigger()` to drain UK via kernel uprobe consumer

### Why allocator bootstrap is required

The relay thread inserts into an arena-backed structure from userspace, so userspace must set an allocation range first:

- compute arena bytes from `bpf_map__max_entries(...) * page_size`
- reserve first page
- call `bpf_arena_userspace_set_range(...)`

Without this, user-side `*_insert_c` calls that allocate arena nodes can fail.

---

## Reusable Integration Pattern

Use this pattern to convert any current `skeleton_<ds>` from single-lane (kernel producer -> user consumer) to two-lane relay.

### 1) BPF file changes (`src/skeleton_<ds>.bpf.c`)

1. Replace single global head with two globals (`*_ku`, `*_uk`).
2. Keep `lsm_inode_create` as producer only, writing to KU.
3. Add `SEC("uprobe.s")` consumer program that pops from UK.
4. Split counters into producer and consumer groups.
5. Keep lazy init for KU in kernel path.

### 2) Userspace file changes (`src/skeleton_<ds>.c`)

1. Add noinline trigger symbol for uprobe attachment.
2. Add allocator setup (`bpf_arena_userspace_set_range`).
3. Do manual link attachment (LSM + uprobe) instead of relying only on skeleton auto-attach.
4. Spawn one relay thread.
5. In relay thread, run busy loop:
   - pop from KU
   - insert to UK
6. On shutdown:
   - set stop flag
   - join relay thread
   - invoke trigger function enough times to drain UK via kernel uprobe consumer

### 3) Verification and stats

At exit, verify both queues/heads (`KU`, `UK`) and print split producer/relay/consumer counters.

---

## Function Mapping for Target Data Structures

Use the exact `_lkmm` functions in BPF and `_c` functions in userspace.

### Vyukhov (`include/ds_vyukhov.h`)

- Type: `struct ds_vyukhov_head`
- Init:
  - kernel: `ds_vyukhov_init_lkmm(head, capacity)`
  - user: `ds_vyukhov_init_c(head, capacity)`
- Produce: `ds_vyukhov_insert_*`
- Consume: `ds_vyukhov_pop_*`
- Note: requires power-of-two capacity for both KU and UK.

### Folly SPSC (`include/ds_folly_spsc.h`)

- Type: `struct ds_spsc_queue_head`
- Init:
  - kernel: `ds_spsc_init_lkmm(head, size)`
  - user: `ds_spsc_init_c(head, size)`
- Produce: `ds_spsc_insert_*`
- Consume: `ds_spsc_delete_*` or `ds_spsc_pop_*`
- Note: one slot is reserved; usable capacity is `size - 1`.

### CK FIFO SPSC (`include/ds_ck_fifo_spsc.h`)

- Type: `struct ds_ck_fifo_spsc_head`
- Init:
  - kernel: `ds_ck_fifo_spsc_init_lkmm(head)`
  - user: `ds_ck_fifo_spsc_init_c(head)`
- Produce: `ds_ck_fifo_spsc_insert_*`
- Consume: `ds_ck_fifo_spsc_pop_*` (or `delete_*`)
- Note: no external capacity argument in init.

### CK Ring SPSC (`include/ds_ck_ring_spsc.h`)

- Type: `struct ds_ck_ring_spsc_head`
- Init:
  - kernel: `ds_ck_ring_spsc_init_lkmm(head, capacity)`
  - user: `ds_ck_ring_spsc_init_c(head, capacity)`
- Produce: `ds_ck_ring_spsc_insert_*`
- Consume: `ds_ck_ring_spsc_pop_*` (or `delete_*`)
- Note: capacity must be power-of-two; usable slots are `capacity - 1`.

### CK Stack UPMC (`include/ds_ck_stack_upmc.h`)

- Type: `ds_ck_stack_upmc_head_t`
- Init:
  - kernel: `ds_ck_stack_upmc_init_lkmm(head)`
  - user: `ds_ck_stack_upmc_init_c(head)`
- Produce: `ds_ck_stack_upmc_insert_*`
- Consume: `ds_ck_stack_upmc_pop_*`
- Note: init returns `void`, so init-success checks must be adapted.

---

## Per-Structure Readiness Checks in Relay Thread

When waiting for KU to become usable, check the structure-specific internal pointers.

- Vyukhov: `head->buffer != NULL`
- Folly SPSC: `head->records != NULL`
- CK FIFO SPSC: `head->fifo.head != NULL && head->fifo.tail != NULL`
- CK Ring SPSC: `head->slots != NULL`
- CK Stack UPMC: head exists; optional stronger check is to rely on `initialized` BPF flag/counter

For UK init from userspace, use the matching `*_init_c` API and the same capacity/size policy as KU.

---

## Minimal Pseudocode Template

```c
// BPF globals
struct ds_X __arena global_ds_ku;
struct ds_X __arena global_ds_uk;

SEC("lsm.s/inode_create")
int BPF_PROG(lsm_inode_create, ...) {
    if (!initialized_ku)
        ds_X_init_lkmm(&global_ds_ku, ...);
    ds_X_insert_lkmm(&global_ds_ku, pid, ts);
    return 0;
}

SEC("uprobe.s")
int bpf_x_consume(struct pt_regs *ctx) {
    struct ds_kv out = {};
    return ds_X_pop_lkmm(&global_ds_uk, &out);
}
```

```c
// Userspace
__attribute__((noinline)) void x_kernel_consume_trigger(void) { asm volatile("" ::: "memory"); }

main() {
    open_and_load();
    setup_userspace_allocator();
    attach_lsm();
    attach_uprobe("x_kernel_consume_trigger");
    spawn_relay_thread();
    wait_for_ctrl_c();
    join_relay_thread();
    while (need_more_kernel_consumption)
        x_kernel_consume_trigger();
}

relay_thread() {
    wait_until_ku_initialized();
    init_uk_if_needed();
    while (!stop)
        if (pop_ku(&kv) == DS_SUCCESS)
            insert_uk(kv.key, kv.value);
}
```

---

## Constraints and Gotchas

- Keep `include/ds_*.h` unchanged unless absolutely required; this pattern should be done in `src/` only.
- Do not share one queue instance between KU and UK lanes.
- For SPSC variants, ensure each lane still has exactly one producer and one consumer:
  - KU: producer=kernel LSM, consumer=relay thread
  - UK: producer=relay thread, consumer=kernel uprobe
- If using `*_open_and_load()` plus custom uprobe opts, ensure links are attached intentionally and not duplicated.
- For bounded structures, keep KU/UK capacity policy explicit and consistent.

---

## Manual Validation Checklist (No Code Changes)

1. Build target skeleton.
2. Run with sudo.
3. Generate inode activity (create files).
4. Confirm relay stats increase (`KU popped`, `UK pushed`).
5. Press Ctrl+C.
6. Confirm kernel-consume counters increase (uprobe consumer runs).
7. Optional: run with `-v` and verify both KU and UK structures pass.
