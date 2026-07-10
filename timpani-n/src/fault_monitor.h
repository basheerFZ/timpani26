// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#pragma once

#include "bpf/maps.h"
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

namespace timpani {
namespace node {

class FaultMonitor {
public:
    using FaultCallback = std::function<void(const FaultEvent&, uint32_t dmiss_count)>;

    FaultMonitor();
    ~FaultMonitor();

    void set_ringbuf_fd(int fd);
    void set_callback(FaultCallback cb);
    void start();
    void stop();

    /**
     * @brief Update the per-task dmiss thresholds (current_limit).
     *
     * Called when a new schedule table is received from O.
     * Key: (workload_id_hash, task_id_hash), Value: max allowed dmiss count.
     * When a task's cumulative dmiss exceeds its current_limit, the fault
     * callback is invoked for the workload (workload-level policy).
     */
    void update_current_limits(
        const std::map<std::pair<uint64_t, uint64_t>, uint32_t>& limits);

    /**
     * @brief Clear all dmiss counters and current_limit entries.
     *
     * Called when a new table replaces the old one (full table update),
     * resetting the fault state for a fresh monitoring cycle.
     */
    void clear_state();

private:
    void poll_loop();
    static int ring_buf_callback(void* ctx, void* data, size_t len);

    FaultCallback callback_;
    bool running_;
    int ringbuf_fd_;
    std::thread poll_thread_;

    /// Per-task cumulative dmiss counts. Key: (workload_id_hash, task_id_hash)
    std::map<std::pair<uint64_t, uint64_t>, uint32_t> task_dmiss_counts_;

    /// Per-task dmiss thresholds from schedule table. Key: (workload_id_hash, task_id_hash)
    std::map<std::pair<uint64_t, uint64_t>, uint32_t> task_current_limits_;

    /// Tracks which workloads have already been reported as exceeding the
    /// threshold, preventing duplicate fault reports for the same workload.
    std::map<uint64_t, bool> workload_reported_;

    std::mutex dmiss_mutex_;
};

} // namespace node
} // namespace timpani
