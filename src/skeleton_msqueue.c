// SPDX-License-Identifier: GPL-2.0
/*
 * skeleton_msqueue: integrated two-lane relay
 *
 * A) MainThread loads and attaches BPF programs.
 * B) MainThread spawns UserThread (busy relay loop).
 * C) inode_create produces onto MSQueueKU in kernel.
 * D) UserThread pops KU and inserts onto MSQueueUK.
 * E) On Ctrl+C, MainThread triggers uprobe for kernel consumer.
 */

#include <errno.h>
#include <pthread.h>
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

#include "ds_api.h"
#include "ds_msqueue.h"
#include "ds_metrics.h"
#include "skeleton_msqueue.skel.h"

struct test_config {
	bool verify;
	bool print_stats;
};

static struct test_config config = {
	.verify = false,
	.print_stats = true,
};

static struct skeleton_msqueue_bpf *skel;
static volatile sig_atomic_t stop_test;
static pthread_t relay_thread;
static bool relay_thread_started;
static __u64 ku_dequeued_count;
static __u64 uk_enqueued_count;

__attribute__((noinline)) void msq_kernel_consume_trigger(void)
{
	asm volatile("" ::: "memory");
}

static void signal_handler(int sig)
{
	(void)sig;
	stop_test = 1;
}

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

static int attach_programs(void)
{
	struct bpf_link *lsm_link;
	struct bpf_link *consume_link;
	struct bpf_uprobe_opts uprobe_opts = {
		.sz = sizeof(uprobe_opts),
		.func_name = "msq_kernel_consume_trigger",
	};
	int err;

	lsm_link = bpf_program__attach_lsm(skel->progs.lsm_inode_create);
	err = libbpf_get_error(lsm_link);
	if (err)
		return err;
	skel->links.lsm_inode_create = lsm_link;

	consume_link = bpf_program__attach_uprobe_opts(
		skel->progs.bpf_msq_consume,
		getpid(),
		"/proc/self/exe",
		0,
		&uprobe_opts);
	err = libbpf_get_error(consume_link);
	if (err)
		return err;
	skel->links.bpf_msq_consume = consume_link;

	return 0;
}

