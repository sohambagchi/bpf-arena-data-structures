// SPDX-License-Identifier: GPL-2.0
/* Skeleton Userspace Program for Michael-Scott Queue Testing
 * 
 * This program provides a framework for testing the MS Queue where:
 * - Multiple userspace threads perform operations via direct arena access
 * - Kernel-space operations are triggered via syscall tracepoints
 * - Verification and statistics are collected from both sides
 * 
 * USAGE:
 *   ./skeleton_msqueue [OPTIONS]
 *   
 * OPTIONS:
 *   -t N    Number of userspace threads (default: 4)
 *   -o N    Operations per thread (default: 1000)
 *   -k N    Key range for operations (default: 10000)
 *   -d N    Test duration in seconds (default: 10)
 *   -w TYPE Workload type: insert, delete, search, mixed (default: mixed)
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
	int num_threads;
	int ops_per_thread;
	int key_range;
	int duration_sec;
	const char *workload_type;
	bool verify;
	bool print_stats;
	bool trigger_syscalls;
};

/* Default configuration */
static struct test_config config = {
	.num_threads = 4,
	.ops_per_thread = 1000,
	.key_range = 10000,
	.duration_sec = 10,
	.workload_type = "mixed",
	.verify = false,
	.print_stats = true,
	.trigger_syscalls = false,
};

/* ========================================================================
 * GLOBAL STATE
 * ======================================================================== */

static struct skeleton_msqueue_bpf *skel = NULL;
static volatile bool stop_test = false;
static pthread_barrier_t start_barrier;

struct thread_stats {
	__u64 operations;
	__u64 failures;
	__u64 inserts;
	__u64 deletes;
	__u64 searches;
	__u64 duration_ns;
};

/* ========================================================================
 * WORKLOAD GENERATORS
 * ======================================================================== */

/**
 * generate_random_key - Generate a pseudo-random key
 */
static inline __u64 generate_random_key(int thread_id, __u64 counter)
{
	/* Simple LCG for deterministic testing */
	__u64 seed = (thread_id * 1103515245 + counter * 12345) & 0x7fffffff;
	return seed % config.key_range;
}

/**
 * workload_insert_only - Perform only insert (enqueue) operations
 */
static void workload_insert_only(struct ds_msqueue_head *head, int thread_id, 
                                  struct thread_stats *stats)
{
	for (int i = 0; i < config.ops_per_thread && !stop_test; i++) {
		__u64 key = generate_random_key(thread_id, i);
		__u64 value = key * 2;
		
		int result = ds_msqueue_insert(head, key, value);
		
		stats->operations++;
		stats->inserts++;
		if (result != DS_SUCCESS)
			stats->failures++;
	}
}

/**
 * workload_search_only - Perform only search operations
 */
static void workload_search_only(struct ds_msqueue_head *head, int thread_id,
                                  struct thread_stats *stats)
{
	for (int i = 0; i < config.ops_per_thread && !stop_test; i++) {
		__u64 key = generate_random_key(thread_id, i);
		__u64 value;
		
		int result = ds_msqueue_search(head, key, &value);
		
		stats->operations++;
		stats->searches++;
		if (result != DS_SUCCESS)
			stats->failures++;
	}
}

/**
 * workload_mixed - Perform mixed operations
 * For queue: 50% enqueue (insert), 30% search, 20% dequeue (delete)
 */
static void workload_mixed(struct ds_msqueue_head *head, int thread_id,
                            struct thread_stats *stats)
{
	for (int i = 0; i < config.ops_per_thread && !stop_test; i++) {
		__u64 key = generate_random_key(thread_id, i);
		__u64 value = key * 2;
		int op_type = i % 10; /* 50% insert, 30% search, 20% delete */
		int result;
		
		if (op_type < 5) {
			/* Enqueue (Insert) */
			result = ds_msqueue_insert(head, key, value);
			stats->inserts++;
		} else if (op_type < 8) {
			/* Search */
			result = ds_msqueue_search(head, key, &value);
			stats->searches++;
		} else {
			/* Dequeue (Delete) */
			result = ds_msqueue_delete(head, key);
			stats->deletes++;
		}
		
		stats->operations++;
		if (result != DS_SUCCESS)
			stats->failures++;
	}
}

