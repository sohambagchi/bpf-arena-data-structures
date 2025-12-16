// SPDX-License-Identifier: GPL-2.0
/* Vyukhov MPMC Queue Userspace Program for BPF Arena Data Structure Testing
 * 
 * SIMPLIFIED DESIGN:
 * - Kernel: LSM hook on inode_create inserts items (triggers on execve)
 * - Userspace: Single thread sleeps, then reads the data structure
 * 
 * USAGE:
 *   ./skeleton_vyukhov [OPTIONS]
 *   
 * OPTIONS:
 *   -d N    Sleep duration in seconds before reading (default: 5)
 *   -v      Verify data structure integrity
 *   -s      Print statistics
 *   -h      Show this help
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>

/* Include data structure headers */
#include "ds_api.h"
#include "ds_vyukhov.h"
#include "skeleton_vyukhov.skel.h"

/* ========================================================================
 * CONFIGURATION
 * ======================================================================== */

struct test_config {
	int sleep_seconds;
	bool verify;
	bool print_stats;
};

/* Default configuration */
static struct test_config config = {
	.sleep_seconds = 5,
	.verify = false,
	.print_stats = true,
};

/* ========================================================================
 * GLOBAL STATE
 * ======================================================================== */

static struct skeleton_vyukhov_bpf *skel = NULL;
static volatile bool stop_test = false;

/* ========================================================================
 * READER THREAD
 * ======================================================================== */

/* Iteration callback */
static __u64 print_count = 0;

static int print_element_callback(__u64 key, __u64 value, void *ctx __attribute__((unused)))
{
	printf("  Element %llu: pid=%llu, last_ts=%llu\n", print_count, key, value);
	print_count++;
	
	/* Stop after 10 elements */
	if (print_count >= 10)
		return 1;
	
	return 0;
}

/**
 * read_data_structure - Sleep then read the data structure
 */
static void read_data_structure()
{
	printf("Sleeping for %d seconds to allow kernel to populate data structure...\n", config.sleep_seconds);
	sleep(config.sleep_seconds);
	
	/* Access the queue head directly from arena memory */
	struct ds_vyukhov_head *head = &skel->arena->global_ds_head;

	if (!head || !head->buffer) {
		printf("Data structure not yet initialized\n");
		return;
	}
	
	printf("Reading data structure...\n");
	
	/* Use ds_vyukhov_iterate API */
	print_count = 0;
	__u64 visited = ds_vyukhov_iterate(head, print_element_callback, NULL);
	
	if (visited >= 10) {
		printf("  ... (showing first 10 elements)\n");
	}
	
	printf("Total elements visited: %llu\n", visited);
	printf("Approximate queue size: %llu\n", head->count);
}

/* ========================================================================
 * VERIFICATION AND STATISTICS
 * ======================================================================== */

/**
 * verify_data_structure - Verify integrity from userspace
 */
static int verify_data_structure(void)
{
	struct ds_vyukhov_head *head = &skel->arena->global_ds_head;
	
	printf("Verifying data structure from userspace...\n");
	
	int result = ds_vyukhov_verify(head);
	if (result == DS_SUCCESS) {
		printf("✓ Data structure verification PASSED\n");
	} else {
		printf("✗ Data structure verification FAILED (error %d)\n", result);
	}
		
	return result;
}

/**
 * print_statistics - Print kernel and data structure statistics
 */
static void print_statistics(void)
{
	printf("\n");
	printf("============================================================\n");
	printf("                    STATISTICS                              \n");
	printf("============================================================\n\n");
	
	/* Kernel statistics */
	printf("Kernel-Side Operations (inode_create LSM hook inserts):\n");
	printf("  Total inserts:    %llu\n", skel->bss->total_kernel_ops);
	printf("  Insert failures:  %llu\n", skel->bss->total_kernel_failures);
	
	/* Data structure statistics */
	printf("\nData Structure State:\n");
	struct ds_vyukhov_head *head = &skel->arena->global_ds_head;
	if (head && head->buffer) {
		printf("  Queue capacity:   %llu\n", head->buffer_mask + 1);
		printf("  Enqueue position: %llu\n", head->enqueue_pos);
		printf("  Dequeue position: %llu\n", head->dequeue_pos);
		printf("  Approximate size: %llu\n", head->count);
		printf("  Current elements: %llu\n", head->enqueue_pos - head->dequeue_pos);
	} else {
		printf("  Data structure not yet initialized\n");
	}
	
	printf("\n============================================================\n\n");
}

/* ========================================================================
 * SIGNAL HANDLING
 * ======================================================================== */

static void signal_handler(int sig)
{
	printf("\nReceived signal %d, stopping test...\n", sig);
	stop_test = true;
}

/* ========================================================================
 * MAIN PROGRAM
 * ======================================================================== */

static void print_usage(const char *prog)
{
	printf("Usage: %s [OPTIONS]\n\n", prog);
	printf("Test Vyukhov's Bounded MPMC Queue with BPF arena\n\n");
	printf("DESIGN:\n");
	printf("  Kernel:    LSM hook on inode_create inserts items (triggers on execve)\n");
	printf("  Userspace: Single thread sleeps, then reads the data structure\n\n");
	printf("OPTIONS:\n");
	printf("  -d N    Sleep duration in seconds before reading (default: 5)\n");
	printf("  -v      Verify data structure integrity\n");
	printf("  -s      Print statistics (default: enabled)\n");
	printf("  -h      Show this help\n");
	printf("\nKernel inserts trigger automatically on program execution (execve)\n");
	printf("\nExamples:\n");
	printf("  %s -d 10     # Sleep 10 seconds before reading\n", prog);
	printf("  %s -d 5 -v   # Sleep 5 seconds then verify\n", prog);
}

static int parse_args(int argc, char **argv)
{
	int opt;
	
	while ((opt = getopt(argc, argv, "d:vsh")) != -1) {
		switch (opt) {
		case 'd':
			config.sleep_seconds = atoi(optarg);
			if (config.sleep_seconds < 0) {
				fprintf(stderr, "Invalid sleep duration: %s\n", optarg);
				return -1;
			}
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

int main(int argc, char **argv)
{
	int err = 0;
	
	/* Parse command line arguments */
	if (parse_args(argc, argv) < 0)
		return 1;
	
	/* Set up signal handlers */
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);
	
	/* Open and load BPF skeleton */
	printf("Loading BPF program...\n");
	skel = skeleton_vyukhov_bpf__open_and_load();
	if (!skel) {
		fprintf(stderr, "Failed to open and load BPF skeleton\n");
		return 1;
	}
	
	/* Attach BPF programs */
	err = skeleton_vyukhov_bpf__attach(skel);
	if (err) {
		fprintf(stderr, "Failed to attach BPF programs: %d\n", err);
		goto cleanup;
	}
	
	printf("BPF programs attached successfully\n");
	printf("Data structure will be lazily initialized on first LSM hook trigger\n");
	printf("Kernel inserts triggered automatically on file creation (inode_create)\n\n");
	
	/* Read the data structure after sleeping */
	read_data_structure();
	
	/* Print statistics */
	if (config.print_stats) {
		print_statistics();
	}
	
	/* Verify data structure if requested */
	if (config.verify) {
		verify_data_structure();
	}
	
	printf("\nDone!\n");

cleanup:
	skeleton_vyukhov_bpf__destroy(skel);
	return err;
}
