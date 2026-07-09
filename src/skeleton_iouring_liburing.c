// SPDX-License-Identifier: GPL-2.0

/* skeleton_iouring_liburing.c - Userspace loader for io_uring/liburing DS
 *
 * Two-lane relay with role-specific memory ordering:
 *   1. Kernel LSM hook CQ-produces into KU lane
 *   2. This program's relay thread CQ-consumes from KU, SQ-produces into UK
 *   3. Kernel uprobe SQ-consumes from UK lane
 */
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>

#include "ds_api.h"
#include "ds_iouring_liburing.h"
#include "ds_metrics.h"
#include "skeleton_iouring_liburing.skel.h"

/* ================================================================
 * CONFIGURATION
 * ================================================================ */
#define IOURING_LIBURING_RING_ENTRIES 512

struct test_config {
	bool verify;
	bool print_stats;
	bool one_way;
};

static struct test_config config = {
	.verify = false,
	.print_stats = true,
	.one_way = false,
};

static struct skeleton_iouring_liburing_bpf *skel;
static volatile sig_atomic_t stop_test;
static pthread_t relay_thread;
static bool relay_thread_started;
static __u64 ku_dequeued_count;
static __u64 uk_enqueued_count;

/* ================================================================
 * UPROBE TRIGGER
 * ================================================================ */
__attribute__((noinline)) void iouring_liburing_kernel_consume_trigger(void)
{
	asm volatile("" ::: "memory");
}

/* ================================================================
 * SIGNAL HANDLER
 * ================================================================ */
static void signal_handler(int sig)
{
	(void)sig;
	stop_test = 1;
}

static void apply_env_config(void)
{
	const char *env = getenv("DS_ONE_WAY");

	if (env && env[0] && strcmp(env, "0") != 0)
		config.one_way = true;
}

/* ================================================================
 * USERSPACE ALLOCATOR SETUP
 * ================================================================ */
static int setup_userspace_allocator(void)
{
	size_t arena_bytes;
	size_t alloc_bytes;
	void *alloc_base;
	long page_size;

	page_size = sysconf(_SC_PAGESIZE);
	if (page_size <= 0)
		return -1;

	arena_bytes = (size_t)bpf_map__max_entries(skel->maps.arena) * (size_t)page_size;
	if (arena_bytes <= (size_t)page_size)
		return -1;

	alloc_base = (void *)((char *)skel->arena + (size_t)page_size);
	alloc_bytes = arena_bytes - (size_t)page_size;
	bpf_arena_userspace_set_range(alloc_base, alloc_bytes);

	printf("Arena alloc range: base=%p size=%zu KB\n", alloc_base, alloc_bytes / 1024);
	return 0;
}

/* ================================================================
 * BPF PROGRAM ATTACHMENT
 * ================================================================ */
static int attach_programs(void)
{
	struct bpf_link *lsm_link;
	struct bpf_link *consume_link;
	struct bpf_uprobe_opts uprobe_opts = {
		.sz = sizeof(uprobe_opts),
		.func_name = "iouring_liburing_kernel_consume_trigger",
	};
	int err;

	lsm_link = bpf_program__attach_lsm(skel->progs.lsm_inode_create);
	err = libbpf_get_error(lsm_link);
	if (err)
		return err;
	skel->links.lsm_inode_create = lsm_link;

	if (config.one_way)
		return 0;

	consume_link = bpf_program__attach_uprobe_opts(
		skel->progs.bpf_iouring_liburing_consume,
		getpid(),
		"/proc/self/exe",
		0,
		&uprobe_opts);
	err = libbpf_get_error(consume_link);
	if (err)
		return err;
	skel->links.bpf_iouring_liburing_consume = consume_link;

	return 0;
}

/* ================================================================
 * RELAY WORKER THREAD
 *
 * Userspace relay: CQ consume from KU lane, SQ produce into UK lane.
 *   - CQ consume (_c): double acquire, plain CQE reads, release head
 *   - SQ produce (_c): acquire head, relaxed entry writes, release tail
 * ================================================================ */
