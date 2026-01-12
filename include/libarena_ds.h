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

/* ========================================================================
 * BPF ARENA ATOMICS (C11)
 * ========================================================================
 * Modern atomic primitives with explicit memory ordering for optimal
 * performance and correctness in concurrent data structures.
 * 
 * NOTE: These must be defined BEFORE the allocator functions that use them.
 * ======================================================================== */

/* Memory ordering constants */
#define ARENA_RELAXED __ATOMIC_RELAXED
#define ARENA_ACQUIRE __ATOMIC_ACQUIRE
#define ARENA_RELEASE __ATOMIC_RELEASE
#define ARENA_ACQ_REL __ATOMIC_ACQ_REL
#define ARENA_SEQ_CST __ATOMIC_SEQ_CST

/**
 * arena_atomic_cmpxchg - Compare and exchange (CAS) with memory ordering
 * @ptr: Pointer to the location
 * @old_val: Expected old value
 * @new_val: New value to set
 * @success_mo: Memory order on success
 * @failure_mo: Memory order on failure
 * 
 * Returns: The value that was at *ptr before the operation (matches __sync behavior)
 * 
 * Usage:
 * - Acquiring lock: ARENA_ACQUIRE / ARENA_RELAXED
 * - General synchronization: ARENA_ACQ_REL / ARENA_ACQUIRE
 * 
 * Note: For pointer types, we cast to unsigned long to avoid address space issues.
 */
#define arena_atomic_cmpxchg(ptr, old_val, new_val, success_mo, failure_mo) \
({ \
	unsigned long __expected = (unsigned long)(old_val); \
	__atomic_compare_exchange_n((unsigned long *)(ptr), &__expected, \
				    (unsigned long)(new_val), 0, \
				    (success_mo), (failure_mo)); \
	(__typeof__(*(ptr)))__expected; \
})

/**
 * arena_atomic_exchange - Atomically exchange (swap) value
 * @ptr: Pointer to value
 * @val: New value to set
 * @mo: Memory order
 * 
 * Returns: The old value that was at *ptr
 */
#define arena_atomic_exchange(ptr, val, mo) \
	__atomic_exchange_n((ptr), (val), (mo))

/**
 * arena_atomic_add - Atomically add (fetch-add)
 * @ptr: Pointer to value
 * @val: Value to add
 * @mo: Memory order
 * 
 * Returns: The old value before addition
 */
#define arena_atomic_add(ptr, val, mo) \
	__atomic_fetch_add((ptr), (val), (mo))

/**
 * arena_atomic_sub - Atomically subtract (fetch-sub)
 * @ptr: Pointer to value
 * @val: Value to subtract
 * @mo: Memory order
 * 
 * Returns: The old value before subtraction
 */
#define arena_atomic_sub(ptr, val, mo) \
	__atomic_fetch_sub((ptr), (val), (mo))

/**
 * arena_atomic_and - Atomically AND (fetch-and)
 * @ptr: Pointer to value
 * @val: Value to AND with
 * @mo: Memory order
 */
#define arena_atomic_and(ptr, val, mo) \
	__atomic_fetch_and((ptr), (val), (mo))

/**
 * arena_atomic_or - Atomically OR (fetch-or)
 * @ptr: Pointer to value
 * @val: Value to OR with
 * @mo: Memory order
 */
#define arena_atomic_or(ptr, val, mo) \
	__atomic_fetch_or((ptr), (val), (mo))

/**
 * arena_atomic_load - Atomically load value
 * @ptr: Pointer to value
 * @mo: Memory order
 */
#define arena_atomic_load(ptr, mo) __atomic_load_n((ptr), (mo))

/**
 * arena_atomic_store - Atomically store value
 * @ptr: Pointer to value
 * @val: Value to store
 * @mo: Memory order
 */
#define arena_atomic_store(ptr, val, mo) __atomic_store_n((ptr), (val), (mo))

/* Convenience wrappers for common patterns */
#define arena_atomic_inc(ptr) arena_atomic_add((ptr), 1, ARENA_RELAXED)
#define arena_atomic_dec(ptr) arena_atomic_sub((ptr), 1, ARENA_RELAXED)
#define arena_memory_barrier() __atomic_thread_fence(ARENA_SEQ_CST)

/* ========================================================================
 * BPF ARENA MEMORY ALLOCATOR
 * ======================================================================== */

#define NR_CPUS (sizeof(struct cpumask) * 8)

static void __arena * __arena page_frag_cur_page[NR_CPUS];
static int __arena page_frag_cur_offset[NR_CPUS];

