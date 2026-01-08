// SPDX-License-Identifier: GPL-2.0
/* Userspace Skeleton for Folly SPSC Queue Testing
 * 
 * SPSC Design (Single-Producer Single-Consumer):
 * - Kernel (Producer):  LSM hook on inode_create inserts (PID, timestamp)
 * - Userspace (Consumer): This program continuously polls and dequeues elements
 * 
 * This is the CONSUMER side, using arena_atomic operations to safely read
 * elements from the ring buffer without locks.
 * 
 * USAGE:
 *   ./skeleton_folly_spsc [OPTIONS]
 *   
 * OPTIONS:
 *   -q N    Queue size (default: 256, must be >= 2)
 *   -v      Verify data structure integrity on exit
 *   -s      Print statistics on exit (default: enabled)
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
#include "ds_folly_spsc.h"
#include "skeleton_folly_spsc.skel.h"

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

static struct skeleton_folly_spsc_bpf *skel = NULL;
static volatile bool stop_test = false;
static __u64 dequeued_count = 0;
static __u64 dequeue_failures = 0;

/* ========================================================================
 * POLLING AND DEQUEUE (CONSUMER)
 * ======================================================================== */

/**
 * poll_and_dequeue - Continuously poll and dequeue elements
 * 
 * This is the CONSUMER in the SPSC design. It continuously polls the
 * ring buffer and dequeues elements as they become available.
 */
static void poll_and_dequeue()
{
	struct ds_spsc_queue_head *head = &skel->arena->global_ds_head;
	struct ds_kv data;
	int result;
	__u64 empty_polls = 0;
	
	if (!head || !head->records) {
		printf("Queue not initialized, waiting for LSM hook trigger...\n");
		/* Keep polling until initialized */
		while ((!head || !head->records) && !stop_test) {
			head = &skel->arena->global_ds_head;
		}
		if (stop_test)
			return;
	}
	
	printf("Starting continuous polling (Ctrl+C to stop)...\n");
	printf("Queue capacity: %u elements (size: %u)\n\n", 
	       head->size - 1, head->size);
	
	while (!stop_test) {
		result = ds_spsc_delete(head, &data);
		
		if (result == DS_SUCCESS) {
			printf("Dequeued element %llu: pid=%llu, ts=%llu\n", 
			       dequeued_count, data.key, data.value);
			dequeued_count++;
			empty_polls = 0; /* Reset empty poll counter */
		} else if (result == DS_ERROR_NOT_FOUND) {
			/* Queue empty, keep polling (busy-wait) */
			empty_polls++;
			
			/* Print periodic status every 100M empty polls */
			if (empty_polls % 100000000 == 0) {
				printf("Still polling... (dequeued so far: %llu)\n", 
				       dequeued_count);
			}
			continue;
		} else {
			/* Some other error */
			printf("Dequeue error: %d\n", result);
			dequeue_failures++;
		}
	}
	
	printf("\nStopped polling. Total dequeued: %llu\n", dequeued_count);
}

/* ========================================================================
 * VERIFICATION AND STATISTICS
 * ======================================================================== */

/**
 * verify_data_structure - Verify SPSC queue integrity from userspace
 */
static int verify_data_structure(void)
{
	struct ds_spsc_queue_head *head = &skel->arena->global_ds_head;
	
	printf("\nVerifying SPSC queue from userspace...\n");
	
	if (!head || !head->records) {
		printf("✗ Queue not initialized\n");
		return DS_ERROR_INVALID;
	}
	
	int result = ds_spsc_verify(head);
	if (result == DS_SUCCESS) {
		printf("✓ SPSC queue verification PASSED\n");
		printf("  Queue size: %u elements\n", ds_spsc_size(head));
		printf("  Read index: %u\n", head->read_idx.idx);
		printf("  Write index: %u\n", head->write_idx.idx);
	} else {
		printf("✗ SPSC queue verification FAILED (error %d)\n", result);
	}
		
	return result;
}

/**
 * print_statistics - Print kernel and userspace statistics
 */
