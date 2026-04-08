// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#pragma once

#include "bpf_loader.h"

namespace timpani {
namespace node {

class TaskRegistry {
public:
    explicit TaskRegistry(BpfLoader& bpf_loader);
    ~TaskRegistry();

    void scan_cgroups();
    void register_task(const char* comm, unsigned long task_id);

private:
    BpfLoader& bpf_loader_;
};

} // namespace node
} // namespace timpani