static void *relay_worker(void *arg)
{
	struct ds_msqueue *queue_ku = &skel->arena->global_ds_queue_ku;
	struct ds_msqueue *queue_uk = &skel->arena->global_ds_queue_uk;
	struct ds_kv data;
	bool uk_initialized = false;
	int ret;

	(void)arg;

	printf("UserThread: waiting for MSQueueKU initialization...\n");
	while (!stop_test) {
		if (queue_ku->head && queue_ku->tail)
			break;
	}
	if (stop_test)
		return NULL;

	printf("UserThread: relay loop started (KU -> UK)\n");

	while (!stop_test) {
		if (!uk_initialized) {
			if (!queue_uk->head || !queue_uk->tail) {
				ret = ds_msqueue_init_c(queue_uk);
				if (ret != DS_SUCCESS)
					continue;
			}
			uk_initialized = true;
		}

		DS_METRICS_RECORD_OP(&skel->arena->global_metrics, DS_METRICS_USER_CONSUMER, {
			ret = ds_msqueue_pop_c(queue_ku, &data);
		}, ret);
		if (ret == DS_SUCCESS) {
			int ins_ret;

			ku_dequeued_count++;
			DS_METRICS_RECORD_OP(&skel->arena->global_metrics, DS_METRICS_USER_PRODUCER, {
				ins_ret = ds_msqueue_insert_c(queue_uk, data.key, data.value);
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
		msq_kernel_consume_trigger();
		return;
	}

	while (attempts < max_attempts &&
	       skel->bss->total_kernel_consumed < target_consumed) {
		msq_kernel_consume_trigger();
		attempts++;
	}

	printf("MainThread: consume triggers=%llu consumed=%llu target=%llu\n",
	       (unsigned long long)attempts,
	       (unsigned long long)skel->bss->total_kernel_consumed,
	       (unsigned long long)target_consumed);
}

static int verify_data_structure(void)
{
	struct ds_msqueue *queue_ku = &skel->arena->global_ds_queue_ku;
	struct ds_msqueue *queue_uk = &skel->arena->global_ds_queue_uk;
	int ku_result = DS_SUCCESS;
	int uk_result = DS_SUCCESS;

	printf("Verifying queues from userspace...\n");

	if (queue_ku->head && queue_ku->tail)
		ku_result = ds_msqueue_verify_c(queue_ku);
	if (queue_uk->head && queue_uk->tail)
		uk_result = ds_msqueue_verify_c(queue_uk);

	if (ku_result == DS_SUCCESS && uk_result == DS_SUCCESS) {
		printf("Verification PASSED (KU=%d UK=%d)\n", ku_result, uk_result);
		return DS_SUCCESS;
	}

	printf("Verification FAILED (KU=%d UK=%d)\n", ku_result, uk_result);
	return DS_ERROR_INVALID;
}

static void print_statistics(void)
{
	struct ds_msqueue *queue_ku = &skel->arena->global_ds_queue_ku;
	struct ds_msqueue *queue_uk = &skel->arena->global_ds_queue_uk;

	printf("\n============================================================\n");
	printf("                         STATISTICS                         \n");
	printf("============================================================\n");
	printf("Kernel producer (inode_create -> KU):\n");
	printf("  ops=%llu failures=%llu\n",
	       (unsigned long long)skel->bss->total_kernel_prod_ops,
	       (unsigned long long)skel->bss->total_kernel_prod_failures);

	printf("Kernel consumer (uprobe pop from UK):\n");
	printf("  ops=%llu failures=%llu consumed=%llu\n",
	       (unsigned long long)skel->bss->total_kernel_consume_ops,
	       (unsigned long long)skel->bss->total_kernel_consume_failures,
	       (unsigned long long)skel->bss->total_kernel_consumed);

	printf("Userspace relay:\n");
	printf("  KU popped=%llu UK pushed=%llu\n",
	       (unsigned long long)ku_dequeued_count,
	       (unsigned long long)uk_enqueued_count);

	printf("Queue states:\n");
	printf("  KU count=%llu\n", (unsigned long long)queue_ku->count);
	printf("  UK count=%llu\n", (unsigned long long)queue_uk->count);
	ds_metrics_print(&skel->arena->global_metrics, "MSQueue");
	printf("============================================================\n\n");
}

static void print_usage(const char *prog)
{
	printf("Usage: %s [OPTIONS]\n\n", prog);
	printf("MSQueue relay test (kernel->user->kernel lanes)\n\n");
	printf("OPTIONS:\n");
	printf("  -v      Verify both queues on exit\n");
	printf("  -s      Print statistics on exit (default: enabled)\n");
	printf("  -h      Show this help\n\n");
	printf("Flow:\n");
	printf("  inode_create -> MSQueueKU (kernel producer)\n");
	printf("  UserThread relays KU -> UK (busy loop)\n");
	printf("  Ctrl+C triggers uprobe-based kernel consumer on UK\n");
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

	printf("Loading BPF program for integrated MSQueue relay...\n");
	skel = skeleton_msqueue_bpf__open_and_load();
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

	printf("MainThread: attached. Trigger inode_create events in another shell.\n");
	printf("Press Ctrl+C to stop and invoke kernel consumer trigger.\n");

	while (!stop_test)
		pause();

	if (relay_thread_started)
		pthread_join(relay_thread, NULL);

	trigger_kernel_consumer_on_exit();

	if (config.verify)
		verify_data_structure();
	if (config.print_stats)
		print_statistics();

	err = 0;

cleanup:
	skeleton_msqueue_bpf__destroy(skel);
	return err;
}
