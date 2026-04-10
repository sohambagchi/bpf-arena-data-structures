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
#include "ds_kcov.h"
#include "ds_metrics.h"

/* Buffer size in words (area[0] = counter + area[1..N] = data).
 * With KCOV_WORDS_PER_ENTRY=2: (509-1)/2 = 254 usable entries.
 * Total bytes: 509 × 8 = 4072 < PAGE_SIZE-8 = 4088 (allocator limit). */
int config_buf_size = 509;

struct ds_kcov_buf __arena global_ds_head_ku;
struct ds_kcov_buf __arena global_ds_head_uk;
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
	struct ds_kcov_buf __arena *head = &global_ds_head_ku;
	int result;
	__u64 pid;
	__u64 ts;

	(void)dir;
	(void)dentry;
	(void)mode;

	if (!initialized_ku) {
		result = ds_kcov_init_lkmm(head, (__u32)config_buf_size);
		if (result != DS_SUCCESS) {
			total_kernel_prod_failures++;
			return 0;
		}
		initialized_ku = true;
	}

	pid = bpf_get_current_pid_tgid() >> 32;
	ts = bpf_ktime_get_ns();
	DS_METRICS_RECORD_OP(&global_metrics, DS_METRICS_LKMM_PRODUCER, {
		result = ds_kcov_insert_lkmm(head, pid, ts);
	}, result);

	total_kernel_prod_ops++;
	if (result != DS_SUCCESS)
		total_kernel_prod_failures++;

	return 0;
}

SEC("uprobe.s")
int bpf_kcov_consume(struct pt_regs *ctx)
{
	struct ds_kcov_buf __arena *head = &global_ds_head_uk;
	struct ds_kv out = {};
	int ret;

	(void)ctx;

	if (!head->area) {
		total_kernel_consume_ops++;
		total_kernel_consume_failures++;
		return DS_ERROR_INVALID;
	}

	DS_METRICS_RECORD_OP(&global_metrics, DS_METRICS_LKMM_CONSUMER, {
		ret = ds_kcov_pop_lkmm(head, &out);
	}, ret);
	total_kernel_consume_ops++;
	if (ret == DS_SUCCESS) {
		total_kernel_consumed++;
		if (out.value > 0)
			DS_METRICS_RECORD_E2E(&global_metrics, out.value);
		bpf_printk("kcov consume key=%llu value=%llu\n", out.key, out.value);
	}
	else
		total_kernel_consume_failures++;

	return ret;
}

char _license[] SEC("license") = "GPL";
