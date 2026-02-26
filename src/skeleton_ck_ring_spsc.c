// SPDX-License-Identifier: GPL-2.0

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>

#include "ds_api.h"
#include "ds_ck_ring_spsc.h"
#include "skeleton_ck_ring_spsc.skel.h"

struct test_config {
	bool verify;
	bool print_stats;
};

static struct test_config config = {
	.verify = false,
	.print_stats = true,
};

static struct skeleton_ck_ring_spsc_bpf *skel;
static volatile bool stop_test;
static __u64 dequeued_count;
static __u64 dequeue_failures;

static void poll_and_dequeue(void)
{
	struct ds_ck_ring_spsc_head *head = &skel->arena->global_ds_head;
	struct ds_kv data;
	int result;
	__u64 empty_polls = 0;

	if (!head || !head->slots) {
		printf("Queue not initialized, waiting for LSM hook trigger...\n");
		while ((!head || !head->slots) && !stop_test)
			head = &skel->arena->global_ds_head;
		if (stop_test)
			return;
	}

	printf("Starting continuous polling (Ctrl+C to stop)...\n");
	printf("Ring capacity: %u usable slots (%u total)\n\n",
	       head->capacity - 1, head->capacity);

	while (!stop_test) {
		result = ds_ck_ring_spsc_pop(head, &data);

		if (result == DS_SUCCESS) {
			printf("Dequeued element %llu: pid=%llu, ts=%llu\n",
			       dequeued_count, data.key, data.value);
			dequeued_count++;
			empty_polls = 0;
			continue;
		}

		if (result == DS_ERROR_NOT_FOUND) {
			empty_polls++;
			if (empty_polls % 100000000 == 0)
				printf("Still polling... (dequeued so far: %llu)\n", dequeued_count);
			continue;
		}

		printf("Dequeue error: %d\n", result);
		dequeue_failures++;
	}

	printf("\nStopped polling. Total dequeued: %llu\n", dequeued_count);
}

static int verify_data_structure(void)
{
	struct ds_ck_ring_spsc_head *head = &skel->arena->global_ds_head;
	int result;

	printf("\nVerifying CK SPSC ring from userspace...\n");

	if (!head || !head->slots) {
		printf("Queue not initialized\n");
		return DS_ERROR_INVALID;
	}

	result = ds_ck_ring_spsc_verify(head);
	if (result == DS_SUCCESS) {
		printf("CK SPSC ring verification PASSED\n");
		printf("  Size: %u\n", ds_ck_ring_spsc_size(head));
		printf("  c_head: %u\n", head->c_head);
		printf("  p_tail: %u\n", head->p_tail);
	} else {
		printf("CK SPSC ring verification FAILED (error %d)\n", result);
	}

	return result;
}

static void print_statistics(void)
{
	struct ds_ck_ring_spsc_head *head = &skel->arena->global_ds_head;

	printf("\n============================================================\n");
	printf("                 CK RING SPSC STATISTICS                    \n");
	printf("============================================================\n\n");

	printf("Kernel-Side Operations (Producer - inode_create LSM hook):\n");
	printf("  Total insert attempts: %llu\n", skel->bss->total_kernel_ops);
	printf("  Insert failures:       %llu\n", skel->bss->total_kernel_failures);

	printf("\nUserspace Operations (Consumer - continuous polling):\n");
	printf("  Elements dequeued:     %llu\n", dequeued_count);
	printf("  Dequeue failures:      %llu\n", dequeue_failures);

	printf("\nRing State:\n");
	if (head && head->slots) {
		__u32 remaining = ds_ck_ring_spsc_size(head);
		__u64 total_produced = skel->bss->total_kernel_ops - skel->bss->total_kernel_failures;
		__u64 total_accounted = dequeued_count + remaining;

		printf("  Remaining:             %u\n", remaining);
		printf("  Usable capacity:       %u\n", head->capacity - 1);
		printf("  c_head:                %u\n", head->c_head);
		printf("  p_tail:                %u\n", head->p_tail);
		printf("\nData Flow Analysis:\n");
		printf("  Successfully inserted: %llu\n", total_produced);
		printf("  Dequeued + Remaining:  %llu\n", total_accounted);
		if (total_produced == total_accounted)
			printf("  No data loss detected\n");
		else
			printf("  Discrepancy: %lld elements\n",
			       (long long)(total_produced - total_accounted));
	} else {
		printf("  Ring not initialized\n");
	}

	printf("\n============================================================\n\n");
}

static void signal_handler(int sig)
{
	printf("\nReceived signal %d, stopping consumer...\n", sig);
	stop_test = true;
}

static void print_usage(const char *prog)
{
	printf("Usage: %s [OPTIONS]\n\n", prog);
	printf("Test CK-faithful SPSC ring with BPF arena\n\n");
	printf("OPTIONS:\n");
	printf("  -v      Verify ring integrity on exit\n");
	printf("  -s      Print statistics on exit (default: enabled)\n");
	printf("  -h      Show this help\n");
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
	int err = 0;

	if (parse_args(argc, argv) < 0)
		return 1;

	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	printf("Loading BPF program...\n");
	skel = skeleton_ck_ring_spsc_bpf__open();
	if (!skel) {
		fprintf(stderr, "Failed to open BPF skeleton\n");
		return 1;
	}

	err = skeleton_ck_ring_spsc_bpf__load(skel);
	if (err) {
		fprintf(stderr, "Failed to load BPF skeleton: %d\n", err);
		goto cleanup;
	}

	err = skeleton_ck_ring_spsc_bpf__attach(skel);
	if (err) {
		fprintf(stderr, "Failed to attach BPF programs: %d\n", err);
		goto cleanup;
	}

	printf("BPF programs attached successfully\n");
	printf("Ring is lazily initialized on first inode_create trigger\n\n");

	poll_and_dequeue();

	if (config.verify)
		verify_data_structure();
	if (config.print_stats)
		print_statistics();

cleanup:
	skeleton_ck_ring_spsc_bpf__destroy(skel);
	return err;
}
