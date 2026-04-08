// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#include "task_registry.h"

namespace timpani {
namespace node {

TaskRegistry::TaskRegistry(BpfLoader& bpf_loader) : bpf_loader_(bpf_loader) {}

TaskRegistry::~TaskRegistry() {
}

void TaskRegistry::scan_cgroups() {
    // TODO: implement cgroup and task scanning logic
}

void TaskRegistry::register_task(const char* /* comm */, unsigned long /* task_id */) {
    // TODO: match comm to task_id and update task_meta_map via BpfLoader
}

} // namespace node
} // namespace timpani
