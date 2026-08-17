// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#include "timer_master.h"

#include <fcntl.h>
#include <linux/futex.h>
#include <sched.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <climits>
#include <csignal>
#include <cstring>
#include <iostream>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>

namespace timpani {
namespace node {

TimerMaster::TimerMaster(BpfLoader& bpf_loader)
    : bpf_loader_(bpf_loader),
      running_(false),
      table_pending_(false),
      hyperperiod_ns_(0),
      epoch_ns_(0),
      shm_fd_(-1),
      ttsched_shm_(nullptr)
{
    shm_fd_ = shm_open("/timpani_ttsched", O_CREAT | O_RDWR, 0666);
    if (shm_fd_ >= 0) {
        if (ftruncate(shm_fd_, sizeof(struct timpani_ttsched_shm)) < 0) {
            std::cerr << "[TimerMaster] ftruncate failed" << std::endl;
        }
        ttsched_shm_ = static_cast<volatile struct timpani_ttsched_shm*>(
            mmap(nullptr, sizeof(struct timpani_ttsched_shm),
                 PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd_, 0));
        if (ttsched_shm_ == MAP_FAILED) {
            ttsched_shm_ = nullptr;
            std::cerr << "[TimerMaster] shm mmap failed" << std::endl;
        } else {
            /* Initialize: magic=0 (not ready), n_tasks=0, generation=1 */
            ttsched_shm_->magic = 0;
            ttsched_shm_->n_tasks = 0;
            ttsched_shm_->generation = 1;
            std::cout << "[TimerMaster] shm /timpani_ttsched created"
                      << std::endl;
        }
    } else {
        std::cerr << "[TimerMaster] shm_open failed" << std::endl;
    }
}

TimerMaster::~TimerMaster()
{
    stop();

    if (ttsched_shm_ && ttsched_shm_ != MAP_FAILED) {
        /* Signal shutdown to all waiting tasks:
         * 1. Zero magic so tasks detect timpani-n is gone after wakeup.
         * 2. FUTEX_WAKE on every task counter to unblock FUTEX_WAIT.
         *    INT_MAX wakes all waiters regardless of count. */
        __atomic_store_n(const_cast<uint32_t*>(&ttsched_shm_->magic),
                         0u, __ATOMIC_SEQ_CST);
        uint32_t n = ttsched_shm_->n_tasks;
        if (n > TIMPANI_MAX_TASKS) n = TIMPANI_MAX_TASKS;
        for (uint32_t i = 0; i < n; i++) {
            syscall(SYS_futex,
                    const_cast<uint32_t*>(&ttsched_shm_->tasks[i].counter),
                    FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0);
        }

        munmap(const_cast<struct timpani_ttsched_shm*>(ttsched_shm_),
               sizeof(struct timpani_ttsched_shm));
    }
    if (shm_fd_ >= 0) close(shm_fd_);
    shm_unlink("/timpani_ttsched");
}

void TimerMaster::set_schedule_table(const std::vector<SlotEntry>& slots,
                                     uint64_t hyperperiod_us, uint64_t epoch_ns)
{
    std::unique_lock<std::mutex> lock(schedule_mutex_);
    /* Runtime table updates are serialized by the gRPC callback path in this
     * component. The slot-table swap, SHM publication, and worker restart are
     * therefore treated as a single state transition. If this becomes
     * multi-threaded, an explicit higher-level serializer should be added. */
    slot_table_ = slots;
    hyperperiod_ns_ = hyperperiod_us * 1000ULL;
    epoch_ns_ = epoch_ns;
    // Sort by offset_ns ascending
    std::sort(slot_table_.begin(), slot_table_.end(),
              [](const SlotEntry& a, const SlotEntry& b) {
                  return a.offset_ns < b.offset_ns;
              });

    /* Publish per-task SHM slots for targeted wake.
     * Collect unique tasks (same task may appear in multiple slots). */
    if (ttsched_shm_) {
        /* Clear ready flag first so tasks don't read a half-written table */
        ttsched_shm_->magic = 0;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);

        /* Wake all tasks currently waiting on SHM slots so they notice magic == 0
         * and re-initialize their slot indices when the new schedule table is published. */
        for (uint32_t i = 0; i < TIMPANI_MAX_TASKS; ++i) {
            __atomic_fetch_add(const_cast<uint32_t*>(&ttsched_shm_->tasks[i].counter),
                               1u, __ATOMIC_SEQ_CST);
            syscall(SYS_futex, const_cast<uint32_t*>(&ttsched_shm_->tasks[i].counter),
                    FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0);
        }

        std::map<uint64_t, int> prev_mapping = task_hash_to_shm_idx_;
        task_hash_to_shm_idx_.clear();

        std::vector<bool> used_slots(TIMPANI_MAX_TASKS, false);

        /* First pass: preserve stable SHM indices for existing tasks */
        for (const auto& entry : slot_table_) {
            if (task_hash_to_shm_idx_.count(entry.task_id_hash)) continue;
            auto it = prev_mapping.find(entry.task_id_hash);
            if (it != prev_mapping.end() && it->second < TIMPANI_MAX_TASKS &&
                !used_slots[it->second]) {
                uint32_t idx = it->second;
                task_hash_to_shm_idx_[entry.task_id_hash] = idx;
                used_slots[idx] = true;
                strncpy(const_cast<char*>(ttsched_shm_->tasks[idx].name),
                        entry.task_name.c_str(), 15);
                ttsched_shm_->tasks[idx].name[15] = '\0';
            }
        }

        /* Second pass: allocate lowest available SHM indices for new tasks */
        for (const auto& entry : slot_table_) {
            if (task_hash_to_shm_idx_.count(entry.task_id_hash)) continue;

            uint32_t idx = 0;
            while (idx < TIMPANI_MAX_TASKS && used_slots[idx]) {
                idx++;
            }
            if (idx >= TIMPANI_MAX_TASKS) {
                std::cerr << "[TimerMaster] Too many unique tasks (max "
                          << TIMPANI_MAX_TASKS << ")" << std::endl;
                break;
            }

            used_slots[idx] = true;
            task_hash_to_shm_idx_[entry.task_id_hash] = idx;
            strncpy(const_cast<char*>(ttsched_shm_->tasks[idx].name),
                    entry.task_name.c_str(), 15);
            ttsched_shm_->tasks[idx].name[15] = '\0';
            ttsched_shm_->tasks[idx].counter = 0;
        }

        uint32_t total_slots = 0;
        for (const auto& kv : task_hash_to_shm_idx_) {
            if (kv.second + 1 > total_slots) {
                total_slots = kv.second + 1;
            }
            std::cout << "[TimerMaster] SHM slot[" << kv.second
                      << "] = " << ttsched_shm_->tasks[kv.second].name
                      << " hash=0x" << std::hex << kv.first << std::dec
                      << std::endl;
        }

        /* Publish atomically: first n_tasks and generation, then magic */
        __atomic_store_n(const_cast<uint32_t*>(&ttsched_shm_->n_tasks),
                         total_slots, __ATOMIC_RELEASE);
        __atomic_fetch_add(const_cast<uint32_t*>(&ttsched_shm_->generation),
                           1u, __ATOMIC_RELEASE);
        __atomic_store_n(const_cast<uint32_t*>(&ttsched_shm_->magic),
                         TIMPANI_TTSCHED_MAGIC, __ATOMIC_RELEASE);

        std::cout << "[TimerMaster] SHM published: " << total_slots
                  << " task slots ready (gen=" << ttsched_shm_->generation
                  << ")" << std::endl;
    }

