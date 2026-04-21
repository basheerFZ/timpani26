// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include "proto/node_control.grpc.pb.h"
#include "schedinfo_service.h"  // SchedInfoMap, NodeSchedInfoMap, sched_info_t, sched_task_t

namespace timpani {
namespace orchestrator {

/**
 * @brief Build a HierarchicalScheduleTable from the global SchedInfoMap.
 *
 * This is the 1차 검증(Phase-1 verification) implementation:
 * - Single partition on CPU 0 (no isolation)
 * - All tasks placed as TtSlots in one LayerConfig (TIME_TRIGGERED)
 * - Slots are packed sequentially with a 100 us gap between them
 * - Priority is read from sched_task_t.sched_priority
 *
 * @param node_id  The node ID to filter tasks for (matches sched_task_t.assigned_node)
 * @param sched_map  Full SchedInfoMap from SchedInfoServer::GetSchedInfoMap()
 * @return Populated HierarchicalScheduleTable (ready for push_full_table)
 */
timpani::node::v1::HierarchicalScheduleTable BuildScheduleTable(
    const std::string& node_id,
    const SchedInfoMap& sched_map);

} // namespace orchestrator
} // namespace timpani
