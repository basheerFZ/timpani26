// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#include <dirent.h>
#include <errno.h>
#include <sched.h>
#include <unistd.h>

#include <cctype>
#include <csignal>
#include <fstream>
#include <iostream>
#include <memory>

#include "bpf_loader.h"
#include "fault_monitor.h"
#include "grpc/node_client.h"
#include "task_registry.h"
#include "timer_master.h"

volatile sig_atomic_t g_shutdown = 0;

void signal_handler(int /* signum */) { g_shutdown = 1; }

/**
 * @brief Search /proc for a process with matching comm name.
 * @param comm  Task comm name (up to 15 chars as in /proc/<pid>/comm)
 * @return PID on success, -1 if not found
 */
static pid_t find_pid_by_comm(const std::string& comm)
{
    DIR* proc = opendir("/proc");
    if (!proc) return -1;

    struct dirent* entry;
    pid_t found = -1;
    while ((entry = readdir(proc)) != nullptr) {
        if (entry->d_type != DT_DIR) continue;
        bool is_num = true;
        for (const char* p = entry->d_name; *p; ++p)
            if (!std::isdigit(static_cast<unsigned char>(*p))) {
                is_num = false;
                break;
            }
        if (!is_num) continue;

        std::string comm_path = std::string("/proc/") + entry->d_name + "/comm";
        std::ifstream f(comm_path);
        if (!f.is_open()) continue;

        std::string proc_comm;
        std::getline(f, proc_comm);
        // comm is truncated to 15 chars in the kernel
        std::string cmp_comm = comm.substr(0, 15);
        if (proc_comm == cmp_comm || proc_comm == comm) {
            found = static_cast<pid_t>(std::stoi(entry->d_name));
            break;
        }
    }
    closedir(proc);
    return found;
}

