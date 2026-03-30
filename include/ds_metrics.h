/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/* Shared metrics infrastructure for BPF arena data structures.
 *
 * Compiles under both:
 *   - BPF context  (__BPF__ defined)  — clang -target bpf -O2
 *   - Userspace    (__BPF__ not defined) — gcc -g -Wall -Wextra -O0
 *
 * All metric stores live in BPF arena memory; pointers use __arena.
 */
#ifndef DS_METRICS_H
#define DS_METRICS_H

#include "ds_api.h"

#ifndef __BPF__
#include <stdio.h>
#include <time.h>
#endif

/* ========================================================================
 * DATA TYPES (shared between BPF and userspace)
 * ======================================================================== */

/* A single latency sample */
struct ds_metric_sample {
	__u64 latency_ns;   /* measured operation latency */
	__u8  success;       /* 1 = DS_SUCCESS (0), 0 = failure */
	__u8  pad[7];        /* alignment */
};

/* Ring buffer for one category of measurements */
#define DS_METRICS_RING_SIZE 8192

struct ds_metrics_ring {
	__u64 write_idx;         /* next write position (wraps via & mask) */
	__u64 count;             /* total operations recorded (may exceed RING_SIZE) */
	__u64 success_count;     /* total successful operations */
	__u64 total_latency_ns;  /* sum of all latencies (for average) */
	__u64 success_latency_ns; /* sum of successful latencies */
	struct ds_metric_sample samples[DS_METRICS_RING_SIZE];
};

/* The four categories */
enum ds_metrics_category {
	DS_METRICS_LKMM_PRODUCER = 0,  /* kernel LSM insert into KU */
	DS_METRICS_USER_CONSUMER = 1,  /* userspace pop from KU */
	DS_METRICS_USER_PRODUCER = 2,  /* userspace insert into UK */
	DS_METRICS_LKMM_CONSUMER = 3,  /* kernel uprobe pop from UK */
	DS_METRICS_NUM_CATEGORIES = 4,
};

/* Top-level metrics store — lives in arena */
struct ds_metrics_store {
	struct ds_metrics_ring rings[DS_METRICS_NUM_CATEGORIES];
};

/* ========================================================================
 * TIMING PRIMITIVES
 * ======================================================================== */

#ifdef __BPF__
#define DS_METRICS_CLOCK_START() bpf_ktime_get_ns()
#endif

#ifndef __BPF__
static inline __u64 ds_metrics_clock(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (__u64)ts.tv_sec * 1000000000ULL + (__u64)ts.tv_nsec;
}
#define DS_METRICS_CLOCK_START() ds_metrics_clock()
#endif

#define DS_METRICS_CLOCK_END(start) (DS_METRICS_CLOCK_START() - (start))

/* ========================================================================
 * RECORDING FUNCTION
 * ======================================================================== */

/**
 * ds_metrics_record - Record a single operation measurement
 * @store:      Arena pointer to the top-level metrics store
 * @cat:        Which category this measurement belongs to
 * @latency_ns: Measured operation latency in nanoseconds
 * @result:     Operation result code (0 = DS_SUCCESS = success)
 *
 * Atomically appends a sample into the ring for @cat and updates the
 * running counters.  Safe to call concurrently from multiple CPUs/threads.
 */
static inline void ds_metrics_record(
	struct ds_metrics_store __arena *store,
	enum ds_metrics_category cat,
	__u64 latency_ns,
	int result)
{
	struct ds_metrics_ring __arena *ring;
	__u64 old_idx;
	__u64 slot;
	__u8 ok;

	if (!store)
		return;

	cast_kern(store);

	ring = &store->rings[cat];
	cast_kern(ring);

	/* Claim a slot atomically */
	old_idx = arena_atomic_add(&ring->write_idx, 1, ARENA_RELAXED);
	slot = old_idx & (DS_METRICS_RING_SIZE - 1);

	/* Write the sample */
	ok = (result == DS_SUCCESS) ? 1 : 0;
	ring->samples[slot].latency_ns = latency_ns;
	ring->samples[slot].success = ok;

	/* Update running counters */
	arena_atomic_add(&ring->count, 1, ARENA_RELAXED);
	arena_atomic_add(&ring->total_latency_ns, latency_ns, ARENA_RELAXED);

	if (ok) {
		arena_atomic_add(&ring->success_count, 1, ARENA_RELAXED);
		arena_atomic_add(&ring->success_latency_ns, latency_ns, ARENA_RELAXED);
	}
}

