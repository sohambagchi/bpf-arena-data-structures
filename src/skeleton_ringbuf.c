// SPDX-License-Identifier: GPL-2.0

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>

#include "ringbuf_bench.h"
#include "skeleton_ringbuf.skel.h"

struct test_config {
	bool verify;
	bool print_stats;
};

static struct test_config config = {
	.verify = false,
	.print_stats = true,
};

static struct skeleton_ringbuf_bpf *skel;
static struct ring_buffer *rb;
static volatile sig_atomic_t stop_test;
static __u64 delivered_count;

static void signal_handler(int sig)
{
	(void)sig;
	stop_test = 1;
}

static int attach_programs(void)
{
	struct bpf_link *lsm_link;
	int err;

	lsm_link = bpf_program__attach_lsm(skel->progs.lsm_inode_create);
	err = libbpf_get_error(lsm_link);
	if (err)
		return err;
	skel->links.lsm_inode_create = lsm_link;

	return 0;
}

static int handle_event(void *ctx, void *data, size_t data_sz)
{
	struct ringbuf_bench_store *metrics = ctx;
	const struct ringbuf_bench_event *event = data;
	__u64 start_ns;
	__u64 callback_ns;
	__u64 prod_ts;

	if (data_sz < sizeof(*event))
		return 0;

	start_ns = ringbuf_bench_clock_now();
	prod_ts = event->value;
	callback_ns = ringbuf_bench_clock_now() - start_ns;
	ringbuf_bench_record(metrics, RINGBUF_BENCH_USER_CONSUMER, callback_ns,
			    true);
	delivered_count++;
	if (prod_ts > 0)
		RINGBUF_BENCH_RECORD_E2E(metrics, prod_ts);

	return 0;
}

static int setup_ring_buffer(void)
{
	rb = ring_buffer__new(bpf_map__fd(skel->maps.events), handle_event,
			      &skel->bss->global_metrics, NULL);
	if (!rb)
		return -1;
	return 0;
}

static int verify_benchmark_state(void)
{
	struct ringbuf_bench_store *metrics = &skel->bss->global_metrics;
	__u64 user_success = metrics->rings[RINGBUF_BENCH_USER_CONSUMER].success_count;
	__u64 e2e_success = metrics->rings[RINGBUF_BENCH_END_TO_END].success_count;

	if (user_success != delivered_count || e2e_success != delivered_count) {
		printf("Verification FAILED (delivered=%llu user_success=%llu e2e_success=%llu)\n",
		       (unsigned long long)delivered_count,
		       (unsigned long long)user_success,
		       (unsigned long long)e2e_success);
		return -1;
	}

	printf("Verification PASSED (delivered=%llu)\n",
	       (unsigned long long)delivered_count);
	return 0;
}

static void print_statistics(void)
{
	printf("\n============================================================\n");
	printf("               RINGBUF ONE-WAY BENCH STATISTICS             \n");
	printf("============================================================\n");
	printf("Kernel producer (inode_create -> ringbuf):\n");
	printf("  ops=%llu failures=%llu\n",
	       (unsigned long long)skel->bss->total_kernel_prod_ops,
	       (unsigned long long)skel->bss->total_kernel_prod_failures);
	printf("Userspace delivery:\n");
	printf("  Delivered=%llu\n", (unsigned long long)delivered_count);
	ringbuf_bench_print(&skel->bss->global_metrics, "BPF_MAP_TYPE_RINGBUF");
	printf("============================================================\n\n");
}

static void print_usage(const char *prog)
{
	printf("Usage: %s [OPTIONS]\n\n", prog);
	printf("BPF ringbuf one-way benchmark (kernel->userspace)\n\n");
	printf("OPTIONS:\n");
	printf("  -v      Verify metric counters on exit\n");
	printf("  -s      Print statistics on exit (default: enabled)\n");
	printf("  -h      Show this help\n\n");
	printf("Flow:\n");
	printf("  inode_create -> BPF_MAP_TYPE_RINGBUF (kernel producer)\n");
	printf("  Userspace polls ringbuf and records one-way latency\n");
}

static int parse_args(int argc, char **argv)
{
	int opt;

	while ((opt = getopt(argc, argv, "vsh")) != -1) {
		switch (opt) {
		case 'v':
			config.verify = true;
			break;
		case 's':
			config.print_stats = true;
			break;
		case 'h':
			print_usage(argv[0]);
			exit(0);
		default:
			print_usage(argv[0]);
			return -1;
		}
	}

	return 0;
}

int main(int argc, char **argv)
{
	int err;

	if (parse_args(argc, argv) < 0)
		return 1;

	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	printf("Loading BPF program for ringbuf one-way benchmark...\n");
	skel = skeleton_ringbuf_bpf__open_and_load();
	if (!skel) {
		fprintf(stderr, "Failed to open and load BPF skeleton\n");
		return 1;
	}

	err = attach_programs();
	if (err) {
		fprintf(stderr, "Failed to attach BPF programs: %d\n", err);
		goto cleanup;
	}

	err = setup_ring_buffer();
	if (err) {
		fprintf(stderr, "Failed to create userspace ring buffer\n");
		goto cleanup;
	}

	printf("MainThread: attached. Trigger inode_create events in another shell.\n");
	printf("Press Ctrl+C to stop.\n");

	while (!stop_test) {
		err = ring_buffer__poll(rb, 100);
		if (err == -EINTR)
			break;
		if (err < 0) {
			fprintf(stderr, "ring_buffer__poll failed: %d\n", err);
			goto cleanup;
		}
	}

	for (;;) {
		err = ring_buffer__poll(rb, 0);
		if (err <= 0)
			break;
	}

	if (config.verify)
		verify_benchmark_state();
	if (config.print_stats)
		print_statistics();

	err = 0;

cleanup:
	ring_buffer__free(rb);
	skeleton_ringbuf_bpf__destroy(skel);
	return err;
}
