// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "bpf_loader.h"

// SHM layout shared with libttsched (C header — include as C struct)
extern "C" {
#include "../../sample-apps/src/libttsched.h"
}

namespace timpani {
namespace node {

class TimerMaster
{
  public:
    struct SlotEntry {
        uint32_t    cpu;
        uint32_t    slot_idx;
        uint64_t    offset_ns;
        uint64_t    duration_ns;
        uint64_t    task_id_hash;
        uint64_t    workload_id_hash;
        std::string task_name;   /* task comm name — used for targeted wake */
    };

    TimerMaster(BpfLoader& bpf_loader);
    ~TimerMaster();

    void set_schedule_table(const std::vector<SlotEntry>& slots,
                            uint64_t hyperperiod_us, uint64_t epoch_ns);

    void start();
    void stop();

    void remove_workload(uint64_t workload_id_hash);

  private:
    void thread_loop();
    void wake_task(uint64_t task_id_hash);  /* targeted wake: only this task's slot */
    uint64_t compute_next_tt_start_ns(size_t current_slot_idx,
                                      uint64_t current_hyperperiod_start) const;

    BpfLoader& bpf_loader_;
    std::atomic<bool> running_;
    std::atomic<bool> table_pending_;
    std::thread loop_thread_;

    // POSIX shm for ttsched futex wake (shared with sample_apps libttsched)
    int shm_fd_;
    volatile struct timpani_ttsched_shm* ttsched_shm_;

    // task_id_hash → index in ttsched_shm_->tasks[]
    std::map<uint64_t, int> task_hash_to_shm_idx_;

    std::vector<SlotEntry> slot_table_;
    uint64_t hyperperiod_ns_;
    uint64_t epoch_ns_;
    std::mutex schedule_mutex_;
};

}  // namespace node
}  // namespace timpani
