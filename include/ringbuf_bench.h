/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
#ifndef RINGBUF_BENCH_H
#define RINGBUF_BENCH_H

#pragma once

#ifndef __BPF__
#include <linux/types.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#endif

#ifdef __BPF__
#define RINGBUF_BENCH_CLOCK_NOW() bpf_ktime_get_ns()
#else
static inline __u64 ringbuf_bench_clock_now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (__u64)ts.tv_sec * 1000000000ULL + (__u64)ts.tv_nsec;
}
#define RINGBUF_BENCH_CLOCK_NOW() ringbuf_bench_clock_now()
#endif

#define RINGBUF_BENCH_RING_SIZE 8192

struct ringbuf_bench_event {
	__u64 key;
	__u64 value;
};

struct ringbuf_bench_sample {
	__u64 latency_ns;
	__u8 success;
	__u8 pad[7];
};

enum ringbuf_bench_category {
	RINGBUF_BENCH_LKMM_PRODUCER = 0,
	RINGBUF_BENCH_USER_CONSUMER = 1,
	RINGBUF_BENCH_END_TO_END = 2,
	RINGBUF_BENCH_NUM_CATEGORIES = 3,
};

struct ringbuf_bench_ring {
	__u64 write_idx;
	__u64 count;
	__u64 success_count;
	__u64 total_latency_ns;
	__u64 success_latency_ns;
	struct ringbuf_bench_sample samples[RINGBUF_BENCH_RING_SIZE];
};

struct ringbuf_bench_store {
	struct ringbuf_bench_ring rings[RINGBUF_BENCH_NUM_CATEGORIES];
};

static inline void ringbuf_bench_record(
	struct ringbuf_bench_store *store,
	enum ringbuf_bench_category cat,
	__u64 latency_ns,
	bool success)
{
	struct ringbuf_bench_ring *ring;
	__u64 old_idx;
	__u64 slot;

	if (!store)
		return;

	ring = &store->rings[cat];
	old_idx = __atomic_fetch_add(&ring->write_idx, 1, __ATOMIC_RELAXED);
	slot = old_idx & (RINGBUF_BENCH_RING_SIZE - 1);

	ring->samples[slot].latency_ns = latency_ns;
	ring->samples[slot].success = success ? 1 : 0;
	__atomic_fetch_add(&ring->count, 1, __ATOMIC_RELAXED);
	__atomic_fetch_add(&ring->total_latency_ns, latency_ns, __ATOMIC_RELAXED);
	if (success) {
		__atomic_fetch_add(&ring->success_count, 1, __ATOMIC_RELAXED);
		__atomic_fetch_add(&ring->success_latency_ns, latency_ns,
					  __ATOMIC_RELAXED);
	}
}

#define RINGBUF_BENCH_RECORD_OP(store, cat, op_block, success_expr) \
	do { \
		__u64 __start = RINGBUF_BENCH_CLOCK_NOW(); \
		op_block; \
		ringbuf_bench_record((store), (cat), \
			RINGBUF_BENCH_CLOCK_NOW() - __start, \
			(success_expr)); \
	} while (0)

#define RINGBUF_BENCH_RECORD_E2E(store, prod_ts_ns) \
	do { \
		__u64 __e2e = RINGBUF_BENCH_CLOCK_NOW() - (prod_ts_ns); \
		ringbuf_bench_record((store), RINGBUF_BENCH_END_TO_END, __e2e, true); \
	} while (0)

#ifndef __BPF__
static const char *ringbuf_bench_category_names[RINGBUF_BENCH_NUM_CATEGORIES] = {
	"LKMM producer",
	"User consumer",
	"End-to-end",
};

static const char *ringbuf_bench_category_tags[RINGBUF_BENCH_NUM_CATEGORIES] = {
	"lkmm_producer",
	"user_consumer",
	"end_to_end",
};

static int ringbuf_bench_cmp_u64(const void *a, const void *b)
{
	__u64 va = *(const __u64 *)a;
	__u64 vb = *(const __u64 *)b;

	if (va < vb)
		return -1;
	if (va > vb)
		return 1;
	return 0;
}

static inline __u64 ringbuf_bench_collect_latencies(
	struct ringbuf_bench_ring *ring,
	__u64 *out,
	__u64 max_n,
	bool success_only)
{
	__u64 total, n, copied;

	if (!ring || !out || max_n == 0)
		return 0;

	total = ring->count;
	if (total == 0)
		return 0;

	n = (total < RINGBUF_BENCH_RING_SIZE) ? total : RINGBUF_BENCH_RING_SIZE;
	copied = 0;
	for (__u64 i = 0; i < n && copied < max_n; i++) {
		if (success_only && !ring->samples[i].success)
			continue;
		out[copied++] = ring->samples[i].latency_ns;
	}

	return copied;
}

