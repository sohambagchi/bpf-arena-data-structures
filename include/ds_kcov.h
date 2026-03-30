/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/* kcov-style Flat Append Buffer for BPF Arena
 *
 * Faithful BPF arena port of Linux kcov's memory model (kernel/kcov.c).
 *
 * Key design decisions (from ring-implementation-spec.md):
 * - Flat array, not a ring: area[0] = entry count, area[1..N] = data
 * - Silent overflow drop (no wrap-around, no overflow counter)
 * - Counter FIRST ordering for interrupt re-entrancy safety
 * - barrier() (compiler-only) between counter write and data writes
 * - No smp_store_release / smp_load_acquire — kcov uses NO hardware fences
 *   on the hot path (only READ_ONCE / WRITE_ONCE / barrier())
 * - Single-writer guarantee: only one BPF program writes to area at a time
 * - Consumer resets area[0] = 0 after reading all entries
 */
#pragma once

#include "ds_api.h"

/* ========================================================================
 * CONSTANTS
 * ======================================================================== */

/*
 * Each entry occupies two consecutive words in the flat array:
 *   area[1 + (n-1)*2 + 0] = key   of the n-th entry (1-indexed)
 *   area[1 + (n-1)*2 + 1] = value of the n-th entry
 *
 * area[0] always holds the current entry count (not word count).
 */
#define KCOV_WORDS_PER_ENTRY 2

/* ========================================================================
 * DATA STRUCTURES
 * ======================================================================== */

/**
 * struct ds_kcov_buf - kcov-style flat append buffer control structure
 * @size: Total words in the area array, including the counter slot (area[0]).
 *        Maximum entries = (size - 1) / KCOV_WORDS_PER_ENTRY.
 * @area: Arena-allocated flat array. area[0] = entry count (READ_ONCE/WRITE_ONCE).
 *        area[1..] = tightly packed key/value pairs.
 *
 * INVARIANTS:
 * - size >= 1 + KCOV_WORDS_PER_ENTRY (at least one entry fits)
 * - area[0] <= (size - 1) / KCOV_WORDS_PER_ENTRY at all times
 * - area[0] is written BEFORE the corresponding data words (kcov ordering)
 */
struct ds_kcov_buf {
	__u64 size;           /* total words including counter slot (area[0]) */
	__u64 __arena *area;  /* arena-allocated flat array; area[0] = count  */
};

typedef struct ds_kcov_buf __arena ds_kcov_buf_t;

/* ========================================================================
 * INIT
 * ======================================================================== */

/**
 * ds_kcov_init_lkmm - Initialize kcov flat buffer (LKMM / BPF side)
 * @head: Buffer control structure to initialize
 * @size: Total words to allocate, including the counter slot.
 *        Usable entries = (size - 1) / KCOV_WORDS_PER_ENTRY.
 *
 * Returns: DS_SUCCESS or DS_ERROR_NOMEM / DS_ERROR_INVALID
 */
static inline __attribute__((unused))
int ds_kcov_init_lkmm(struct ds_kcov_buf __arena *head, __u64 size)
{
	__u64 __arena *area;

	cast_kern(head);
	if (size < 1 + KCOV_WORDS_PER_ENTRY)
		return DS_ERROR_INVALID;

	area = bpf_arena_alloc((__u32)(size * sizeof(__u64)));
	if (!area)
		return DS_ERROR_NOMEM;

	/* Initialize counter to zero before handing area to arena */
	cast_kern(area);
	WRITE_ONCE(area[0], 0);

	/* Store pointer and size into head — cast_user before arena field write */
	cast_user(area);
	head->area = area;
	head->size = size;

	return DS_SUCCESS;
}

#ifndef __BPF__
/**
 * ds_kcov_init_c - Initialize kcov flat buffer (C11 / userspace side)
 * @head: Buffer control structure to initialize
 * @size: Total words to allocate, including the counter slot.
 *
 * Returns: DS_SUCCESS or DS_ERROR_NOMEM / DS_ERROR_INVALID
 */
static inline __attribute__((unused))
int ds_kcov_init_c(struct ds_kcov_buf __arena *head, __u64 size)
{
	__u64 __arena *area;

	cast_kern(head);
	if (size < 1 + KCOV_WORDS_PER_ENTRY)
		return DS_ERROR_INVALID;

	area = bpf_arena_alloc((__u32)(size * sizeof(__u64)));
	if (!area)
		return DS_ERROR_NOMEM;

	cast_kern(area);
	/* LKMM: relaxed store is sufficient here — no concurrent readers yet */
	arena_atomic_store(&area[0], (__u64)0, ARENA_RELAXED);

	cast_user(area);
	arena_atomic_store(&head->area, area, ARENA_RELAXED);
	arena_atomic_store(&head->size, size, ARENA_RELAXED);

	return DS_SUCCESS;
}
#endif /* !__BPF__ */

