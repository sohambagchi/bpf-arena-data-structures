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

/* The five categories */
enum ds_metrics_category {
	DS_METRICS_LKMM_PRODUCER = 0,  /* kernel LSM insert into KU */
	DS_METRICS_USER_CONSUMER = 1,  /* userspace pop from KU */
	DS_METRICS_USER_PRODUCER = 2,  /* userspace insert into UK */
	DS_METRICS_LKMM_CONSUMER = 3,  /* kernel uprobe pop from UK */
	DS_METRICS_END_TO_END    = 4,  /* full pipeline: producer -> consumer e2e */
	DS_METRICS_NUM_CATEGORIES = 5,
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
 * CONVENIENCE MACROS
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

/**
 * DS_METRICS_RECORD_E2E - Record end-to-end latency for a completed message
 * @store:        Arena pointer to ds_metrics_store
 * @prod_ts_ns:   Production timestamp (bpf_ktime_get_ns() stored in msg.value)
 *
 * Call this in the LKMM consumer (uprobe) after a successful pop.  It computes
 * the wall-clock latency from the original kernel producer to the final kernel
 * consumer and records it in the DS_METRICS_END_TO_END ring as a success.
 */
#define DS_METRICS_RECORD_E2E(store, prod_ts_ns) \
do { \
	__u64 __e2e = DS_METRICS_CLOCK_START() - (prod_ts_ns); \
	ds_metrics_record(store, DS_METRICS_END_TO_END, __e2e, DS_SUCCESS); \
} while (0)

/* ========================================================================
 * BATCH-SIZE VARIATION
 * ======================================================================== */

/**
 * DS_METRICS_MAX_BATCH_SIZE - Upper bound for batch loops.
 *
 * In BPF context the verifier needs a provably-bounded loop.  Userspace
 * has no such constraint, but we keep the constant universal for API
 * symmetry.
 */
#define DS_METRICS_MAX_BATCH_SIZE 256

/**
 * DS_METRICS_RECORD_BATCH - Time a batch of N operations and record
 * @store:          Arena pointer to ds_metrics_store
 * @cat:            ds_metrics_category value
 * @batch_sz:       Number of operations in the batch (clamped to
 *                  DS_METRICS_MAX_BATCH_SIZE)
 * @per_item_block: Code block executed once per item.  The block may
 *                  use __batch_idx (int, 0-based) and must leave the
 *                  per-item result in @result_var.
 * @result_var:     Variable that holds each item's result.  After the
 *                  macro completes it reflects the *last* item's result.
 *
 * The macro records one sample whose latency is the total wall-clock time
 * for the whole batch.  The sample is marked successful only when every
 * individual item succeeded.  Callers that need per-message latency can
 * divide the recorded latency by @batch_sz.
 *
 * Usage:
 *   DS_METRICS_RECORD_BATCH(store, DS_METRICS_USER_PRODUCER, 16, {
 *       result = ds_msqueue_insert_c(queue, keys[__batch_idx],
 *                                    values[__batch_idx]);
 *   }, result);
 */
#define DS_METRICS_RECORD_BATCH(store, cat, batch_sz, per_item_block, result_var) \
do { \
	int __batch_ok = 0; \
	int __batch_n = (batch_sz); \
	if (__batch_n > DS_METRICS_MAX_BATCH_SIZE) \
		__batch_n = DS_METRICS_MAX_BATCH_SIZE; \
	if (__batch_n < 1) \
		__batch_n = 1; \
	__u64 __start = DS_METRICS_CLOCK_START(); \
	for (int __batch_idx = 0; __batch_idx < __batch_n; __batch_idx++) { \
		per_item_block; \
		if ((result_var) == DS_SUCCESS) \
			__batch_ok++; \
	} \
	__u64 __elapsed = DS_METRICS_CLOCK_END(__start); \
	int __batch_result = (__batch_ok == __batch_n) \
		? DS_SUCCESS : DS_ERROR_INVALID; \
	ds_metrics_record(store, cat, __elapsed, __batch_result); \
} while (0)

/* ========================================================================
 * USERSPACE-ONLY STATS PRINTER
 * ======================================================================== */

#ifndef __BPF__

#include <stdlib.h>  /* qsort, malloc, free */

static const char *ds_metrics_category_names[DS_METRICS_NUM_CATEGORIES] = {
	"LKMM producer",
	"User consumer",
	"User producer",
	"LKMM consumer",
	"End-to-end",
};

/* Short machine-friendly category tags (no spaces) */
static const char *ds_metrics_category_tags[DS_METRICS_NUM_CATEGORIES] = {
	"lkmm_producer",
	"user_consumer",
	"user_producer",
	"lkmm_consumer",
	"end_to_end",
};

/* ========================================================================
 * TAIL LATENCY (PERCENTILE) CALCULATIONS
 * ======================================================================== */

/* qsort comparator for __u64 */
static int ds_metrics_cmp_u64(const void *a, const void *b)
{
	__u64 va = *(const __u64 *)a;
	__u64 vb = *(const __u64 *)b;
	if (va < vb) return -1;
	if (va > vb) return  1;
	return 0;
}

/**
 * ds_metrics_collect_latencies - Copy latency samples out of a ring
 * @ring:     Arena pointer to the metrics ring
 * @out:      Caller-allocated array of at least @max_n elements
 * @max_n:    Maximum number of samples to copy
 * @success_only: If non-zero, copy only successful-operation samples
 *
 * Returns: number of samples actually copied into @out.
 *
 * Samples are copied from the ring in slot order.  When the ring has
 * wrapped (count > DS_METRICS_RING_SIZE) the oldest samples have been
 * overwritten; we read the most-recent DS_METRICS_RING_SIZE entries.
 */
static inline __u64 ds_metrics_collect_latencies(
	struct ds_metrics_ring __arena *ring,
	__u64 *out,
	__u64 max_n,
	int success_only)
{
	__u64 total, n, copied;

	if (!ring || !out || max_n == 0)
		return 0;

	cast_kern(ring);

	total = ring->count;
	if (total == 0)
		return 0;

	n = (total < DS_METRICS_RING_SIZE) ? total : DS_METRICS_RING_SIZE;

	copied = 0;
	for (__u64 i = 0; i < n && copied < max_n; i++) {
		if (success_only && !ring->samples[i].success)
			continue;
		out[copied++] = ring->samples[i].latency_ns;
	}
	return copied;
}

/**
 * ds_metrics_percentile - Compute a latency percentile from a ring
 * @ring:         Arena pointer to the metrics ring
 * @pct:          Desired percentile (0.0 – 100.0, e.g. 50.0 for p50)
 * @success_only: If non-zero, consider only successful-operation samples
 *
 * Returns the latency (ns) at the given percentile, or 0 when the ring
 * contains no qualifying samples.  Allocates a temporary array internally
 * which is freed before return.
 */
static inline __u64 ds_metrics_percentile(
	struct ds_metrics_ring __arena *ring,
	double pct,
	int success_only)
{
	__u64 *buf;
	__u64 n, idx, result;

	if (!ring)
		return 0;

	cast_kern(ring);

	/* Allocate workspace for at most DS_METRICS_RING_SIZE samples */
	buf = (__u64 *)malloc(DS_METRICS_RING_SIZE * sizeof(__u64));
	if (!buf)
		return 0;

	n = ds_metrics_collect_latencies(ring, buf, DS_METRICS_RING_SIZE,
					 success_only);
	if (n == 0) {
		free(buf);
		return 0;
	}

	qsort(buf, (size_t)n, sizeof(__u64), ds_metrics_cmp_u64);

	/* Nearest-rank method */
	if (pct <= 0.0)
		idx = 0;
	else if (pct >= 100.0)
		idx = n - 1;
	else
		idx = (__u64)((pct / 100.0) * (double)(n - 1) + 0.5);

	if (idx >= n)
		idx = n - 1;

	result = buf[idx];
	free(buf);
	return result;
}

/* Forward declaration so ds_metrics_print() can call the parseable emitter */
static inline void ds_metrics_dump_parseable(
	struct ds_metrics_store __arena *store,
	const char *ds_name,
	double elapsed_sec);

/* ========================================================================
 * HUMAN-READABLE STATS PRINTERS
 * ======================================================================== */

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

