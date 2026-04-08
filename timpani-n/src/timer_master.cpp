// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#include "timer_master.h"

namespace timpani {
namespace node {

TimerMaster::TimerMaster() : running_(false) {
}

TimerMaster::~TimerMaster() {
    stop();
}

void TimerMaster::start() {
    running_ = true;
    // TODO: Implement RT priority pinning (SCHED_FIFO)
}

void TimerMaster::stop() {
    running_ = false;
}

void TimerMaster::thread_loop() {
    // Timer Master Thread must strictly remain in RT scope.
    // No gRPC, no dynamic allocation, no blocking mutex.
}

} // namespace node
} // namespace timpani
