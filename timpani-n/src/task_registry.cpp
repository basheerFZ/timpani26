// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#include "task_registry.h"

namespace timpani {
namespace node {

TaskRegistry::TaskRegistry() {
}

TaskRegistry::~TaskRegistry() {
}

void TaskRegistry::scan_cgroups() {
    // TODO: implement cgroup and task scanning logic
}

void TaskRegistry::register_task(int /* pid */, int /* tid */, const std::string& /* comm */) {
    // TODO: match comm to task_id and update task_meta_map via BpfLoader
}

} // namespace node
} // namespace timpani