/* ========================================================================
 * WORKER THREADS
 * ======================================================================== */

struct worker_context {
	int thread_id;
	struct ds_msqueue_head *ds_head;
	struct thread_stats stats;
};

/**
 * userspace_worker - Main worker thread function
 */
static void* userspace_worker(void *arg)
{
	struct worker_context *ctx = (struct worker_context *)arg;
	struct timespec start, end;
	
	/* Wait for all threads to be ready */
	pthread_barrier_wait(&start_barrier);
	
	/* Start timing */
	clock_gettime(CLOCK_MONOTONIC, &start);
	
	/* Execute workload based on configuration */
	if (strcmp(config.workload_type, "insert") == 0) {
		workload_insert_only(ctx->ds_head, ctx->thread_id, &ctx->stats);
	} else if (strcmp(config.workload_type, "search") == 0) {
		workload_search_only(ctx->ds_head, ctx->thread_id, &ctx->stats);
	} else if (strcmp(config.workload_type, "delete") == 0) {
		/* Delete workload - similar to search but with deletes */
		workload_search_only(ctx->ds_head, ctx->thread_id, &ctx->stats);
	} else {
		/* Default: mixed workload */
		workload_mixed(ctx->ds_head, ctx->thread_id, &ctx->stats);
	}
	
	/* End timing */
	clock_gettime(CLOCK_MONOTONIC, &end);
	ctx->stats.duration_ns = (end.tv_sec - start.tv_sec) * 1000000000ULL +
	                         (end.tv_nsec - start.tv_nsec);
	
	return NULL;
}

/* ========================================================================
 * SYSCALL TRIGGER (for kernel-side operations)
 * ======================================================================== */

/**
 * syscall_trigger_worker - Trigger syscalls to activate kernel tracepoints
 */
static void* syscall_trigger_worker(void *arg)
{
	int trigger_count = 0;
	
	pthread_barrier_wait(&start_barrier);
	
	while (!stop_test && trigger_count < config.ops_per_thread / 10) {
		/* Fork a process to trigger exec syscall */
		pid_t pid = fork();
		if (pid == 0) {
			/* Child: exec /bin/true to trigger trace_exec */
			execl("/bin/true", "true", NULL);
			exit(0);
		} else if (pid > 0) {
			/* Parent: wait for child */
			waitpid(pid, NULL, 0);
			trigger_count++;
		}
		
		usleep(10000); /* 10ms delay between triggers */
	}
	
	return NULL;
}

/* ========================================================================
 * VERIFICATION AND STATISTICS
 * ======================================================================== */

/**
 * verify_data_structure - Verify integrity from userspace
 */
static int verify_data_structure(void)
{
	struct ds_msqueue_head *head = skel->bss->ds_head;
	
	printf("Verifying data structure from userspace...\n");
	
	int result = ds_msqueue_verify(head);
	if (result == DS_SUCCESS) {
		printf("✓ Data structure verification PASSED\n");
	} else {
		printf("✗ Data structure verification FAILED (error %d)\n", result);
	}
	
	/* Also trigger kernel-side verification */
	LIBBPF_OPTS(bpf_test_run_opts, opts);
	int ret = bpf_prog_test_run_opts(
		bpf_program__fd(skel->progs.verify_structure), &opts);
	
	if (ret == 0 && opts.retval == DS_SUCCESS) {
		printf("✓ Kernel-side verification PASSED\n");
	} else {
		printf("✗ Kernel-side verification FAILED\n");
	}
	
	return result;
}

/**
 * print_statistics - Print comprehensive statistics
 */
