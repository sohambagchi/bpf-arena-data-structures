// SPDX-License-Identifier: GPL-2.0

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

#include <bpf/libbpf.h>
#include <bpf/bpf.h>

#include "ds_api.h"
#include "ds_ck_stack_upmc.h"
#include "skeleton_ck_stack_upmc.skel.h"

struct test_config {
	bool verify;
	bool print_stats;
};

static struct test_config config = {
	.verify = false,
	.print_stats = true,
};

static struct skeleton_ck_stack_upmc_bpf *skel;
static volatile bool stop_test = false;
static __u64 popped_count = 0;

static void signal_handler(int sig)
{
	printf("\nReceived signal %d, stopping test...\n", sig);
	stop_test = true;
}

static void poll_and_pop(void)
{
	struct ds_ck_stack_upmc_head *head;
	struct ds_kv out;
	int rc;

	printf("Waiting for stack initialization...\n");
	while (!stop_test) {
		head = &skel->arena->global_ds_head;
		if (head)
			break;
	}

	if (stop_test)
		return;

	printf("Stack initialized. Polling pops (Ctrl+C to stop)...\n\n");
	while (!stop_test) {
		head = &skel->arena->global_ds_head;
		rc = ds_ck_stack_upmc_pop_c(head, &out);

		if (rc == DS_SUCCESS) {
			printf("Popped element %llu: key=%llu value=%llu\n",
			       popped_count, out.key, out.value);
			popped_count++;
		} else if (rc != DS_ERROR_NOT_FOUND) {
			printf("Pop error: %d\n", rc);
		}

		sleep(1);
	}

	printf("\nStopped polling. Total popped: %llu\n", popped_count);
}

static int verify_data_structure(void)
{
	struct ds_ck_stack_upmc_head *head = &skel->arena->global_ds_head;
	int rc;

	rc = ds_ck_stack_upmc_verify_c(head);
	if (rc == DS_SUCCESS)
		printf("Verification passed\n");
	else
		printf("Verification failed: %d\n", rc);

	return rc;
}

static void print_statistics(void)
{
	struct ds_ck_stack_upmc_head *head = &skel->arena->global_ds_head;

	printf("\nKernel inserts: %llu\n", skel->bss->total_kernel_ops);
	printf("Kernel insert failures: %llu\n", skel->bss->total_kernel_failures);
	printf("Userspace pops: %llu\n", popped_count);
	printf("Approximate stack count: %llu\n", head->count);
}

static void print_usage(const char *prog)
{
	printf("Usage: %s [OPTIONS]\n", prog);
	printf("  -v    Verify stack on exit\n");
	printf("  -s    Print stats on exit (default: enabled)\n");
	printf("  -h    Show this help\n");
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

	printf("Loading BPF program for CK stack UPMC...\n");
	skel = skeleton_ck_stack_upmc_bpf__open_and_load();
	if (!skel) {
		fprintf(stderr, "Failed to open and load BPF skeleton\n");
		return 1;
	}

	err = skeleton_ck_stack_upmc_bpf__attach(skel);
	if (err) {
		fprintf(stderr, "Failed to attach BPF programs: %d\n", err);
		goto cleanup;
	}

	poll_and_pop();

	if (config.verify)
		verify_data_structure();
	if (config.print_stats)
		print_statistics();

	err = 0;

cleanup:
	skeleton_ck_stack_upmc_bpf__destroy(skel);
	return err;
}