	/* Also emit machine-parseable BENCH lines so scripts can parse
	 * p50/p99 without requiring callers to separately invoke
	 * ds_metrics_dump_parseable().  The elapsed_sec is 0 here because
	 * the caller does not pass it; scripts compute wall-clock time
	 * externally.
	 */
	ds_metrics_dump_parseable(store, ds_name, 0.0);
}

/**
 * ds_metrics_print_extended - Print performance table with tail latencies
 * @store:   Arena pointer to the metrics store
 * @ds_name: Human-readable name of the data structure
 *
 * Like ds_metrics_print but appends p50 and p99 latency columns computed
 * over the successful-operation samples in each ring.
 */
static inline void ds_metrics_print_extended(
	struct ds_metrics_store __arena *store,
	const char *ds_name)
{
	if (!store || !ds_name)
		return;

	cast_kern(store);

	printf("============================================================"
	       "========================\n");
	printf("              PERFORMANCE METRICS (extended): %s\n", ds_name);
	printf("============================================================"
	       "========================\n");
	printf("%-18s %7s %9s %6s %9s %9s %9s %11s\n",
	       "Category", "Total", "Success", "Rate%",
	       "Avg(ns)", "p50(ns)", "p99(ns)", "Tput-OK");

	for (int i = 0; i < DS_METRICS_NUM_CATEGORIES; i++) {
		struct ds_metrics_ring __arena *ring = &store->rings[i];
		cast_kern(ring);

		__u64 total     = ring->count;
		__u64 success   = ring->success_count;
		__u64 lat_ok    = ring->success_latency_ns;

		double rate = (total > 0)
			? (double)success / (double)total * 100.0
			: 0.0;

		__u64 avg_ok = (success > 0) ? lat_ok / success : 0;
		__u64 p50    = ds_metrics_percentile(ring, 50.0, 1);
		__u64 p99    = ds_metrics_percentile(ring, 99.0, 1);

		__u64 throughput = 0;
		if (lat_ok > 0)
			throughput = (__u64)((double)success
					    / ((double)lat_ok / 1e9));

		printf("%-18s %7llu %9llu %5.1f%% %9llu %9llu %9llu %11llu\n",
		       ds_metrics_category_names[i],
		       (unsigned long long)total,
		       (unsigned long long)success,
		       rate,
		       (unsigned long long)avg_ok,
		       (unsigned long long)p50,
		       (unsigned long long)p99,
		       (unsigned long long)throughput);
	}

	printf("============================================================"
	       "========================\n");
}

