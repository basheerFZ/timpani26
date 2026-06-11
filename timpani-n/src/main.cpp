// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#include <dirent.h>
#include <errno.h>
#include <getopt.h>
#include <sched.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <cctype>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "bpf_loader.h"
#include "fault_monitor.h"
#ifdef CONFIG_TRACE_BPF_EVENT
#include "gpdata_writer.h"
#endif
#include "grpc/node_client.h"
#ifdef CONFIG_TRACE_BPF_EVENT
#include "schedstat_monitor.h"
#endif
#include "task_registry.h"
#include "timer_master.h"
#include "version.h"

volatile sig_atomic_t g_shutdown = 0;

void signal_handler(int /* signum */) { g_shutdown = 1; }

#ifndef SCHED_EXT
#define SCHED_EXT 7
#endif

/* Minimal sched_attr layout for sched_setattr(2) */
struct sched_attr {
    uint32_t size;
    uint32_t sched_policy;
    uint64_t sched_flags;
    int32_t  sched_nice;
    uint32_t sched_priority;
    uint64_t sched_runtime;
    uint64_t sched_deadline;
    uint64_t sched_period;
};

/**
 * apply_sched_ext() - Set SCHED_EXT policy on a task via pid.
 *
 * Called from table_callback after update_task_meta() so that
 * scx_timpani (SCX_OPS_SWITCH_PARTIAL) takes ownership of the task.
 * Must be called with scx_timpani already loaded.
 *
 * Required: timpani-n runs with CAP_SYS_NICE (setuid or systemd capability).
 */
static int apply_sched_ext(pid_t pid)
{
    struct sched_attr attr = {};
    attr.size        = sizeof(attr);
    attr.sched_policy = SCHED_EXT;
    if (syscall(SYS_sched_setattr, pid, &attr, 0) < 0) {
        int saved = errno;
        std::cerr << "[main] sched_setattr(SCHED_EXT) failed for pid=" << pid
                  << ": " << strerror(saved) << std::endl;
        return -1;
    }
    return 0;
}

