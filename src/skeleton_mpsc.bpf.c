// SPDX-License-Identifier: GPL-2.0
/* Vyukhov MPSC Queue BPF Program for Concurrent Data Structure Testing
 * 
 * This program tests the Vyukhov MPSC (Multi-Producer Single-Consumer) Queue
 * data structure where multiple kernel contexts produce and userspace consumes.
 * 
 * DESIGN:
 * - Uses ds_mpsc.h for Vyukhov's wait-free MPSC queue implementation
 * - LSM hook on inode_create automatically inserts (pid, timestamp) pairs
 * - Userspace polls and dequeues via direct arena access
 * - Unidirectional: BPF produces, userspace consumes
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
 * INCLUDE DATA STRUCTURE HEADER
 * ======================================================================== */
#include "ds_mpsc.h"

/* ========================================================================
 * GLOBAL STATE
 * ======================================================================== */

/* Data structure head (accessible from userspace via arena) */
struct ds_mpsc_head __arena *ds_head;
struct ds_mpsc_head __arena global_ds_head;

/* Statistics and control */
__u64 total_kernel_ops = 0;
__u64 total_kernel_failures = 0;
bool initialized = false;

/* ========================================================================
 * INITIALIZATION
 * ======================================================================== */

/**
 * init_data_structure - Initialize the MPSC queue
 * 
 * Called once to set up the data structure before any operations.
 * Returns: DS_SUCCESS on success, error code otherwise
 */
static __always_inline int init_data_structure(void)
{
	int result;
	
	if (initialized)
		return DS_SUCCESS;
	
	/* Initialize pointer to global head on first call */
	ds_head = &global_ds_head;
	
	result = ds_mpsc_init(ds_head);
	if (result == DS_SUCCESS) {
		initialized = true;
	}
	
	return result;
}

/* ========================================================================
 * LSM HOOK: AUTOMATIC PRODUCER
 * ======================================================================== */

/**
 * lsm_inode_create - LSM hook that fires when inodes are created
 * 
 * This hook automatically triggers when files are created (e.g., during
 * execve, touch, file writes). We use it as a convenient way to generate
 * concurrent insertions from the kernel side.
 * 
 * The hook inserts a (pid, timestamp) pair into the MPSC queue.
 */
SEC("lsm.s/inode_create")
int BPF_PROG(restrict_dir, struct inode *dir, struct dentry *dentry, umode_t mode)
{
	__u64 pid;
	__u64 ts;
	int result;
	
	/* Initialize on first use */
	if (!initialized) {
		result = init_data_structure();
		if (result != DS_SUCCESS) {
			total_kernel_failures++;
			return 0; /* LSM hooks should return 0 for allow */
		}
	}
	
	/* Get operation data */
	pid = bpf_get_current_pid_tgid() >> 32;
	ts = bpf_ktime_get_ns();
	
	/* Insert into queue (wait-free operation) */
	result = ds_mpsc_insert(ds_head, pid, ts);
	
	/* Track statistics */
	total_kernel_ops++;
	if (result != DS_SUCCESS) {
		total_kernel_failures++;
	}
	
	return 0; /* LSM: 0 = allow operation */
}

/* ========================================================================
 * LICENSE
 * ======================================================================== */

char LICENSE[] SEC("license") = "GPL";
