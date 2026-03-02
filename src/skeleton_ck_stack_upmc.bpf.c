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
#include "ds_ck_stack_upmc.h"

struct ds_ck_stack_upmc_head __arena *ds_head;
struct ds_ck_stack_upmc_head __arena global_ds_head;

__u64 total_kernel_ops = 0;
__u64 total_kernel_failures = 0;
bool initialized = false;

static __always_inline int init_data_structure(void)
{
	if (initialized)
		return DS_SUCCESS;

	ds_head = &global_ds_head;
	ds_ck_stack_upmc_init_lkmm(ds_head);
	initialized = true;
	return DS_SUCCESS;
}

SEC("lsm.s/inode_create")
int BPF_PROG(lsm_inode_create, struct inode *dir, struct dentry *dentry, umode_t mode)
{
	__u64 pid;
	__u64 ts;
	int result;

	if (!initialized) {
		result = init_data_structure();
		if (result != DS_SUCCESS) {
			total_kernel_failures++;
			return 0;
		}
	}

	pid = bpf_get_current_pid_tgid() >> 32;
	ts = bpf_ktime_get_ns();
	result = ds_ck_stack_upmc_insert_lkmm(ds_head, pid, ts);

	total_kernel_ops++;
	if (result != DS_SUCCESS)
		total_kernel_failures++;

	return 0;
}

char LICENSE[] SEC("license") = "GPL";
