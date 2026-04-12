/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/* io_uring/liburing-style SPSC Ring Buffer for BPF Arena
 *
 * Faithful port of the four SQPOLL producer/consumer roles from Linux
 * io_uring (kernel) and liburing (userspace), preserving every memory
 * ordering asymmetry documented in references/IOURING-LIBURING.md.
 *
 * Architecture:
 *   SQ ring -> UK lane (user-to-kernel): userspace produces, kernel consumes
 *   CQ ring -> KU lane (kernel-to-user): kernel produces, userspace consumes
 *
 * Four role-specific functions, each with _lkmm and _c variants:
 *
 *   1. SQ Produce (user -> UK insert):
 *      acquire(cons.head), plain SQE writes, release(prod.tail)
 *
 *   2. SQ Consume (kernel -> UK pop):
 *      acquire(prod.tail), READ_ONCE entry fields, release(cons.head)
 *
 *   3. CQ Produce (kernel -> KU insert):
 *      READ_ONCE(cons.head) + control dependency (_lkmm) / acquire (_c),
 *      WRITE_ONCE entry fields, release(prod.tail)
 *
 *   4. CQ Consume (user -> KU pop):
 *      acquire(prod.tail), acquire(cons.head) [double acquire],
 *      plain CQE reads, release(cons.head)
 *
 * Key asymmetries preserved from the original:
 *   - SQ consume: READ_ONCE on entry fields (kernel doesn't trust userspace)
 *   - CQ consume: plain loads on entry fields (userspace trusts kernel)
 *   - SQ produce: plain stores for entry fields (liburing prep_* pattern)
 *   - CQ produce: WRITE_ONCE for entry fields (kernel io_fill_cqe_aux)
 *   - CQ produce _lkmm: control dependency replaces acquire (the ONE
 *     signature LKMM optimization, per io_uring.c comment lines 762-766)
 *
 * NO_SQARRAY mode: direct indexing, no SQ array indirection.
 */
#pragma once

#include "ds_api.h"

/* ========================================================================
 * CONSTANTS
 * ======================================================================== */

/* sq_flags bit definitions — mirrors io_uring's IORING_SQ_* flags */
#define DS_IOURING_LIBURING_SQ_FLAG_FULL  (1U << 0)

/* ========================================================================
 * DATA STRUCTURES
 * ======================================================================== */

/**
 * struct ds_iouring_liburing_ring - io_uring/liburing faithful SPSC ring
 *
 * Same struct is used for both SQ (UK lane) and CQ (KU lane).
 * The four role-specific functions apply different memory ordering.
 *
 * @prod: Producer index (tail) — sole writer is the producer.
 * @cons: Consumer index (head) — sole writer is the consumer.
 * @ring_entries: Number of slots; must be a power of 2.
 * @ring_mask: ring_entries - 1.
 * @sq_flags: Atomic backpressure signaling (advisory).
 * @entries: Arena-allocated flat array of ds_kv slots.
 */
struct ds_iouring_liburing_ring {
	/* Producer index (tail) — separate cache line */
	struct {
		__u32 tail;
	} prod __attribute__((aligned(64)));

	/* Consumer index (head) — separate cache line */
	struct {
		__u32 head;
	} cons __attribute__((aligned(64)));

	__u32 ring_entries;
	__u32 ring_mask;
	__u32 sq_flags;
	struct ds_kv __arena *entries;
};

typedef struct ds_iouring_liburing_ring __arena ds_iouring_liburing_ring_t;

/* ========================================================================
 * INIT — LKMM
 * ======================================================================== */

/**
 * ds_iouring_liburing_init_lkmm - Initialize ring (LKMM, BPF-safe)
 * @head: Ring to initialize (arena pointer)
 * @ring_entries: Number of slots; MUST be a power of 2
 */
