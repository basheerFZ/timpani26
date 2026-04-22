// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#include <dirent.h>
#include <errno.h>
#include <getopt.h>
#include <sched.h>
#include <unistd.h>

#include <cctype>
#include <csignal>
#include <cstdlib>
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
    std::cerr
        << "Usage: " << prog << " [options] [orchestrator_host]\n"
        << "Options:\n"
        << "  -p <port>   Orchestrator gRPC port (default: 50060)\n"
        << "  -n <name>   Node ID override (default: hostname)\n"
        << "  -l <level>  (compat) log level flag accepted but handled elsewhere\n"
        << "  -P <prio>   (compat) RT priority flag accepted but handled elsewhere\n"
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
                            &gpdata_pid_map_mutex](const schedstat_event&
                                                       event) {
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
        timpani::node::NodeClient node_client(
            orchestrator_endpoint, runtime_options.node_id_override);

        // Connect callbacks
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
        node_client.set_table_callback([&timer_master, &runtime_options
    #ifdef CONFIG_TRACE_BPF_EVENT
                        , &schedstat_monitor,
                        &gpdata_pid_to_task,
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
#ifdef CONFIG_TRACE_BPF_EVENT
                            if (runtime_options.enable_plot &&
                                schedstat_monitor.is_active()) {
                                if (schedstat_monitor.add_pid(pid)) {
                                    std::lock_guard<std::mutex> lock(
                                        gpdata_pid_map_mutex);
                                    gpdata_pid_to_task[pid] = task_id;
                                } else {
                                    std::cerr
                                        << "[main] Failed to register PID for gpdata: "
                                        << pid << std::endl;
                                }
                            }
#endif

                            // We do NOT apply SCHED_FIFO here. BPF scheduler will handle it.

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