/* ========================================================================
 * CONVENIENCE MACRO
 * ======================================================================== */

/**
 * DS_METRICS_RECORD_OP - Time an operation and record the result
 * @store:      Arena pointer to ds_metrics_store
 * @cat:        ds_metrics_category value
 * @op_block:   Code block that performs the operation
 * @result_var: Variable that holds the operation result after op_block
 *
 * Usage:
 *   DS_METRICS_RECORD_OP(store, DS_METRICS_LKMM_PRODUCER, {
 *       result = ds_msqueue_insert_lkmm(queue, key, value);
 *   }, result);
 */
#define DS_METRICS_RECORD_OP(store, cat, op_block, result_var) \
do { \
	__u64 __start = DS_METRICS_CLOCK_START(); \
	op_block; \
	__u64 __elapsed = DS_METRICS_CLOCK_END(__start); \
	ds_metrics_record(store, cat, __elapsed, result_var); \
} while (0)

/* ========================================================================
 * USERSPACE-ONLY STATS PRINTER
 * ======================================================================== */

#ifndef __BPF__

static const char *ds_metrics_category_names[DS_METRICS_NUM_CATEGORIES] = {
	"LKMM producer",
	"User consumer",
	"User producer",
	"LKMM consumer",
};

/**
 * ds_metrics_print - Print a formatted performance table
 * @store:   Arena pointer to the metrics store
 * @ds_name: Human-readable name of the data structure being measured
 *
 * Columns: category, total ops, successful ops, success rate (%),
 * average latency (all), average latency (successful only), throughput.
 */
static inline void ds_metrics_print(
	struct ds_metrics_store __arena *store,
	const char *ds_name)
{
	if (!store || !ds_name)
		return;

	cast_kern(store);

	printf("============================================================\n");
	printf("              PERFORMANCE METRICS: %s\n", ds_name);
	printf("============================================================\n");
	printf("%-20s %7s %9s %6s %9s %11s %11s\n",
	       "Category", "Total", "Success", "Rate%",
	       "Avg(ns)", "Avg-OK(ns)", "Tput-OK");

	for (int i = 0; i < DS_METRICS_NUM_CATEGORIES; i++) {
		struct ds_metrics_ring __arena *ring = &store->rings[i];
		cast_kern(ring);

		__u64 total     = ring->count;
		__u64 success   = ring->success_count;
		__u64 lat_all   = ring->total_latency_ns;
		__u64 lat_ok    = ring->success_latency_ns;

		double rate = (total > 0)
			? (double)success / (double)total * 100.0
			: 0.0;

		__u64 avg_all = (total > 0) ? lat_all / total : 0;
		__u64 avg_ok  = (success > 0) ? lat_ok / success : 0;

		__u64 throughput = 0;
		if (lat_ok > 0)
			throughput = (__u64)((double)success / ((double)lat_ok / 1e9));

		printf("%-20s %7llu %9llu %5.1f%% %9llu %11llu %11llu\n",
		       ds_metrics_category_names[i],
		       (unsigned long long)total,
		       (unsigned long long)success,
		       rate,
		       (unsigned long long)avg_all,
		       (unsigned long long)avg_ok,
		       (unsigned long long)throughput);
	}

	printf("============================================================\n");
}

#endif /* !__BPF__ */

#endif /* DS_METRICS_H */
