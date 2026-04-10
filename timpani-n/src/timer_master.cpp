// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#include "timer_master.h"
#include <iostream>
#include <sched.h>
#include <time.h>
#include <vector>
#include <algorithm>
#include <csignal>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <linux/futex.h>
#include <fcntl.h>
#include <unistd.h>

namespace timpani {
namespace node {

TimerMaster::TimerMaster(BpfLoader& bpf_loader)
    : bpf_loader_(bpf_loader), running_(false), table_pending_(false),
      hyperperiod_ns_(0), epoch_ns_(0), shm_fd_(-1), slot_counter_(nullptr)
{
    // Create/open POSIX shm for ttsched futex wake
    shm_fd_ = shm_open("/timpani_ttsched", O_CREAT | O_RDWR, 0666);
    if (shm_fd_ >= 0) {
        ftruncate(shm_fd_, sizeof(uint32_t));
        slot_counter_ = static_cast<volatile uint32_t*>(
            mmap(nullptr, sizeof(uint32_t), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd_, 0));
        if (slot_counter_ == MAP_FAILED) {
            slot_counter_ = nullptr;
            std::cerr << "[TimerMaster] shm mmap failed" << std::endl;
        } else {
            *slot_counter_ = 0;
            std::cout << "[TimerMaster] shm /timpani_ttsched created" << std::endl;
        }
    } else {
        std::cerr << "[TimerMaster] shm_open failed" << std::endl;
    }
}
}

TimerMaster::~TimerMaster() {
    stop();
    if (slot_counter_ && slot_counter_ != MAP_FAILED)
        munmap(const_cast<uint32_t*>(slot_counter_), sizeof(uint32_t));
    if (shm_fd_ >= 0)
        close(shm_fd_);
    shm_unlink("/timpani_ttsched");
}

void TimerMaster::set_schedule_table(const std::vector<SlotEntry>& slots, uint64_t hyperperiod_us, uint64_t epoch_ns) {
    slot_table_ = slots;
    hyperperiod_ns_ = hyperperiod_us * 1000ULL;
    epoch_ns_ = epoch_ns;
    // Sort by offset_ns ascending
    std::sort(slot_table_.begin(), slot_table_.end(), [](const SlotEntry& a, const SlotEntry& b) {
        return a.offset_ns < b.offset_ns;
    });
    table_pending_ = true;  // signal idle loop to restart
}

void TimerMaster::start() {
    running_ = true;
    loop_thread_ = std::thread(&TimerMaster::thread_loop, this);
}

void TimerMaster::stop() {
    running_ = false;
    if (loop_thread_.joinable()) {
        loop_thread_.join();
    }
}

void TimerMaster::thread_loop() {
    struct sched_param param = {};
    param.sched_priority = 99;
    sched_setscheduler(0, SCHED_FIFO, &param);

    std::vector<long long> jitters;
    jitters.reserve(1500);

    // Outer loop: restart slot execution when a new table arrives
    while (running_) {
        table_pending_ = false;

        if (slot_table_.empty() || hyperperiod_ns_ == 0) {
            // No table yet — idle loop, wakes every 1ms to check for new table
            struct timespec next_ts;
            clock_gettime(CLOCK_REALTIME, &next_ts);
            while (running_ && !table_pending_) {
                next_ts.tv_nsec += 1000000;
                if (next_ts.tv_nsec >= 1000000000) { next_ts.tv_sec += 1; next_ts.tv_nsec -= 1000000000; }
                clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &next_ts, nullptr);
            }
            continue;  // re-evaluate slot_table_ at top of outer loop
        }

        size_t next_slot_idx = 0;
        uint64_t current_hyperperiod_start = epoch_ns_;
        if (epoch_ns_ == 0) {
            struct timespec now;
            clock_gettime(CLOCK_REALTIME, &now);
            current_hyperperiod_start = (uint64_t)now.tv_sec * 1000000000ULL + now.tv_nsec;
        }

        while (running_ && !table_pending_) {
            const auto& slot = slot_table_[next_slot_idx];
            uint64_t target_ns = current_hyperperiod_start + slot.offset_ns;

        struct timespec next_ts;
        next_ts.tv_sec = target_ns / 1000000000ULL;
        next_ts.tv_nsec = target_ns % 1000000000ULL;

        clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &next_ts, nullptr);

        struct timespec wakeup_ts;
        clock_gettime(CLOCK_REALTIME, &wakeup_ts);
        
        long long actual_ns = (long long)wakeup_ts.tv_sec * 1000000000LL + wakeup_ts.tv_nsec;
        long long jitter = actual_ns - (long long)target_ns;

        // C3: Update current slot to map directly
        bpf_loader_.update_current_slot(slot.cpu, slot.slot_idx);

        jitters.push_back(jitter);

        if (jitters.size() == 1000) {
            long long sum = 0, min_j = jitters[0], max_j = jitters[0];
            for (long long j : jitters) {
                sum += j;
                if (j < min_j) min_j = j;
                if (j > max_j) max_j = j;
            }
            std::sort(jitters.begin(), jitters.end());
            
            std::cout << "\nTimer Master Wakeup Jitter (CLOCK_REALTIME ABSTIME):" << std::endl;
            std::cout << "  Samples: 1000" << std::endl;
            std::cout << "  Min: " << min_j << " ns" << std::endl;
            std::cout << "  Max: " << max_j << " ns" << std::endl;
            std::cout << "  Avg: " << sum / 1000 << " ns" << std::endl;
            std::cout << "  P99: " << jitters[990] << " ns" << std::endl;
            std::cout << "  P999: " << jitters[999] << " ns" << std::endl;
            
            std::cout << "\nHistogram (10us buckets):" << std::endl;
            int buckets[10] = {0}; // 0-10us, 10-20us...
            int outliers = 0;
            for (long long j : jitters) {
                int bucket_idx = j / 10000;
                if (bucket_idx >= 0 && bucket_idx < 10) buckets[bucket_idx]++;
                else outliers++;
            }
            for (int i = 0; i < 10; ++i) {
                std::cout << "  " << (i*10) << "us - " << ((i+1)*10) << "us: " << buckets[i] << std::endl;
            }
            std::cout << "  > 100us outliers: " << outliers << std::endl;

            std::cout << "Measurement complete. Continuing..." << std::endl;
            jitters.clear();
            jitters.reserve(1500);
        }

            wake_dummy_tasks();

            next_slot_idx++;
            if (next_slot_idx >= slot_table_.size()) {
                next_slot_idx = 0;
                current_hyperperiod_start += hyperperiod_ns_;
            }
        }  // end slot loop
    }  // end outer loop
}  // end thread_loop

void TimerMaster::wake_dummy_tasks() {
    if (!slot_counter_) return;
    // Increment counter and wake all FUTEX_WAIT waiters
    __atomic_fetch_add(const_cast<uint32_t*>(slot_counter_), 1, __ATOMIC_SEQ_CST);
    syscall(SYS_futex, const_cast<uint32_t*>(slot_counter_),
            FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0);
}

} // namespace node
} // namespace timpani
