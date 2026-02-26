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
#include "ds_ck_fifo_spsc.h"

int config_key_range = 1000;

struct ds_ck_fifo_spsc_head __arena *ds_head;
struct ds_ck_fifo_spsc_head __arena global_ds_head;

__u64 total_kernel_ops = 0;
__u64 total_kernel_failures = 0;
bool initialized = false;

static __always_inline int handle_operation(struct ds_operation *op)
{
	int result = DS_ERROR_INVALID;

	if (!ds_head)
		return DS_ERROR_INVALID;

	switch (op->type) {
	case DS_OP_INIT:
		result = ds_ck_fifo_spsc_init(ds_head);
		if (result == DS_SUCCESS)
			initialized = true;
		break;
	case DS_OP_INSERT:
		result = ds_ck_fifo_spsc_insert(ds_head, op->kv.key, op->kv.value);
		break;
	case DS_OP_DELETE:
	case DS_OP_POP:
		result = ds_ck_fifo_spsc_pop(ds_head, &op->kv);
		break;
	case DS_OP_SEARCH:
		result = ds_ck_fifo_spsc_search(ds_head, op->kv.key);
		break;
	case DS_OP_VERIFY:
		result = ds_ck_fifo_spsc_verify(ds_head);
		break;
	default:
		result = DS_ERROR_INVALID;
	}

	op->result = result;
	total_kernel_ops++;
	if (result != DS_SUCCESS)
		total_kernel_failures++;

	return result;
}

SEC("lsm.s/inode_create")
int BPF_PROG(lsm_inode_create, struct inode *dir, struct dentry *dentry, umode_t mode)
{
	int result;
	__u64 pid;
	__u64 ts;

	(void)dir;
	(void)dentry;
	(void)mode;

	/* Lazy initialization on first trigger */
	if (!initialized) {
		ds_head = &global_ds_head;
		result = ds_ck_fifo_spsc_init(ds_head);
		if (result != DS_SUCCESS) {
			total_kernel_failures++;
			return 0;
		}
		initialized = true;
	}

	ds_head = &global_ds_head;

	/* Producer operation: insert (PID, timestamp) */
	pid = bpf_get_current_pid_tgid() >> 32;
	ts = bpf_ktime_get_ns();
	result = ds_ck_fifo_spsc_insert(ds_head, pid, ts);

	total_kernel_ops++;
	if (result != DS_SUCCESS)
		total_kernel_failures++;

	return 0;
}

char _license[] SEC("license") = "GPL";