static inline __attribute__((unused))
int ds_iouring_liburing_init_lkmm(struct ds_iouring_liburing_ring __arena *head,
				  __u32 ring_entries)
{
	struct ds_kv __arena *entries;

	cast_kern(head);

	if (!ring_entries || (ring_entries & (ring_entries - 1)))
		return DS_ERROR_INVALID;

	entries = (struct ds_kv __arena *)bpf_arena_alloc(
		ring_entries * sizeof(struct ds_kv));
	if (!entries)
		return DS_ERROR_NOMEM;

	cast_kern(entries);

	head->ring_entries = ring_entries;
	head->ring_mask    = ring_entries - 1;
	WRITE_ONCE(head->prod.tail, 0);
	WRITE_ONCE(head->cons.head, 0);
	WRITE_ONCE(head->sq_flags, 0);

	cast_user(entries);
	head->entries = entries;

	return DS_SUCCESS;
}

/* ========================================================================
 * INIT — C11
 * ======================================================================== */

#ifndef __BPF__
/**
 * ds_iouring_liburing_init_c - Initialize ring (C11, userspace)
 * @head: Ring to initialize (arena pointer)
 * @ring_entries: Number of slots; MUST be a power of 2
 */
static inline __attribute__((unused))
int ds_iouring_liburing_init_c(struct ds_iouring_liburing_ring __arena *head,
			       __u32 ring_entries)
{
	struct ds_kv __arena *entries;

	cast_kern(head);

	if (!ring_entries || (ring_entries & (ring_entries - 1)))
		return DS_ERROR_INVALID;

	entries = (struct ds_kv __arena *)bpf_arena_alloc(
		ring_entries * sizeof(struct ds_kv));
	if (!entries)
		return DS_ERROR_NOMEM;

	cast_kern(entries);

	arena_atomic_store(&head->ring_entries, ring_entries, ARENA_RELAXED);
	arena_atomic_store(&head->ring_mask,    ring_entries - 1, ARENA_RELAXED);
	arena_atomic_store(&head->prod.tail,    0, ARENA_RELAXED);
	arena_atomic_store(&head->cons.head,    0, ARENA_RELAXED);
	arena_atomic_store(&head->sq_flags,     0, ARENA_RELAXED);

	cast_user(entries);
	arena_atomic_store(&head->entries, entries, ARENA_RELAXED);

	return DS_SUCCESS;
}
#endif /* !__BPF__ */

/* ========================================================================
 * INIT — Router
 * ======================================================================== */

static inline __attribute__((unused))
int ds_iouring_liburing_init(struct ds_iouring_liburing_ring __arena *head,
			     __u32 ring_entries)
{
#ifdef __BPF__
	return ds_iouring_liburing_init_lkmm(head, ring_entries);
#else
	return ds_iouring_liburing_init_c(head, ring_entries);
#endif
}

/* ========================================================================
 * SQ PRODUCE — User produces into UK lane (insert)
 *
 * Reference: liburing _io_uring_get_sqe + __io_uring_flush_sq
 * From IOURING-LIBURING.md Section 1 (User-Producer SQ):
 *   1. smp_load_acquire(sq.khead) — see freed slots
 *   2. Plain writes to SQE fields (io_uring_prep_*)
 *   3. smp_store_release(sq.ktail) — publish entries
 * ======================================================================== */

/**
 * ds_iouring_liburing_sq_produce_lkmm - SQ produce (LKMM)
 *
 * Userspace produces SQEs for kernel SQPOLL consumption.
 * Maps to: _io_uring_get_sqe (slot allocation) + io_uring_prep_*
 * (plain SQE writes) + __io_uring_flush_sq (release tail).
 *
 * Memory ordering:
 *   smp_load_acquire(cons.head)  — pairs with kernel's release in
 *     io_commit_sqring, ensures SQE slot reuse is safe
 *   Plain stores to entry fields — ordered before tail by the release
 *   smp_store_release(prod.tail) — publishes entries, pairs with
 *     kernel's acquire in io_sqring_entries
 */