/**
 * ds_kcov_init - Initialize kcov flat buffer (router)
 */
static inline __attribute__((unused))
int ds_kcov_init(struct ds_kcov_buf __arena *head, __u64 size)
{
#ifdef __BPF__
	return ds_kcov_init_lkmm(head, size);
#else
	return ds_kcov_init_c(head, size);
#endif
}

/* ========================================================================
 * INSERT (PRODUCER — APPEND)
 * ======================================================================== */

/**
 * ds_kcov_insert_lkmm - Append a key/value entry (LKMM / BPF side)
 * @head: Buffer control structure
 * @key:  Entry key word
 * @value: Entry value word
 *
 * Faithful kcov PC-trace ordering (kernel/kcov.c):
 *   1. READ_ONCE(area[0]) — load current count
 *   2. Compute tentative new count and slot indices
 *   3. Bounds check — drop silently if full
 *   4. WRITE_ONCE(area[0], new_count) — claim slot FIRST
 *   5. barrier() — compiler fence only, no hardware fence
 *   6. WRITE_ONCE(area[start], key); WRITE_ONCE(area[start+1], value)
 *
 * The counter-first ordering guarantees interrupt re-entrancy: a recursive
 * invocation sees the updated count and writes to a different slot, never
 * colliding with the outer invocation's data words.
 *
 * Returns: DS_SUCCESS or DS_ERROR_FULL / DS_ERROR_INVALID
 */
static inline __attribute__((unused))
int ds_kcov_insert_lkmm(struct ds_kcov_buf __arena *head, __u64 key, __u64 value)
{
	__u64 __arena *area;
	__u64 count, start_index, end_index;

	cast_kern(head);
	/* LKMM: address dependency from head dereference to area load */
	area = READ_ONCE(head->area);
	if (!area)
		return DS_ERROR_INVALID;

	/* LKMM: address dependency from area pointer to area[0] load */
	cast_kern(area);
	count = READ_ONCE(area[0]) + 1; /* tentative new count after this entry */

	start_index = 1 + (count - 1) * KCOV_WORDS_PER_ENTRY;
	end_index   = start_index + KCOV_WORDS_PER_ENTRY;

	if (end_index > head->size)
		return DS_ERROR_FULL;

	/* kcov PC-trace ordering: counter FIRST, then data.
	 * LKMM: barrier() is compiler-only — no hardware fence needed.
	 * This ordering ensures interrupt re-entrancy: a recursive invocation
	 * sees the updated count and writes to a different slot. */
	WRITE_ONCE(area[0], count);
	barrier();
	WRITE_ONCE(area[start_index],     key);
	WRITE_ONCE(area[start_index + 1], value);

	return DS_SUCCESS;
}

#ifndef __BPF__
/**
 * ds_kcov_insert_c - Append a key/value entry (C11 / userspace side)
 * @head: Buffer control structure
 * @key:  Entry key word
 * @value: Entry value word
 *
 * Mirrors the LKMM variant's counter-first ordering using C11 relaxed
 * atomics and a compiler fence (asm volatile("" ::: "memory")) in place
 * of kcov's barrier().  No acquire/release needed — single-writer model.
 *
 * Returns: DS_SUCCESS or DS_ERROR_FULL / DS_ERROR_INVALID
 */
static inline __attribute__((unused))
int ds_kcov_insert_c(struct ds_kcov_buf __arena *head, __u64 key, __u64 value)
{
	__u64 __arena *area;
	__u64 count, start_index, end_index;

	cast_kern(head);
	/* LKMM: relaxed load — area pointer is stable after init */
	area = arena_atomic_load(&head->area, ARENA_RELAXED);
	if (!area)
		return DS_ERROR_INVALID;

	cast_kern(area);
	/* LKMM: relaxed load — single writer owns area[0] on the insert path */
	count = arena_atomic_load(&area[0], ARENA_RELAXED) + 1;

	start_index = 1 + (count - 1) * KCOV_WORDS_PER_ENTRY;
	end_index   = start_index + KCOV_WORDS_PER_ENTRY;

	if (end_index > arena_atomic_load(&head->size, ARENA_RELAXED))
		return DS_ERROR_FULL;

	/* Counter FIRST — mirrors kcov's WRITE_ONCE(area[0]) + barrier() */
	arena_atomic_store(&area[0], count, ARENA_RELAXED);
	/* Compiler fence: prevent reordering of counter write past data writes */
	asm volatile("" ::: "memory");
	arena_atomic_store(&area[start_index],     key,   ARENA_RELAXED);
	arena_atomic_store(&area[start_index + 1], value, ARENA_RELAXED);

	return DS_SUCCESS;
}
#endif /* !__BPF__ */

