// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT
// Dual MIT/GPL is required for BPF programs to load

#include <vmlinux.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>

#define _LINUX_TYPES_H // Prevent linux/types.h redefinition clash with vmlinux.h
#include "maps.h"

// SCX DSQ IDs
#define DSQ_CBS       (1ULL << 61)
#define DSQ_THROTTLED ((1ULL << 61) | 1)
#define DSQ_BE        ((1ULL << 61) | 2)

// Missing SCX flags if omitted
#ifndef SCX_DSQ_LOCAL
#define SCX_DSQ_LOCAL 0
#endif
#ifndef SCX_SLICE_DFL
#define SCX_SLICE_DFL 0
#endif

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 1024);
    __type(key, __u64);
    __type(value, struct PartitionInfo);
} partition_map SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 2048);
    __type(key, struct TtSlotKey);
    __type(value, struct TtSlotBpf);
} tt_table_map SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 1024);
    __type(key, __u64);
    __type(value, struct CbsState);
} cbs_map SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 128); // Max CPUs
    __type(key, __u32);
    __type(value, __u32);
} current_slot_map SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 4096);
    __type(key, __u32); // pid
    __type(value, struct TaskMeta);
} task_meta_map SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 4096 * 16);
} fault_ringbuf SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 128); // Max CPUs
    __type(key, __u32);
    __type(value, __u32);
} kick_map SEC(".maps");

// SCX helpers (kfuncs) usually predefined if correctly linked, or we can declare them as extern
extern s32 scx_bpf_create_dsq(__u64 dsq_id, __s32 node) __ksym;
extern void scx_bpf_dispatch(struct task_struct *p, __u64 dsq_id, u64 slice, u64 enq_flags) __ksym;
extern bool scx_bpf_consume(__u64 dsq_id) __ksym;
extern void scx_bpf_kick_cpu(__s32 cpu, u64 flags) __ksym;

SEC("struct_ops.s/init_task")
int BPF_PROG(init_task, struct task_struct *p, struct scx_init_task_args *args) {
    __u32 pid = p->pid;
    struct TaskMeta *meta = bpf_map_lookup_elem(&task_meta_map, &pid);
    if (!meta) return 0;
    
    if (meta->scheduling_type == SCHED_TYPE_TT) {
        scx_bpf_create_dsq(meta->task_id_hash, -1);
    }
    return 0;
}

SEC("struct_ops/select_cpu")
s32 BPF_PROG(select_cpu, struct task_struct *p, s32 prev_cpu, u64 wake_flags) {
    __u32 pid = p->pid;
    struct TaskMeta *meta = bpf_map_lookup_elem(&task_meta_map, &pid);
    if (meta) {
        struct PartitionInfo *pinfo = bpf_map_lookup_elem(&partition_map, &meta->cgroup_id);
        if (pinfo && pinfo->cpu_mask != 0) {
            __u64 mask = pinfo->cpu_mask;
            /* Manual bit-scan: BPF ISA has no ffs instruction */
            #pragma unroll
            for (int i = 0; i < 64; i++) {
                if (mask & (1ULL << i))
                    return (s32)i;
            }
        }
    }
    return prev_cpu;
}

SEC("struct_ops/enqueue")
void BPF_PROG(enqueue, struct task_struct *p, u64 enq_flags) {
    __u32 pid = p->pid;
    struct TaskMeta *meta = bpf_map_lookup_elem(&task_meta_map, &pid);
    if (!meta) {
        scx_bpf_dispatch(p, SCX_DSQ_LOCAL, SCX_SLICE_DFL, enq_flags);
        return;
    }

    if (meta->scheduling_type == SCHED_TYPE_TT) {
        scx_bpf_dispatch(p, meta->task_id_hash, SCX_SLICE_DFL, enq_flags);
    } else if (meta->scheduling_type == SCHED_TYPE_CBS) {
        scx_bpf_dispatch(p, DSQ_CBS, SCX_SLICE_DFL, enq_flags);
    } else {
        scx_bpf_dispatch(p, DSQ_BE, SCX_SLICE_DFL, enq_flags);
    }
}

SEC("struct_ops/dispatch")
void BPF_PROG(dispatch, s32 cpu, struct task_struct *prev) {
    __u32 key = cpu;

    /* N3: Check kick_map — userspace signals which CPUs need rescheduling */
    __u32 *kick_flag = bpf_map_lookup_elem(&kick_map, &key);
    if (kick_flag && *kick_flag) {
        *kick_flag = 0;  // Clear the flag
        scx_bpf_kick_cpu(cpu, 0);
    }

    __u32 *slot_idx = bpf_map_lookup_elem(&current_slot_map, &key);
    if (slot_idx) {
        struct TtSlotKey tt_key = { .cpu = cpu, .slot_idx = *slot_idx };
        struct TtSlotBpf *slot = bpf_map_lookup_elem(&tt_table_map, &tt_key);
        if (slot) {
            // Task matching slot is preferred
            if (scx_bpf_consume(slot->task_id_hash)) {
                return;
            }
        }
    }
    
    // consume CBS
    scx_bpf_consume(DSQ_CBS);
    // consume BE
    scx_bpf_consume(DSQ_BE);
}

SEC("struct_ops/running")
void BPF_PROG(running, struct task_struct *p) {
    __u32 pid = p->pid;
    struct TaskMeta *meta = bpf_map_lookup_elem(&task_meta_map, &pid);
    if (meta) {
        meta->activation_ns = bpf_ktime_get_ns();
    }
}

SEC("struct_ops/stopping")
void BPF_PROG(stopping, struct task_struct *p, bool runnable) {
    __u32 pid = p->pid;
    struct TaskMeta *meta = bpf_map_lookup_elem(&task_meta_map, &pid);
    
    // M2: Deadline miss check for TT tasks on yield
    if (meta && meta->scheduling_type == SCHED_TYPE_TT && !runnable) {
        __u64 now = bpf_ktime_get_ns();
        // Dummy deadline assumed as 10ms (10000000ns) for PoC structural mapping
        __u64 deadline = meta->activation_ns + 10000000ULL;
        if (now > deadline) {
            struct FaultEvent *fault;
            fault = bpf_ringbuf_reserve(&fault_ringbuf, sizeof(*fault), 0);
            if (fault) {
                fault->fault_type = 0; // FAULT_DMISS
                fault->task_id_hash = meta->task_id_hash;
                fault->workload_id_hash = meta->workload_id_hash;
                fault->cpu = bpf_get_smp_processor_id();
                fault->expected_deadline_ns = deadline;
                bpf_ringbuf_submit(fault, 0);
            }
        }
    }
}

SEC(".struct_ops.link")
struct sched_ext_ops timpani_ops = {
    .init_task = (void *)init_task,
    .select_cpu = (void *)select_cpu,
    .enqueue = (void *)enqueue,
    .dispatch = (void *)dispatch,
    .running = (void *)running,
    .stopping = (void *)stopping,
    .name = "timpani"
};

char _license[] SEC("license") = "Dual MIT/GPL";