static inline __attribute__((unused))
int ds_iouring_liburing_sq_produce_lkmm(
	struct ds_iouring_liburing_ring __arena *head,
	__u64 key, __u64 value)
{
	struct ds_kv __arena *slot;
	__u32 tail, h;

	cast_kern(head);

	/* Producer-private: sole writer of prod.tail in SPSC */
	tail = READ_ONCE(head->prod.tail);

	/* ACQUIRE: pairs with kernel's smp_store_release(sq.head) in
	 * io_commit_sqring — ensures slot is safe to reuse.
	 * Reference: liburing io_uring_load_sq_head (line 1693) */
	h = smp_load_acquire(&head->cons.head);

	if (tail - h >= head->ring_entries) {
		arena_atomic_or(&head->sq_flags,
				DS_IOURING_LIBURING_SQ_FLAG_FULL,
				ARENA_RELAXED);
		return DS_ERROR_FULL;
	}

	/* Plain writes to SQE fields — io_uring_prep_* uses plain stores.
	 * Ordered before tail update by the release store below. */
	slot = &head->entries[tail & head->ring_mask];
	cast_kern(slot);
	slot->key   = key;
	slot->value = value;

	/* RELEASE: publish SQE data before advancing tail.
	 * Reference: __io_uring_flush_sq (queue.c:228) */
	smp_store_release(&head->prod.tail, tail + 1);

	arena_atomic_and(&head->sq_flags,
			 ~DS_IOURING_LIBURING_SQ_FLAG_FULL,
			 ARENA_RELAXED);

	return DS_SUCCESS;
}

#ifndef __BPF__
/**
 * ds_iouring_liburing_sq_produce_c - SQ produce (C11)
 *
 * Same logic as _lkmm but uses C11 atomics.
 */
static inline __attribute__((unused))
int ds_iouring_liburing_sq_produce_c(
	struct ds_iouring_liburing_ring __arena *head,
	__u64 key, __u64 value)
{
	struct ds_kv __arena *slot;
	__u32 tail, h;

	cast_kern(head);

	tail = arena_atomic_load(&head->prod.tail, ARENA_RELAXED);

	/* ACQUIRE: see all cons.head stores released by the kernel consumer */
	h = arena_atomic_load(&head->cons.head, ARENA_ACQUIRE);

	if (tail - h >= head->ring_entries) {
		arena_atomic_or(&head->sq_flags,
				DS_IOURING_LIBURING_SQ_FLAG_FULL,
				ARENA_RELAXED);
		return DS_ERROR_FULL;
	}

	slot = &head->entries[tail & head->ring_mask];
	cast_kern(slot);
	/* C11: RELAXED stores for entry fields (ordered by release below) */
	arena_atomic_store(&slot->key,   key,   ARENA_RELAXED);
	arena_atomic_store(&slot->value, value, ARENA_RELAXED);

	/* RELEASE: publish entry data before advancing tail */
	arena_atomic_store(&head->prod.tail, tail + 1, ARENA_RELEASE);

	arena_atomic_and(&head->sq_flags,
			 ~DS_IOURING_LIBURING_SQ_FLAG_FULL,
			 ARENA_RELAXED);

	return DS_SUCCESS;
}
#endif /* !__BPF__ */

/* ========================================================================
 * SQ CONSUME — Kernel consumes from UK lane (pop)
 *
 * Reference: io_sqring_entries + io_get_sqe + io_init_req + io_commit_sqring
 * From IOURING-LIBURING.md Section 2 (Kernel-Consumer SQ):
 *   1. smp_load_acquire(sq.tail) — see new SQEs
 *   2. READ_ONCE(sqe->*) fields — volatile loads, untrusted userspace
 *   3. smp_store_release(sq.head) — free slots for producer
 * ======================================================================== */