static inline __u64 ringbuf_bench_percentile(
	struct ringbuf_bench_ring *ring,
	double pct,
	bool success_only)
{
	__u64 *buf;
	__u64 n, idx, result;

	if (!ring)
		return 0;

	buf = (__u64 *)malloc(RINGBUF_BENCH_RING_SIZE * sizeof(__u64));
	if (!buf)
		return 0;

	n = ringbuf_bench_collect_latencies(ring, buf, RINGBUF_BENCH_RING_SIZE,
					 success_only);
	if (n == 0) {
		free(buf);
		return 0;
	}

	qsort(buf, (size_t)n, sizeof(__u64), ringbuf_bench_cmp_u64);
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

static inline void ringbuf_bench_dump_parseable(
	struct ringbuf_bench_store *store,
	const char *name,
	double elapsed_sec)
{
	if (!store || !name)
		return;

	printf("BENCH_METRICS_BEGIN %s\n", name);
	for (int i = 0; i < RINGBUF_BENCH_NUM_CATEGORIES; i++) {
		struct ringbuf_bench_ring *ring = &store->rings[i];
		__u64 total = ring->count;
		__u64 success = ring->success_count;
		__u64 lat_ok = ring->success_latency_ns;
		__u64 avg_ok = (success > 0) ? lat_ok / success : 0;
		__u64 p50 = ringbuf_bench_percentile(ring, 50.0, true);
		__u64 p99 = ringbuf_bench_percentile(ring, 99.0, true);
		__u64 tput = 0;

		if (lat_ok > 0)
			tput = (__u64)((double)success / ((double)lat_ok / 1e9));

		printf("BENCH %s total=%llu success=%llu avg_ns=%llu p50_ns=%llu p99_ns=%llu tput=%llu\n",
		       ringbuf_bench_category_tags[i],
		       (unsigned long long)total,
		       (unsigned long long)success,
		       (unsigned long long)avg_ok,
		       (unsigned long long)p50,
		       (unsigned long long)p99,
		       (unsigned long long)tput);
	}

	if (elapsed_sec > 0.0)
		printf("BENCH_ELAPSED_SEC %.6f\n", elapsed_sec);
	printf("BENCH_METRICS_END\n");
}

static inline void ringbuf_bench_print(
	struct ringbuf_bench_store *store,
	const char *name)
{
	if (!store || !name)
		return;

	printf("============================================================"
	       "========================\n");
	printf("              PERFORMANCE METRICS (one-way): %s\n", name);
	printf("============================================================"
	       "========================\n");
	printf("%-18s %7s %9s %6s %9s %9s %9s %11s\n",
	       "Category", "Total", "Success", "Rate%",
	       "Avg(ns)", "p50(ns)", "p99(ns)", "Tput-OK");

	for (int i = 0; i < RINGBUF_BENCH_NUM_CATEGORIES; i++) {
		struct ringbuf_bench_ring *ring = &store->rings[i];
		__u64 total = ring->count;
		__u64 success = ring->success_count;
		__u64 lat_ok = ring->success_latency_ns;
		__u64 avg_ok = (success > 0) ? lat_ok / success : 0;
		__u64 p50 = ringbuf_bench_percentile(ring, 50.0, true);
		__u64 p99 = ringbuf_bench_percentile(ring, 99.0, true);
		__u64 tput = 0;
		double rate = (total > 0)
			? (double)success / (double)total * 100.0 : 0.0;

		if (lat_ok > 0)
			tput = (__u64)((double)success / ((double)lat_ok / 1e9));

		printf("%-18s %7llu %9llu %5.1f%% %9llu %9llu %9llu %11llu\n",
		       ringbuf_bench_category_names[i],
		       (unsigned long long)total,
		       (unsigned long long)success,
		       rate,
		       (unsigned long long)avg_ok,
		       (unsigned long long)p50,
		       (unsigned long long)p99,
		       (unsigned long long)tput);
	}

	printf("============================================================"
	       "========================\n");
	ringbuf_bench_dump_parseable(store, name, 0.0);
}
#endif /* !__BPF__ */

#endif /* RINGBUF_BENCH_H */