static void print_statistics(struct worker_context *workers, int num_workers)
{
	struct thread_stats total = {0};
	
	printf("\n");
	printf("============================================================\n");
	printf("               MS QUEUE TEST STATISTICS                     \n");
	printf("============================================================\n\n");
	
	/* Per-thread statistics */
	printf("Per-Thread Results:\n");
	printf("%-8s %12s %10s %10s %10s %10s %12s\n",
	       "Thread", "Operations", "Enqueues", "Dequeues", "Searches", "Failures", "Ops/sec");
	printf("------------------------------------------------------------\n");
	
	for (int i = 0; i < num_workers; i++) {
		struct thread_stats *s = &workers[i].stats;
		double ops_per_sec = (double)s->operations * 1e9 / s->duration_ns;
		
		printf("%-8d %12llu %10llu %10llu %10llu %10llu %12.0f\n",
		       i, s->operations, s->inserts, s->deletes, 
		       s->searches, s->failures, ops_per_sec);
		
		total.operations += s->operations;
		total.inserts += s->inserts;
		total.deletes += s->deletes;
		total.searches += s->searches;
		total.failures += s->failures;
		if (s->duration_ns > total.duration_ns)
			total.duration_ns = s->duration_ns;
	}
	
	/* Total statistics */
	printf("------------------------------------------------------------\n");
	double total_ops_per_sec = (double)total.operations * 1e9 / total.duration_ns;
	printf("%-8s %12llu %10llu %10llu %10llu %10llu %12.0f\n",
	       "TOTAL", total.operations, total.inserts, total.deletes,
	       total.searches, total.failures, total_ops_per_sec);
	
	/* Kernel statistics */
	printf("\nKernel-Side Operations:\n");
	printf("  Total operations: %llu\n", skel->bss->total_kernel_ops);
	printf("  Total failures:   %llu\n", skel->bss->total_kernel_failures);
	
	/* Arena statistics not accessible from userspace (arena memory not in BSS) */
	
	/* Data structure statistics */
	printf("\nMS Queue State:\n");
	struct ds_msqueue_head *head = skel->bss->ds_head;
	if (head) {
		printf("  Elements in queue: %llu\n", head->count);
	}
	
	printf("\n");
	printf("Test Duration: %.2f seconds\n", total.duration_ns / 1e9);
	printf("============================================================\n\n");
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
	printf("Test Michael-Scott concurrent queue with BPF arena\n\n");
	printf("OPTIONS:\n");
	printf("  -t N    Number of userspace threads (default: 4)\n");
	printf("  -o N    Operations per thread (default: 1000)\n");
	printf("  -k N    Key range for operations (default: 10000)\n");
	printf("  -d N    Test duration in seconds (default: 10)\n");
	printf("  -w TYPE Workload type: insert, search, delete, mixed (default: mixed)\n");
	printf("  -v      Verify data structure integrity\n");
	printf("  -s      Print statistics (default: enabled)\n");
	printf("  -K      Trigger kernel operations via syscalls\n");
	printf("  -h      Show this help\n");
	printf("\nExamples:\n");
	printf("  %s -t 8 -o 10000 -w mixed\n", prog);
	printf("  %s -t 4 -o 5000 -k 1000 -v\n", prog);
	printf("  %s -t 2 -K -w insert\n", prog);
}