/**
 * ds_iouring_liburing_sq_consume_lkmm - SQ consume (LKMM)
 *
 * Kernel SQPOLL thread consumes SQEs submitted by userspace.
 * Maps to: io_sqring_entries (acquire tail) + io_get_sqe/io_init_req
 * (READ_ONCE fields) + io_commit_sqring (release head).
 *
 * Memory ordering:
 *   smp_load_acquire(prod.tail)  — pairs with user's release of tail
 *     in __io_uring_flush_sq, ensures SQE data is visible
 *   READ_ONCE(entry->*)          — volatile loads prevent compiler
 *     re-reads of untrusted userspace memory (io_uring.c comment
 *     lines 2383-2389: "care needs to be taken to ensure that reads
 *     are stable, as we cannot rely on userspace always being a good
 *     citizen")
 *   smp_store_release(cons.head) — frees slots; pairs with user's
 *     acquire of head in io_uring_load_sq_head
 */
static inline __attribute__((unused))
int ds_iouring_liburing_sq_consume_lkmm(
	struct ds_iouring_liburing_ring __arena *head,
	struct ds_kv *out)
{
	struct ds_kv __arena *slot;
	__u32 h, t;

	cast_kern(head);

	/* Consumer-private: sole writer of cons.head in SPSC */
	h = READ_ONCE(head->cons.head);

	/* ACQUIRE: pairs with user's smp_store_release(prod.tail).
	 * Reference: io_sqring_entries (io_uring.h:455) */
	t = smp_load_acquire(&head->prod.tail);

	if (h == t)
		return DS_ERROR_NOT_FOUND;

	slot = &head->entries[h & head->ring_mask];
	cast_kern(slot);

	/* READ_ONCE on entry fields: the kernel does NOT trust userspace.
	 * Prevents compiler re-reads of userspace-mapped memory.
	 * Reference: io_init_req uses READ_ONCE(sqe->opcode), READ_ONCE(sqe->flags),
	 * READ_ONCE(sqe->user_data), etc. (io_uring.c:2150-2237) */
	if (out) {
		out->key   = READ_ONCE(slot->key);
		out->value = READ_ONCE(slot->value);
	}

	/* RELEASE: "Ensure any loads from the SQEs are done at this point,
	 * since once we write the new head, the application could write
	 * new data to them." (io_uring.c:2375-2378)
	 * Reference: io_commit_sqring (io_uring.c:2380) */
	smp_store_release(&head->cons.head, h + 1);

	return DS_SUCCESS;
}

#ifndef __BPF__
/**
 * ds_iouring_liburing_sq_consume_c - SQ consume (C11)
 *
 * Same logic as _lkmm but uses C11 atomics.
 * Note: READ_ONCE maps to arena_atomic_load RELAXED in C11.
 */
static inline __attribute__((unused))
int ds_iouring_liburing_sq_consume_c(
	struct ds_iouring_liburing_ring __arena *head,
	struct ds_kv *out)
{
	struct ds_kv __arena *slot;
	__u32 h, t;

	cast_kern(head);

	h = arena_atomic_load(&head->cons.head, ARENA_RELAXED);

	/* ACQUIRE: see all data written by user before prod.tail */
	t = arena_atomic_load(&head->prod.tail, ARENA_ACQUIRE);

	if (h == t)
		return DS_ERROR_NOT_FOUND;

	slot = &head->entries[h & head->ring_mask];
	cast_kern(slot);

	/* RELAXED loads for entry fields (C11 equivalent of READ_ONCE) */
	if (out) {
		out->key   = arena_atomic_load(&slot->key,   ARENA_RELAXED);
		out->value = arena_atomic_load(&slot->value, ARENA_RELAXED);
	}

	/* RELEASE: free slots for producer */
	arena_atomic_store(&head->cons.head, h + 1, ARENA_RELEASE);

	return DS_SUCCESS;
}
#endif /* !__BPF__ */

/* ========================================================================
 * CQ PRODUCE — Kernel produces into KU lane (insert)
 *
 * Reference: __io_cqring_events + io_fill_cqe_aux + io_commit_cqring
 * From IOURING-LIBURING.md Section 3 (Kernel-Producer CQ):
 *   1. READ_ONCE(cq.head) + CONTROL DEPENDENCY (NOT acquire!)
 *   2. WRITE_ONCE CQE fields
 *   3. smp_store_release(cq.tail)
 *
 * THE signature LKMM optimization: in _lkmm, the fullness check
 * creates a control dependency that orders subsequent WRITE_ONCE
 * stores, so acquire on head is unnecessary.
 *
 * io_uring.c comment (lines 762-766):
 *   "writes to the cq entry need to come after reading head; the
 *    control dependency is enough as we're using WRITE_ONCE to fill
 *    the cq entry."
 *
 * In _c, we must use ARENA_ACQUIRE because C11 does NOT recognize
 * control dependencies.
 * ======================================================================== */

