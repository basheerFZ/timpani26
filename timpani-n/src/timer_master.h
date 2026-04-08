// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#pragma once

#include <thread>

namespace timpani {
namespace node {

class TimerMaster {
public:
    TimerMaster();
    ~TimerMaster();

    void start();
    void stop();

private:
    void thread_loop();

    bool running_;
    std::thread loop_thread_;
};

} // namespace node
} // namespace timpani
