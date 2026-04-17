// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#include "table_builder.h"
#include "tlog.h"

#include <chrono>
#include <cstring>
#include <functional>  // std::hash

namespace timpani {
namespace orchestrator {

using namespace timpani::node::v1;

/**
 * @brief Simple FNV-1a hash for a null-terminated string.
 *        Matches the hash approach used by task_registry.cpp so that
 *        BPF map lookups remain consistent.
 */
static uint64_t fnv1a_hash(const char* s)
{
    uint64_t hash = 14695981039346656037ULL;
    while (*s) {
        hash ^= static_cast<uint64_t>(static_cast<unsigned char>(*s++));
        hash *= 1099511628211ULL;
    }
    return hash;
}

timpani::node::v1::HierarchicalScheduleTable BuildScheduleTable(
    const std::string& node_id,
    const SchedInfoMap& sched_map)
{
    HierarchicalScheduleTable table;
    table.set_table_id("table_v1");
    table.set_node_id(node_id);

    // epoch_ns = current wall-clock time (CLOCK_REALTIME)
    // Must match the clock used by timer_master (TIMER_ABSTIME + CLOCK_REALTIME)
    auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    table.set_epoch_ns(static_cast<uint64_t>(now_ns));

    // Single partition — CPUs derived from GlobalScheduler assignments
    PartitionConfig* partition = table.add_partitions();
    partition->set_partition_id("p0");

    CpuSetSpec* cpuset = partition->mutable_cpuset();
    cpuset->set_isolated(false);
    // cpuset.cpus populated per-task below

    LayerConfig* layer = partition->add_layers();
    layer->set_layer_index(1);
    layer->set_model(ExecutionModel::TIME_TRIGGERED);

    uint32_t offset_us   = 0;
    uint64_t hyperperiod = 0;
    int      total_slots = 0;

    for (const auto& [wl_id, node_sched_map] : sched_map) {
        // Filter: only tasks assigned to this node
        auto it = node_sched_map.find(node_id);
        if (it == node_sched_map.end()) {
            TLOG_DEBUG("BuildScheduleTable: workload '", wl_id,
                       "' has no tasks for node '", node_id, "' — skipping");
            continue;
        }

        const sched_info_t& sched_info = it->second;

        for (int i = 0; i < sched_info.num_tasks; i++) {
            const sched_task_t& task = sched_info.tasks[i];

            uint32_t duration_us = (task.runtime_ns  > 0)
                                   ? static_cast<uint32_t>(task.runtime_ns  / 1000)
                                   : 2000u;   // 2 ms default
            uint32_t period_us   = (task.period_ns   > 0)
                                   ? static_cast<uint32_t>(task.period_ns   / 1000)
                                   : 10000u;  // 10 ms default
            uint32_t deadline_us = (task.deadline_ns > 0)
                                   ? static_cast<uint32_t>(task.deadline_ns / 1000)
                                   : period_us;

            TtSlot* slot = layer->add_tt_slots();
            slot->set_workload_id(wl_id);
            slot->set_task_id(task.task_name);
            slot->set_workload_id_hash(fnv1a_hash(wl_id.c_str()));
            slot->set_task_id_hash(fnv1a_hash(task.task_name));
            uint32_t assigned_cpu = static_cast<uint32_t>(task.cpu_affinity);

            slot->set_offset_us(offset_us);
            slot->set_duration_us(duration_us);
            slot->set_deadline_us(deadline_us);
            slot->set_cpu(assigned_cpu);

            // Add CPU to partition cpuset if not already present
            bool already_in_cpuset = false;
            for (int c = 0; c < cpuset->cpus_size(); ++c) {
                if (cpuset->cpus(c) == assigned_cpu) { already_in_cpuset = true; break; }
            }
            if (!already_in_cpuset)
                cpuset->add_cpus(assigned_cpu);

            offset_us += duration_us + 100u;  // 100 us inter-slot gap

            if (period_us > hyperperiod)
                hyperperiod = period_us;

            TLOG_DEBUG("BuildScheduleTable: slot[", total_slots, "] task=", task.task_name,
                       " offset=", slot->offset_us(), "us duration=", duration_us,
                       "us cpu=", assigned_cpu);
            ++total_slots;
        }
    }

    table.set_hyperperiod_us(static_cast<uint32_t>(hyperperiod));

    TLOG_INFO("BuildScheduleTable: node='", node_id, "' slots=", total_slots,
              " hyperperiod=", hyperperiod, "us");

    return table;
}

} // namespace orchestrator
} // namespace timpani