/**
 * ds_kcov_insert - Append a key/value entry (router)
 */
static inline __attribute__((unused))
int ds_kcov_insert(struct ds_kcov_buf __arena *head, __u64 key, __u64 value)
{
#ifdef __BPF__
	return ds_kcov_insert_lkmm(head, key, value);
#else
	return ds_kcov_insert_c(head, key, value);
#endif
}

/* ========================================================================
 * POP (CONSUMER — STACK-DISCIPLINE DESTRUCTIVE READ)
 * ======================================================================== */

/**
 * ds_kcov_pop_lkmm - Remove and return the most-recently inserted entry (LKMM)
 * @head: Buffer control structure
 * @out:  Output key/value pair; may be NULL to discard the entry
 *
 * kcov has no consumer head pointer — userspace is expected to read all
 * entries at once (area[1..area[0]]) then reset area[0] = 0.  For the
 * framework relay that needs one-at-a-time delivery, this pop implements
 * a stack (LIFO) discipline: it reads the newest entry (highest slot),
 * then decrements area[0] to reclaim that slot.
 *
 * Ordering: data read BEFORE counter decrement (reverse of insert), with
 * a compiler barrier() between them.
 *
 * Returns: DS_SUCCESS or DS_ERROR_NOT_FOUND (empty) / DS_ERROR_INVALID
 */
static inline __attribute__((unused))
int ds_kcov_pop_lkmm(struct ds_kcov_buf __arena *head, struct ds_kv *out)
{
	__u64 __arena *area;
	__u64 count, start_index;

	cast_kern(head);
	/* LKMM: address dependency from head dereference to area load */
	area = READ_ONCE(head->area);
	if (!area)
		return DS_ERROR_INVALID;

	/* LKMM: address dependency from area pointer to area[0] load */
	cast_kern(area);
	count = READ_ONCE(area[0]);
	if (count == 0)
		return DS_ERROR_NOT_FOUND;

	start_index = 1 + (count - 1) * KCOV_WORDS_PER_ENTRY;

	if (out) {
		out->key   = READ_ONCE(area[start_index]);
		out->value = READ_ONCE(area[start_index + 1]);
	}

	/* Data read before counter decrement — reverse of insert ordering.
	 * barrier() (compiler-only) prevents the counter write from floating
	 * above the data reads. */
	barrier();
	WRITE_ONCE(area[0], count - 1);

	return DS_SUCCESS;
}

#ifndef __BPF__
/**
 * ds_kcov_pop_c - Remove and return the most-recently inserted entry (C11)
 * @head: Buffer control structure
 * @out:  Output key/value pair; may be NULL to discard the entry
 *
 * Returns: DS_SUCCESS or DS_ERROR_NOT_FOUND (empty) / DS_ERROR_INVALID
 */
static inline __attribute__((unused))
int ds_kcov_pop_c(struct ds_kcov_buf __arena *head, struct ds_kv *out)
{
	__u64 __arena *area;
	__u64 count, start_index;

	cast_kern(head);
	area = arena_atomic_load(&head->area, ARENA_RELAXED);
	if (!area)
		return DS_ERROR_INVALID;

	cast_kern(area);
	count = arena_atomic_load(&area[0], ARENA_RELAXED);
	if (count == 0)
		return DS_ERROR_NOT_FOUND;

	start_index = 1 + (count - 1) * KCOV_WORDS_PER_ENTRY;

	if (out) {
		out->key   = arena_atomic_load(&area[start_index],     ARENA_RELAXED);
		out->value = arena_atomic_load(&area[start_index + 1], ARENA_RELAXED);
	}

	/* Compiler fence: data reads must not sink below the counter decrement */
	asm volatile("" ::: "memory");
	arena_atomic_store(&area[0], count - 1, ARENA_RELAXED);

	return DS_SUCCESS;
}
#endif /* !__BPF__ */