/* ========================================================================
 * MACHINE-PARSEABLE OUTPUT
 * ======================================================================== */

/**
 * ds_metrics_dump_parseable - Emit metrics in a script-friendly format
 * @store:       Arena pointer to the metrics store
 * @ds_name:     Data structure name
 * @elapsed_sec: Wall-clock seconds the workload ran (0 to omit)
 *
 * Output format (one line per category, bracketed by sentinels):
 *
 *   BENCH_METRICS_BEGIN <ds_name>
 *   BENCH <tag> total=N success=N avg_ns=N p50_ns=N p99_ns=N tput=N
 *   ...
 *   BENCH_ELAPSED_SEC <seconds>
 *   BENCH_METRICS_END
 *
 * All numeric fields are unsigned 64-bit decimals.  The Python
 * benchmarking script parses these lines to aggregate across runs.
 */
static inline void ds_metrics_dump_parseable(
	struct ds_metrics_store __arena *store,
	const char *ds_name,
	double elapsed_sec)
{
	if (!store || !ds_name)
		return;

	cast_kern(store);

	printf("BENCH_METRICS_BEGIN %s\n", ds_name);

	for (int i = 0; i < DS_METRICS_NUM_CATEGORIES; i++) {
		struct ds_metrics_ring __arena *ring = &store->rings[i];
		cast_kern(ring);

		__u64 total   = ring->count;
		__u64 success = ring->success_count;
		__u64 lat_ok  = ring->success_latency_ns;

		__u64 avg_ok = (success > 0) ? lat_ok / success : 0;
		__u64 p50    = ds_metrics_percentile(ring, 50.0, 1);
		__u64 p99    = ds_metrics_percentile(ring, 99.0, 1);

		__u64 throughput = 0;
		if (lat_ok > 0)
			throughput = (__u64)((double)success
					    / ((double)lat_ok / 1e9));

		printf("BENCH %s total=%llu success=%llu avg_ns=%llu "
		       "p50_ns=%llu p99_ns=%llu tput=%llu\n",
		       ds_metrics_category_tags[i],
		       (unsigned long long)total,
		       (unsigned long long)success,
		       (unsigned long long)avg_ok,
		       (unsigned long long)p50,
		       (unsigned long long)p99,
		       (unsigned long long)throughput);
	}

	if (elapsed_sec > 0.0)
		printf("BENCH_ELAPSED_SEC %.6f\n", elapsed_sec);

	printf("BENCH_METRICS_END\n");
}

#endif /* !__BPF__ */

#endif /* DS_METRICS_H */
