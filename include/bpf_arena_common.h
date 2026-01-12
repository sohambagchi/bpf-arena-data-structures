/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/* Copyright (c) 2024 Meta Platforms, Inc. and affiliates. */
#pragma once

#ifndef NUMA_NO_NODE
#define	NUMA_NO_NODE	(-1)
#endif

#ifdef __BPF__
#ifndef WRITE_ONCE
#define WRITE_ONCE(x, val) ((*(volatile typeof(x) *) &(x)) = (val))
#endif

#ifndef READ_ONCE
#define READ_ONCE(x) (*(volatile typeof(x) *)&(x))
#endif

#ifndef barrier
#define barrier()		asm volatile("" ::: "memory")
#endif
#endif

#ifndef __BPF__
#ifndef WRITE_ONCE
#define WRITE_ONCE(x, val) \
	atomic_store_explicit(&(x), (val), memory_order_relaxed)
#endif
#ifndef READ_ONCE
#define READ_ONCE(x) \
	atomic_load_explicit(&(x), memory_order_relaxed)
#endif
#ifndef barrier
#define barrier()		atomic_thread_fence(memory_order_seq_cst)
#endif
#endif


#ifdef __BPF__
#ifndef smp_store_release
# define smp_store_release(p, v)		\
do {						\
	barrier();				\
	WRITE_ONCE(*p, v);			\
} while (0)
#endif

#ifndef smp_load_acquire
# define smp_load_acquire(p)			\
({						\
	uintptr_t __p = (uintptr_t)READ_ONCE(*p);	\
	barrier();				\
	(typeof(*p))__p;					\
})
#endif
#endif /* __BPF__ */

/*
 * Userspace atomic definitions
 * These provide C11 atomic semantics for userspace programs (skeleton and usertest)
 * using __atomic_load_n and __atomic_store_n with proper memory ordering.
 * 
 * Note: __atomic_store_n returns void (unlike smp_store_release which could be used in expressions)
 *       __atomic_load_n returns the loaded value
 */
#ifndef __BPF__
#include <stdatomic.h>

#ifndef smp_store_release
# define smp_store_release(p, v) \
	__atomic_store_n((p), (v), memory_order_release)
#endif

#ifndef smp_load_acquire
# define smp_load_acquire(p) \
	__atomic_load_n((p), memory_order_acquire)
#endif

#endif /* !__BPF__ */

#ifndef arena_container_of
#define arena_container_of(ptr, type, member)			\
	({							\
		void __arena *__mptr = (void __arena *)(ptr);	\
		((type *)(__mptr - offsetof(type, member)));	\
	})
#endif

#ifdef __BPF__ /* when compiled as bpf program */

#ifndef PAGE_SIZE
#define PAGE_SIZE __PAGE_SIZE
/*
 * for older kernels try sizeof(struct genradix_node)
 * or flexible:
 * static inline long __bpf_page_size(void) {
 *   return bpf_core_enum_value(enum page_size_enum___l, __PAGE_SIZE___l) ?: sizeof(struct genradix_node);
 * }
 * but generated code is not great.
 */
#endif

#if defined(__BPF_FEATURE_ADDR_SPACE_CAST) && !defined(BPF_ARENA_FORCE_ASM)
#define __arena __attribute__((address_space(1)))
#define __arena_global __attribute__((address_space(1)))
#define cast_kern(ptr) /* nop for bpf prog. emitted by LLVM */
#define cast_user(ptr) /* nop for bpf prog. emitted by LLVM */
#else
#define __arena
#define __arena_global SEC(".addr_space.1")
#define cast_kern(ptr) bpf_addr_space_cast(ptr, 0, 1)
#define cast_user(ptr) bpf_addr_space_cast(ptr, 1, 0)
#endif

void __arena* bpf_arena_alloc_pages(void *map, void __arena *addr, __u32 page_cnt,
				    int node_id, __u64 flags) __ksym __weak;
void bpf_arena_free_pages(void *map, void __arena *ptr, __u32 page_cnt) __ksym __weak;

#else /* when compiled as user space code */

#define __arena
#define __arg_arena
#define cast_kern(ptr) /* nop for user space */
#define cast_user(ptr) /* nop for user space */
extern char arena[1] __attribute__((weak));

#ifndef offsetof
#define offsetof(type, member)  ((unsigned long)&((type *)0)->member)
#endif

static inline void __arena* bpf_arena_alloc_pages(void *map __attribute__((unused)),
		void *addr __attribute__((unused)),
		__u32 page_cnt __attribute__((unused)),
		int node_id __attribute__((unused)),
		__u64 flags __attribute__((unused)))
{
	return NULL;
}
static inline void bpf_arena_free_pages(void *map __attribute__((unused)),
		void __arena *ptr __attribute__((unused)),
		__u32 page_cnt __attribute__((unused)))
{
}

#endif
