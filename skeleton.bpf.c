// SPDX-License-Identifier: GPL-2.0
/* Skeleton BPF Program for Concurrent Data Structure Testing
 * 
 * This program provides a template for testing concurrent data structures
 * where operations can be triggered from both kernel space (via syscalls)
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
/* Example: #include "ds_list.h" */
/* Example: #include "ds_tree.h" */
#include "ds_list.h"  /* Default: include list implementation */

/* ========================================================================
 * GLOBAL STATE
 * ======================================================================== */

/* Configuration (set by userspace before running) */
int config_num_operations = 100;
int config_key_range = 1000;
bool config_enable_stats = true;

/* Operation queue for syscall-triggered operations */
#define MAX_PENDING_OPS 256
struct ds_operation __arena pending_ops[MAX_PENDING_OPS];
int __arena pending_ops_head = 0;
int __arena pending_ops_tail = 0;

/* DS_API_INSERT: Declare your data structure head here */
/* Example: struct ds_tree_head __arena *ds_head; */
struct ds_list_head __arena global_ds_head;
struct ds_list_head __arena *ds_head;

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
 */
static __always_inline int handle_operation(struct ds_operation *op)
{
	int result = DS_ERROR_INVALID;
	
	if (!ds_head)
		return DS_ERROR_INVALID;
	
	switch (op->type) {
	case DS_OP_INIT:
		/* DS_API_INSERT: Call your init function */
		result = ds_list_init(ds_head);
		initialized = true;
		break;
		
	case DS_OP_INSERT:
		/* DS_API_INSERT: Call your insert function */
		result = ds_list_insert(ds_head, op->key, op->value);
		break;
		
	case DS_OP_DELETE:
		/* DS_API_INSERT: Call your delete function */
		result = ds_list_delete(ds_head, op->key);
		break;
		
	case DS_OP_SEARCH:
		/* DS_API_INSERT: Call your search function */
		result = ds_list_search(ds_head, op->key, &op->value);
		break;
		
	case DS_OP_VERIFY:
		/* DS_API_INSERT: Call your verify function */
		result = ds_list_verify(ds_head);
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
 * SYSCALL HOOKS - Operations triggered from kernel context
 * ======================================================================== */

/**
 * trace_exec - Hook exec syscall for kernel-side operations
 * 
 * This runs in kernel context when any process calls exec(). We use this
 * as a trigger point to perform data structure operations from kernel space.
 * This simulates kernel threads performing operations concurrently with
 * userspace threads.
 */
SEC("tp/syscalls/sys_enter_execve")
int trace_exec(struct trace_event_raw_sys_enter *ctx)
{
	struct ds_operation op = {};
	__u64 key, value;
	int result;
	
	if (!initialized) {
		ds_head = &global_ds_head;
		cast_kern(ds_head);
		ds_list_init(ds_head);
		initialized = true;
		return 0;
	}
	
	/* Generate a pseudo-random key based on PID and timestamp */
	__u32 pid = bpf_get_current_pid_tgid() >> 32;
	key = (bpf_ktime_get_ns() ^ pid) % config_key_range;
	value = key * 2;
	
	/* Perform insert operation */
	op.type = DS_OP_INSERT;
	op.key = key;
	op.value = value;
	op.timestamp = bpf_ktime_get_ns();
	
	result = handle_operation(&op);
	
	return 0;
}

/**
 * trace_exit - Hook exit syscall for cleanup operations
 * 
 * This runs when processes exit. We can use this to perform delete
 * operations, testing concurrent deletions with userspace.
 */
SEC("tp/syscalls/sys_enter_exit_group")
int trace_exit(struct trace_event_raw_sys_enter *ctx)
{
	struct ds_operation op = {};
	__u64 key;
	int result;
	
	if (!initialized)
		return 0;
	
	/* Generate a key to delete */
	__u32 pid = bpf_get_current_pid_tgid() >> 32;
	key = (bpf_ktime_get_ns() ^ pid) % config_key_range;
	
	/* Perform delete operation */
	op.type = DS_OP_DELETE;
	op.key = key;
	op.timestamp = bpf_ktime_get_ns();
	
	result = handle_operation(&op);
	
	return 0;
}

/* ========================================================================
 * MANUAL TRIGGER PROGRAM
 * ======================================================================== */

/**
 * manual_operation - Execute a single operation via bpf_prog_test_run
 * @ctx: Context containing the operation parameters
 * 
 * Userspace can call this directly to trigger specific operations from
 * kernel context for testing purposes.
 */
SEC("syscall")
int manual_operation(struct ds_operation *ctx)
{
	struct ds_operation op;
	int result;
	
	if (!initialized) {
		ds_head = &global_ds_head;
		cast_kern(ds_head);
		ds_list_init(ds_head);
		initialized = true;
	}
	
	/* Copy operation from context */
	__builtin_memcpy(&op, ctx, sizeof(op));
	
	/* Execute operation */
	result = handle_operation(&op);
	
	/* Write back result */
	__builtin_memcpy(ctx, &op, sizeof(op));
	
	return result;
}

/**
 * batch_operations - Execute multiple operations in a batch
 * 
 * This can be called from userspace to execute a batch of operations
 * from kernel context, useful for benchmarking.
 */
SEC("syscall")
int batch_operations(void *ctx)
{
	int i;
	int success = 0;
	
	if (!initialized) {
		ds_head = &global_ds_head;
		cast_kern(ds_head);
		ds_list_init(ds_head);
		initialized = true;
	}
	
	/* Execute up to config_num_operations operations */
	for (i = 0; i < config_num_operations && i < 1000 && can_loop; i++) {
		struct ds_operation op = {};
		
		/* Alternate between inserts and searches */
		if (i % 2 == 0) {
			op.type = DS_OP_INSERT;
			op.key = i;
			op.value = i * 10;
		} else {
			op.type = DS_OP_SEARCH;
			op.key = i - 1;
		}
		
		if (handle_operation(&op) == DS_SUCCESS)
			success++;
	}
	
	return success;
}

/**
 * verify_structure - Verify data structure integrity
 */
SEC("syscall")
int verify_structure(void *ctx)
{
	struct ds_operation op = {};
	
	if (!initialized)
		return DS_ERROR_INVALID;
	
	op.type = DS_OP_VERIFY;
	return handle_operation(&op);
}

/**
 * reset_structure - Clear the data structure
 */
SEC("syscall")
int reset_structure(void *ctx)
{
	if (!initialized)
		return 0;
	
	/* DS_API_INSERT: Add cleanup code for your data structure */
	/* For now, just reinitialize */
	ds_list_init(ds_head);
	
	total_kernel_ops = 0;
	total_kernel_failures = 0;
	
	return 0;
}

char _license[] SEC("license") = "GPL";
