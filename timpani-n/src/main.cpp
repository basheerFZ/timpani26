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
            std::cerr << "Failed to load BPF programs (skeleton ready)." << std::endl;
        }

        // 2. Initialize Task Registry (cgroup/thread tracker)
        timpani::node::TaskRegistry task_registry;
        task_registry.scan_cgroups();

        // 3. Initialize Fault Monitor
        timpani::node::FaultMonitor fault_monitor;
        fault_monitor.set_callback([](const FaultEvent& event) {
            std::cerr << "Fault detected! Type: " << static_cast<int>(event.fault_type) << std::endl;
        });
        fault_monitor.start();

        // 4. Initialize gRPC Node Client
        timpani::node::NodeClient node_client("localhost:50051");
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
