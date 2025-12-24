// SPDX-License-Identifier: GPL-2.0
/* Skeleton Userspace Program for Ellen Binary Search Tree Testing
 * 
 * SIMPLIFIED DESIGN:
 * - Kernel: LSM hook on inode_create inserts items (triggers on file creation)
 * - Userspace: Single thread sleeps, then pops minimum elements from BST
 * 
 * USAGE:
 *   ./skeleton_bst [OPTIONS]
 *   
 * OPTIONS:
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
#include "ds_bst.h"
#include "skeleton_bst.skel.h"

/* ========================================================================
 * CONFIGURATION
 * ======================================================================== */

struct test_config {
	bool verify;
	bool print_stats;
};

/* Default configuration */
static struct test_config config = {
	.verify = false,
	.print_stats = true,
};

/* ========================================================================
 * GLOBAL STATE
 * ======================================================================== */

static struct skeleton_bst_bpf *skel = NULL;
static volatile bool stop_test = false;
static __u64 dequeued_count = 0;

/* ========================================================================
 * POLLING AND DEQUEUE
 * ======================================================================== */

/**
 * poll_and_dequeue - Continuously poll and pop minimum elements
 */
static void poll_and_dequeue()
{
	struct ds_bst_head *head = skel->bss->ds_head;
	struct ds_kv data;
	int result;
	
	if (!head) {
		printf("BST not initialized, waiting for LSM hook trigger...\n");
		/* Keep polling until initialized */
		while (!head && !stop_test) {
			head = skel->bss->ds_head;
		}
		if (stop_test)
			return;
	}
	
	printf("Starting continuous polling (Ctrl+C to stop)...\n\n");
	
	while (!stop_test) {
		result = ds_bst_pop(head, &data);
		
		if (result == DS_SUCCESS) {
			printf("Popped minimum element %llu: pid=%llu, ts=%llu\n", 
			       dequeued_count, data.key, data.value);
			dequeued_count++;
		} else if (result == DS_ERROR_NOT_FOUND) {
			/* Tree empty, keep polling (busy-wait) */
			continue;
		} else {
			/* Some other error */
			printf("Pop error: %d\n", result);
		}
	}
	
	printf("\nStopped polling. Total popped: %llu\n", dequeued_count);
}

/* ========================================================================
 * VERIFICATION AND STATISTICS
 * ======================================================================== */

/**
 * verify_data_structure - Verify integrity from userspace
 */
static int verify_data_structure(void)
{
	struct ds_bst_head *head = skel->bss->ds_head;
	
	printf("Verifying data structure from userspace...\n");
	
	int result = ds_bst_verify(head);
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
	
	/* Userspace statistics */
	printf("\nUserspace Operations:\n");
	printf("  Elements popped:  %llu\n", dequeued_count);
	
	/* Data structure statistics */
	printf("\nData Structure State:\n");
	struct ds_bst_head *head = skel->bss->ds_head;
	if (head) {
		printf("  Total inserts:     %llu\n", skel->bss->bst_insert_count);
		printf("  Total deletes:     %llu\n", skel->bss->bst_delete_count);
		printf("  Total searches:    %llu\n", skel->bss->bst_search_count);
		printf("  Insert retries:    %llu\n", skel->bss->bst_insert_retries);
		printf("  Delete retries:    %llu\n", skel->bss->bst_delete_retries);
		printf("  Elements in tree:  ~%llu\n", 
		       skel->bss->bst_insert_count - skel->bss->bst_delete_count);
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
	printf("Test Ellen Binary Search Tree with BPF arena\n\n");
	printf("DESIGN:\n");
	printf("  Kernel:    LSM hook on inode_create inserts items (triggers on file creation)\n");
	printf("  Userspace: Continuously polls and pops minimum elements from the BST\n\n");
	printf("OPTIONS:\n");
	printf("  -v      Verify data structure integrity on exit\n");
	printf("  -s      Print statistics on exit (default: enabled)\n");
	printf("  -h      Show this help\n");
	printf("\nKernel inserts trigger automatically on file creation (inode_create)\n");
	printf("Userspace continuously pops minimum elements and prints (Ctrl+C to stop)\n");
	printf("\nExamples:\n");
	printf("  %s          # Run with default options\n", prog);
	printf("  %s -v       # Run and verify on exit\n", prog);
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
	
	/* Parse command line arguments */
	if (parse_args(argc, argv) < 0)
		return 1;
	
	/* Set up signal handlers */
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);
	
	/* Open and load BPF skeleton */
	printf("Loading BPF program...\n");
	skel = skeleton_bst_bpf__open_and_load();
	if (!skel) {
		fprintf(stderr, "Failed to open and load BPF skeleton\n");
		return 1;
	}
	
	/* Attach BPF programs */
	err = skeleton_bst_bpf__attach(skel);
	if (err) {
		fprintf(stderr, "Failed to attach BPF programs: %d\n", err);
		goto cleanup;
	}
	
	printf("BPF programs attached successfully\n");
	printf("Data structure will be lazily initialized on first LSM hook trigger\n");
	printf("Kernel inserts triggered automatically on file creation (inode_create)\n\n");
	
	/* Start continuous polling and dequeuing */
	poll_and_dequeue();
	
	/* Verify data structure if requested */
	if (config.verify) {
		verify_data_structure();
	}
	
	/* Print statistics */
	if (config.print_stats) {
		print_statistics();
	}
	
	printf("\nDone!\n");

cleanup:
	skeleton_bst_bpf__destroy(skel);
	return err;
}
