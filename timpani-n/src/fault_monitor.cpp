// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#include "fault_monitor.h"

namespace timpani {
namespace node {

FaultMonitor::FaultMonitor() : running_(false) {
}

FaultMonitor::~FaultMonitor() {
    stop();
}

void FaultMonitor::set_callback(FaultCallback cb) {
    callback_ = cb;
}

void FaultMonitor::start() {
    running_ = true;
    // TODO: start polling thread for ringbuf
}

void FaultMonitor::stop() {
    running_ = false;
}

void FaultMonitor::poll_loop() {
    // TODO: eBPF ringbuf polling logic
}

} // namespace node
} // namespace timpani