int main(int argc, char** argv)
{
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::cout << "Starting TIMPANI Node Executor (timpani-n C++ rework)..."
              << std::endl;

    try {
        // 1. Initialize BPF Loader and ensure safe termination
        timpani::node::BpfLoader bpf_loader;
        if (!bpf_loader.load_programs()) {
            std::cerr << "Failed to load BPF programs." << std::endl;
            return 1;
        }

        // Initialize Task Registry
        timpani::node::TaskRegistry task_registry(bpf_loader);
        task_registry.scan_cgroups();

        // Initialize Fault Monitor
        timpani::node::FaultMonitor fault_monitor;
        fault_monitor.set_ringbuf_fd(bpf_loader.get_fault_ringbuf_fd());

        // Initialize NodeClient (gRPC)
        timpani::node::NodeClient node_client("127.0.0.1:50060");

        // Connect callbacks
        node_client.set_table_callback([&bpf_loader](const auto& table) {
            std::cout << "[main] Received table: " << table.table_id()
                      << " hyperperiod=" << table.hyperperiod_us() << "us"
                      << " partitions=" << table.partitions_size() << std::endl;

            for (const auto& partition : table.partitions()) {
                // Build CPU affinity mask from partition cpuset
                cpu_set_t cpuset;
                CPU_ZERO(&cpuset);
                for (uint32_t cpu : partition.cpuset().cpus())
                    CPU_SET(cpu, &cpuset);

                for (const auto& layer : partition.layers()) {
                    for (const auto& slot : layer.tt_slots()) {
                        const std::string& task_id = slot.task_id();
                        pid_t pid = find_pid_by_comm(task_id);
                        if (pid < 0) {
                            std::cerr
                                << "[main] Task not found in /proc: " << task_id
                                << std::endl;
                            continue;
                        }

                        // Apply SCHED_FIFO (priority 20 fixed for Phase 1)
                        struct sched_param param;
                        param.sched_priority = 20;
                        if (sched_setscheduler(pid, SCHED_FIFO, &param) == 0) {
                            std::cout << "[main] Applied SCHED_FIFO to "
                                      << task_id << " pid=" << pid << std::endl;
                        } else {
                            std::cerr << "[main] sched_setscheduler failed for "
                                      << task_id << " pid=" << pid
                                      << " errno=" << errno << std::endl;
                        }

                        // Apply CPU affinity
                        if (sched_setaffinity(pid, sizeof(cpuset), &cpuset) ==
                            0) {
                            std::cout << "[main] Applied CPU affinity to "
                                      << task_id << " pid=" << pid << std::endl;
                        } else {
                            std::cerr << "[main] sched_setaffinity failed for "
                                      << task_id << " pid=" << pid
                                      << " errno=" << errno << std::endl;
                        }
                    }
                }
            }
        });

        node_client.set_shutdown_callback([](uint32_t grace_period_ms) {
            std::cout << "[main] Received shutdown command: " << grace_period_ms
                      << " ms" << std::endl;
            // Handle shutdown
        });

        fault_monitor.set_callback([&node_client](const auto& event) {
            timpani::node::v1::FaultInfo fault;
            fault.set_workload_id_hash(event.workload_id_hash);
            fault.set_task_id_hash(event.task_id_hash);
            fault.set_fault_type(
                static_cast<timpani::node::v1::FaultType>(event.fault_type));
            node_client.send_fault(fault);
        });

        fault_monitor.start();

        // 5. Initialize Timer Master (RT Priority Thread) last
        timpani::node::TimerMaster timer_master(bpf_loader);

        // Wire table_callback: received HierarchicalScheduleTable → TimerMaster
        // slots (Re-set callback now that timer_master is in scope)
        node_client.set_table_callback([&bpf_loader,
                                        &timer_master](const auto& table) {
            std::cout << "[main] Received table: " << table.table_id()
                      << " hyperperiod=" << table.hyperperiod_us() << "us"
                      << " partitions=" << table.partitions_size() << std::endl;

            // Build SlotEntry list from TtSlots
            std::vector<timpani::node::TimerMaster::SlotEntry> slots;
            uint32_t slot_idx = 0;
            for (const auto& partition : table.partitions()) {
                // Build CPU affinity mask from partition cpuset
                cpu_set_t cpuset;
                CPU_ZERO(&cpuset);
                for (uint32_t cpu : partition.cpuset().cpus())
                    CPU_SET(cpu, &cpuset);

                for (const auto& layer : partition.layers()) {
                    for (const auto& tt_slot : layer.tt_slots()) {
                        // Apply SCHED_FIFO + affinity to matching process
                        const std::string& task_id = tt_slot.task_id();
                        pid_t pid = find_pid_by_comm(task_id);
                        if (pid < 0) {
                            std::cerr
                                << "[main] Task not found in /proc: " << task_id
                                << std::endl;
                        } else {
                            struct sched_param param;
                            param.sched_priority = 20;
                            if (sched_setscheduler(pid, SCHED_FIFO, &param) ==
                                0)
                                std::cout << "[main] Applied SCHED_FIFO to "
                                          << task_id << " pid=" << pid
                                          << std::endl;
                            else
                                std::cerr
                                    << "[main] sched_setscheduler failed for "
                                    << task_id << " pid=" << pid
                                    << " errno=" << errno << std::endl;

                            if (sched_setaffinity(pid, sizeof(cpuset),
                                                  &cpuset) == 0)
                                std::cout << "[main] Applied CPU affinity to "
                                          << task_id << " pid=" << pid
                                          << std::endl;
                            else
                                std::cerr
                                    << "[main] sched_setaffinity failed for "
                                    << task_id << " pid=" << pid
                                    << " errno=" << errno << std::endl;
                        }

                        // Add TimerMaster slot entry
                        timpani::node::TimerMaster::SlotEntry entry;
                        entry.cpu = tt_slot.cpu();
                        entry.slot_idx = slot_idx++;
                        entry.offset_ns =
                            static_cast<uint64_t>(tt_slot.offset_us()) *
                            1000ULL;
                        entry.task_id_hash = tt_slot.task_id_hash();
                        slots.push_back(entry);
                    }
                }
            }

            uint64_t hyperperiod_us = table.hyperperiod_us();
            uint64_t epoch_ns = table.epoch_ns();
            timer_master.set_schedule_table(slots, hyperperiod_us, epoch_ns);
            std::cout << "[main] TimerMaster table updated: " << slots.size()
                      << " slots, hyperperiod=" << hyperperiod_us << "us"
                      << std::endl;
        });

        node_client.connect();
        timer_master.start();

        // Daemon simply waits for shutdown signal
        while (!g_shutdown) {
            sleep(1);
        }

        std::cout << "\nShutting down..." << std::endl;

        timer_master.stop();
        fault_monitor.stop();
        node_client.disconnect();

    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
