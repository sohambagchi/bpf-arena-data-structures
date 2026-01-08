// SPDX-License-Identifier: GPL-2.0
/* Skeleton Userspace Program for BPF Arena Non-Blocking BST Testing
 * 
 * SIMPLIFIED DESIGN:
 * - Kernel: LSM hook on inode_create inserts items (triggers on file creation)
 * - Userspace: Single thread searches the data structure
 * 
 * USAGE:
 *   ./skeleton_bintree [OPTIONS]
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
#include "ds_bintree.h"
#include "skeleton_bintree.skel.h"

/* ========================================================================
 * CONFIGURATION
 * ======================================================================== */

struct test_config {
	int sleep_duration;
	bool verify;
	bool print_stats;
};

/* Default configuration */
static struct test_config config = {
	.sleep_duration = 5,
	.verify = false,
	.print_stats = true,
};

/* ========================================================================
 * GLOBAL STATE
 * ======================================================================== */

static struct skeleton_bintree_bpf *skel = NULL;
static volatile bool stop_test = false;
static __u64 search_count = 0;
static __u64 found_count = 0;

/* ========================================================================
 * SEARCH OPERATIONS
 * ======================================================================== */

/**
 * search_for_pids - Search for known PIDs in the tree
 */
static void search_for_pids()
{
	struct ds_bst_head *head = skel->bss->ds_head;
	int result;
	__u64 test_keys[] = {1, 100, 1000, 9999, 12345};
	int num_keys = sizeof(test_keys) / sizeof(test_keys[0]);
	
	if (!head) {
		printf("Tree not initialized, waiting for LSM hook trigger...\n");
		/* Keep polling until initialized */
		while (!head && !stop_test) {
			head = skel->bss->ds_head;
			sleep(1);
		}
		if (stop_test)
			return;
	}
	
	printf("Starting continuous search operations (Ctrl+C to stop)...\n\n");
	
	while (!stop_test) {
		/* Try to search for various keys */
		for (int i = 0; i < num_keys && !stop_test; i++) {
            struct ds_kv kv = { .key = test_keys[i], .value = 0 };
			result = ds_bintree_search(head, kv);
			search_count++;
			
			if (result == DS_SUCCESS) {
				printf("Found key %llu in tree (search #%llu)\n", 
				       test_keys[i], search_count);
				found_count++;
			}
		}
		
		/* Also try to search for current PID */
		__u64 my_pid = getpid();
		struct ds_kv kv = { .key = my_pid, .value = 0 };
		result = ds_bintree_search(head, kv);
		search_count++;
		
		if (result == DS_SUCCESS) {
			printf("Found our PID %llu in tree (search #%llu)\n",
			       my_pid, search_count);
			found_count++;
		}
		
		/* Sleep a bit between search batches */
		sleep(1);
	}
	
	printf("\nStopped searching. Total searches: %llu, Found: %llu\n",
	       search_count, found_count);
}

/* ========================================================================
 * TREE TRAVERSAL
 * ======================================================================== */

/**
 * traverse_and_print_tree - In-order traversal to print all key-value pairs
 */
