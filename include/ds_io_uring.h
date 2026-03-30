/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/* io_uring-style SPSC Ring Buffer for BPF Arena
 *
 * Based on the memory layout and barrier model of Linux io_uring
 * (include/linux/io_uring_types.h, io_uring/io_uring.c).
 *
 * Key design decisions (from ring-implementation-spec.md):
 * - Power-of-2 ring_entries with index & mask; u32 natural 2^32 wrap-around
 * - store-release / load-acquire on head/tail — 1:1 mapping from io_uring's
 *   smp_store_release / smp_load_acquire
 * - sq_flags atomic field (arena_atomic_or/arena_atomic_and) mirrors
 *   io_uring's atomic_or / atomic_andnot on sq_flags
 * - No SQ indirection array (NO_SQARRAY mode)
 * - No CQ overflow list (simplified SPSC)
 * - Single contiguous arena region (vs io_uring's two mmap regions)
 */
#pragma once

#include "ds_api.h"

/* ========================================================================
 * CONSTANTS
 * ======================================================================== */

/* sq_flags bit definitions — mirrors io_uring's IORING_SQ_* flags */
#define DS_IO_URING_SQ_FLAG_FULL  (1U << 0)   /* ring is full, producer blocked */

/* ========================================================================
 * DATA STRUCTURES
 * ======================================================================== */

/**
 * struct ds_io_uring_ring_head - io_uring-style SPSC ring control block
 * @prod: Producer index (tail) — written exclusively by the producer.
 *        Placed on its own cache line to eliminate false sharing with
 *        the consumer index.
 * @cons: Consumer index (head) — written exclusively by the consumer.
 *        Cache-line-separated from prod for the same reason.
 * @ring_entries: Number of slots; must be a power of 2.
 * @ring_mask: ring_entries - 1; used for cheap modulo via bitwise AND.
 * @sq_flags: Atomic status word. Bit 0 (DS_IO_URING_SQ_FLAG_FULL) is set
 *            via arena_atomic_or() when the producer finds the ring full,
 *            and cleared via arena_atomic_and() after a successful insert
 *            clears the backpressure condition.
 * @entries: Flat arena-allocated array of ds_kv slots.
 *
 * INVARIANTS:
 * - ring_entries > 0 and is a power of 2
 * - ring_mask == ring_entries - 1
 * - prod.tail - cons.head <= ring_entries  (u32 unsigned subtraction)
 * - Empty: prod.tail == cons.head
 * - Full:  prod.tail - cons.head == ring_entries
 *
 * LKMM ORDERING CONTRACT:
 * Producer: WRITE payload → smp_store_release(prod.tail)
 * Consumer: smp_load_acquire(prod.tail) → READ payload
 * Consumer: RELEASE cons.head so producer can observe free slots
 * Producer: smp_load_acquire(cons.head) to check available space
 *
 * This is a direct port of io_uring's sq_ring / cq_ring layout.  Both
 * the KU lane (kernel→userspace, maps to CQ in io_uring) and the UK
 * lane (userspace→kernel, maps to SQ in io_uring) use the same struct.
 */
struct ds_io_uring_ring_head {
	/* Producer index (written by producer only) — separate cache line */
	struct {
		__u32 tail;
	} prod __attribute__((aligned(64)));

	/* Consumer index (written by consumer only) — separate cache line */
	struct {
		__u32 head;
	} cons __attribute__((aligned(64)));

	__u32 ring_entries;             /* power-of-2 number of slots */
	__u32 ring_mask;                /* ring_entries - 1 */
	__u32 sq_flags;                 /* status flags: bit 0 = DS_IO_URING_SQ_FLAG_FULL */
	                                /* NOTE: accessed via arena_atomic_or/and/store — no _Atomic needed */
	struct ds_kv __arena *entries;  /* arena-allocated flat array */
};

typedef struct ds_io_uring_ring_head __arena ds_io_uring_ring_head_t;

/* ========================================================================
 * INIT
 * ======================================================================== */

/**
 * ds_io_uring_init_lkmm - Initialize ring using LKMM primitives (BPF-safe)
 * @head: Ring head to initialize (arena pointer)
 * @ring_entries: Number of slots; MUST be a power of 2
 *
 * Allocates the flat entries array and zeroes all indices.
 * Returns DS_ERROR_INVALID if ring_entries is not a power of 2.
 * Returns DS_ERROR_NOMEM if arena allocation fails.
 */
