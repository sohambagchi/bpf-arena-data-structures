// SPDX-License-Identifier: GPL-2.0

#define BPF_NO_KFUNC_PROTOTYPES
#include <vmlinux.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>

#include "ringbuf_bench.h"

struct {
	__uint(type, BPF_MAP_TYPE_RINGBUF);
	__uint(max_entries, 1 << 24);
} events SEC(".maps");

struct ringbuf_bench_store global_metrics;
__u64 total_kernel_prod_ops = 0;
__u64 total_kernel_prod_failures = 0;

SEC("lsm.s/inode_create")
int BPF_PROG(lsm_inode_create, struct inode *dir, struct dentry *dentry, umode_t mode)
{
	struct ringbuf_bench_event *event;
	__u64 pid;
	__u64 ts;
	bool success = false;

	(void)dir;
	(void)dentry;
	(void)mode;

	pid = bpf_get_current_pid_tgid() >> 32;
	ts = bpf_ktime_get_ns();
	RINGBUF_BENCH_RECORD_OP(&global_metrics, RINGBUF_BENCH_LKMM_PRODUCER, {
		event = bpf_ringbuf_reserve(&events, sizeof(*event), 0);
		if (event) {
			event->key = pid;
			event->value = ts;
			bpf_ringbuf_submit(event, 0);
			success = true;
		} else {
			total_kernel_prod_failures++;
		}
	}, success);

	total_kernel_prod_ops++;
	return 0;
}

char _license[] SEC("license") = "GPL";
