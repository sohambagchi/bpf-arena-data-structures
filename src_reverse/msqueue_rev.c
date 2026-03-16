// SPDX-License-Identifier: GPL-2.0
#include <errno.h>
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
#include "msqueue_rev.skel.h"

#define NUM_ITEMS 8

__attribute__((noinline)) void msq_consume_trigger(void)
{
	asm volatile("" ::: "memory");
}

static __u64 now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (__u64)ts.tv_sec * 1000000000ULL + (__u64)ts.tv_nsec;
}

int main(void)
{
	struct msqueue_rev_bpf *skel = NULL;
	struct ds_msqueue *queue;
	struct bpf_link *consume_link;
	struct bpf_uprobe_opts uprobe_opts = {
		.sz = sizeof(uprobe_opts),
		.func_name = "msq_consume_trigger",
	};
	size_t arena_bytes;
	size_t alloc_bytes;
	void *alloc_base;
	long page_size;
	int enqueued = 0;
	int consumed = 0;
	int err = 0;

	printf("=== msqueue_rev: user-producer / kernel-consumer ===\n\n");

	skel = msqueue_rev_bpf__open_and_load();
	if (!skel) {
		fprintf(stderr, "ERROR: failed to open/load BPF skeleton: %s\n",
			strerror(errno));
		return 1;
	}

	page_size = sysconf(_SC_PAGESIZE);
	if (page_size <= 0) {
		fprintf(stderr, "ERROR: could not read page size\n");
		err = 1;
		goto cleanup;
	}

	arena_bytes = (size_t)bpf_map__max_entries(skel->maps.arena) * (size_t)page_size;
	if (arena_bytes <= (size_t)page_size) {
		fprintf(stderr, "ERROR: arena too small (%zu bytes)\n", arena_bytes);
		err = 1;
		goto cleanup;
	}

	alloc_base = (void *)((char *)skel->arena + (size_t)page_size);
	alloc_bytes = arena_bytes - (size_t)page_size;
	bpf_arena_userspace_set_range(alloc_base, alloc_bytes);

	queue = &skel->arena->global_queue;
	err = ds_msqueue_init_c(queue);
	if (err != DS_SUCCESS) {
		fprintf(stderr, "ERROR: ds_msqueue_init_c failed: %d\n", err);
		err = 1;
		goto cleanup;
	}

	printf("Arena alloc range: base=%p size=%zu KB\n",
	       alloc_base, alloc_bytes / 1024);
	printf("Queue initialized from userspace\n\n");

	consume_link = bpf_program__attach_uprobe_opts(
		skel->progs.bpf_msq_consume,
		getpid(),
		"/proc/self/exe",
		0,
		&uprobe_opts);
	if (!consume_link) {
		fprintf(stderr, "ERROR: failed to attach uprobe: %s\n", strerror(errno));
		err = 1;
		goto cleanup;
	}
	skel->links.bpf_msq_consume = consume_link;

	printf("Attached uprobe consumer on '%s'\n\n", uprobe_opts.func_name);

	for (int i = 0; i < NUM_ITEMS; i++) {
		__u64 key = (__u64)(i + 1);
		__u64 value = now_ns();

		err = ds_msqueue_insert_c(queue, key, value);
		if (err != DS_SUCCESS) {
			fprintf(stderr,
				"enqueue[%d] failed: %d (enqueued=%d)\n",
				i, err, enqueued);
			break;
		}

		printf("enqueued[%d] key=%llu value=%llu\n",
		       i,
		       (unsigned long long)key,
		       (unsigned long long)value);
		enqueued++;
	}

	printf("\nEnqueued %d items, queue->count=%llu\n",
	       enqueued, (unsigned long long)queue->count);

	for (int i = 0; i < enqueued; i++) {
		msq_consume_trigger();
		printf("consume-trigger[%d]\n", i);
	}

	consumed = (int)skel->bss->total_consumed;

	err = ds_msqueue_verify_c(queue);
	printf("\nverify -> %d\n", err);
	if (err != DS_SUCCESS)
		printf("WARNING: queue verification failed\n");

	printf("\nSummary:\n");
	printf("  enqueued=%d\n", enqueued);
	printf("  consumed=%d\n", consumed);
	printf("  queue->count=%llu\n", (unsigned long long)queue->count);
	printf("  bpf total_consumed=%llu\n", (unsigned long long)skel->bss->total_consumed);
	printf("  bpf total_failures=%llu\n", (unsigned long long)skel->bss->total_failures);

	err = (consumed == enqueued) ? 0 : 1;

cleanup:
	msqueue_rev_bpf__destroy(skel);
	return err;
}