    table_pending_ = true;  // signal existing threads to exit

    /* If already running, respawn per-CPU threads with the new table */
    if (running_) {
        /* Release lock before calling start() which also acquires it */
        lock.unlock();
        start();
    }
}

void TimerMaster::start()
{
    /* Lock ordering rule: lifecycle_mutex_ must be acquired before
     * schedule_mutex_ whenever both are needed. This prevents lock inversion
     * between the gRPC-driven table updates and the timer-thread lifecycle. */
    std::lock_guard<std::recursive_mutex> lk(lifecycle_mutex_);

    /* Stop any existing per-CPU threads before spawning new ones */
    stop();

    running_ = true;
    table_pending_ = false;

    /* Partition slot_table_ by CPU */
    std::map<uint32_t, std::vector<SlotEntry>> per_cpu_slots;
    {
        std::lock_guard<std::mutex> lock(schedule_mutex_);
        for (const auto& entry : slot_table_) {
            per_cpu_slots[entry.cpu].push_back(entry);
        }
    }

    if (per_cpu_slots.empty()) {
        std::cout << "[TimerMaster] Started in idle mode (awaiting initial schedule table)" << std::endl;
        /* No slots yet — spawn a single idle-poll thread that waits for table_pending_ */
        cpu_threads_.emplace_back([this]() {
            struct sched_param param = {};
            param.sched_priority = 99;
            sched_setscheduler(0, SCHED_FIFO, &param);

            struct timespec next_ts;
            clock_gettime(CLOCK_REALTIME, &next_ts);
            while (running_ && !table_pending_) {
                next_ts.tv_nsec += 1000000;
                if (next_ts.tv_nsec >= 1000000000) {
                    next_ts.tv_sec += 1;
                    next_ts.tv_nsec -= 1000000000;
                }
                clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &next_ts,
                                nullptr);
            }
        });
        return;
    }

