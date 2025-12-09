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

#ifdef __BPF__

/* ========================================================================
 * BPF KERNEL-SIDE IMPLEMENTATION
 * ======================================================================== */

/* Simple bump allocator state - stored in arena space */
static void __arena *__arena alloc_base;
static __u64 __arena alloc_offset;

/**
 * bpf_arena_alloc - Allocate memory from BPF arena
 * @size: Size in bytes to allocate
 * 
 * Returns: Arena pointer (__arena) to allocated memory, or NULL on failure
 * 
 * IMPORTANT CAST BEHAVIOR with clang-20:
 * - __arena means address_space(1) attribute
 * - cast_kern() and cast_user() are COMPILE-TIME NOPs
 * - LLVM automatically inserts address space casts in generated BPF bytecode
 * - Return type void __arena* tells LLVM this is an arena pointer
 * - DO NOT manually call cast_kern/cast_user on modern arena pointers
 */
static inline void __arena* bpf_arena_alloc(unsigned int size)
{
	__u64 offset;

	size = round_up(size, 8);
	if (size >= PAGE_SIZE)
		return NULL;

	/* First allocation - get pages from arena */
	if (!alloc_base) {
		/* Allocate 100 pages (400KB) - returns arena pointer */
		alloc_base = bpf_arena_alloc_pages(&arena, NULL, 100, NUMA_NO_NODE, 0);
		if (!alloc_base)
			return NULL;
		alloc_offset = 0;
	}

	/* Atomically bump the offset */
	offset = __sync_fetch_and_add(&alloc_offset, size);
	if (offset + size > 100 * PAGE_SIZE)
		return NULL;

	/* Return arena pointer - LLVM preserves address space through arithmetic */
	cast_kern(alloc_base);
	return alloc_base + offset;
}

/**
 * bpf_arena_free - Free memory allocated from BPF arena
 * @addr: Arena pointer to free
 * 
 * Note: Current bump allocator doesn't actually free - memory is reused
 * when the program restarts. This is a placeholder for future implementations.
 */
static inline void bpf_arena_free(void __arena *addr)
{
	/* Bump allocator - no-op for now */
	(void)addr;
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