/* Helper function to handle the "Slow Path" allocation */
static inline void __arena* bpf_arena_refill_page(int cpu)
{
    void __arena *page;
    __u64 __arena *obj_cnt;

    // 1. Allocate a fresh page
    page = bpf_arena_alloc_pages(&arena, NULL, 1, NUMA_NO_NODE, 0);
    if (!page)
        return NULL;

    // 2. Prepare the page
    cast_kern(page);

    // 3. Update global per-CPU state
    page_frag_cur_page[cpu] = page;
    page_frag_cur_offset[cpu] = PAGE_SIZE - 8;

    // 4. Initialize object counter at the end of the page
    obj_cnt = page + PAGE_SIZE - 8;
    *obj_cnt = 0;

    return page;
}

/* Main allocation function */
static inline void __arena* bpf_arena_alloc(unsigned int size)
{
    __u64 __arena *obj_cnt;
    __u32 cpu = bpf_get_smp_processor_id();
    void __arena *page = page_frag_cur_page[cpu];
    int __arena *cur_offset = &page_frag_cur_offset[cpu];
    int offset;

    size = round_up(size, 8);
    if (size >= PAGE_SIZE - 8)
        return NULL;

    // CHECK: Do we need to refill?
    // Condition A: We don't have a page yet (!page)
    // Condition B: We have a page, but not enough space (*cur_offset - size < 0)
    if (!page || (*cur_offset - size < 0)) {
        page = bpf_arena_refill_page(cpu);
        if (!page)
            return NULL;
        // Note: The refill helper has already reset *cur_offset to (PAGE_SIZE - 8)
    } else {
        // FAST PATH: Prepare existing page
        cast_kern(page);
    }

    // ALLOCATION: At this point, 'page' is valid and has sufficient space.
    offset = *cur_offset - size;
    obj_cnt = page + PAGE_SIZE - 8;

    (*obj_cnt)++;
    *cur_offset = offset;

    return page + offset;
}

static inline void bpf_arena_free(void __arena *addr)
{
	__u64 __arena *obj_cnt;

	addr = (void __arena *)(((long)addr) & ~(PAGE_SIZE - 1));
	obj_cnt = addr + PAGE_SIZE - 8;
	if (--(*obj_cnt) == 0)
		bpf_arena_free_pages(&arena, addr, 1);
}

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
 * USERSPACE SYNCHRONIZATION PRIMITIVES (C11)
 * ========================================================================
 * Userspace uses the same C11 atomic API for consistency.
 * ======================================================================== */

/* Memory ordering constants */
#define ARENA_RELAXED __ATOMIC_RELAXED
#define ARENA_ACQUIRE __ATOMIC_ACQUIRE
#define ARENA_RELEASE __ATOMIC_RELEASE
#define ARENA_ACQ_REL __ATOMIC_ACQ_REL
#define ARENA_SEQ_CST __ATOMIC_SEQ_CST

/* Atomic operations - identical to BPF side */
#define arena_atomic_cmpxchg(ptr, old_val, new_val, success_mo, failure_mo) \
({ \
	unsigned long __expected = (unsigned long)(old_val); \
	__atomic_compare_exchange_n((unsigned long *)(ptr), &__expected, \
				    (unsigned long)(new_val), 0, \
				    (success_mo), (failure_mo)); \
	(__typeof__(*(ptr)))__expected; \
})

#define arena_atomic_exchange(ptr, val, mo) \
	__atomic_exchange_n((ptr), (val), (mo))

#define arena_atomic_add(ptr, val, mo) \
	__atomic_fetch_add((ptr), (val), (mo))

#define arena_atomic_sub(ptr, val, mo) \
	__atomic_fetch_sub((ptr), (val), (mo))

#define arena_atomic_and(ptr, val, mo) \
	__atomic_fetch_and((ptr), (val), (mo))

#define arena_atomic_or(ptr, val, mo) \
	__atomic_fetch_or((ptr), (val), (mo))

#define arena_atomic_load(ptr, mo)       __atomic_load_n((ptr), (mo))
#define arena_atomic_store(ptr, val, mo) __atomic_store_n((ptr), (val), (mo))

/* Convenience wrappers */
#define arena_atomic_inc(ptr) arena_atomic_add((ptr), 1, ARENA_RELAXED)
#define arena_atomic_dec(ptr) arena_atomic_sub((ptr), 1, ARENA_RELAXED)
#define arena_memory_barrier() __atomic_thread_fence(ARENA_SEQ_CST)

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