static void traverse_and_print_tree(void)
{
	struct ds_bst_head *head = skel->bss->ds_head;
	
	if (!head || !head->root) {
		printf("Tree not initialized or empty\n");
		return;
	}
	
	printf("\n");
	printf("============================================================\n");
	printf("              TREE CONTENTS (In-Order)                     \n");
	printf("============================================================\n\n");
	
	/* Allocate stack for iterative traversal (generous size) */
	#define MAX_STACK_SIZE 4096
	struct ds_bst_tree_node **stack = malloc(sizeof(void *) * MAX_STACK_SIZE);
	if (!stack) {
		fprintf(stderr, "Failed to allocate stack for traversal\n");
		return;
	}
	
	int stack_top = 0;
	__u64 leaf_count = 0;
	
	/* Push root to start */
	stack[stack_top++] = (struct ds_bst_tree_node *)head->root;
	
	/* Iterative in-order traversal */
	while (stack_top > 0) {
		struct ds_bst_tree_node *node = stack[--stack_top];
		
		if (bst_is_internal(node)) {
			struct ds_bst_internal *internal = (struct ds_bst_internal *)node;
			
			/* Push right first (so left is processed first) */
			if (internal->pRight)
				stack[stack_top++] = internal->pRight;
			if (internal->pLeft)
				stack[stack_top++] = internal->pLeft;
		} else {
			/* Leaf node - print if not sentinel */
			struct ds_bst_leaf *leaf = (struct ds_bst_leaf *)node;
			if (leaf->kv.key < BST_SENTINEL_KEY1) {
				printf("  Key: %-20llu Value: %llu\n", 
				       leaf->kv.key, leaf->kv.value);
				leaf_count++;
			}
		}
		
		if (stack_top >= MAX_STACK_SIZE - 1) {
			fprintf(stderr, "Stack overflow during traversal\n");
			break;
		}
	}
	
	free(stack);
	
	printf("\n");
	printf("Total leaves: %llu (expected: %llu)\n", leaf_count, head->count);
	
	/* Validate count */
	if (leaf_count == head->count) {
		printf("✓ Leaf count matches head->count\n");
	} else {
		printf("✗ WARNING: Leaf count mismatch! Found %llu, expected %llu\n",
		       leaf_count, head->count);
	}
	
	printf("============================================================\n\n");
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
	
	int result = ds_bintree_verify(head);
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
	printf("  Total searches:   %llu\n", search_count);
	printf("  Keys found:       %llu\n", found_count);
	
	/* Data structure statistics */
	printf("\nData Structure State:\n");
	struct ds_bst_head *head = skel->bss->ds_head;
	if (head) {
		printf("  Elements in tree: %llu\n", head->count);
	} else {
		printf("  Data structure not yet initialized\n");
	}
	
	printf("\n============================================================\n\n");

	printf("Operation Statistics:\n");
	printf("  Total inserts:                    %u\n", head->stats.total_inserts);
	printf("  Total deletes:                    %u\n", head->stats.total_deletes);
	printf("  Total searches:                   %u\n", head->stats.total_searches);
	printf("  Total rebalances:                 %u\n", head->stats.total_rebalances);
	printf("  Total failures:                   %u\n", head->stats.total_failures);
	printf("  Max tree depth:                   %u\n", head->stats.max_tree_depth);
	printf("  Insert failures (invalid head):   %u\n", head->stats.insert_failure_invalid_head);
	printf("  Insert failures (invalid key):    %u\n", head->stats.insert_failure_invalid_key);
	
	printf("  Insert failures (exists):         %u\n", head->stats.insert_failure_exists);
	printf("  Insert failures (no parent):      %u\n", head->stats.insert_failure_no_parent);
	printf("  Insert failures (no leaf):        %u\n", head->stats.insert_failure_no_leaf);
	printf("  Insert failures (leaf is internal): %u\n", head->stats.insert_failure_leaf_is_internal);
	printf("  Insert failures (CAS fail):       %u\n", head->stats.insert_failure_cas_fail);
	printf("  Insert failures (nomem):          %u\n", head->stats.insert_failure_nomem);
	printf("  Insert failures (busy):           %u\n", head->stats.insert_failure_busy);
	printf("  Insert retries (didn't help):     %u\n", head->stats.insert_retry_didnt_help);
	printf("  Inserts that became updates:      %u\n", head->stats.insert_into_updates);
	printf("  Delete failures (invalid head):   %u\n", head->stats.delete_failure_invalid_head);
	printf("  Delete failures (not found):	    %u\n", head->stats.delete_failure_not_found);
	printf("  Delete failures (nomem):          %u\n", head->stats.delete_failure_nomem);
	printf("  Delete failures (busy):           %u\n", head->stats.delete_failure_busy);
	printf("  Delete retries (didn't help GP):  %u\n", head->stats.delete_retry_didnt_help_gp);
	printf("  Delete retries (didn't help P):   %u\n", head->stats.delete_retry_didnt_help_p);
	printf("  Search failures (invalid head):   %u\n", head->stats.search_failure_invalid_head);
	printf("  Searches not found:               %u\n", head->stats.search_not_found);
	printf("  Searches found:                   %u\n", head->stats.search_found);
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
	printf("Test non-blocking binary search tree with BPF arena\n\n");
	printf("DESIGN:\n");
	printf("  Kernel:    LSM hook on inode_create inserts items (triggers on file creation)\n");
	printf("  Userspace: Continuously searches for known keys in the tree\n\n");
	printf("OPTIONS:\n");
	printf("  -d N    Sleep duration before starting search (default: 5 seconds)\n");
	printf("  -v      Verify data structure integrity on exit\n");
	printf("  -s      Print statistics on exit (default: enabled)\n");
	printf("  -h      Show this help\n");
	printf("\nKernel inserts trigger automatically on file creation (inode_create)\n");
	printf("Userspace continuously searches for elements (Ctrl+C to stop)\n");
	printf("\nExamples:\n");
	printf("  %s          # Run with default options\n", prog);
	printf("  %s -v -d 10 # Wait 10 seconds, then search and verify on exit\n", prog);
}

static int parse_args(int argc, char **argv)
{
	int opt;
	
	while ((opt = getopt(argc, argv, "d:vsh")) != -1) {
		switch (opt) {
		case 'd':
			config.sleep_duration = atoi(optarg);
			if (config.sleep_duration < 0)
				config.sleep_duration = 5;
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
	skel = skeleton_bintree_bpf__open_and_load();
	if (!skel) {
		fprintf(stderr, "Failed to open and load BPF skeleton\n");
		return 1;
	}
	
	/* Attach BPF programs */
	err = skeleton_bintree_bpf__attach(skel);
	if (err) {
		fprintf(stderr, "Failed to attach BPF programs: %d\n", err);
		goto cleanup;
	}
	
	printf("BPF programs attached successfully\n");
	printf("Data structure will be lazily initialized on first LSM hook trigger\n");
	printf("Kernel inserts triggered automatically on file creation (inode_create)\n\n");
	
	if (config.sleep_duration > 0) {
		printf("Sleeping for %d seconds to allow kernel inserts...\n",
		       config.sleep_duration);
		sleep(config.sleep_duration);
		printf("Starting search operations...\n\n");
	}
	
	/* Start continuous searching */
	search_for_pids();
	
	/* Traverse and print tree contents */
	traverse_and_print_tree();
	
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
	skeleton_bintree_bpf__destroy(skel);
	return err;
}
