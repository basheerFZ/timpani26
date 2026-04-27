// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT
// Dual MIT/GPL is required for BPF programs to load

#include <vmlinux.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>

#define _LINUX_TYPES_H // Prevent linux/types.h redefinition clash with vmlinux.h
#include "maps.h"

// SCX DSQ IDs — simple integers to avoid kernel validation issues
#define DSQ_TT_WAIT   100
#define DSQ_CBS       101
#define DSQ_THROTTLED 102
#define DSQ_BE        103

// SCX_DSQ_LOCAL and SCX_SLICE_DFL are enums in vmlinux.h
// Removing incorrect macro fallbacks that redefine them to 0

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
    /* Per-task DSQ creation removed — using global DSQ_TT_WAIT instead.
     * init_task() runs before task_meta_map is populated, causing a race. */
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
        scx_bpf_dispatch(p, SCX_DSQ_GLOBAL, SCX_SLICE_DFL, enq_flags);
        return;
    }

    if (meta->scheduling_type == SCHED_TYPE_TT) {
        scx_bpf_dispatch(p, DSQ_TT_WAIT, SCX_SLICE_DFL, enq_flags);
    } else if (meta->scheduling_type == SCHED_TYPE_CBS) {
        /* N1: CBS budget check — throttle if budget exhausted */
        struct CbsState *cbs = bpf_map_lookup_elem(&cbs_map, &meta->task_id_hash);
        if (cbs) {
            __u64 now = bpf_ktime_get_ns();
            /* Check if replenishment is due */
            if (cbs->remaining_us == 0 && now >= cbs->replenish_at_ns) {
                cbs->remaining_us = cbs->budget_us;
                cbs->replenish_at_ns = now + (__u64)cbs->period_us * 1000ULL;
            }
            if (cbs->remaining_us == 0) {
                /* Budget exhausted, throttle to DSQ_THROTTLED */
                scx_bpf_dispatch(p, DSQ_THROTTLED, SCX_SLICE_DFL, enq_flags);
                return;
            }
        }
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
    if (slot_idx && *slot_idx != SLOT_NONE) {
        struct TtSlotKey tt_key = { .cpu = cpu, .slot_idx = *slot_idx };
        struct TtSlotBpf *slot = bpf_map_lookup_elem(&tt_table_map, &tt_key);
        if (slot) {
            // Consume from global TT wait queue (all TT tasks go here)
            if (scx_bpf_consume(DSQ_TT_WAIT)) {
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
        __u64 now = bpf_ktime_get_ns();
        meta->activation_ns = now;
        /* N1: Record CBS execution start */
        if (meta->scheduling_type == SCHED_TYPE_CBS) {
            struct CbsState *cbs = bpf_map_lookup_elem(&cbs_map, &meta->task_id_hash);
            if (cbs) cbs->exec_start_ns = now;
        }
    }
}

SEC("struct_ops/stopping")
void BPF_PROG(stopping, struct task_struct *p, bool runnable) {
    __u32 pid = p->pid;
    struct TaskMeta *meta = bpf_map_lookup_elem(&task_meta_map, &pid);
    if (!meta) return;

    __u64 now = bpf_ktime_get_ns();

    /* N1: CBS budget deduction */
    if (meta->scheduling_type == SCHED_TYPE_CBS) {
        struct CbsState *cbs = bpf_map_lookup_elem(&cbs_map, &meta->task_id_hash);
        if (cbs && cbs->exec_start_ns > 0) {
            __u64 elapsed_ns = now - cbs->exec_start_ns;
            __u32 elapsed_us = (__u32)(elapsed_ns / 1000ULL);
            if (elapsed_us >= cbs->remaining_us)
                cbs->remaining_us = 0;
            else
                cbs->remaining_us -= elapsed_us;
            cbs->exec_start_ns = 0;

            /* If budget exhausted, emit BUDGET_EXCEED fault */
            if (cbs->remaining_us == 0) {
                struct FaultEvent *fault;
                fault = bpf_ringbuf_reserve(&fault_ringbuf, sizeof(*fault), 0);
                if (fault) {
                    fault->fault_type = FAULT_BUDGET_EXCEED;
                    fault->task_id_hash = meta->task_id_hash;
                    fault->workload_id_hash = meta->workload_id_hash;
                    fault->cpu = bpf_get_smp_processor_id();
                    fault->expected_deadline_ns = cbs->replenish_at_ns;
                    bpf_ringbuf_submit(fault, 0);
                }
            }
        }
    }

    /* N4: Deadline miss check for TT tasks — lookup from tt_table_map */
    if (meta->scheduling_type == SCHED_TYPE_TT && !runnable) {
        __u32 cpu_key = bpf_get_smp_processor_id();
        __u32 *slot_idx = bpf_map_lookup_elem(&current_slot_map, &cpu_key);
        __u64 deadline_ns = 10000000ULL; /* 10ms fallback */
        if (slot_idx) {
            struct TtSlotKey tt_key = { .cpu = (__u32)cpu_key, .slot_idx = *slot_idx };
            struct TtSlotBpf *slot = bpf_map_lookup_elem(&tt_table_map, &tt_key);
            if (slot && slot->deadline_us > 0)
                deadline_ns = (__u64)slot->deadline_us * 1000ULL;
        }
        __u64 deadline = meta->activation_ns + deadline_ns;
        if (now > deadline) {
            struct FaultEvent *fault;
            fault = bpf_ringbuf_reserve(&fault_ringbuf, sizeof(*fault), 0);
            if (fault) {
                fault->fault_type = FAULT_DMISS;
                fault->task_id_hash = meta->task_id_hash;
                fault->workload_id_hash = meta->workload_id_hash;
                fault->cpu = cpu_key;
                fault->expected_deadline_ns = deadline;
                fault->actual_completion_ns = now;
                bpf_ringbuf_submit(fault, 0);
            }
        }
    }
}

SEC("struct_ops.s/init")
s32 BPF_PROG(init) {
    /* Initialize global custom DSQs */
    scx_bpf_create_dsq(DSQ_TT_WAIT, -1);
    scx_bpf_create_dsq(DSQ_CBS, -1);
    scx_bpf_create_dsq(DSQ_THROTTLED, -1);
    scx_bpf_create_dsq(DSQ_BE, -1);

    return 0;
}

SEC(".struct_ops.link")
struct sched_ext_ops timpani_ops = {
    .flags = SCX_OPS_SWITCH_PARTIAL,
    .init = (void *)init,
    .init_task = (void *)init_task,
    .select_cpu = (void *)select_cpu,
    .enqueue = (void *)enqueue,
    .dispatch = (void *)dispatch,
    .running = (void *)running,
    .stopping = (void *)stopping,
    .name = "timpani"
};

char _license[] SEC("license") = "Dual MIT/GPL";
