// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#pragma once

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "bpf_loader.h"

namespace timpani {
namespace node {

class TimerMaster
{
  public:
    struct SlotEntry {
        uint32_t cpu;
        uint32_t slot_idx;
        uint64_t offset_ns;
        uint64_t task_id_hash;
    };

    TimerMaster(BpfLoader& bpf_loader);
    ~TimerMaster();

    void set_schedule_table(const std::vector<SlotEntry>& slots,
                            uint64_t hyperperiod_us, uint64_t epoch_ns);

    void start();
    void stop();

  private:
    void thread_loop();
    void wake_dummy_tasks();

    BpfLoader& bpf_loader_;
    std::atomic<bool> running_;
    std::atomic<bool> table_pending_;  // set when new table arrives during idle
    std::thread loop_thread_;

    // POSIX shm for ttsched futex wake (shared with sample_apps libttsched)
    int shm_fd_;
    volatile uint32_t* slot_counter_;

    std::vector<SlotEntry> slot_table_;
    uint64_t hyperperiod_ns_;
    uint64_t epoch_ns_;
};

}  // namespace node
}  // namespace timpani