/**
 * ds_kcov_pop - Remove and return the most-recently inserted entry (router)
 */
static inline __attribute__((unused))
int ds_kcov_pop(struct ds_kcov_buf __arena *head, struct ds_kv *out)
{
#ifdef __BPF__
	return ds_kcov_pop_lkmm(head, out);
#else
	return ds_kcov_pop_c(head, out);
#endif
}

/* ========================================================================
 * SEARCH — NOT SUPPORTED
 * ======================================================================== */

/**
 * ds_kcov_search_lkmm - Key lookup (unsupported for flat append buffers)
 *
 * kcov is a pure append buffer with no key-based indexing.  The consumer
 * bulk-reads all entries by position, not by key.
 *
 * Returns: DS_ERROR_INVALID always
 */
static inline __attribute__((unused))
int ds_kcov_search_lkmm(struct ds_kcov_buf __arena *head, __u64 key)
{
	(void)head;
	(void)key;
	return DS_ERROR_INVALID;
}

#ifndef __BPF__
static inline __attribute__((unused))
int ds_kcov_search_c(struct ds_kcov_buf __arena *head, __u64 key)
{
	(void)head;
	(void)key;
	return DS_ERROR_INVALID;
}
#endif /* !__BPF__ */

/**
 * ds_kcov_search - Key lookup router (always unsupported)
 */
static inline __attribute__((unused))
int ds_kcov_search(struct ds_kcov_buf __arena *head, __u64 key)
{
#ifdef __BPF__
	return ds_kcov_search_lkmm(head, key);
#else
	return ds_kcov_search_c(head, key);
#endif
}

/* ========================================================================
 * VERIFY
 * ======================================================================== */

/**
 * ds_kcov_verify_lkmm - Verify flat buffer invariants (LKMM / BPF side)
 * @head: Buffer control structure
 *
 * Checks:
 *   1. area pointer is non-NULL
 *   2. area[0] (entry count) does not exceed the maximum capacity derived
 *      from head->size: max_entries = (size - 1) / KCOV_WORDS_PER_ENTRY
 *
 * Returns: DS_SUCCESS or DS_ERROR_CORRUPT / DS_ERROR_INVALID
 */
static inline __attribute__((unused))
int ds_kcov_verify_lkmm(struct ds_kcov_buf __arena *head)
{
	__u64 __arena *area;
	__u64 count, max_entries;

	cast_kern(head);
	/* LKMM: address dependency from head to area load */
	area = READ_ONCE(head->area);
	if (!area)
		return DS_ERROR_INVALID;

	cast_kern(area);
	count = READ_ONCE(area[0]);

	/* head->size includes the counter slot; subtract it before dividing */
	if (head->size < 1)
		return DS_ERROR_CORRUPT;

	max_entries = (head->size - 1) / KCOV_WORDS_PER_ENTRY;

	if (count > max_entries)
		return DS_ERROR_CORRUPT;

	return DS_SUCCESS;
}

#ifndef __BPF__
/**
 * ds_kcov_verify_c - Verify flat buffer invariants (C11 / userspace side)
 * @head: Buffer control structure
 *
 * Returns: DS_SUCCESS or DS_ERROR_CORRUPT / DS_ERROR_INVALID
 */
static inline __attribute__((unused))
int ds_kcov_verify_c(struct ds_kcov_buf __arena *head)
{
	__u64 __arena *area;
	__u64 count, size, max_entries;

	cast_kern(head);
	area = arena_atomic_load(&head->area, ARENA_RELAXED);
	if (!area)
		return DS_ERROR_INVALID;

	cast_kern(area);
	count = arena_atomic_load(&area[0], ARENA_RELAXED);
	size  = arena_atomic_load(&head->size, ARENA_RELAXED);

	if (size < 1)
		return DS_ERROR_CORRUPT;

	max_entries = (size - 1) / KCOV_WORDS_PER_ENTRY;

	if (count > max_entries)
		return DS_ERROR_CORRUPT;

	return DS_SUCCESS;
}
#endif /* !__BPF__ */

/**
 * ds_kcov_verify - Verify flat buffer invariants (router)
 */
static inline __attribute__((unused))
int ds_kcov_verify(struct ds_kcov_buf __arena *head)
{
#ifdef __BPF__
	return ds_kcov_verify_lkmm(head);
#else
	return ds_kcov_verify_c(head);
#endif
}
