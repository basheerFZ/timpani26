// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#pragma once

#include <string>

namespace timpani {
namespace node {

class TaskRegistry {
public:
    TaskRegistry();
    ~TaskRegistry();

    void scan_cgroups();
    void register_task(int pid, int tid, const std::string& comm);

private:
    // Internal state for discovered pids
};

} // namespace node
} // namespace timpani
