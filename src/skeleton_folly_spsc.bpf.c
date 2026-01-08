// SPDX-License-Identifier: GPL-2.0
/* BPF Skeleton for Folly SPSC Queue Testing
 * 
 * This BPF program acts as the PRODUCER in a Single-Producer Single-Consumer
 * queue. It inserts elements via the inode_create LSM hook (file creation).
 * 
 * SPSC Design:
 * - Kernel (Producer): LSM hook inserts (key=PID, value=timestamp) into ring buffer
 * - Userspace (Consumer): Continuously polls and dequeues elements
 * 
 * Memory Ordering:
 * - Producer writes data → RELEASE write_idx
 * - Consumer reads data → RELEASE read_idx
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
 * DS_API_INSERT: Include data structure header
 * ======================================================================== */
#include "ds_folly_spsc.h"

/* ========================================================================
 * GLOBAL STATE
 * ======================================================================== */

/* Configuration (set by userspace before running) */
int config_key_range = 1000;
int config_queue_size = 128; /* SPSC queue capacity (usable: size - 1) - max ~255 due to page size */

/* DS_API_INSERT: Declare data structure head */
struct ds_spsc_queue_head __arena *ds_head;
struct ds_spsc_queue_head __arena global_ds_head;

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
 * DS_API_INSERT: Dispatcher for SPSC queue operations
 * 
 * NOTE: Kernel (Producer) only does inserts via inode_create hook.
 * Userspace (Consumer) polls and dequeues.
 */
static __always_inline int handle_operation(struct ds_operation *op)
{
	int result = DS_ERROR_INVALID;
	
	if (!ds_head)
		return DS_ERROR_INVALID;
	
	switch (op->type) {
	case DS_OP_INIT:
		/* DS_API_INSERT: Initialize SPSC queue with configured size */
		result = ds_spsc_init(ds_head, config_queue_size);
		initialized = true;
		break;
		
	case DS_OP_INSERT:
		/* DS_API_INSERT: Producer insert operation */
		result = ds_spsc_insert(ds_head, op->kv.key, op->kv.value);
		break;
		
	case DS_OP_DELETE:
		/* DS_API_INSERT: Consumer delete/pop operation (not called in kernel) */
		result = ds_spsc_delete(ds_head, &op->kv);
		break;
		
	case DS_OP_SEARCH:
		/* DS_API_INSERT: Search not supported for SPSC queue */
		result = ds_spsc_search(ds_head, op->kv.key);
		break;
		
	case DS_OP_VERIFY:
		/* DS_API_INSERT: Verify queue integrity */
		result = ds_spsc_verify(ds_head);
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
 * KERNEL-SIDE OPERATIONS - LSM Hooks (PRODUCER)
 * ======================================================================== */

/**
 * lsm_inode_create - Hook file creation (sleepable LSM)
 * 
 * This is the PRODUCER in the SPSC design. It runs when any process creates
 * a file and inserts (PID, timestamp) into the ring buffer.
 * 
 * Can safely call bpf_arena_alloc_pages() ✓
 */
SEC("lsm.s/inode_create")
int BPF_PROG(lsm_inode_create, struct inode *dir, struct dentry *dentry, umode_t mode)
{
	int result;

	/* Lazy initialization on first trigger */
	if (!initialized) {
		ds_head = &global_ds_head;
		cast_user(ds_head);
		result = ds_spsc_init(ds_head, config_queue_size);
		if (result != DS_SUCCESS) {
			total_kernel_failures++;
			return 0;
		}
		initialized = true;
	}
	
	ds_head = &global_ds_head;
	cast_user(ds_head);
	
	/* Producer operation: insert (PID, timestamp) */
	__u64 pid = bpf_get_current_pid_tgid() >> 32;
	__u64 ts = bpf_ktime_get_ns();
	
	result = ds_spsc_insert(ds_head, pid, ts);
	
	/* Update statistics */
	total_kernel_ops++;
	if (result != DS_SUCCESS) {
		total_kernel_failures++;
		/* Queue full - this is expected under high load */
	}
	
	return 0; /* LSM returns 0 to allow operation */
}

/* ========================================================================
 * INITIALIZATION
 * ======================================================================== */

char _license[] SEC("license") = "GPL";
