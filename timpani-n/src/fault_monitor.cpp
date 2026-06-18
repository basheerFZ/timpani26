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

void FaultMonitor::update_current_limits(
    const std::map<std::pair<uint64_t, uint64_t>, uint32_t>& limits) {
    std::lock_guard<std::mutex> lock(dmiss_mutex_);
    task_current_limits_ = limits;
}

void FaultMonitor::clear_state() {
    std::lock_guard<std::mutex> lock(dmiss_mutex_);
    task_dmiss_counts_.clear();
    task_current_limits_.clear();
    workload_reported_.clear();
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
    bool should_report = false;

    if (event->fault_type == FAULT_DMISS) {
        std::lock_guard<std::mutex> lock(self->dmiss_mutex_);
        auto key = std::make_pair(event->workload_id_hash, event->task_id_hash);

        // Increment per-task cumulative dmiss counter
        self->task_dmiss_counts_[key]++;
        current_dmiss = self->task_dmiss_counts_[key];

        // Check threshold: only report if current_limit is set and exceeded
        auto limit_it = self->task_current_limits_.find(key);
        if (limit_it != self->task_current_limits_.end() &&
            limit_it->second > 0) {
            uint32_t current_limit = limit_it->second;

            if (current_dmiss > current_limit) {
                // Workload-level policy: if any task in the workload exceeds
                // its limit, report the entire workload. Guard against
                // duplicate reports for the same workload.
                uint64_t wid_hash = event->workload_id_hash;
                if (!self->workload_reported_[wid_hash]) {
                    self->workload_reported_[wid_hash] = true;
                    should_report = true;
                    std::cerr << "[FaultMonitor] Threshold exceeded: "
                              << "workload_hash=0x" << std::hex << wid_hash
                              << " task_hash=0x" << event->task_id_hash
                              << std::dec
                              << " dmiss=" << current_dmiss
                              << " limit=" << current_limit << std::endl;
                }
            } else {
                std::cout << "[FaultMonitor] Dmiss within limit: "
                          << "task_hash=0x" << std::hex << event->task_id_hash
                          << std::dec
                          << " dmiss=" << current_dmiss
                          << "/" << current_limit << std::endl;
            }
        } else {
            // No current_limit configured for this task — log only
            std::cout << "[FaultMonitor] Dmiss (no limit configured): "
                      << "task_hash=0x" << std::hex << event->task_id_hash
                      << std::dec
                      << " dmiss=" << current_dmiss << std::endl;
        }
    } else {
        // Non-DMISS faults (e.g., BUDGET_EXCEED) are always reported
        should_report = true;
    }

    if (should_report && self->callback_) {
        self->callback_(*event, current_dmiss);
    }
    return 0;
}

} // namespace node
} // namespace timpani
