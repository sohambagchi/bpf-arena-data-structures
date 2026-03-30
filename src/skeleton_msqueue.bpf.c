// SPDX-License-Identifier: GPL-2.0
/* Skeleton BPF Program for Michael-Scott Queue Testing
 * 
 * This program provides a template for testing the MS Queue data structure
 * where operations can be triggered from both kernel space (via LSM hooks)
 * and userspace (via direct arena access).
 * 
 * CUSTOMIZATION POINTS:
 * - Include your data structure header below (search for DS_API_INSERT)
 * - Add operation dispatch cases in handle_operation()
 * - Configure arena size in the map definition
 */

#define BPF_NO_KFUNC_PROTOTYPES
#include <vmlinux.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>
#include "bpf_experimental.h"

/* ========================================================================
 * ARENA MAP DEFINITION
 * ======================================================================== */

struct {
	__uint(type, BPF_MAP_TYPE_ARENA);
	__uint(map_flags, BPF_F_MMAPABLE);
	__uint(max_entries, 1000); /* Number of pages (4KB each) - 4MB total */
#ifdef __TARGET_ARCH_arm64
	__ulong(map_extra, 0x1ull << 32); /* Start of mmap() region */
#else
	__ulong(map_extra, 0x1ull << 44); /* Start of mmap() region */
#endif
} arena SEC(".maps");

/* Include arena library and API definitions */
#include "libarena_ds.h"
#include "ds_api.h"

/* ========================================================================
 * DS_API_INSERT: Include your data structure headers here
 * ======================================================================== */
#include "ds_msqueue.h"  /* Michael-Scott Queue implementation */
#include "ds_metrics.h"

/* ========================================================================
 * GLOBAL STATE
 * ======================================================================== */

/* kernel-producer -> user-consumer queue */
struct ds_msqueue __arena global_ds_queue_ku;

/* user-producer -> kernel-consumer queue */
struct ds_msqueue __arena global_ds_queue_uk;

struct ds_metrics_store __arena global_metrics;

/* Statistics and control */
__u64 total_kernel_prod_ops = 0;
__u64 total_kernel_prod_failures = 0;
__u64 total_kernel_consume_ops = 0;
__u64 total_kernel_consume_failures = 0;
__u64 total_kernel_consumed = 0;
bool initialized_ku = false;

/* ========================================================================
 * KERNEL-SIDE OPERATIONS - LSM Hooks
 * ======================================================================== */

/**
 * lsm_inode_create - Hook file creation (sleepable LSM)
 * 
 * This runs in sleepable context when any process creates a file.
 * Can safely call bpf_arena_alloc_pages() ✓
 */
SEC("lsm.s/inode_create")
int BPF_PROG(lsm_inode_create, struct inode *dir, struct dentry *dentry, umode_t mode)
{
	struct ds_msqueue __arena *ds_queue;
	int result;

	(void)dir;
	(void)dentry;
	(void)mode;

	ds_queue = &global_ds_queue_ku;
	
	/* Lazy initialization on first use */
	if (!initialized_ku) {
		result = ds_msqueue_init_lkmm(ds_queue);
		if (result != DS_SUCCESS) {
			total_kernel_prod_failures++;
			return 0;
		}
		initialized_ku = true;
	}
	
	__u64 pid;
	__u64 ts;

	pid = bpf_get_current_pid_tgid() >> 32;
	ts = bpf_ktime_get_ns();
	DS_METRICS_RECORD_OP(&global_metrics, DS_METRICS_LKMM_PRODUCER, {
		result = ds_msqueue_insert_lkmm(ds_queue, pid, ts);
	}, result);
	
	/* Update statistics */
	total_kernel_prod_ops++;
	if (result != DS_SUCCESS)
		total_kernel_prod_failures++;
	
	return 0; /* LSM returns 0 to allow operation */
}

SEC("uprobe.s")
int bpf_msq_consume(struct pt_regs *ctx)
{
	struct ds_msqueue __arena *q = &global_ds_queue_uk;
	struct ds_msqueue_elem __arena *head;
	struct ds_msqueue_elem __arena *tail;
	struct ds_kv data = {};
	int ret;

	(void)ctx;

	head = READ_ONCE(q->head);
	tail = READ_ONCE(q->tail);
	if (!head || !tail) {
		total_kernel_consume_ops++;
		total_kernel_consume_failures++;
		return DS_ERROR_INVALID;
	}

	DS_METRICS_RECORD_OP(&global_metrics, DS_METRICS_LKMM_CONSUMER, {
		ret = ds_msqueue_pop_lkmm(q, &data);
	}, ret);
	total_kernel_consume_ops++;
	if (ret == DS_SUCCESS) {
		total_kernel_consumed++;
		bpf_printk("msqueue consume key=%llu value=%llu\n", data.key, data.value);
	} else {
		total_kernel_consume_failures++;
	}

	return ret;
}

/* ========================================================================
 * INITIALIZATION PROGRAM
 * ======================================================================== */

char _license[] SEC("license") = "GPL";
