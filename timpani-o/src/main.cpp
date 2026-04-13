/*
 * SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
 * SPDX-License-Identifier: MIT
 */

#include <getopt.h>
#include <unistd.h>  // gethostname

#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "fault_client.h"
#include "node_config.h"
#include "orchestrator_service.h"
#include "schedinfo_service.h"
#include "table_builder.h"
#include "tlog.h"

bool RunSchedInfoServer(int port, std::unique_ptr<SchedInfoServer>& server,
                        std::shared_ptr<NodeConfigManager> node_config_manager)
{
    server = std::make_unique<SchedInfoServer>(node_config_manager);
    if (!server->Start(port)) {
        TLOG_ERROR("Failed to start SchedInfoServer on port ", port);
        return false;
    }
    TLOG_INFO("SchedInfoServer listening on port ", port);
    return true;
}

bool InitFaultClient(const std::string& addr, int port)
{
    std::string piccolo_addr = addr + ":" + std::to_string(port);

    FaultServiceClient& client = FaultServiceClient::GetInstance();
    return client.Initialize(piccolo_addr);
}

bool NotifyFaultDemo()
{
    FaultServiceClient& client = FaultServiceClient::GetInstance();

    return client.NotifyFault("workload_demo", "node_demo", "task_demo",
                              FaultType::DMISS);
}

std::unique_ptr<grpc::Server> g_orchestrator_server;
std::unique_ptr<timpani::orchestrator::OrchestratorServiceImpl>
    g_orchestrator_service;

bool RunOrchestratorServer(int port)
{
    std::string server_address("0.0.0.0:" + std::to_string(port));
    g_orchestrator_service =
        std::make_unique<timpani::orchestrator::OrchestratorServiceImpl>();

    grpc::ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(g_orchestrator_service.get());

    g_orchestrator_server = builder.BuildAndStart();
    if (!g_orchestrator_server) {
        TLOG_ERROR("Failed to start OrchestratorServer on port ", port);
        return false;
    }
    TLOG_INFO("OrchestratorServer listening on port ", port);
    return true;
}

bool GetOptions(int argc, char* argv[], int& sinfo_port,
                std::string& fault_addr, int& fault_port,
                bool& notify_fault, std::string& node_config_file)
{
    const char* short_opts = "hs:f:p:nc:";
    const struct option long_opts[] = {
        {"help", no_argument, nullptr, 'h'},
        {"sinfoport", required_argument, nullptr, 's'},
        {"faulthost", required_argument, nullptr, 'f'},
        {"faultport", required_argument, nullptr, 'p'},
        {"notifyfault", no_argument, nullptr, 'n'},
        {"node-config", required_argument, nullptr, 'c'},
        {nullptr, 0, nullptr, 0}};
    int opt;

    while ((opt = getopt_long(argc, argv, short_opts, long_opts, nullptr)) >=
           0) {
        switch (opt) {
            case 's':
                sinfo_port = std::stoi(optarg);
                break;
            case 'f':
                fault_addr = optarg;
                break;
            case 'p':
                fault_port = std::stoi(optarg);
                break;
            case 'n':
                // FIXME: NotifyFault option for testing
                notify_fault = true;
                break;
            case 'c':
                node_config_file = optarg;
                break;
            case 'h':
            default:
                std::cerr
                    << "Usage: " << argv[0] << " [options] [host]\n"
                    << "Options:\n"
                    << "  -s <port>\t\tPort for SchedInfoService (default: "
                       "50052)\n"
                    << "  -f <address>\t\tFaultService host address (default: "
                       "localhost)\n"
                    << "  -p <port>\t\tPort for FaultService (default: 50053)\n"
                    << "  -n\t\t\tEnable NotifyFault demo (default: false)\n"
                    << "  -c, --node-config <file>\tNode configuration YAML "
                       "file\n"
                    << "  -h\t\t\tShow this help message\n";
                std::cerr
                    << "Example: " << argv[0]
                    << " -s 50052 -f localhost -p 50053 --node-config "
                       "examples/node_configurations.yaml\n";
                return false;
        }
    }

    return true;
}

int main(int argc, char** argv)
{
    int sinfo_port = 50052;
    std::string fault_addr = "localhost";
    int fault_port = 50053;
    bool notify_fault = false;     // Flag for NotifyFault method demo
    std::string node_config_file;  // Node configuration file path

    if (!GetOptions(argc, argv, sinfo_port, fault_addr, fault_port,
                    notify_fault, node_config_file)) {
        exit(EXIT_FAILURE);
    }

    // Initialize the logger
    TLOG_SET_LOG_LEVEL(LogLevel::DEBUG);
    TLOG_SET_PRINT_FILENAME(false);
    TLOG_SET_FULL_TIMESTAMP(false);

    // Load node configuration if provided
    std::shared_ptr<NodeConfigManager> node_config_manager =
        std::make_shared<NodeConfigManager>();

    if (!node_config_file.empty()) {
        if (!node_config_manager->LoadFromFile(node_config_file)) {
            TLOG_ERROR(
                "Failed to load node configuration, using default settings");
        }
    } else {
        TLOG_INFO(
            "No node configuration file provided, using default node settings");
    }

    // Run the gRPC SchedInfoService server (with internal scheduling and node
    // config)
    std::unique_ptr<SchedInfoServer> sinfo_server;
    if (!RunSchedInfoServer(sinfo_port, sinfo_server, node_config_manager)) {
        return EXIT_FAILURE;
    }

    // Initialize the gRPC FaultServiceClient
    if (!InitFaultClient(fault_addr, fault_port)) {
        return EXIT_FAILURE;
    }

    // Run the OrchestratorServer
    int orchestrator_port = 50060;
    if (!RunOrchestratorServer(orchestrator_port)) {
        return EXIT_FAILURE;
    }

    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        // Check if SchedInfo has been updated and push schedule table to nodes
        bool changed = false;
        SchedInfoMap sched_map = sinfo_server->GetSchedInfoMap(&changed);
        if (changed && !sched_map.empty()) {
            char hostname_buf[256] = {};
            gethostname(hostname_buf, sizeof(hostname_buf) - 1);
            std::string node_id(hostname_buf);
            TLOG_INFO("SchedInfo changed — building schedule table for node '",
                      node_id, "'");
            auto table =
                timpani::orchestrator::BuildScheduleTable(node_id, sched_map);
            bool ok = g_orchestrator_service->push_full_table(node_id, table);
            TLOG_INFO("push_full_table(\"", node_id, "\") => ",
                      ok ? "OK" : "FAILED (no node connected yet)");
        }

        if (notify_fault) {
            if (NotifyFaultDemo()) {
                notify_fault = false;  // Reset the flag after notification
            }
        }
    }

    return EXIT_SUCCESS;
}
