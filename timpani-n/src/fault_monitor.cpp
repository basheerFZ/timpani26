// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#include "fault_monitor.h"
#include <iostream>
#include <bpf/libbpf.h>
#include <utility>

namespace timpani {
namespace node {

FaultMonitor::FaultMonitor() : running_(false), ringbuf_fd_(-1) {
}

FaultMonitor::~FaultMonitor() {
    stop();
}

void FaultMonitor::set_ringbuf_fd(int fd) {
    ringbuf_fd_ = fd;
}

void FaultMonitor::set_callback(FaultCallback cb) {
    callback_ = cb;
}

void FaultMonitor::start() {
    running_ = true;
    poll_thread_ = std::thread(&FaultMonitor::poll_loop, this);
}

void FaultMonitor::stop() {
    running_ = false;
    if (poll_thread_.joinable()) {
        poll_thread_.join();
    }
}

void FaultMonitor::poll_loop() {
    if (ringbuf_fd_ < 0) return;
    struct ring_buffer* rb = ring_buffer__new(ringbuf_fd_, ring_buf_callback, this, NULL);
    if (!rb) return;

    while (running_) {
        ring_buffer__poll(rb, 100);
    }
    ring_buffer__free(rb);
}

int FaultMonitor::ring_buf_callback(void* ctx, void* data, size_t len) {
    auto* self = static_cast<FaultMonitor*>(ctx);
    auto* event = static_cast<const FaultEvent*>(data);
    uint32_t current_dmiss = 0;
    if (event->fault_type == FAULT_DMISS) {
        std::lock_guard<std::mutex> lock(self->dmiss_mutex_);
        auto key = std::make_pair(event->workload_id_hash, event->task_id_hash);
        self->task_dmiss_counts_[key]++;
        current_dmiss = self->task_dmiss_counts_[key];
    }

    if (self->callback_) {
        self->callback_(*event, current_dmiss);
    }
    return 0;
}

} // namespace node
} // namespace timpani