static void print_statistics(void)
{
	printf("\n");
	printf("============================================================\n");
	printf("           SPSC QUEUE STATISTICS                            \n");
	printf("============================================================\n\n");
	
	/* Kernel statistics (Producer) */
	printf("Kernel-Side Operations (Producer - inode_create LSM hook):\n");
	printf("  Total insert attempts: %llu\n", skel->bss->total_kernel_ops);
	printf("  Insert failures:       %llu", skel->bss->total_kernel_failures);
	if (skel->bss->total_kernel_ops > 0) {
		double fail_rate = (100.0 * skel->bss->total_kernel_failures) / 
		                   skel->bss->total_kernel_ops;
		printf(" (%.2f%%)", fail_rate);
	}
	printf("\n");
	
	/* Userspace statistics (Consumer) */
	printf("\nUserspace Operations (Consumer - continuous polling):\n");
	printf("  Elements dequeued:     %llu\n", dequeued_count);
	printf("  Dequeue failures:      %llu\n", dequeue_failures);
	
	/* Data structure state */
	printf("\nQueue State:\n");
	struct ds_spsc_queue_head *head = &skel->arena->global_ds_head;
	if (head && head->records) {
		__u32 remaining = ds_spsc_size(head);
		printf("  Elements remaining:    %u\n", remaining);
		printf("  Queue capacity:        %u\n", head->size - 1);
		printf("  Read index:            %u\n", head->read_idx.idx);
		printf("  Write index:           %u\n", head->write_idx.idx);
		
		/* Loss analysis */
		__u64 total_produced = skel->bss->total_kernel_ops - 
		                       skel->bss->total_kernel_failures;
		__u64 total_accounted = dequeued_count + remaining;
		
		printf("\nData Flow Analysis:\n");
		printf("  Successfully inserted: %llu\n", total_produced);
		printf("  Dequeued + Remaining:  %llu\n", total_accounted);
		if (total_produced == total_accounted) {
			printf("  ✓ No data loss detected\n");
		} else {
			printf("  ⚠ Discrepancy: %lld elements\n", 
			       (long long)(total_produced - total_accounted));
		}
	} else {
		printf("  Queue not initialized\n");
	}
	
	printf("\n============================================================\n\n");
}

/* ========================================================================
 * SIGNAL HANDLING
 * ======================================================================== */

static void signal_handler(int sig)
{
	printf("\nReceived signal %d, stopping consumer...\n", sig);
	stop_test = true;
}

/* ========================================================================
 * MAIN PROGRAM
 * ======================================================================== */

static void print_usage(const char *prog)
{
	printf("Usage: %s [OPTIONS]\n\n", prog);
	printf("Test Folly SPSC (Single-Producer Single-Consumer) Queue with BPF arena\n\n");
	printf("DESIGN:\n");
	printf("  Producer (Kernel):  LSM hook on inode_create inserts (PID, timestamp)\n");
	printf("  Consumer (Userspace): Continuously polls and dequeues elements\n\n");
	printf("OPTIONS:\n");
	printf("  -v      Verify queue integrity on exit\n");
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
	printf("Loading BPF program...\n");
	skel = skeleton_folly_spsc_bpf__open();
	if (!skel) {
		fprintf(stderr, "Failed to open BPF skeleton\n");
		return 1;
	}
	
	err = skeleton_folly_spsc_bpf__load(skel);
	if (err) {
		fprintf(stderr, "Failed to load BPF skeleton: %d\n", err);
		goto cleanup;
	}
	
	/* Attach BPF programs */
	err = skeleton_folly_spsc_bpf__attach(skel);
	if (err) {
		fprintf(stderr, "Failed to attach BPF programs: %d\n", err);
		goto cleanup;
	}
	
	printf("BPF programs attached successfully\n");
	printf("Queue will be lazily initialized on first LSM hook trigger\n");
	printf("Kernel inserts triggered automatically on file creation (inode_create)\n\n");
	
	/* Start continuous polling and dequeuing (Consumer) */
	poll_and_dequeue();
	
	/* Verify queue if requested */
	if (config.verify) {
		verify_data_structure();
	}
	
	/* Print statistics */
	if (config.print_stats) {
		print_statistics();
	}
	
	printf("\nDone!\n");

cleanup:
	skeleton_folly_spsc_bpf__destroy(skel);
	return err;
}