static void *relay_worker(void *arg)
{
	struct ds_iouring_liburing_ring *head_ku = &skel->arena->global_ds_head_ku;
	struct ds_iouring_liburing_ring *head_uk = &skel->arena->global_ds_head_uk;
	struct ds_kv data;
	bool uk_initialized = false;
	int ret;

	(void)arg;

	printf("UserThread: waiting for IOURING_LIBURING KU initialization...\n");
	while (!stop_test) {
		if (head_ku->entries)
			break;
	}
	if (stop_test)
		return NULL;

	printf("UserThread: relay loop started (KU -> UK)\n");

	while (!stop_test) {
		if (!config.one_way && !uk_initialized) {
			if (!head_uk->entries) {
				ret = ds_iouring_liburing_init_c(head_uk,
								 IOURING_LIBURING_RING_ENTRIES);
				if (ret != DS_SUCCESS)
					continue;
			}
			uk_initialized = true;
		}

		/* CQ consume from KU lane: double acquire, plain CQE reads */
		DS_METRICS_RECORD_OP(&skel->arena->global_metrics, DS_METRICS_USER_CONSUMER, {
			ret = ds_iouring_liburing_cq_consume_c(head_ku, &data);
		}, ret);
		if (ret == DS_SUCCESS) {
			int ins_ret;

			ku_dequeued_count++;
			if (config.one_way) {
				if (data.value > 0)
					DS_METRICS_RECORD_E2E(&skel->arena->global_metrics, data.value);
				continue;
			}
			/* SQ produce into UK lane: acquire head, relaxed writes, release tail */
			DS_METRICS_RECORD_OP(&skel->arena->global_metrics, DS_METRICS_USER_PRODUCER, {
				ins_ret = ds_iouring_liburing_sq_produce_c(head_uk,
									   data.key,
									   data.value);
			}, ins_ret);
			if (ins_ret == DS_SUCCESS)
				uk_enqueued_count++;
			continue;
		}

		if (ret == DS_ERROR_NOT_FOUND || ret == DS_ERROR_INVALID)
			continue;
	}

	return NULL;
}

/* ================================================================
 * KERNEL CONSUMER DRAIN
 * ================================================================ */
static void trigger_kernel_consumer_on_exit(void)
{
	__u64 initial_consumed;
	__u64 target_consumed;
	__u64 attempts = 0;
	__u64 max_attempts;

	initial_consumed = skel->bss->total_kernel_consumed;
	target_consumed = initial_consumed + uk_enqueued_count;
	max_attempts = uk_enqueued_count + 1024;

	printf("MainThread: triggering kernel consumer uprobe...\n");

	if (uk_enqueued_count == 0) {
		iouring_liburing_kernel_consume_trigger();
		return;
	}

	while (attempts < max_attempts &&
	       skel->bss->total_kernel_consumed < target_consumed) {
		iouring_liburing_kernel_consume_trigger();
		attempts++;
	}

	printf("MainThread: consume triggers=%llu consumed=%llu target=%llu\n",
	       (unsigned long long)attempts,
	       (unsigned long long)skel->bss->total_kernel_consumed,
	       (unsigned long long)target_consumed);
}

/* ================================================================
 * VERIFICATION
 * ================================================================ */
static int verify_data_structure(void)
{
	struct ds_iouring_liburing_ring *head_ku = &skel->arena->global_ds_head_ku;
	struct ds_iouring_liburing_ring *head_uk = &skel->arena->global_ds_head_uk;
	int ku_result = DS_SUCCESS;
	int uk_result = DS_SUCCESS;

	printf("Verifying IOURING_LIBURING rings from userspace...\n");

	if (head_ku->entries)
		ku_result = ds_iouring_liburing_verify_c(head_ku);
	if (head_uk->entries)
		uk_result = ds_iouring_liburing_verify_c(head_uk);

	if (ku_result == DS_SUCCESS && uk_result == DS_SUCCESS) {
		printf("Verification PASSED (KU=%d UK=%d)\n", ku_result, uk_result);
		return DS_SUCCESS;
	}

	printf("Verification FAILED (KU=%d UK=%d)\n", ku_result, uk_result);
	return DS_ERROR_INVALID;
}

/* ================================================================
 * STATISTICS
 * ================================================================ */