/**
 * ds_iouring_liburing_cq_produce_lkmm - CQ produce (LKMM)
 *
 * Kernel produces CQEs for userspace consumption.
 * Maps to: __io_cqring_events (READ_ONCE head) + io_fill_cqe_aux
 * (WRITE_ONCE fields) + io_commit_cqring (release tail).
 *
 * Memory ordering:
 *   READ_ONCE(cons.head) — NOT acquire! The branch checking fullness
 *     provides a control dependency that orders subsequent stores.
 *     This is the KEY LKMM optimization from io_uring.
 *   WRITE_ONCE(entry->*) — volatile stores (io_fill_cqe_aux pattern)
 *   smp_store_release(prod.tail) — "order cqe stores with ring update"
 *     (io_uring.h:402), pairs with user's acquire of tail
 */
static inline __attribute__((unused))
int ds_iouring_liburing_cq_produce_lkmm(
	struct ds_iouring_liburing_ring __arena *head,
	__u64 key, __u64 value)
{
	struct ds_kv __arena *slot;
	__u32 tail, h;

	cast_kern(head);

	/* Producer-private: sole writer of prod.tail in SPSC */
	tail = READ_ONCE(head->prod.tail);

	/* LKMM: control dependency from this READ_ONCE to the subsequent
	 * WRITE_ONCE stores provides sufficient ordering; acquire NOT needed.
	 *
	 * Reference: __io_cqring_events (io_uring.c:192) uses
	 * READ_ONCE(ctx->rings->cq.head), and the comment at lines 762-766
	 * explains: "writes to the cq entry need to come after reading head;
	 * the control dependency is enough as we're using WRITE_ONCE to fill
	 * the cq entry."
	 *
	 * The branch below (if tail - h >= ring_entries) creates the control
	 * dependency: the WRITE_ONCE stores in the success path cannot be
	 * hoisted above this branch by either the compiler or hardware. */
	h = READ_ONCE(head->cons.head);

	if (tail - h >= head->ring_entries) {
		arena_atomic_or(&head->sq_flags,
				DS_IOURING_LIBURING_SQ_FLAG_FULL,
				ARENA_RELAXED);
		return DS_ERROR_FULL;
	}

	/* WRITE_ONCE for CQE fields — kernel io_fill_cqe_aux pattern.
	 * WRITE_ONCE (not plain store) because LKMM control dependency
	 * orders READ_ONCE → branch → WRITE_ONCE but NOT READ_ONCE →
	 * branch → plain_store (compiler may hoist plain stores).
	 * Reference: io_fill_cqe_aux (io_uring.c:831-833) */
	slot = &head->entries[tail & head->ring_mask];
	cast_kern(slot);
	WRITE_ONCE(slot->key,   key);
	WRITE_ONCE(slot->value, value);

	/* RELEASE: "order cqe stores with ring update" (io_uring.h:402)
	 * Reference: io_commit_cqring (io_uring.h:403) */
	smp_store_release(&head->prod.tail, tail + 1);

	arena_atomic_and(&head->sq_flags,
			 ~DS_IOURING_LIBURING_SQ_FLAG_FULL,
			 ARENA_RELAXED);

	return DS_SUCCESS;
}

#ifndef __BPF__
/**
 * ds_iouring_liburing_cq_produce_c - CQ produce (C11)
 *
 * Same logic as _lkmm but uses ARENA_ACQUIRE on cons.head because
 * C11 does NOT recognize control dependencies. Without acquire, the
 * compiler is free to reorder stores above the branch.
 */