    /* Each per-CPU slot list is already sorted by offset_ns (inherited from
     * slot_table_ which was sorted in set_schedule_table). */
    uint64_t hp_ns, ep_ns;
    {
        std::lock_guard<std::mutex> lock(schedule_mutex_);
        hp_ns = hyperperiod_ns_;
        ep_ns = epoch_ns_;
    }

    std::cout << "[TimerMaster] Starting per-CPU timer threads across "
              << per_cpu_slots.size() << " CPU(s) (hyperperiod="
              << (hp_ns / 1000ULL) << "us)" << std::endl;

    for (auto& [cpu, cpu_slots] : per_cpu_slots) {
        std::cout << "[TimerMaster] Spawning per-CPU thread for CPU " << cpu
                  << " with " << cpu_slots.size() << " slots" << std::endl;
        cpu_threads_.emplace_back(&TimerMaster::cpu_thread_loop, this,
                                  cpu, std::move(cpu_slots), hp_ns, ep_ns);
    }
}

void TimerMaster::stop()
{
    std::lock_guard<std::recursive_mutex> lk(lifecycle_mutex_);
    running_ = false;
    for (auto& t : cpu_threads_) {
        if (t.joinable()) {
            t.join();
        }
    }
    cpu_threads_.clear();
}

uint64_t TimerMaster::compute_next_tt_start_for_cpu(
    const std::vector<SlotEntry>& cpu_slots,
    size_t current_slot_idx,
    uint64_t current_hyperperiod_start,
    uint64_t hyperperiod_ns)
{
    if (cpu_slots.empty() || hyperperiod_ns == 0) return 0;

    size_t next_idx = (current_slot_idx + 1) % cpu_slots.size();
    uint64_t next_hyperperiod_start = current_hyperperiod_start;
    if (next_idx <= current_slot_idx) {
        next_hyperperiod_start += hyperperiod_ns;
    }
    return next_hyperperiod_start + cpu_slots[next_idx].offset_ns;
}