static void print_statistics(void)
{
	struct ds_iouring_liburing_ring *head_ku = &skel->arena->global_ds_head_ku;
	struct ds_iouring_liburing_ring *head_uk = &skel->arena->global_ds_head_uk;
	__u32 ku_size = head_ku->entries
		? (head_ku->prod.tail - head_ku->cons.head) : 0;
	__u32 uk_size = head_uk->entries
		? (head_uk->prod.tail - head_uk->cons.head) : 0;

	printf("\n============================================================\n");
	printf("          IOURING_LIBURING RELAY STATISTICS                 \n");
	printf("============================================================\n");
	printf("Kernel producer (CQ produce, inode_create -> KU):\n");
	printf("  ops=%llu failures=%llu\n",
	       (unsigned long long)skel->bss->total_kernel_prod_ops,
	       (unsigned long long)skel->bss->total_kernel_prod_failures);

	printf("Kernel consumer (SQ consume, uprobe pop from UK):\n");
	printf("  ops=%llu failures=%llu consumed=%llu\n",
	       (unsigned long long)skel->bss->total_kernel_consume_ops,
	       (unsigned long long)skel->bss->total_kernel_consume_failures,
	       (unsigned long long)skel->bss->total_kernel_consumed);

	printf("Userspace relay (CQ consume KU -> SQ produce UK):\n");
	printf("  KU popped=%llu UK pushed=%llu\n",
	       (unsigned long long)ku_dequeued_count,
	       (unsigned long long)uk_enqueued_count);
	if (config.one_way)
		printf("  Delivered=%llu\n",
		       (unsigned long long)ku_dequeued_count);

	printf("Ring states:\n");
	printf("  KU size=%u\n", ku_size);
	printf("  UK size=%u\n", uk_size);
	if (config.one_way)
		ds_metrics_print_oneway(&skel->arena->global_metrics, "IOURING_LIBURING Ring");
	else
		ds_metrics_print(&skel->arena->global_metrics, "IOURING_LIBURING Ring");
	printf("============================================================\n\n");
}

/* ================================================================
 * ARGUMENT PARSING
 * ================================================================ */
static void print_usage(const char *prog)
{
	printf("Usage: %s [OPTIONS]\n\n", prog);
	printf("io_uring/liburing relay test (kernel->user->kernel lanes)\n\n");
	printf("OPTIONS:\n");
	printf("  -1      Run in one-way kernel->userspace benchmark mode\n");
	printf("  -v      Verify both rings on exit\n");
	printf("  -s      Print statistics on exit (default: enabled)\n");
	printf("  -h      Show this help\n\n");
	printf("Flow:\n");
	printf("  inode_create -> CQ produce KU (kernel producer)\n");
	printf("  UserThread: CQ consume KU -> SQ produce UK (relay)\n");
	printf("  Ctrl+C triggers uprobe: SQ consume UK (kernel consumer)\n");
	printf("\nEnvironment:\n");
	printf("  DS_ONE_WAY=1   enable one-way kernel->userspace mode\n");
}

static int parse_args(int argc, char **argv)
{
	int opt;

	while ((opt = getopt(argc, argv, "1vsh")) != -1) {
		switch (opt) {
		case '1':
			config.one_way = true;
			break;
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

/* ================================================================
 * MAIN
 * ================================================================ */
int main(int argc, char **argv)
{
	int err;

	if (parse_args(argc, argv) < 0)
		return 1;

	apply_env_config();

	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	printf("Loading BPF program for IOURING_LIBURING relay...\n");
	skel = skeleton_iouring_liburing_bpf__open_and_load();
	if (!skel) {
		fprintf(stderr, "Failed to open and load BPF skeleton\n");
		return 1;
	}

	err = setup_userspace_allocator();
	if (err) {
		fprintf(stderr, "Failed to set userspace arena allocator range\n");
		goto cleanup;
	}

	err = attach_programs();
	if (err) {
		fprintf(stderr, "Failed to attach BPF programs: %d\n", err);
		goto cleanup;
	}

	err = pthread_create(&relay_thread, NULL, relay_worker, NULL);
	if (err) {
		fprintf(stderr, "Failed to create relay thread: %s\n", strerror(err));
		err = -1;
		goto cleanup;
	}
	relay_thread_started = true;

	if (config.one_way) {
		printf("MainThread: attached in one-way mode. Trigger inode_create events in another shell.\n");
		printf("Press Ctrl+C to stop.\n");
	} else {
		printf("MainThread: attached. Trigger inode_create events in another shell.\n");
		printf("Press Ctrl+C to stop and invoke kernel consumer trigger.\n");
	}

	while (!stop_test) {
		usleep(1000);
		if (!config.one_way)
			iouring_liburing_kernel_consume_trigger();
	}

	if (relay_thread_started)
		pthread_join(relay_thread, NULL);

	if (!config.one_way)
		trigger_kernel_consumer_on_exit();

	if (config.verify)
		verify_data_structure();
	if (config.print_stats)
		print_statistics();

	err = 0;

cleanup:
	skeleton_iouring_liburing_bpf__destroy(skel);
	return err;
}