static inline __attribute__((unused))
int ds_iouring_liburing_cq_produce_c(
	struct ds_iouring_liburing_ring __arena *head,
	__u64 key, __u64 value)
{
	struct ds_kv __arena *slot;
	__u32 tail, h;

	cast_kern(head);

	tail = arena_atomic_load(&head->prod.tail, ARENA_RELAXED);

	/* ACQUIRE: C11 must use acquire because it does NOT recognize
	 * control dependencies. The _lkmm version uses READ_ONCE + control
	 * dependency as the kernel does. */
	h = arena_atomic_load(&head->cons.head, ARENA_ACQUIRE);

	if (tail - h >= head->ring_entries) {
		arena_atomic_or(&head->sq_flags,
				DS_IOURING_LIBURING_SQ_FLAG_FULL,
				ARENA_RELAXED);
		return DS_ERROR_FULL;
	}

	slot = &head->entries[tail & head->ring_mask];
	cast_kern(slot);
	/* C11: RELAXED stores for CQE fields (ordered by release below;
	 * the acquire on head above prevents hoisting above the branch) */
	arena_atomic_store(&slot->key,   key,   ARENA_RELAXED);
	arena_atomic_store(&slot->value, value, ARENA_RELAXED);

	/* RELEASE: publish CQE data before advancing tail */
	arena_atomic_store(&head->prod.tail, tail + 1, ARENA_RELEASE);

	arena_atomic_and(&head->sq_flags,
			 ~DS_IOURING_LIBURING_SQ_FLAG_FULL,
			 ARENA_RELAXED);

	return DS_SUCCESS;
}
#endif /* !__BPF__ */

/* ========================================================================
 * CQ CONSUME — User consumes from KU lane (pop)
 *
 * Reference: __io_uring_peek_cqe + io_uring_cq_advance
 * From IOURING-LIBURING.md Section 4 (User-Consumer CQ):
 *   1. smp_load_acquire(cq.ktail) — see new CQEs
 *   2. smp_load_acquire(cq.khead) — DOUBLE ACQUIRE
 *   3. Plain reads of CQE fields (trusted kernel memory)
 *   4. smp_store_release(cq.khead) — free slots
 *
 * The double-acquire pattern is distinctive: head is acquired even
 * though the user is the sole writer. liburing comment (line 1860-1862):
 *   "A load_acquire on the head prevents reordering with the cqe load
 *    below, ensuring that we see the correct cq entry."
 * ======================================================================== */

/**
 * ds_iouring_liburing_cq_consume_lkmm - CQ consume (LKMM)
 *
 * Userspace consumes CQEs produced by the kernel.
 * Maps to: __io_uring_peek_cqe (double acquire) + io_uring_cq_advance
 * (release head).
 *
 * Memory ordering:
 *   smp_load_acquire(prod.tail) — pairs with kernel's release of tail
 *     in io_commit_cqring, ensures CQE data is visible
 *   smp_load_acquire(cons.head) — double acquire; prevents reordering
 *     with CQE loads even though user is sole head writer. Critical
 *     for correctness in loop scenarios where io_uring_cq_advance
 *     (release on head) may have been called in the loop body
 *   Plain reads of CQE fields — trusted kernel memory, ordered by
 *     tail acquire above
 *   smp_store_release(cons.head) — "Ensure that the kernel only sees
 *     the new value of the head index after the CQEs have been read"
 *     (liburing.h:487-489)
 */
static inline __attribute__((unused))
int ds_iouring_liburing_cq_consume_lkmm(
	struct ds_iouring_liburing_ring __arena *head,
	struct ds_kv *out)
{
	struct ds_kv __arena *slot;
	__u32 h, t;

	cast_kern(head);

	/* ACQUIRE: read kernel's tail; pairs with kernel's
	 * smp_store_release(cq.tail) in io_commit_cqring.
	 * Reference: __io_uring_peek_cqe (liburing.h:1858) */
	t = smp_load_acquire(&head->prod.tail);

