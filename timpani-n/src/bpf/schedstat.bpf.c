// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#include "vmlinux.h"

#include <bpf/bpf_core_read.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

#include "../trace_bpf.h"

struct {
	__uint(type, BPF_MAP_TYPE_RINGBUF);
	__uint(max_entries, 64 * 4096);
} buffer SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 1024);
	__type(key, int);
	__type(value, __u8);
} pid_filter_map SEC(".maps");

struct sched_time {
	__u64 ts_wakeup;
	__u64 ts_start;
};

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 1024);
	__type(key, int);
	__type(value, struct sched_time);
} sched_waking_map SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 1024);
	__type(key, int);
	__type(value, struct sched_time);
} sched_switch_map SEC(".maps");

#define TASK_RUNNING	0x0

struct task_struct___o {
	volatile long int state;
} __attribute__((preserve_access_index));

struct task_struct___x {
	unsigned int __state;
} __attribute__((preserve_access_index));

static __always_inline __s64 get_task_state(void *task)
{
	struct task_struct___x *t = task;

	if (bpf_core_field_exists(t->__state))
		return BPF_CORE_READ(t, __state);
	return BPF_CORE_READ((struct task_struct___o *)task, state);
}

SEC("tp_btf/sched_waking")
int BPF_PROG(sched_waking, struct task_struct *task)
{
	__u64 now = bpf_ktime_get_ns();
	struct sched_time data = {};
	__u8 *filtered;
	pid_t pid;

	BPF_CORE_READ_INTO(&pid, task, pid);
	filtered = bpf_map_lookup_elem(&pid_filter_map, &pid);
	if (!filtered)
		return 0;

	data.ts_wakeup = now;
	bpf_map_update_elem(&sched_waking_map, &pid, &data, BPF_ANY);

	return 0;
}

SEC("tp_btf/sched_switch")
int BPF_PROG(sched_switch, bool preempt, struct task_struct *prev,
	     struct task_struct *next)
{
	__u64 now = bpf_ktime_get_ns();
	struct sched_time *pdata;
	struct schedstat_event *event;
	struct sched_time data = {};
	__u8 *filtered;
	pid_t pid;

	BPF_CORE_READ_INTO(&pid, prev, pid);
	pdata = bpf_map_lookup_elem(&sched_switch_map, &pid);
	if (pdata) {
		event = bpf_ringbuf_reserve(&buffer, sizeof(*event), 0);
		if (event) {
			event->pid = pid;
			event->cpu = bpf_get_smp_processor_id();
			event->ts_wakeup = pdata->ts_wakeup;
			event->ts_start = pdata->ts_start;
			event->ts_stop = now;
			bpf_ringbuf_submit(event, 0);
		}

		if (get_task_state(prev) == TASK_RUNNING) {
			data.ts_wakeup = pdata->ts_wakeup;
			bpf_map_update_elem(&sched_waking_map, &pid, &data, BPF_ANY);
		}

		bpf_map_delete_elem(&sched_switch_map, &pid);
	}

	BPF_CORE_READ_INTO(&pid, next, pid);
	filtered = bpf_map_lookup_elem(&pid_filter_map, &pid);
	if (filtered) {
		pdata = bpf_map_lookup_elem(&sched_waking_map, &pid);
		if (pdata) {
			data.ts_wakeup = pdata->ts_wakeup;
			data.ts_start = now;
			bpf_map_update_elem(&sched_switch_map, &pid, &data, BPF_ANY);
			bpf_map_delete_elem(&sched_waking_map, &pid);
		}
	}

	return 0;
}

char LICENSE[] SEC("license") = "GPL";
