#pragma once

#include <errno.h>
#include <inttypes.h>
#include <linux/types.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/*
 * Important: include libarena_ds first so its #pragma once prevents re-entry
 * via ds_api.h later. Then macro-redirect bpf_arena_alloc/free for tests.
 */
#include "libarena_ds.h"

/* A simple, thread-safe bump allocator to emulate "arena alloc" in userspace. */
#ifndef USERTEST_ARENA_BYTES
#define USERTEST_ARENA_BYTES (64u * 1024u * 1024u)
#endif

static pthread_once_t usertest_arena_once = PTHREAD_ONCE_INIT;
static _Atomic size_t usertest_arena_off = 0;
static unsigned char *usertest_arena_base;
static size_t usertest_arena_bytes;

static inline uint64_t usertest_now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static inline void usertest_sleep_us(uint32_t us)
{
	(void)usleep(us);
}

static void usertest_arena_init_once(void)
{
	void *mem = NULL;
	size_t bytes = (size_t)USERTEST_ARENA_BYTES;
	const size_t align = 64;

	if (bytes % align != 0) {
		bytes = ((bytes + align - 1) / align) * align;
	}

	if (posix_memalign(&mem, align, bytes) != 0) {
		fprintf(stderr, "usertest: posix_memalign(%zu) failed: %s\n",
			bytes, strerror(errno));
		abort();
	}

	memset(mem, 0, bytes);
	usertest_arena_base = (unsigned char *)mem;
	usertest_arena_bytes = bytes;
	atomic_store_explicit(&usertest_arena_off, 0, memory_order_relaxed);
}

static inline void *usertest_arena_alloc(unsigned int size)
{
	const size_t align = 8;
	size_t n = (size + align - 1) & ~(align - 1);
	size_t off;

	pthread_once(&usertest_arena_once, usertest_arena_init_once);

	off = atomic_fetch_add_explicit(&usertest_arena_off, n, memory_order_relaxed);
	if (off + n > usertest_arena_bytes) {
		return NULL;
	}
	return (void *)(usertest_arena_base + off);
}

static inline void usertest_arena_free(void *addr __attribute__((unused)))
{
	/* No reclamation in arena model */
}

/*
 * Redirect DS headers to use our userspace arena allocation without changing
 * anything in include/.
 */
#define bpf_arena_alloc usertest_arena_alloc
#define bpf_arena_free usertest_arena_free

/*
 * Note: smp_store_release and smp_load_acquire are now provided by
 * bpf_arena_common.h (included via libarena_ds.h) using __atomic_store_n
 * and __atomic_load_n with memory_order_release/acquire.
 */

static inline void usertest_print_config(const char *name, int producers, int consumers, int items_per_producer)
{
	fprintf(stdout,
		"%s userspace concurrent test\n"
		"  producers=%d consumers=%d items_per_producer=%d\n",
		name, producers, consumers, items_per_producer);
}
