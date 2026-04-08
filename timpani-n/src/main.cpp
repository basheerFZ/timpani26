// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#include "timer_master.h"
#include "bpf_loader.h"
#include "fault_monitor.h"
#include "task_registry.h"
#include "grpc/node_client.h"

#include <iostream>
#include <memory>
#include <csignal>
#include <unistd.h>

volatile sig_atomic_t g_shutdown = 0;

void signal_handler(int /* signum */) {
    g_shutdown = 1;
}

int main(int argc, char** argv) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::cout << "Starting TIMPANI Node Executor (timpani-n C++ rework)..." << std::endl;

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

        // Initialize NodeClient (gRPC)
        timpani::node::NodeClient node_client("127.0.0.1:50060");

        // Connect callbacks
        node_client.set_table_callback([&bpf_loader](const auto& table) {
            // Dummy callback for hot update
            std::cout << "[main] Received table: " << table.table_id() << std::endl;
        });

        node_client.set_shutdown_callback([](uint32_t grace_period_ms) {
            std::cout << "[main] Received shutdown command: " << grace_period_ms << " ms" << std::endl;
            // Handle shutdown
        });

        fault_monitor.set_callback([&node_client](const auto& event) {
            timpani::node::v1::FaultInfo fault;
            fault.set_workload_id_hash(event.workload_id_hash);
            fault.set_task_id_hash(event.task_id_hash);
            fault.set_fault_type(static_cast<timpani::node::v1::FaultType>(event.fault_type));
            node_client.send_fault(fault);
        });

        fault_monitor.start();
        node_client.connect();
        node_client.send_ready();

        // 5. Initialize Timer Master (RT Priority Thread) last
        timpani::node::TimerMaster timer_master;
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