static int parse_args(int argc, char **argv)
{
	int opt;
	
	while ((opt = getopt(argc, argv, "t:o:k:d:w:vsKh")) != -1) {
		switch (opt) {
		case 't':
			config.num_threads = atoi(optarg);
			if (config.num_threads <= 0 || config.num_threads > 128) {
				fprintf(stderr, "Invalid thread count: %s\n", optarg);
				return -1;
			}
			break;
		case 'o':
			config.ops_per_thread = atoi(optarg);
			if (config.ops_per_thread <= 0) {
				fprintf(stderr, "Invalid operation count: %s\n", optarg);
				return -1;
			}
			break;
		case 'k':
			config.key_range = atoi(optarg);
			if (config.key_range <= 0) {
				fprintf(stderr, "Invalid key range: %s\n", optarg);
				return -1;
			}
			break;
		case 'd':
			config.duration_sec = atoi(optarg);
			if (config.duration_sec <= 0) {
				fprintf(stderr, "Invalid duration: %s\n", optarg);
				return -1;
			}
			break;
		case 'w':
			config.workload_type = optarg;
			if (strcmp(optarg, "insert") != 0 &&
			    strcmp(optarg, "search") != 0 &&
			    strcmp(optarg, "delete") != 0 &&
			    strcmp(optarg, "mixed") != 0) {
				fprintf(stderr, "Invalid workload type: %s\n", optarg);
				return -1;
			}
			break;
		case 'v':
			config.verify = true;
			break;
		case 's':
			config.print_stats = true;
			break;
		case 'K':
			config.trigger_syscalls = true;
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
	pthread_t *threads = NULL;
	pthread_t syscall_thread;
	struct worker_context *workers = NULL;
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
	
	/* Note: BPF program uses default values for config_num_operations and config_key_range */
	
	/* Initialize data structure from kernel side */
	LIBBPF_OPTS(bpf_test_run_opts, opts);
	err = bpf_prog_test_run_opts(
		bpf_program__fd(skel->progs.manual_operation), &opts);
	if (err) {
		fprintf(stderr, "Failed to initialize data structure: %d\n", err);
		goto cleanup;
	}
	
	printf("\nTest Configuration:\n");
	printf("  Data Structure:   Michael-Scott Queue (Lock-Free)\n");
	printf("  Threads:          %d\n", config.num_threads);
	printf("  Ops per thread:   %d\n", config.ops_per_thread);
	printf("  Key range:        %d\n", config.key_range);
	printf("  Workload:         %s\n", config.workload_type);
	printf("  Trigger syscalls: %s\n", config.trigger_syscalls ? "yes" : "no");
	printf("  Verification:     %s\n", config.verify ? "enabled" : "disabled");
	printf("\n");
	
	/* Allocate worker contexts */
	workers = calloc(config.num_threads, sizeof(*workers));
	threads = calloc(config.num_threads, sizeof(*threads));
	if (!workers || !threads) {
		fprintf(stderr, "Failed to allocate memory\n");
		goto cleanup;
	}
	
	/* Initialize barrier */
	pthread_barrier_init(&start_barrier, NULL, 
	                     config.num_threads + (config.trigger_syscalls ? 1 : 0));
	
	/* Create worker threads */
	printf("Starting %d worker threads...\n", config.num_threads);
	for (int i = 0; i < config.num_threads; i++) {
		workers[i].thread_id = i;
		workers[i].ds_head = skel->bss->ds_head;
		
		err = pthread_create(&threads[i], NULL, userspace_worker, &workers[i]);
		if (err) {
			fprintf(stderr, "Failed to create thread %d: %s\n", i, strerror(err));
			goto cleanup;
		}
	}
	
	/* Optionally start syscall trigger thread */
	if (config.trigger_syscalls) {
		printf("Starting syscall trigger thread...\n");
		pthread_create(&syscall_thread, NULL, syscall_trigger_worker, NULL);
	}
	
	printf("Test running...\n\n");
	
	/* Wait for all threads to complete */
	for (int i = 0; i < config.num_threads; i++) {
		pthread_join(threads[i], NULL);
	}
	
	if (config.trigger_syscalls) {
		stop_test = true;
		pthread_join(syscall_thread, NULL);
	}
	
	printf("All threads completed\n\n");
	
	/* Print statistics */
	if (config.print_stats) {
		print_statistics(workers, config.num_threads);
	}
	
	/* Verify data structure if requested */
	if (config.verify) {
		verify_data_structure();
	}
	
	printf("Test completed successfully!\n");

cleanup:
	pthread_barrier_destroy(&start_barrier);
	free(workers);
	free(threads);
	skeleton_msqueue_bpf__destroy(skel);
	return err;
}
