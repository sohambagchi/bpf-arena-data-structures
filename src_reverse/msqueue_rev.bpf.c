// SPDX-License-Identifier: GPL-2.0
/*
 * msqueue_rev.bpf.c — Reversed MS-Queue: user-producer, kernel-consumer.
 *
 * Userspace initializes and enqueues through ds_msqueue_*_c() directly in the
 * arena map. Kernel side consumes from a uprobe attached to a userspace
 * trigger function.
 */

#define BPF_NO_KFUNC_PROTOTYPES
#include <vmlinux.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>
#include "bpf_experimental.h"

/* =========================================================================
 * Arena map
 * ========================================================================= */

struct {
	__uint(type, BPF_MAP_TYPE_ARENA);
	__uint(map_flags, BPF_F_MMAPABLE);
	__uint(max_entries, 1000); /* 1000 x 4 KB = 4 MB */
#ifdef __TARGET_ARCH_arm64
	__ulong(map_extra, 0x1ull << 32);
#else
	__ulong(map_extra, 0x1ull << 44);
#endif
} arena SEC(".maps");

#include "libarena_ds.h"
#include "ds_api.h"
#include "ds_msqueue.h"

/* queue state in arena, shared with userspace */
struct ds_msqueue __arena global_queue;

/* Counters readable from userspace */
__u64 total_consumed  = 0;
__u64 total_failures  = 0;

/*
 * Uprobe consumer; userspace explicitly calls msq_consume_trigger() after
 * enqueueing and this program pops one element from the arena queue.
 */
SEC("uprobe.s")
int bpf_msq_consume(struct pt_regs *ctx)
{
	(void)ctx;
	struct ds_msqueue __arena *q = &global_queue;
	struct ds_msqueue_elem __arena *head;
	struct ds_msqueue_elem __arena *tail;
	struct ds_kv data = {};
	int ret;

	head = READ_ONCE(q->head);
	tail = READ_ONCE(q->tail);
	if (!head || !tail)
		return DS_ERROR_INVALID;

	ret = ds_msqueue_pop_lkmm(q, &data);
	if (ret == DS_SUCCESS) {
		bpf_printk("msqueue_rev: consumed key=%llu value=%llu\n",
			   data.key, data.value);
		total_consumed++;
	} else if (ret == DS_ERROR_NOT_FOUND) {
		bpf_printk("msqueue_rev: queue empty on consume attempt\n");
		total_failures++;
	} else {
		bpf_printk("msqueue_rev: consume error %d\n", ret);
		total_failures++;
	}

	return ret;
}

char _license[] SEC("license") = "GPL";
