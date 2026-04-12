// SPDX-License-Identifier: GPL-2.0

/* skeleton_iouring_liburing.bpf.c - BPF Arena io_uring/liburing Data Structure
 *
 * Two-lane relay architecture with role-specific memory ordering:
 *   KU lane (CQ ring): kernel CQ-produces → userspace CQ-consumes
 *   UK lane (SQ ring): userspace SQ-produces → kernel SQ-consumes
 */
#define BPF_NO_KFUNC_PROTOTYPES
#include <vmlinux.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>
#include "bpf_experimental.h"

/* ================================================================
 * ARENA MAP DEFINITION
 * ================================================================ */
struct {
	__uint(type, BPF_MAP_TYPE_ARENA);
	__uint(map_flags, BPF_F_MMAPABLE);
	__uint(max_entries, 1000);
#ifdef __TARGET_ARCH_arm64
	__ulong(map_extra, 0x1ull << 32);
#else
	__ulong(map_extra, 0x1ull << 44);
#endif
} arena SEC(".maps");

/* ================================================================
 * LIBRARY AND DATA STRUCTURE INCLUDES
 * (MUST come AFTER arena map definition)
 * ================================================================ */
#include "libarena_ds.h"
#include "ds_api.h"
#include "ds_iouring_liburing.h"
#include "ds_metrics.h"

/* ================================================================
 * CONFIGURATION
 * ================================================================ */
int config_ring_entries = 512;   /* MUST be power of 2 */

/* ================================================================
 * GLOBAL STATE (arena-resident)
 * ================================================================ */
struct ds_iouring_liburing_ring __arena global_ds_head_ku;
struct ds_iouring_liburing_ring __arena global_ds_head_uk;
struct ds_metrics_store __arena global_metrics;

/* ================================================================
 * STATISTICS (BSS, accessible from userspace via skel->bss)
 * ================================================================ */
__u64 total_kernel_prod_ops = 0;
__u64 total_kernel_prod_failures = 0;
__u64 total_kernel_consume_ops = 0;
__u64 total_kernel_consume_failures = 0;
__u64 total_kernel_consumed = 0;
bool initialized_ku = false;

/* ================================================================
 * KERNEL-SIDE PRODUCER (LSM Hook) — CQ produce into KU lane
 *
 * Uses cq_produce_lkmm: READ_ONCE(cons.head) + control dependency,
 * WRITE_ONCE entry fields, smp_store_release(prod.tail).
 * ================================================================ */
SEC("lsm.s/inode_create")
int BPF_PROG(lsm_inode_create, struct inode *dir, struct dentry *dentry, umode_t mode)
{
	struct ds_iouring_liburing_ring __arena *head = &global_ds_head_ku;
	int result;
	__u64 pid;
	__u64 ts;

	(void)dir;
	(void)dentry;
	(void)mode;

	if (!initialized_ku) {
		result = ds_iouring_liburing_init_lkmm(head,
						       (__u32)config_ring_entries);
		if (result != DS_SUCCESS) {
			total_kernel_prod_failures++;
			return 0;
		}
		initialized_ku = true;
	}

	pid = bpf_get_current_pid_tgid() >> 32;
	ts = bpf_ktime_get_ns();
	DS_METRICS_RECORD_OP(&global_metrics, DS_METRICS_LKMM_PRODUCER, {
		result = ds_iouring_liburing_cq_produce_lkmm(head, pid, ts);
	}, result);

	total_kernel_prod_ops++;
	if (result != DS_SUCCESS)
		total_kernel_prod_failures++;

	return 0;
}

/* ================================================================
 * KERNEL-SIDE CONSUMER (Uprobe) — SQ consume from UK lane
 *
 * Uses sq_consume_lkmm: smp_load_acquire(prod.tail),
 * READ_ONCE entry fields (untrusted userspace),
 * smp_store_release(cons.head).
 * ================================================================ */
SEC("uprobe.s")
int bpf_iouring_liburing_consume(struct pt_regs *ctx)
{
	struct ds_iouring_liburing_ring __arena *head = &global_ds_head_uk;
	struct ds_kv out = {};
	int ret;

	(void)ctx;

	if (!head->entries) {
		total_kernel_consume_ops++;
		total_kernel_consume_failures++;
		return DS_ERROR_INVALID;
	}

	DS_METRICS_RECORD_OP(&global_metrics, DS_METRICS_LKMM_CONSUMER, {
		ret = ds_iouring_liburing_sq_consume_lkmm(head, &out);
	}, ret);
	total_kernel_consume_ops++;
	if (ret == DS_SUCCESS) {
		total_kernel_consumed++;
		if (out.value > 0)
			DS_METRICS_RECORD_E2E(&global_metrics, out.value);
		bpf_printk("iouring_liburing consume key=%llu value=%llu\n",
			   out.key, out.value);
	} else {
		total_kernel_consume_failures++;
	}

	return ret;
}

char _license[] SEC("license") = "GPL";