	/* DOUBLE ACQUIRE: acquire on head even though sole writer.
	 * Prevents reordering with CQE load below.
	 * Reference: __io_uring_peek_cqe (liburing.h:1864)
	 * Comment: "A load_acquire on the head prevents reordering with
	 * the cqe load below, ensuring that we see the correct cq entry." */
	h = smp_load_acquire(&head->cons.head);

	if (h == t)
		return DS_ERROR_NOT_FOUND;

	/* Plain reads of CQE fields — trusted kernel memory.
	 * Ordered after tail acquire by program order + acquire semantics.
	 * Reference: __io_uring_peek_cqe (liburing.h:1871+) uses plain
	 * reads of cqe->user_data, cqe->res, cqe->flags */
	slot = &head->entries[h & head->ring_mask];
	cast_kern(slot);
	if (out) {
		out->key   = slot->key;
		out->value = slot->value;
	}

	/* RELEASE: "Ensure that the kernel only sees the new value of the
	 * head index after the CQEs have been read." (liburing.h:487-489)
	 * Pairs with kernel's READ_ONCE(cq.head) in __io_cqring_events.
	 * Reference: io_uring_cq_advance (liburing.h:491) */
	smp_store_release(&head->cons.head, h + 1);

	return DS_SUCCESS;
}

#ifndef __BPF__
/**
 * ds_iouring_liburing_cq_consume_c - CQ consume (C11)
 *
 * Same logic as _lkmm but uses C11 atomics.
 * Double-acquire preserved: both tail and head use ARENA_ACQUIRE.
 */
static inline __attribute__((unused))
int ds_iouring_liburing_cq_consume_c(
	struct ds_iouring_liburing_ring __arena *head,
	struct ds_kv *out)
{
	struct ds_kv __arena *slot;
	__u32 h, t;

	cast_kern(head);

	/* ACQUIRE: read kernel's tail */
	t = arena_atomic_load(&head->prod.tail, ARENA_ACQUIRE);

	/* DOUBLE ACQUIRE: acquire on head even though sole writer.
	 * C11 equivalent of the liburing double-acquire pattern. */
	h = arena_atomic_load(&head->cons.head, ARENA_ACQUIRE);

	if (h == t)
		return DS_ERROR_NOT_FOUND;

	slot = &head->entries[h & head->ring_mask];
	cast_kern(slot);
	/* RELAXED loads for CQE entry fields (trusted kernel data) */
	if (out) {
		out->key   = arena_atomic_load(&slot->key,   ARENA_RELAXED);
		out->value = arena_atomic_load(&slot->value, ARENA_RELAXED);
	}

	/* RELEASE: free slots for kernel producer */
	arena_atomic_store(&head->cons.head, h + 1, ARENA_RELEASE);

	return DS_SUCCESS;
}
#endif /* !__BPF__ */

/* ========================================================================
 * ROUTER FUNCTIONS — #ifdef __BPF__ dispatches
 *
 * In the relay architecture:
 *   KU lane (kernel produces, user consumes) = CQ ring
 *   UK lane (user produces, kernel consumes) = SQ ring
 *
 * So:
 *   insert (KU) = CQ produce (kernel side)
 *   pop    (KU) = CQ consume (user side)
 *   insert (UK) = SQ produce (user side)
 *   pop    (UK) = SQ consume (kernel side)
 *
 * The #ifdef __BPF__ routers resolve naturally:
 *   - __BPF__:  insert -> CQ produce (kernel), pop -> SQ consume (kernel)
 *   - !__BPF__: insert -> SQ produce (user),   pop -> CQ consume (user)
 * ======================================================================== */

/**
 * ds_iouring_liburing_insert - Insert key/value into the ring
 *
 * In __BPF__ context: kernel CQ produce (_lkmm) — uses control dependency
 * In userspace: user SQ produce (_c) — uses acquire on head
 */
