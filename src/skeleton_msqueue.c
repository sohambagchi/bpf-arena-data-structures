// SPDX-License-Identifier: GPL-2.0
/* Skeleton Userspace Program for BPF Arena MS Queue Testing
 * 
 * SIMPLIFIED DESIGN:
 * - Kernel: LSM hook on inode_create inserts items (triggers on file creation)
 * - Userspace: Single thread sleeps, then reads the data structure
 * 
 * USAGE:
 *   ./skeleton_msqueue [OPTIONS]
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
#include "ds_msqueue.h"
#include "skeleton_msqueue.skel.h"

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

static struct skeleton_msqueue_bpf *skel = NULL;
static volatile bool stop_test = false;
static __u64 dequeued_count = 0;

/* ========================================================================
 * POLLING AND DEQUEUE
 * ======================================================================== */

/**
 * poll_and_dequeue - Continuously poll and dequeue elements
 */
static void poll_and_dequeue()
{
	struct ds_msqueue *queue;
	struct ds_kv data;
	int result;
	
	printf("Waiting for queue initialization...\n");
	
	/* Wait for queue to be initialized by kernel */
	while (!stop_test) {
		queue = &skel->arena->global_ds_queue;
		if (queue && queue->head)
			break;
	}
	
	if (stop_test)
		return;
	
	printf("Queue initialized! Starting continuous polling (Ctrl+C to stop)...\n\n");
	
	while (!stop_test) {
		queue = &skel->arena->global_ds_queue;
		if (!queue) {
			printf("DEBUG: queue is NULL\n");
			continue;
		}
		
		result = ds_msqueue_pop(queue, &data);
		
		if (result == DS_SUCCESS) {
			/* Successfully dequeued an element */
			printf("Dequeued element %llu: pid=%llu, ts=%llu\n", 
			       dequeued_count, data.key, data.value);
			dequeued_count++;
		} else if (result == DS_ERROR_NOT_FOUND) {
			/* Queue is empty - this is normal, just continue polling */
		} else {
			/* Actual error (negative return value) */
			printf("Dequeue error: %d (queue=%p, data=%p)\n", result, queue, &data);
			sleep(2);
			continue;
		}

		sleep(1); /* Throttle polling to avoid busy-waiting */
	}
	
	printf("\nStopped polling. Total dequeued: %llu\n", dequeued_count);
}

/* ========================================================================
 * VERIFICATION AND STATISTICS
 * ======================================================================== */

/**
 * verify_data_structure - Verify integrity from userspace
 */
static int verify_data_structure(void)
{
	struct ds_msqueue *queue = skel->bss->ds_queue;
	
	printf("Verifying data structure from userspace...\n");
	
	int result = ds_msqueue_verify(queue);
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
	printf("  Elements dequeued: %llu\n", dequeued_count);
	
	/* Data structure statistics */
	printf("\nData Structure State:\n");
	struct ds_msqueue *queue = skel->bss->ds_queue;
	if (queue) {
		printf("  Elements in queue: %llu\n", queue->count);
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
	printf("Test Michael-Scott queue with BPF arena\n\n");
	printf("DESIGN:\n");
	printf("  Kernel:    LSM hook on inode_create inserts items (triggers on file creation)\n");
	printf("  Userspace: Continuously polls and dequeues elements as they arrive\n\n");
	printf("OPTIONS:\n");
	printf("  -v      Verify data structure integrity on exit\n");
	printf("  -s      Print statistics on exit (default: enabled)\n");
	printf("  -h      Show this help\n");
	printf("\nKernel inserts trigger automatically on file creation (inode_create)\n");
	printf("Userspace continuously dequeues and prints elements (Ctrl+C to stop)\n");
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
	printf("Loading BPF program for MS Queue...\n");
	skel = skeleton_msqueue_bpf__open_and_load();
	if (!skel) {
		fprintf(stderr, "Failed to open and load BPF skeleton\n");
		return 1;
	}
	
	/* Attach BPF programs */
	err = skeleton_msqueue_bpf__attach(skel);
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
	skeleton_msqueue_bpf__destroy(skel);
	return err;
}