void TimerMaster::remove_workload(uint64_t workload_id_hash)
{
    std::unique_lock<std::mutex> lock(schedule_mutex_);
    /* remove_workload() follows the same gRPC-driven update path as
     * set_schedule_table() and reuses the same serialized state transition. */
    auto it = std::remove_if(slot_table_.begin(), slot_table_.end(),
                             [workload_id_hash](const SlotEntry& entry) {
                                 return entry.workload_id_hash == workload_id_hash;
                             });
    if (it != slot_table_.end()) {
        slot_table_.erase(it, slot_table_.end());
        table_pending_ = true;
        std::cout << "[TimerMaster] Removed slots for workload hash: 0x" 
                  << std::hex << workload_id_hash << std::dec << std::endl;
        if (running_) {
            lock.unlock();
            start();
        }
    }
}

void TimerMaster::cpu_thread_loop(uint32_t cpu,
                                  std::vector<SlotEntry> cpu_slots,
                                  uint64_t hyperperiod_ns,
                                  uint64_t epoch_ns)
{
    /* "Per-CPU" here means each worker loop manages one partition of the slot
     * table for a specific CPU. The thread itself is not pinned to that CPU
     * with sched_setaffinity(), so this is a logical partitioning scheme rather
     * than an execution-affinity guarantee. */
    struct sched_param param = {};
    param.sched_priority = 99;
    sched_setscheduler(0, SCHED_FIFO, &param);

    std::vector<long long> jitters;
    jitters.reserve(1500);

    /* Sort per-CPU slots by offset_ns ascending */
    std::sort(cpu_slots.begin(), cpu_slots.end(),
              [](const SlotEntry& a, const SlotEntry& b) {
                  return a.offset_ns < b.offset_ns;
              });

    std::cout << "[TimerMaster:CPU" << cpu << "] Thread started with "
              << cpu_slots.size() << " slots, hyperperiod="
              << (hyperperiod_ns / 1000ULL) << "us" << std::endl;

    while (running_) {
        if (table_pending_) break;

        if (cpu_slots.empty() || hyperperiod_ns == 0) {
            struct timespec next_ts;
            clock_gettime(CLOCK_REALTIME, &next_ts);
            while (running_ && !table_pending_) {
                next_ts.tv_nsec += 1000000;
                if (next_ts.tv_nsec >= 1000000000) {
                    next_ts.tv_sec += 1;
                    next_ts.tv_nsec -= 1000000000;
                }
                clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &next_ts,
                                nullptr);
            }
            break;
        }

        size_t next_slot_idx = 0;
        uint64_t current_hyperperiod_start = epoch_ns;
        struct timespec now_ts;
        clock_gettime(CLOCK_REALTIME, &now_ts);
        uint64_t now_ns =
            (uint64_t)now_ts.tv_sec * 1000000000ULL + now_ts.tv_nsec;

        if (epoch_ns == 0) {
            current_hyperperiod_start = now_ns;
        } else if (epoch_ns < now_ns) {
            uint64_t elapsed_ns = now_ns - epoch_ns;
            uint64_t periods = (elapsed_ns / hyperperiod_ns) + 1;
            current_hyperperiod_start =
                epoch_ns + periods * hyperperiod_ns;
            std::cout << "[TimerMaster:CPU" << cpu << "] Catch-up applied: jumped "
                      << periods << " periods into the future." << std::endl;
        }
        std::cout << "[TimerMaster:CPU" << cpu << "] Hyperperiod start time set: "
                  << (current_hyperperiod_start / 1000ULL) << " us"
                  << std::endl;

        while (running_ && !table_pending_) {
            const auto& slot = cpu_slots[next_slot_idx];
            uint64_t target_ns = current_hyperperiod_start + slot.offset_ns;

            struct timespec next_ts;
            next_ts.tv_sec = target_ns / 1000000000ULL;
            next_ts.tv_nsec = target_ns % 1000000000ULL;

            clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &next_ts, nullptr);

            struct timespec wakeup_ts;
            clock_gettime(CLOCK_REALTIME, &wakeup_ts);

            long long actual_ns =
                (long long)wakeup_ts.tv_sec * 1000000000LL + wakeup_ts.tv_nsec;
            long long jitter = actual_ns - (long long)target_ns;

            bpf_loader_.update_current_slot(slot.cpu, slot.slot_idx);

            struct timespec mono_ts;
            clock_gettime(CLOCK_MONOTONIC, &mono_ts);
            uint64_t now_mono_ns =
                (uint64_t)mono_ts.tv_sec * 1000000000ULL + mono_ts.tv_nsec;
            uint64_t next_tt_realtime_ns = compute_next_tt_start_for_cpu(
                cpu_slots, next_slot_idx, current_hyperperiod_start,
                hyperperiod_ns);
            if (next_tt_realtime_ns > target_ns) {
                uint64_t next_delta_ns = next_tt_realtime_ns - target_ns;
                bpf_loader_.update_next_tt_start(slot.cpu,
                                                 now_mono_ns + next_delta_ns);
            }

            jitters.push_back(jitter);

            if (jitters.size() == 1000) {
                long long sum = 0, min_j = jitters[0], max_j = jitters[0];
                for (long long j : jitters) {
                    sum += j;
                    if (j < min_j) min_j = j;
                    if (j > max_j) max_j = j;
                }
                std::sort(jitters.begin(), jitters.end());

                std::cout
                    << "\n[CPU" << cpu
                    << "] Timer Master Wakeup Jitter (CLOCK_REALTIME ABSTIME):"
                    << std::endl;
                std::cout << "  Samples: 1000" << std::endl;
                std::cout << "  Min: " << min_j << " ns" << std::endl;
                std::cout << "  Max: " << max_j << " ns" << std::endl;
                std::cout << "  Avg: " << sum / 1000 << " ns" << std::endl;
                std::cout << "  P99: " << jitters[990] << " ns" << std::endl;
                std::cout << "  P999: " << jitters[999] << " ns" << std::endl;

                std::cout << "\n[CPU" << cpu
                          << "] Histogram (10us buckets, absolute jitter):"
                          << std::endl;
                int buckets[10] = {0};
                int outliers = 0;
                for (long long j : jitters) {
                    long long abs_j = j < 0 ? -j : j;
                    int bucket_idx = (int)(abs_j / 10000);
                    if (bucket_idx >= 0 && bucket_idx < 10)
                        buckets[bucket_idx]++;
                    else
                        outliers++;
                }
                for (int i = 0; i < 10; ++i) {
                    std::cout << "  " << (i * 10) << "us - " << ((i + 1) * 10)
                              << "us: " << buckets[i] << std::endl;
                }
                std::cout << "  > 100us outliers: " << outliers << std::endl;

                std::cout << "[CPU" << cpu
                          << "] Measurement complete. Continuing..."
                          << std::endl;
                jitters.clear();
                jitters.reserve(1500);
            }

            wake_task(slot.task_id_hash);

            next_slot_idx++;
            if (next_slot_idx >= cpu_slots.size()) {
                next_slot_idx = 0;
                current_hyperperiod_start += hyperperiod_ns;
            }
        }
    }

    std::cout << "[TimerMaster:CPU" << cpu << "] Thread exiting" << std::endl;
}

void TimerMaster::wake_task(uint64_t task_id_hash)
{
    if (!ttsched_shm_) return;

    /* Read the task-to-SHM mapping under schedule_mutex_ so this wake-up path
     * stays consistent with set_schedule_table()/remove_workload() updates. */
    std::lock_guard<std::mutex> lock(schedule_mutex_);

    auto it = task_hash_to_shm_idx_.find(task_id_hash);
    if (it == task_hash_to_shm_idx_.end()) return;

    int idx = it->second;
    /* Increment per-task counter and wake exactly 1 waiter */
    __atomic_fetch_add(const_cast<uint32_t*>(&ttsched_shm_->tasks[idx].counter),
                       1u, __ATOMIC_SEQ_CST);
    syscall(SYS_futex, const_cast<uint32_t*>(&ttsched_shm_->tasks[idx].counter),
            FUTEX_WAKE, 1, /* wake only the ONE task waiting on this counter */
            nullptr, nullptr, 0);
}

}  // namespace node
}  // namespace timpani
