// SPDX-License-Identifier: GPL-2.0

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>

#include "ds_api.h"
#include "ds_ck_fifo_spsc.h"
#include "skeleton_ck_fifo_spsc.skel.h"

struct test_config {
	bool verify;
	bool print_stats;
};

static struct test_config config = {
	.verify = false,
	.print_stats = true,
};

static struct skeleton_ck_fifo_spsc_bpf *skel;
static volatile bool stop_test;
static __u64 dequeued_count;
static __u64 dequeue_failures;

static void poll_and_dequeue(void)
{
	struct ds_ck_fifo_spsc_head *head;
	struct ds_kv out;
	int rc;

	head = &skel->arena->global_ds_head;
	printf("Waiting for FIFO initialization...\n");
	while (!stop_test) {
		head = &skel->arena->global_ds_head;
		if (head && head->fifo.head && head->fifo.tail)
			break;
	}

	if (stop_test)
		return;

	printf("FIFO initialized, polling (Ctrl+C to stop)\n\n");

	while (!stop_test) {
		rc = ds_ck_fifo_spsc_delete(head, &out);
		if (rc == DS_SUCCESS) {
			printf("dequeue[%llu]: pid=%llu ts=%llu\n",
			       dequeued_count,
			       (unsigned long long)out.key,
			       (unsigned long long)out.value);
			dequeued_count++;
			continue;
		}

		if (rc == DS_ERROR_NOT_FOUND) {
			usleep(1000);
			continue;
		}

		dequeue_failures++;
		printf("dequeue error: %d\n", rc);
		usleep(1000);
	}
}

static int verify_data_structure(void)
{
	struct ds_ck_fifo_spsc_head *head = &skel->arena->global_ds_head;
	int rc;

	rc = ds_ck_fifo_spsc_verify(head);
	if (rc == DS_SUCCESS)
		printf("verify: PASS\n");
	else
		printf("verify: FAIL (%d)\n", rc);

	return rc;
}

static void print_statistics(void)
{
	printf("\n========================================\n");
	printf("CK FIFO SPSC statistics\n");
	printf("========================================\n");
	printf("kernel ops:      %llu\n", (unsigned long long)skel->bss->total_kernel_ops);
	printf("kernel failures: %llu\n", (unsigned long long)skel->bss->total_kernel_failures);
	printf("userspace dequeued: %llu\n", (unsigned long long)dequeued_count);
	printf("userspace failures: %llu\n", (unsigned long long)dequeue_failures);
}

static void signal_handler(int signo)
{
	printf("\nreceived signal %d, stopping...\n", signo);
	stop_test = true;
}

static void print_usage(const char *prog)
{
	printf("Usage: %s [OPTIONS]\n\n", prog);
	printf("Options:\n");
	printf("  -v      Verify queue on exit\n");
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
	int err;

	if (parse_args(argc, argv) < 0)
		return 1;

	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	printf("Loading BPF skeleton...\n");
	skel = skeleton_ck_fifo_spsc_bpf__open();
	if (!skel) {
		fprintf(stderr, "failed to open skeleton\n");
		return 1;
	}

	err = skeleton_ck_fifo_spsc_bpf__load(skel);
	if (err) {
		fprintf(stderr, "failed to load skeleton: %d\n", err);
		goto cleanup;
	}

	err = skeleton_ck_fifo_spsc_bpf__attach(skel);
	if (err) {
		fprintf(stderr, "failed to attach skeleton: %d\n", err);
		goto cleanup;
	}

	printf("Attached. Trigger inode_create activity in another shell.\n");
	poll_and_dequeue();

	if (config.verify)
		verify_data_structure();

	if (config.print_stats)
		print_statistics();

	err = 0;

cleanup:
	skeleton_ck_fifo_spsc_bpf__destroy(skel);
	return err;
}
