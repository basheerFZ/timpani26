// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#pragma once

#include <linux/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SLOT_NONE 0xFFFFFFFF

#define SCHED_TYPE_TT 0
#define SCHED_TYPE_CBS 1
#define SCHED_TYPE_BEST_EFFORT 2

struct PartitionInfo {
    __u64 task_id_hash;
    __u64 cgroup_id;
    __u8  layer;
    __u8  asil_level;
    __u64 cpu_mask;
};

struct TtSlotKey {
    __u32 cpu;
    __u32 slot_idx;
};

struct TtSlotBpf {
    __u64 workload_id_hash;
    __u64 task_id_hash;
    __u32 offset_us;
    __u32 duration_us;
    __u32 deadline_us;
    __u32 cpu;
};

struct CbsState {
    __u64 task_id_hash;
    __u32 budget_us;
    __u32 period_us;
    __u32 remaining_us;
    __u64 exec_start_ns;
    __u64 replenish_at_ns;
    __u32 deadline_us;
};

struct TaskMeta {
    __u64 workload_id_hash;
    __u64 task_id_hash;
    __u32 assigned_cpu;       /* CPU this TT/CBS task is scheduled on */
    __u8  scheduling_type;
    __u8  layer;
    __u16 _pad;
    __u64 activation_ns;
    __u64 cgroup_id;
};

#define FAULT_DMISS 0
#define FAULT_BUDGET_EXCEED 1

struct FaultEvent {
    __u64 workload_id_hash;
    __u64 task_id_hash;
    __u32 cpu;
    __u64 expected_deadline_ns;
    __u64 actual_completion_ns;
    __u8  fault_type;
};

#ifdef __cplusplus
}
#endif
