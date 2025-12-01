/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/* Copyright (c) 2024 Meta Platforms, Inc. and affiliates. */
/* Extended for concurrent data structure testing framework */
#pragma once

#include "bpf_arena_common.h"

#ifndef __round_mask
#define __round_mask(x, y) ((__typeof__(x))((y)-1))
#endif
#ifndef round_up
#define round_up(x, y) ((((x)-1) | __round_mask(x, y))+1)
#endif

/* ========================================================================
 * ARENA STATISTICS AND TRACKING
 * ======================================================================== */

struct arena_stats {
	__u64 total_allocs;
	__u64 total_frees;
	__u64 current_allocations;
	__u64 bytes_allocated;
	__u64 bytes_freed;
	__u64 failed_allocs;
};

#ifdef __BPF__

/* ========================================================================
 * BPF KERNEL-SIDE IMPLEMENTATION
 * ======================================================================== */

#define NR_CPUS (sizeof(struct cpumask) * 8)

/* Per-CPU page fragment allocator state */
static void __arena * __arena page_frag_cur_page[NR_CPUS];
static int __arena page_frag_cur_offset[NR_CPUS];

/* Global statistics (can be accessed from userspace via skeleton->bss) */
static struct arena_stats __arena global_stats;

/**
 * bpf_arena_alloc - Allocate memory from BPF arena
 * @size: Size in bytes to allocate
 * 
 * Returns: Arena pointer to allocated memory, or NULL on failure
 * 
 * This implements a per-CPU page fragment allocator to avoid locking.
 * Each CPU maintains its own current page and offset. Memory is allocated
 * by decrementing the offset. When a page is exhausted, a new page is
 * requested from the kernel. Each page maintains a reference count in its
 * last 8 bytes.
 */
static inline void __arena* bpf_arena_alloc(unsigned int size)
{
	__u64 __arena *obj_cnt;
	__u32 cpu = bpf_get_smp_processor_id();
	void __arena *page = page_frag_cur_page[cpu];
	int __arena *cur_offset = &page_frag_cur_offset[cpu];
	int offset;

	size = round_up(size, 8);
	if (size >= PAGE_SIZE - 8) {
		__sync_fetch_and_add(&global_stats.failed_allocs, 1);
		return NULL;
	}

	if (!page) {
refill:
		page = bpf_arena_alloc_pages(&arena, NULL, 1, NUMA_NO_NODE, 0);
		if (!page) {
			__sync_fetch_and_add(&global_stats.failed_allocs, 1);
			return NULL;
		}
		cast_kern(page);
		page_frag_cur_page[cpu] = page;
		*cur_offset = PAGE_SIZE - 8;
		obj_cnt = page + PAGE_SIZE - 8;
		*obj_cnt = 0;
	} else {
		cast_kern(page);
		obj_cnt = page + PAGE_SIZE - 8;
	}

	offset = *cur_offset - size;
	if (offset < 0)
		goto refill;

	(*obj_cnt)++;
	*cur_offset = offset;
	
	/* Update statistics */
	__sync_fetch_and_add(&global_stats.total_allocs, 1);
	__sync_fetch_and_add(&global_stats.current_allocations, 1);
	__sync_fetch_and_add(&global_stats.bytes_allocated, size);

	return page + offset;
}

/**
 * bpf_arena_free - Free memory allocated from BPF arena
 * @addr: Arena pointer to free
 * 
 * Decrements the reference count for the page containing this address.
 * When the count reaches zero, returns the page to the kernel.
 */
static inline void bpf_arena_free(void __arena *addr)
{
	__u64 __arena *obj_cnt;

	if (!addr)
		return;

	addr = (void __arena *)(((long)addr) & ~(PAGE_SIZE - 1));
	obj_cnt = addr + PAGE_SIZE - 8;
	
	/* Update statistics */
	__sync_fetch_and_add(&global_stats.total_frees, 1);
	__sync_fetch_and_sub(&global_stats.current_allocations, 1);
	
	if (--(*obj_cnt) == 0)
		bpf_arena_free_pages(&arena, addr, 1);
}

/**
 * bpf_arena_get_stats - Get current arena statistics
 * @stats: Pointer to stats structure to fill
 */
static inline void bpf_arena_get_stats(struct arena_stats *stats)
{
	stats->total_allocs = global_stats.total_allocs;
	stats->total_frees = global_stats.total_frees;
	stats->current_allocations = global_stats.current_allocations;
	stats->bytes_allocated = global_stats.bytes_allocated;
	stats->bytes_freed = global_stats.bytes_freed;
	stats->failed_allocs = global_stats.failed_allocs;
}

/**
 * bpf_arena_reset_stats - Reset arena statistics
 */
