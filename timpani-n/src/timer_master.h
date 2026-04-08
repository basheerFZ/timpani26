// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#pragma once

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
};

} // namespace node
} // namespace timpani