static inline __attribute__((unused))
int ds_iouring_liburing_insert(struct ds_iouring_liburing_ring __arena *head,
			       __u64 key, __u64 value)
{
#ifdef __BPF__
	return ds_iouring_liburing_cq_produce_lkmm(head, key, value);
#else
	return ds_iouring_liburing_sq_produce_c(head, key, value);
#endif
}

/**
 * ds_iouring_liburing_pop - Pop key/value from the ring
 *
 * In __BPF__ context: kernel SQ consume (_lkmm) — READ_ONCE on entries
 * In userspace: user CQ consume (_c) — double acquire, plain CQE reads
 */
static inline __attribute__((unused))
int ds_iouring_liburing_pop(struct ds_iouring_liburing_ring __arena *head,
			    struct ds_kv *out)
{
#ifdef __BPF__
	return ds_iouring_liburing_sq_consume_lkmm(head, out);
#else
	return ds_iouring_liburing_cq_consume_c(head, out);
#endif
}

/* ========================================================================
 * SEARCH (not applicable to SPSC ring)
 * ======================================================================== */

static inline __attribute__((unused))
int ds_iouring_liburing_search_lkmm(
	struct ds_iouring_liburing_ring __arena *head,
	__u64 key)
{
	(void)head;
	(void)key;
	return DS_ERROR_INVALID;
}

#ifndef __BPF__
static inline __attribute__((unused))
int ds_iouring_liburing_search_c(
	struct ds_iouring_liburing_ring __arena *head,
	__u64 key)
{
	(void)head;
	(void)key;
	return DS_ERROR_INVALID;
}
#endif /* !__BPF__ */

static inline __attribute__((unused))
int ds_iouring_liburing_search(
	struct ds_iouring_liburing_ring __arena *head,
	__u64 key)
{
#ifdef __BPF__
	return ds_iouring_liburing_search_lkmm(head, key);
#else
	return ds_iouring_liburing_search_c(head, key);
#endif
}

/* ========================================================================
 * VERIFY
 * ======================================================================== */

/**
 * ds_iouring_liburing_verify_lkmm - Verify ring invariants (LKMM)
 */
static inline __attribute__((unused))
int ds_iouring_liburing_verify_lkmm(
	struct ds_iouring_liburing_ring __arena *head)
{
	__u32 tail, h, ring_entries;

	cast_kern(head);

	ring_entries = head->ring_entries;

	if (!ring_entries || (ring_entries & (ring_entries - 1)))
		return DS_ERROR_CORRUPT;

	if (head->ring_mask != ring_entries - 1)
		return DS_ERROR_CORRUPT;

	tail = READ_ONCE(head->prod.tail);
	h    = READ_ONCE(head->cons.head);

	if (tail - h > ring_entries)
		return DS_ERROR_CORRUPT;

	return DS_SUCCESS;
}

#ifndef __BPF__
/**
 * ds_iouring_liburing_verify_c - Verify ring invariants (C11)
 */
static inline __attribute__((unused))
int ds_iouring_liburing_verify_c(
	struct ds_iouring_liburing_ring __arena *head)
{
	__u32 tail, h, ring_entries;

	cast_kern(head);

	ring_entries = arena_atomic_load(&head->ring_entries, ARENA_RELAXED);

	if (!ring_entries || (ring_entries & (ring_entries - 1)))
		return DS_ERROR_CORRUPT;

	if (arena_atomic_load(&head->ring_mask, ARENA_RELAXED) != ring_entries - 1)
		return DS_ERROR_CORRUPT;

	tail = arena_atomic_load(&head->prod.tail, ARENA_RELAXED);
	h    = arena_atomic_load(&head->cons.head, ARENA_RELAXED);

	if (tail - h > ring_entries)
		return DS_ERROR_CORRUPT;

	return DS_SUCCESS;
}
#endif /* !__BPF__ */

static inline __attribute__((unused))
int ds_iouring_liburing_verify(
	struct ds_iouring_liburing_ring __arena *head)
{
#ifdef __BPF__
	return ds_iouring_liburing_verify_lkmm(head);
#else
	return ds_iouring_liburing_verify_c(head);
#endif
}
