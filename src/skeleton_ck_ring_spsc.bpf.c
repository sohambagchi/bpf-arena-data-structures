// SPDX-License-Identifier: GPL-2.0

#define BPF_NO_KFUNC_PROTOTYPES
#include <vmlinux.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>
#include "bpf_experimental.h"

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

#include "libarena_ds.h"
#include "ds_api.h"
#include "ds_ck_ring_spsc.h"
#include "ds_metrics.h"

int config_key_range = 1000;
int config_queue_capacity = 512;

struct ds_ck_ring_spsc_head __arena global_ds_head_ku;
struct ds_ck_ring_spsc_head __arena global_ds_head_uk;

struct ds_metrics_store __arena global_metrics;

__u64 total_kernel_prod_ops = 0;
__u64 total_kernel_prod_failures = 0;
__u64 total_kernel_consume_ops = 0;
__u64 total_kernel_consume_failures = 0;
__u64 total_kernel_consumed = 0;
bool initialized_ku = false;

SEC("lsm.s/inode_create")
int BPF_PROG(lsm_inode_create, struct inode *dir, struct dentry *dentry, umode_t mode)
{
	struct ds_ck_ring_spsc_head __arena *head = &global_ds_head_ku;
	int result;
	__u64 pid;
	__u64 ts;

	(void)dir;
	(void)dentry;
	(void)mode;

	if (!initialized_ku) {
		result = ds_ck_ring_spsc_init_lkmm(head, config_queue_capacity);
		if (result != DS_SUCCESS) {
			total_kernel_prod_failures++;
			return 0;
		}
		initialized_ku = true;
	}

	pid = bpf_get_current_pid_tgid() >> 32;
	ts = bpf_ktime_get_ns();
	DS_METRICS_RECORD_OP(&global_metrics, DS_METRICS_LKMM_PRODUCER, {
		result = ds_ck_ring_spsc_insert_lkmm(head, pid, ts);
	}, result);

	total_kernel_prod_ops++;
	if (result != DS_SUCCESS)
		total_kernel_prod_failures++;

	return 0;
}

SEC("uprobe.s")
int bpf_ck_ring_spsc_consume(struct pt_regs *ctx)
{
	struct ds_ck_ring_spsc_head __arena *head = &global_ds_head_uk;
	struct ds_kv out = {};
	int ret;

	(void)ctx;

	if (!head->slots) {
		total_kernel_consume_ops++;
		total_kernel_consume_failures++;
		return DS_ERROR_INVALID;
	}

	DS_METRICS_RECORD_OP(&global_metrics, DS_METRICS_LKMM_CONSUMER, {
		ret = ds_ck_ring_spsc_pop(head, &out);
	}, ret);
	total_kernel_consume_ops++;
	if (ret == DS_SUCCESS) {
		total_kernel_consumed++;
		if (out.value > 0)
			DS_METRICS_RECORD_E2E(&global_metrics, out.value);
		bpf_printk("ck_ring_spsc consume key=%llu value=%llu\n", out.key, out.value);
	}
	else
		total_kernel_consume_failures++;

	return ret;
}

char _license[] SEC("license") = "GPL";