static inline __attribute__((unused))
int ds_io_uring_init_lkmm(struct ds_io_uring_ring_head __arena *head,
			   __u32 ring_entries)
{
	struct ds_kv __arena *entries;

	cast_kern(head);

	/* ring_entries must be a non-zero power of 2 */
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
	/* sq_flags: plain volatile store — no other thread races during init */
	WRITE_ONCE(head->sq_flags, 0);

	cast_user(entries);
	head->entries = entries;

	return DS_SUCCESS;
}

#ifndef __BPF__
/**
 * ds_io_uring_init_c - Initialize ring using C11 atomics (userspace)
 * @head: Ring head to initialize (arena pointer)
 * @ring_entries: Number of slots; MUST be a power of 2
 */
static inline __attribute__((unused))
int ds_io_uring_init_c(struct ds_io_uring_ring_head __arena *head,
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

/**
 * ds_io_uring_init - Initialize ring (dispatches to lkmm or c variant)
 */
static inline __attribute__((unused))
int ds_io_uring_init(struct ds_io_uring_ring_head __arena *head,
		     __u32 ring_entries)
{
#ifdef __BPF__
	return ds_io_uring_init_lkmm(head, ring_entries);
#else
	return ds_io_uring_init_c(head, ring_entries);
#endif
}

/* ========================================================================
 * INSERT (producer side)
 * ======================================================================== */

/**
 * ds_io_uring_insert_lkmm - Enqueue a key/value pair (PRODUCER ONLY, LKMM)
 * @head: Ring head
 * @key: Key to enqueue
 * @value: Value to enqueue
 *
 * Returns DS_ERROR_FULL if the ring is full (sets DS_IO_URING_SQ_FLAG_FULL).
 * Returns DS_SUCCESS on success (clears DS_IO_URING_SQ_FLAG_FULL if set).
 *
 * LKMM: smp_load_acquire on cons.head synchronizes with consumer's
 * smp_store_release; smp_store_release on prod.tail publishes the entry
 * before the tail update becomes visible to the consumer.
 */
static inline __attribute__((unused))
int ds_io_uring_insert_lkmm(struct ds_io_uring_ring_head __arena *head,
			     __u64 key, __u64 value)
{
	struct ds_kv __arena *slot;
	__u32 tail, h;

	cast_kern(head);

	/* RELAXED: producer is the sole writer of prod.tail in SPSC */
	tail = READ_ONCE(head->prod.tail);

	/* ACQUIRE: synchronizes with consumer's RELEASE store to cons.head,
	 * ensuring we see all slots the consumer has freed. */
	h = smp_load_acquire(&head->cons.head);

	/* u32 unsigned subtraction handles 2^32 wrap-around correctly */
	if (tail - h >= head->ring_entries) {
		/* Signal backpressure — producer is blocked */
		arena_atomic_or(&head->sq_flags, DS_IO_URING_SQ_FLAG_FULL,
				ARENA_RELAXED);
		return DS_ERROR_FULL;
	}

	/* Write entry into the slot selected by the masked tail index */
	slot = &head->entries[tail & head->ring_mask];
	cast_kern(slot);
	slot->key   = key;
	slot->value = value;

	/* RELEASE: all writes to *slot must be visible before prod.tail
	 * is updated — this is the io_uring smp_store_release(sq_tail) pattern. */
	smp_store_release(&head->prod.tail, tail + 1);

	/* Clear FULL flag now that we successfully inserted; RELAXED because
	 * the consumer does not synchronize on sq_flags for correctness — it
	 * is advisory backpressure signalling only. */
	arena_atomic_and(&head->sq_flags, ~DS_IO_URING_SQ_FLAG_FULL,
			 ARENA_RELAXED);

	return DS_SUCCESS;
}

#ifndef __BPF__
/**
 * ds_io_uring_insert_c - Enqueue a key/value pair (PRODUCER ONLY, C11)
 * @head: Ring head
 * @key: Key to enqueue
 * @value: Value to enqueue
 */
static inline __attribute__((unused))
int ds_io_uring_insert_c(struct ds_io_uring_ring_head __arena *head,
			  __u64 key, __u64 value)
{
	struct ds_kv __arena *slot;
	__u32 tail, h;

	cast_kern(head);

	/* RELAXED: sole writer of prod.tail on the producer side */
	tail = arena_atomic_load(&head->prod.tail, ARENA_RELAXED);

	/* ACQUIRE: see all cons.head stores released by the consumer */
	h = arena_atomic_load(&head->cons.head, ARENA_ACQUIRE);

	if (tail - h >= head->ring_entries) {
		arena_atomic_or(&head->sq_flags, DS_IO_URING_SQ_FLAG_FULL,
				ARENA_RELAXED);
		return DS_ERROR_FULL;
	}

	slot = &head->entries[tail & head->ring_mask];
	cast_kern(slot);
	arena_atomic_store(&slot->key,   key,   ARENA_RELAXED);
	arena_atomic_store(&slot->value, value, ARENA_RELAXED);

	/* RELEASE: publish slot data before advancing prod.tail */
	arena_atomic_store(&head->prod.tail, tail + 1, ARENA_RELEASE);

	arena_atomic_and(&head->sq_flags, ~DS_IO_URING_SQ_FLAG_FULL,
			 ARENA_RELAXED);

	return DS_SUCCESS;
}
#endif /* !__BPF__ */

/**
 * ds_io_uring_insert - Enqueue a key/value pair (dispatches to lkmm or c)
 */
static inline __attribute__((unused))
int ds_io_uring_insert(struct ds_io_uring_ring_head __arena *head,
		       __u64 key, __u64 value)
{
#ifdef __BPF__
	return ds_io_uring_insert_lkmm(head, key, value);
#else
	return ds_io_uring_insert_c(head, key, value);
#endif
}

/* ========================================================================
 * POP (consumer side)
 * ======================================================================== */

/**
 * ds_io_uring_pop_lkmm - Dequeue the front entry (CONSUMER ONLY, LKMM)
 * @head: Ring head
 * @out: Output buffer to receive the dequeued key/value pair
 *
 * Returns DS_ERROR_NOT_FOUND if the ring is empty.
 * Returns DS_SUCCESS and fills *out on success.
 *
 * LKMM notes:
 * - cons.head is consumer-only in SPSC; READ_ONCE suffices (no contention).
 * - smp_load_acquire on prod.tail creates a happens-before edge with the
 *   producer's smp_store_release, ensuring entry data is visible.
 * - Address dependency from (h & ring_mask) to slot->key preserves the
 *   ordering chain; READ_ONCE on slot fields keeps the dependency intact.
 * - RELEASE on cons.head free the slot for the producer — mirrors
 *   io_uring's smp_store_release(cq_head) after CQE consumption.
 */
static inline __attribute__((unused))
int ds_io_uring_pop_lkmm(struct ds_io_uring_ring_head __arena *head,
			  struct ds_kv *out)
{
	struct ds_kv __arena *slot;
	__u32 h, t;

	cast_kern(head);

	/* LKMM: consumer-only field in SPSC; READ_ONCE for cons.head suffices */
	h = READ_ONCE(head->cons.head);

	/* LKMM: smp_load_acquire on prod.tail creates ordering with
	 * producer's smp_store_release — guarantees entry payload is visible. */
	t = smp_load_acquire(&head->prod.tail);

	if (h == t)
		return DS_ERROR_NOT_FOUND;

	/* LKMM: address dependency from h & ring_mask to slot->key provides
	 * ordering; READ_ONCE preserves the dependency chain. */
	slot = &head->entries[h & head->ring_mask];
	cast_kern(slot);
	if (out) {
		out->key   = READ_ONCE(slot->key);
		out->value = READ_ONCE(slot->value);
	}

	/* LKMM: RELEASE on cons.head — producer's smp_load_acquire(cons.head)
	 * will see this store, making the freed slot available for reuse. */
	smp_store_release(&head->cons.head, h + 1);

	return DS_SUCCESS;
}

#ifndef __BPF__
/**
 * ds_io_uring_pop_c - Dequeue the front entry (CONSUMER ONLY, C11)
 * @head: Ring head
 * @out: Output buffer to receive the dequeued key/value pair
 */
static inline __attribute__((unused))
int ds_io_uring_pop_c(struct ds_io_uring_ring_head __arena *head,
		      struct ds_kv *out)
{
	struct ds_kv __arena *slot;
	__u32 h, t;

	cast_kern(head);

	/* RELAXED: cons.head has a single writer (this consumer) in SPSC */
	h = arena_atomic_load(&head->cons.head, ARENA_RELAXED);

	/* ACQUIRE: see all data written by the producer before prod.tail */
	t = arena_atomic_load(&head->prod.tail, ARENA_ACQUIRE);

	if (h == t)
		return DS_ERROR_NOT_FOUND;

	slot = &head->entries[h & head->ring_mask];
	cast_kern(slot);
	if (out) {
		out->key   = arena_atomic_load(&slot->key,   ARENA_RELAXED);
		out->value = arena_atomic_load(&slot->value, ARENA_RELAXED);
	}

	/* RELEASE: make the consumed slot visible to the producer */
	arena_atomic_store(&head->cons.head, h + 1, ARENA_RELEASE);

	return DS_SUCCESS;
}
#endif /* !__BPF__ */

/**
 * ds_io_uring_pop - Dequeue front entry (dispatches to lkmm or c)
 */
static inline __attribute__((unused))
int ds_io_uring_pop(struct ds_io_uring_ring_head __arena *head,
		    struct ds_kv *out)
{
#ifdef __BPF__
	return ds_io_uring_pop_lkmm(head, out);
#else
	return ds_io_uring_pop_c(head, out);
#endif
}

/* ========================================================================
 * SEARCH (not applicable to SPSC ring)
 * ======================================================================== */

/**
 * ds_io_uring_search_lkmm - Search by key (unsupported for SPSC ring)
 *
 * Linear search over a live SPSC ring is inherently racy and rarely useful.
 * Returns DS_ERROR_INVALID unconditionally.
 */
static inline __attribute__((unused))
int ds_io_uring_search_lkmm(struct ds_io_uring_ring_head __arena *head,
			     __u64 key)
{
	(void)head;
	(void)key;
	return DS_ERROR_INVALID;
}

#ifndef __BPF__
static inline __attribute__((unused))
int ds_io_uring_search_c(struct ds_io_uring_ring_head __arena *head,
			  __u64 key)
{
	(void)head;
	(void)key;
	return DS_ERROR_INVALID;
}
#endif /* !__BPF__ */

static inline __attribute__((unused))
int ds_io_uring_search(struct ds_io_uring_ring_head __arena *head,
		       __u64 key)
{
#ifdef __BPF__
	return ds_io_uring_search_lkmm(head, key);
#else
	return ds_io_uring_search_c(head, key);
#endif
}

/* ========================================================================
 * VERIFY
 * ======================================================================== */

/**
 * ds_io_uring_verify_lkmm - Verify ring invariants (LKMM)
 * @head: Ring head
 *
 * Checks:
 * 1. ring_entries is a non-zero power of 2.
 * 2. The occupancy (tail - head, u32) does not exceed ring_entries.
 *
 * Returns DS_SUCCESS or DS_ERROR_CORRUPT.
 */
static inline __attribute__((unused))
int ds_io_uring_verify_lkmm(struct ds_io_uring_ring_head __arena *head)
{
	__u32 tail, h, ring_entries;

	cast_kern(head);

	ring_entries = head->ring_entries;

	/* ring_entries must be a non-zero power of 2 */
	if (!ring_entries || (ring_entries & (ring_entries - 1)))
		return DS_ERROR_CORRUPT;

	/* ring_mask must be consistent */
	if (head->ring_mask != ring_entries - 1)
		return DS_ERROR_CORRUPT;

	tail = READ_ONCE(head->prod.tail);
	h    = READ_ONCE(head->cons.head);

	/* u32 unsigned subtraction: tail - head gives occupancy regardless
	 * of wrap-around, and must never exceed ring_entries. */
	if (tail - h > ring_entries)
		return DS_ERROR_CORRUPT;

	return DS_SUCCESS;
}

#ifndef __BPF__
/**
 * ds_io_uring_verify_c - Verify ring invariants (C11)
 * @head: Ring head
 */
static inline __attribute__((unused))
int ds_io_uring_verify_c(struct ds_io_uring_ring_head __arena *head)
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

/**
 * ds_io_uring_verify - Verify ring invariants (dispatches to lkmm or c)
 */
static inline __attribute__((unused))
int ds_io_uring_verify(struct ds_io_uring_ring_head __arena *head)
{
#ifdef __BPF__
	return ds_io_uring_verify_lkmm(head);
#else
	return ds_io_uring_verify_c(head);
#endif
}