static inline void bpf_arena_reset_stats(void)
{
	global_stats.total_allocs = 0;
	global_stats.total_frees = 0;
	global_stats.current_allocations = 0;
	global_stats.bytes_allocated = 0;
	global_stats.bytes_freed = 0;
	global_stats.failed_allocs = 0;
}

/* ========================================================================
 * SYNCHRONIZATION PRIMITIVES FOR CONCURRENT DATA STRUCTURES
 * ======================================================================== */

/**
 * arena_atomic_cmpxchg - Compare and exchange for arena pointers
 * @ptr: Pointer to the location
 * @old: Expected old value
 * @new: New value to set
 * 
 * Returns: The value that was at *ptr before the operation
 */
#define arena_atomic_cmpxchg(ptr, old, new) \
	__sync_val_compare_and_swap(ptr, old, new)

/**
 * arena_atomic_inc - Atomically increment
 * @ptr: Pointer to value to increment
 */
#define arena_atomic_inc(ptr) __sync_fetch_and_add(ptr, 1)

/**
 * arena_atomic_dec - Atomically decrement
 * @ptr: Pointer to value to decrement
 */
#define arena_atomic_dec(ptr) __sync_fetch_and_sub(ptr, 1)

/**
 * arena_atomic_add - Atomically add
 * @ptr: Pointer to value
 * @val: Value to add
 */
#define arena_atomic_add(ptr, val) __sync_fetch_and_add(ptr, val)

/**
 * arena_memory_barrier - Full memory barrier
 */
#define arena_memory_barrier() __sync_synchronize()

#else /* !__BPF__ */

/* ========================================================================
 * USERSPACE IMPLEMENTATION
 * ======================================================================== */

#include <stdatomic.h>
#include <string.h>

/**
 * In userspace, we don't allocate - we only access arena memory
 * that was allocated by the BPF side. These are stubs.
 */
static inline void __arena* bpf_arena_alloc(unsigned int size __attribute__((unused)))
{
	return NULL;
}

static inline void bpf_arena_free(void __arena *addr __attribute__((unused)))
{
	/* No-op in userspace */
}

/**
 * Userspace can read statistics from the BPF program's BSS section
 * via the skeleton: skel->bss->global_stats
 */
static inline void bpf_arena_get_stats(struct arena_stats *stats __attribute__((unused)))
{
	/* In userspace, access via skeleton: memcpy(stats, &skel->bss->global_stats, sizeof(*stats)) */
}

static inline void bpf_arena_reset_stats(void)
{
	/* In userspace, reset via skeleton: memset(&skel->bss->global_stats, 0, sizeof(struct arena_stats)) */
}

/* ========================================================================
 * USERSPACE SYNCHRONIZATION PRIMITIVES
 * ======================================================================== */

/**
 * arena_atomic_cmpxchg - Compare and exchange for arena pointers
 */
#define arena_atomic_cmpxchg(ptr, old, new) \
	__sync_val_compare_and_swap(ptr, old, new)

/**
 * arena_atomic_inc - Atomically increment
 */
#define arena_atomic_inc(ptr) __sync_fetch_and_add(ptr, 1)

/**
 * arena_atomic_dec - Atomically decrement
 */
#define arena_atomic_dec(ptr) __sync_fetch_and_sub(ptr, 1)

/**
 * arena_atomic_add - Atomically add
 */
#define arena_atomic_add(ptr, val) __sync_fetch_and_add(ptr, val)

/**
 * arena_memory_barrier - Full memory barrier
 */
#define arena_memory_barrier() __sync_synchronize()

#endif /* __BPF__ */

/* ========================================================================
 * BPF VERIFIER COMPATIBILITY
 * ======================================================================== */

/* In BPF context, can_loop is provided by bpf_experimental.h
 * In userspace, define it as 1 to allow compilation */
#ifndef can_loop
#define can_loop 1
#endif

/* ========================================================================
 * COMMON DEBUGGING AND VALIDATION HELPERS
 * ======================================================================== */

/**
 * arena_validate_ptr - Check if pointer is within arena bounds
 * @ptr: Pointer to validate
 * 
 * Returns: 1 if valid, 0 if invalid
 * Note: This is a basic check. In production, you'd validate against
 * actual arena boundaries.
 */
static inline int arena_validate_ptr(void __arena *ptr)
{
	if (!ptr)
		return 0;
	
	/* Check for poison values */
	if (ptr == (void __arena *)0x100 || ptr == (void __arena *)0x122)
		return 0;
	
	return 1;
}

/* ========================================================================
 * COMPATIBILITY MACROS
 * ======================================================================== */

/* Provide backward compatibility with existing code */
#define bpf_alloc bpf_arena_alloc
#define bpf_free bpf_arena_free