namespace {
struct RuntimeOptions {
    std::string orchestrator_host = "127.0.0.1";
    int orchestrator_port = 50060;
    std::string node_id_override;
    bool enable_plot = false;
    bool show_help = false;
    bool show_version = false;
};

void print_usage(const char* prog)
{
    std::cerr << "Usage: " << prog << " [options] [orchestrator_host]\n"
              << "Options:\n"
              << "  -p <port>   Orchestrator gRPC port (default: 50060)\n"
              << "  -n <name>   Node ID override (default: hostname)\n"
              << "  -l <level>  (compat) log level flag accepted but handled "
                 "elsewhere\n"
              << "  -P <prio>   (compat) RT priority flag accepted but handled "
                 "elsewhere\n"
              << "  -g          Enable gpdata output (<node>.gpdata)\n"
              << "  -s          (compat) accepted\n"
              << "  -V          Show version information\n"
              << "  -h          Show this help\n";
}

bool parse_port(const char* text, int& port_out)
{
    if (!text || *text == '\0') return false;

    char* end = nullptr;
    long parsed = std::strtol(text, &end, 10);
    if (end == text || *end != '\0') return false;
    if (parsed < 1 || parsed > 65535) return false;

    port_out = static_cast<int>(parsed);
    return true;
}

bool parse_runtime_options(int argc, char** argv, RuntimeOptions& options)
{
    opterr = 0;
    int opt;
    while ((opt = getopt(argc, argv, "hVn:p:l:P:gs")) != -1) {
        switch (opt) {
            case 'h':
                options.show_help = true;
                return true;
            case 'V':
                options.show_version = true;
                return true;
            case 'n':
                options.node_id_override = optarg ? std::string(optarg) : "";
                break;
            case 'p':
                if (!parse_port(optarg, options.orchestrator_port)) {
                    std::cerr << "Invalid orchestrator port: "
                              << (optarg ? optarg : "<null>") << std::endl;
                    return false;
                }
                break;
            case 'l':
            case 'P':
                // Compatibility option: accepted for legacy launch scripts.
                break;
            case 'g':
                options.enable_plot = true;
                break;
            case 's':
                // Compatibility flag: accepted for legacy launch scripts.
                break;
            case '?':
            default:
                std::cerr << "Unknown option: -" << static_cast<char>(optopt)
                          << std::endl;
                return false;
        }
    }

    if (optind < argc) {
        options.orchestrator_host = argv[optind++];
    }

    if (optind < argc) {
        std::cerr << "Unexpected extra arguments." << std::endl;
        return false;
    }

    return true;
}

std::string resolve_node_id(const RuntimeOptions& options)
{
    if (!options.node_id_override.empty()) {
        return options.node_id_override;
    }

    char hostname[256] = {};
    if (gethostname(hostname, sizeof(hostname) - 1) == 0 &&
        hostname[0] != '\0') {
        return std::string(hostname);
    }

    return "node";
}

uint64_t monotonic_now_ns()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + ts.tv_nsec;
}
}  // namespace

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
    RuntimeOptions runtime_options;
    if (!parse_runtime_options(argc, argv, runtime_options)) {
        print_usage(argv[0]);
        return 2;
    }
    if (runtime_options.show_help) {
        print_usage(argv[0]);
        return 0;
    }
    if (runtime_options.show_version) {
        std::cout << "timpani-n version " << PROJECT_VERSION << std::endl;
        std::cout << "  Git commit: " << GIT_COMMIT_HASH << std::endl;
        std::cout << "  Build time: " << BUILD_TIMESTAMP << std::endl;
        return 0;
    }

    const std::string orchestrator_endpoint =
        runtime_options.orchestrator_host + ":" +
        std::to_string(runtime_options.orchestrator_port);

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::cout << "Starting TIMPANI Node Executor (timpani-n C++ rework)..."
              << std::endl;
    std::cout << "[main] Orchestrator endpoint: " << orchestrator_endpoint
              << std::endl;
    if (!runtime_options.node_id_override.empty()) {
        std::cout << "[main] Node ID override: "
                  << runtime_options.node_id_override << std::endl;
    }
    if (runtime_options.enable_plot) {
#ifdef CONFIG_TRACE_BPF_EVENT
        std::cout << "[main] gpdata output enabled (-g)" << std::endl;
#else
        std::cerr << "[main] -g was requested but this binary was built "
                     "without CONFIG_TRACE_BPF_EVENT=ON."
                  << std::endl;
#endif
    }

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

#ifdef CONFIG_TRACE_BPF_EVENT
        std::unique_ptr<timpani::node::GpdataWriter> gpdata_writer;
        timpani::node::SchedstatMonitor schedstat_monitor;
        std::map<pid_t, std::string> gpdata_pid_to_task;
        std::mutex gpdata_pid_map_mutex;

        if (runtime_options.enable_plot) {
            const std::string gpdata_node_id = resolve_node_id(runtime_options);
            gpdata_writer =
                std::make_unique<timpani::node::GpdataWriter>(gpdata_node_id);

            if (!gpdata_writer->start()) {
                std::cerr << "[main] Failed to start gpdata writer. "
                             "Continuing without gpdata output."
                          << std::endl;
                gpdata_writer.reset();
            } else if (!schedstat_monitor.start(
                           [&gpdata_writer, &gpdata_pid_to_task,
                            &gpdata_pid_map_mutex](
                               const schedstat_event& event) {
                               if (!gpdata_writer) {
                                   return;
                               }

                               std::string task_name;
                               {
                                   std::lock_guard<std::mutex> lock(
                                       gpdata_pid_map_mutex);
                                   auto it = gpdata_pid_to_task.find(
                                       static_cast<pid_t>(event.pid));
                                   if (it != gpdata_pid_to_task.end()) {
                                       task_name = it->second;
                                   }
                               }

                               if (task_name.empty()) {
                                   return;
                               }

                               gpdata_writer->write_event(event, task_name);
                           })) {
                std::cerr << "[main] Failed to start schedstat monitor. "
                             "Continuing without gpdata output."
                          << std::endl;
                gpdata_writer->stop();
                gpdata_writer.reset();
            } else {
                std::cout << "[main] gpdata path: " << gpdata_node_id
                          << ".gpdata" << std::endl;
            }
        }
