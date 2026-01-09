// SPDX-License-Identifier: GPL-2.0
/* Skeleton BPF Program for Non-Blocking Binary Search Tree Testing
 * 
 * This program tests the Ellen et al. (2010) non-blocking BINTREE implementation
 * where operations can be triggered from both kernel space (via LSM hooks)
 * and userspace (via direct arena access).
 * 
 * Data Structure: Non-blocking Binary Search Tree (ds_bintree.h)
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
#include "ds_bintree.h"  /* Non-blocking Binary Search Tree */

/* ========================================================================
 * GLOBAL STATE
 * ======================================================================== */

/* Configuration (set by userspace before running) */
int config_key_range = 1000;

/* DS_API_INSERT: Declare your data structure head here */
struct ds_bintree_head __arena *ds_head;
struct ds_bintree_head __arena global_ds_head;

/* Statistics and control */
__u64 total_kernel_ops = 0;
__u64 total_kernel_failures = 0;
bool initialized = false;

/* ========================================================================
 * OPERATION DISPATCH
 * ======================================================================== */

/**
 * handle_operation - Execute a data structure operation
 * @op: The operation to execute
 * 
 * This function dispatches operations to the appropriate data structure
 * handler based on the operation type.
 * 
 * DS_API_INSERT: Add cases for your data structure operations here
 * 
 * NOTE: Simplified design - kernel only does inserts via inode_create hook,
 * userspace threads poll/search the data structure.
 */
static __always_inline int handle_operation(struct ds_operation *op)
{
	int result = DS_ERROR_INVALID;
	
	if (!ds_head)
		return DS_ERROR_INVALID;
	
	switch (op->type) {
	case DS_OP_INIT:
		/* DS_API_INSERT: Call your init function */
		result = ds_bintree_init(ds_head);
		initialized = true;
		break;
		
	case DS_OP_INSERT:
		/* DS_API_INSERT: Call your insert function */
		result = ds_bintree_insert(ds_head, op->kv);
		break;
		
	case DS_OP_DELETE:
		/* DS_API_INSERT: Call your delete function */
		result = ds_bintree_delete(ds_head, op->kv);
		break;
		
	case DS_OP_SEARCH:
		/* DS_API_INSERT: Call your search function */
		result = ds_bintree_search(ds_head, op->kv);
		break;
		
	case DS_OP_VERIFY:
		/* DS_API_INSERT: Call your verify function */
		result = ds_bintree_verify(ds_head);
		break;
		
	default:
		result = DS_ERROR_INVALID;
	}
	
	op->result = result;
	
	/* Update statistics */
	total_kernel_ops++;
	if (result != DS_SUCCESS)
		total_kernel_failures++;
	
	return result;
}

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
	int result;
	ds_head = &global_ds_head;

	if (!initialized) {
		result = ds_bintree_init(ds_head);
		if (result != DS_SUCCESS) {
			total_kernel_failures++;
			return 0;
		}
		initialized = true;
	}
	
	__u64 pid;
	__u64 ts;

	pid = bpf_get_current_pid_tgid() >> 32;
	ts = bpf_ktime_get_ns();
	struct ds_kv kv = { .key = pid, .value = ts };
	result = ds_bintree_insert(ds_head, kv);
	
	/* Update statistics */
	total_kernel_ops++;
	if (result != DS_SUCCESS)
		total_kernel_failures++;
	
	return 0; /* LSM returns 0 to allow operation */
}

// SEC("lsm.s/inode_unlink")
// int BPF_PROG(lsm_inode_unlink, struct inode *dir, struct dentry *dentry)
// {
// 	int result;
// 	ds_head = &global_ds_head;

// 	if (!initialized) {
// 		result = ds_bintree_init(ds_head);
// 		if (result != DS_SUCCESS) {
// 			total_kernel_failures++;
// 			return 0;
// 		}
// 		initialized = true;
// 	}

// 	__u64 pid;
// 	pid = bpf_get_current_pid_tgid() >> 32;
// 	struct ds_kv kv = { .key = pid, .value = 0 };
// 	result = ds_bintree_delete(ds_head, kv);

// 	/* Update statistics */
// 	total_kernel_ops++;
// 	if (result != DS_SUCCESS)
// 		total_kernel_failures++;
// 	return 0; /* LSM returns 0 to allow operation */
// }

/* ========================================================================
 * INITIALIZATION PROGRAM
 * ======================================================================== */

char _license[] SEC("license") = "GPL";
