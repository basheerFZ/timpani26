// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#pragma once

#include <thread>
#include <vector>
#include <atomic>
#include "bpf_loader.h"

namespace timpani {
namespace node {

class TimerMaster {
public:
    struct SlotEntry {
        uint32_t cpu;
        uint32_t slot_idx;
        uint64_t offset_ns;
        uint64_t task_id_hash;
    };

    TimerMaster(BpfLoader& bpf_loader);
    ~TimerMaster();

    void set_schedule_table(const std::vector<SlotEntry>& slots, uint64_t hyperperiod_us, uint64_t epoch_ns);

    void start();
    void stop();

private:
    void thread_loop();
    void wake_dummy_tasks();

    BpfLoader& bpf_loader_;
    std::atomic<bool> running_;
    std::thread loop_thread_;

    std::vector<SlotEntry> slot_table_;
    uint64_t hyperperiod_ns_;
    uint64_t epoch_ns_;
};

} // namespace node
} // namespace timpani