#endif

        // Initialize NodeClient (gRPC)
        timpani::node::NodeClient node_client(orchestrator_endpoint,
                                              runtime_options.node_id_override);

        // Connect callbacks
        node_client.set_shutdown_callback([](uint32_t grace_period_ms) {
            std::cout << "[main] Received shutdown command: " << grace_period_ms
                      << " ms" << std::endl;
            // Handle shutdown
        });

/*
        node_client.set_recovery_callback([&timer_master](const auto& recovery_signal) {
            std::cout << "[main] Received recovery signal for workload: " << recovery_signal.workload_id()
                      << " action: " << recovery_signal.action() << std::endl;
            if (recovery_signal.action() == timpani::node::v1::RecoverySignal::ACTION_STOP) {
                std::cout << "[main] Stopping workload " << recovery_signal.workload_id() << std::endl;
                // Currently timer_master might not have an interface to clear a specific workload easily, 
                // but we can log the action as requested.
                // Depending on the BPF structures we could clear the TT maps.
            }
        });
*/

        fault_monitor.set_callback([&node_client](const auto& event, uint32_t current_dmiss) {
            timpani::node::v1::FaultInfo fault;
            fault.set_workload_id_hash(event.workload_id_hash);
            fault.set_task_id_hash(event.task_id_hash);
            if (event.fault_type == FAULT_DMISS) {
                fault.set_fault_type(timpani::node::v1::FaultType::DMISS);
            } else {
                fault.set_fault_type(static_cast<timpani::node::v1::FaultType>(event.fault_type));
            }
            fault.set_dmiss_count(current_dmiss);
            node_client.send_fault(fault);
        });

        fault_monitor.start();

        // 5. Initialize Timer Master (RT Priority Thread) last
        timpani::node::TimerMaster timer_master(bpf_loader);

        // Wire table_callback: received HierarchicalScheduleTable → TimerMaster
        // slots (Re-set callback now that timer_master is in scope)
        node_client.set_table_callback([&timer_master, &bpf_loader,
                        &node_client,
                                        &runtime_options
#ifdef CONFIG_TRACE_BPF_EVENT
                                        ,
                                        &schedstat_monitor, &gpdata_pid_to_task,
                                        &gpdata_pid_map_mutex
#endif
        ](const auto& table) {
            std::cout << "[main] Received table: " << table.table_id()
                      << " hyperperiod=" << table.hyperperiod_us() << "us"
                      << " partitions=" << table.partitions_size() << std::endl;

#ifdef CONFIG_TRACE_BPF_EVENT
            if (runtime_options.enable_plot && schedstat_monitor.is_active()) {
                std::vector<pid_t> stale_pids;
                {
                    std::lock_guard<std::mutex> lock(gpdata_pid_map_mutex);
                    for (const auto& entry : gpdata_pid_to_task) {
                        stale_pids.push_back(entry.first);
                    }
                    gpdata_pid_to_task.clear();
                }

                for (pid_t stale_pid : stale_pids) {
                    schedstat_monitor.remove_pid(stale_pid);
                }
            }
#endif

            // Build SlotEntry list from TtSlots
            std::vector<timpani::node::TimerMaster::SlotEntry> slots;
            uint32_t slot_idx = 0;
            bool apply_success = true;
            std::string apply_error;

            auto record_apply_error = [&](const std::string& error) {
                apply_success = false;
                if (!apply_error.empty()) {
                    apply_error += "; ";
                }
                apply_error += error;
            };

            for (const auto& partition : table.partitions()) {
                uint32_t partition_cpu = 0;
                bool has_partition_cpu = partition.has_cpuset() &&
                                         partition.cpuset().cpus_size() > 0;
                if (has_partition_cpu) {
                    partition_cpu = partition.cpuset().cpus(0);
                }

                for (const auto& layer : partition.layers()) {
                    for (const auto& tt_slot : layer.tt_slots()) {
                        // Apply SCHED_FIFO + affinity to matching process
                        const std::string& task_id = tt_slot.task_id();
                        pid_t pid = find_pid_by_comm(task_id);
                        if (pid < 0) {
                            std::cerr
                                << "[main] Task not found in /proc: " << task_id
                                << std::endl;
                            record_apply_error("TT task not found: " + task_id);
                        } else {
#ifdef CONFIG_TRACE_BPF_EVENT
                            if (runtime_options.enable_plot &&
                                schedstat_monitor.is_active()) {
                                if (schedstat_monitor.add_pid(pid)) {
                                    std::lock_guard<std::mutex> lock(
                                        gpdata_pid_map_mutex);
                                    gpdata_pid_to_task[pid] = task_id;
                                } else {
                                    std::cerr << "[main] Failed to register "
                                                 "PID for gpdata: "
                                              << pid << std::endl;
                                }
                            }
#endif

                            // Apply CPU affinity to matching process (BPF
                            // handles scheduling) Register task in BPF
                            // task_meta_map so scx_timpani can route it to the
                            // correct DSQ.
                            TaskMeta meta = {};
                            meta.workload_id_hash = tt_slot.workload_id_hash();
                            meta.task_id_hash = tt_slot.task_id_hash();
                            meta.scheduling_type = SCHED_TYPE_TT;
                            meta.layer = layer.layer_index();
                            meta.assigned_cpu =
                                static_cast<uint32_t>(tt_slot.cpu());
                            meta.slot_duration_us = tt_slot.duration_us();
                            meta.activation_ns = 0;
                            meta.cgroup_id =
                                0;  // TODO: resolve from cgroup path
                            if (bpf_loader.update_task_meta(
                                    static_cast<uint32_t>(pid), meta)) {
                                std::cout << "[main] Registered task in BPF: "
                                          << task_id << " pid=" << pid
                                          << " type=TT hash=0x" << std::hex
                                          << meta.task_id_hash << std::dec
                                          << std::endl;
                                /* Ensure task is managed by scx_timpani.
                                 * On timpani-n restart scx unload demotes
                                 * SCHED_EXT tasks to SCHED_OTHER; re-apply
                                 * here so select_cpu()/enqueue() fire. */
                                if (apply_sched_ext(pid) == 0) {
                                    std::cout << "[main] Applied SCHED_EXT to "
                                              << task_id << " pid=" << pid
                                              << std::endl;
                                }
                            } else {
                                std::cerr
                                    << "[main] Failed to register task in "
                                       "BPF task_meta_map: "
                                    << task_id << " pid=" << pid << std::endl;
                                record_apply_error("TT task_meta update failed: " +
                                                   task_id);
                            }

                            /* sched_setaffinity is NOT used with scx_timpani:
                             * CPU placement is enforced by BPF select_cpu() and
                             * per-CPU DSQ dispatch. Calling setaffinity on an
                             * isolated CPU partition returns EINVAL. */
                        }

                        // Add TimerMaster slot entry
                        timpani::node::TimerMaster::SlotEntry entry;
                        entry.cpu = tt_slot.cpu();
                        entry.slot_idx = slot_idx;
                        entry.offset_ns =
                            static_cast<uint64_t>(tt_slot.offset_us()) *
                            1000ULL;
                        entry.duration_ns =
                            static_cast<uint64_t>(tt_slot.duration_us()) *
                            1000ULL;
                        entry.task_id_hash = tt_slot.task_id_hash();
                        entry.task_name =
                            tt_slot.task_id(); /* comm name, e.g. "task_1" */
                        slots.push_back(entry);

                        // Populate BPF tt_table_map so dispatch() can consume
                        // from DSQ_TT_WAIT
                        TtSlotKey tt_key = {.cpu = tt_slot.cpu(),
                                            .slot_idx = slot_idx};
                        TtSlotBpf slot_bpf = {};
                        slot_bpf.workload_id_hash = tt_slot.workload_id_hash();
                        slot_bpf.task_id_hash = tt_slot.task_id_hash();
                        slot_bpf.offset_us = tt_slot.offset_us();
                        slot_bpf.duration_us = tt_slot.duration_us();
                        slot_bpf.deadline_us = tt_slot.deadline_us();
                        slot_bpf.cpu = tt_slot.cpu();
                        if (!bpf_loader.update_tt_slot(tt_key, slot_bpf)) {
                            record_apply_error("TT slot update failed: " +
                                               tt_slot.task_id());
                        }

                        slot_idx++;
                    }

                    if (!has_partition_cpu && layer.cbs_entries_size() > 0) {
                        record_apply_error("CBS partition has empty cpuset: " +
                                           partition.partition_id());
                    }

                    for (const auto& cbs_entry : layer.cbs_entries()) {
                        const std::string& task_id = cbs_entry.task_id();
                        pid_t pid = find_pid_by_comm(task_id);
                        if (pid < 0) {
                            std::cerr << "[main] CBS task not found in /proc: "
                                      << task_id << std::endl;
                            record_apply_error("CBS task not found: " + task_id);
                            continue;
                        }

#ifdef CONFIG_TRACE_BPF_EVENT
                        if (runtime_options.enable_plot &&
                            schedstat_monitor.is_active()) {
                            if (schedstat_monitor.add_pid(pid)) {
                                std::lock_guard<std::mutex> lock(
                                    gpdata_pid_map_mutex);
                                gpdata_pid_to_task[pid] = task_id;
                            } else {
                                std::cerr << "[main] Failed to register "
                                             "CBS PID for gpdata: "
                                          << pid << std::endl;
                            }
                        }
#endif

                        TaskMeta meta = {};
                        meta.workload_id_hash = cbs_entry.workload_id_hash();
                        meta.task_id_hash = cbs_entry.task_id_hash();
                        meta.scheduling_type = SCHED_TYPE_CBS;
                        meta.layer = layer.layer_index();
                        meta.assigned_cpu = partition_cpu;
                        meta.slot_duration_us = 0;
                        meta.activation_ns = 0;
                        meta.cgroup_id = 0;

                        if (!bpf_loader.update_task_meta(
                                static_cast<uint32_t>(pid), meta)) {
                            std::cerr << "[main] Failed to register CBS task "
                                         "in BPF task_meta_map: "
                                      << task_id << " pid=" << pid << std::endl;
                            record_apply_error("CBS task_meta update failed: " +
                                               task_id);
                            continue;
                        }

                        if (apply_sched_ext(pid) == 0) {
                            std::cout << "[main] Applied SCHED_EXT to CBS "
                                      << task_id << " pid=" << pid << std::endl;
                        }

                        CbsState cbs_state = {};
                        cbs_state.task_id_hash = cbs_entry.task_id_hash();
                        cbs_state.budget_us = cbs_entry.budget_us();
                        cbs_state.period_us = cbs_entry.period_us();
                        cbs_state.remaining_us = cbs_entry.budget_us();
                        cbs_state.exec_start_ns = 0;
                        cbs_state.replenish_at_ns =
                            cbs_entry.period_us() > 0
                                ? monotonic_now_ns() +
                                      static_cast<uint64_t>(
                                          cbs_entry.period_us()) *
                                          1000ULL
                                : 0;
                        cbs_state.deadline_us = cbs_entry.deadline_us();

                        uint64_t cbs_key = cbs_entry.workload_id_hash() ^
                                           cbs_entry.task_id_hash();
                        if (!bpf_loader.update_cbs_state(cbs_key, cbs_state)) {
                            std::cerr << "[main] Failed to update CBS state: "
                                      << task_id << std::endl;
                            record_apply_error("CBS state update failed: " +
                                               task_id);
                            continue;
                        }

                        std::cout << "[main] Registered CBS task in BPF: "
                                  << task_id << " pid=" << pid << " cpu="
                                  << partition_cpu << " budget="
                                  << cbs_state.budget_us << "us period="
                                  << cbs_state.period_us << "us hash=0x"
                                  << std::hex << meta.task_id_hash << std::dec
                                  << std::endl;
                    }
                }
            }

            uint64_t hyperperiod_us = table.hyperperiod_us();
            uint64_t epoch_ns = table.epoch_ns();
            timer_master.set_schedule_table(slots, hyperperiod_us, epoch_ns);
            std::cout << "[main] TimerMaster table updated: " << slots.size()
                      << " slots, hyperperiod=" << hyperperiod_us << "us"
                      << std::endl;
            node_client.send_table_applied(table.table_id(), apply_success,
                                           apply_error);
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
#ifdef CONFIG_TRACE_BPF_EVENT
        schedstat_monitor.stop();
        if (gpdata_writer) {
            gpdata_writer->stop();
        }
#endif
        node_client.disconnect();

    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
