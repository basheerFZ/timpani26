/*
 * SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
 * SPDX-License-Identifier: MIT
 */

#include <getopt.h>
#include <unistd.h>  // gethostname

#include <chrono>
#include <iostream>
#include <memory>
#include <algorithm>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>

#include "fault_client.h"
#include "node_config.h"
#include "orchestrator_service.h"
#include "schedinfo_service.h"
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
                std::string& node_config_file)
{
    const char* short_opts = "hs:f:p:c:";
    const struct option long_opts[] = {
        {"help", no_argument, nullptr, 'h'},
        {"sinfoport", required_argument, nullptr, 's'},
        {"faulthost", required_argument, nullptr, 'f'},
        {"faultport", required_argument, nullptr, 'p'},
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
    std::string node_config_file;  // Node configuration file path

    if (!GetOptions(argc, argv, sinfo_port, fault_addr, fault_port,
                    node_config_file)) {
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

    // Replay scheduler state when:
    // 1) sched_map changes, or
    // 2) nodes connect/reconnect after schedule generation.
    bool replay_pending = false;
    bool warned_no_nodes_for_replay = false;
    std::set<std::string> known_connected_nodes;
    std::set<std::string> synced_nodes;
    std::unordered_map<std::string, int> replay_failures;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point>
        next_retry_time;
    const auto kRetryBaseDelay = std::chrono::milliseconds(500);
    const auto kRetryMaxDelay = std::chrono::seconds(8);
    constexpr int kRetryBackoffMaxShift = 4;

    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        // Check if SchedInfo has been updated and queue replay as needed.
        bool changed = false;
        ScheduleTableMap sched_tables = sinfo_server->GetScheduleTables(&changed);

        auto connected_nodes_vec = g_orchestrator_service->get_connected_node_ids();
        std::set<std::string> connected_nodes(connected_nodes_vec.begin(),
                                              connected_nodes_vec.end());

        // Remove sync marks for disconnected nodes.
        for (const auto& node_id : known_connected_nodes) {
            if (connected_nodes.find(node_id) == connected_nodes.end()) {
                synced_nodes.erase(node_id);
                replay_failures.erase(node_id);
                next_retry_time.erase(node_id);
            }
        }

        // New or reconnected nodes should receive the latest schedule snapshot.
        for (const auto& node_id : connected_nodes) {
            if (known_connected_nodes.find(node_id) == known_connected_nodes.end()) {
                replay_pending = true;
                synced_nodes.erase(node_id);
                replay_failures.erase(node_id);
                next_retry_time.erase(node_id);
                TLOG_INFO("Node connected/reconnected - schedule replay queued for '",
                          node_id, "'");
            }
        }
        known_connected_nodes = connected_nodes;

        if (changed) {
            replay_pending = true;
            synced_nodes.clear();
            replay_failures.clear();
            next_retry_time.clear();
            if (sched_tables.empty()) {
                TLOG_WARN("SchedInfo marked changed but map is empty");
            } else {
                TLOG_INFO("SchedInfo changed - replay queued for all connected nodes");
            }
            sinfo_server->DumpSchedInfo();
        }

        if (replay_pending && !sched_tables.empty()) {
            if (connected_nodes.empty()) {
                if (!warned_no_nodes_for_replay) {
                    TLOG_WARN("SchedInfo available but no nodes connected yet");
                    warned_no_nodes_for_replay = true;
                }
            } else {
                warned_no_nodes_for_replay = false;

                bool all_push_ok = true;
                auto now = std::chrono::steady_clock::now();
                for (const auto& node_id : connected_nodes) {
                    if (synced_nodes.find(node_id) != synced_nodes.end()) {
                        continue;
                    }

                    auto retry_it = next_retry_time.find(node_id);
                    if (retry_it != next_retry_time.end() && now < retry_it->second) {
                        continue;
                    }

                    // Look up the combined table for this node
                    auto it = sched_tables.find(node_id);
                    if (it == sched_tables.end()) {
                        TLOG_DEBUG("No schedule table for node '", node_id, "' — skipping");
                        synced_nodes.insert(node_id);
                        continue;
                    }

                    TLOG_INFO("Replaying schedule table for node '", node_id, "'");
                    bool ok = g_orchestrator_service->push_full_table(node_id, it->second);
                    TLOG_INFO("push_full_table(\"", node_id, "\") => ",
                              ok ? "OK" : "FAILED");

                    if (ok) {
                        synced_nodes.insert(node_id);
                        replay_failures.erase(node_id);
                        next_retry_time.erase(node_id);
                    } else {
                        all_push_ok = false;
                        synced_nodes.erase(node_id);

                        int fail_count = ++replay_failures[node_id];
                        int backoff_shift =
                            std::min(fail_count - 1, kRetryBackoffMaxShift);
                        auto retry_delay =
                            kRetryBaseDelay * (1 << backoff_shift);
                        if (retry_delay > kRetryMaxDelay) {
                            retry_delay = kRetryMaxDelay;
                        }

                        next_retry_time[node_id] = now + retry_delay;
                        TLOG_WARN("push_full_table(\"", node_id,
                                  "\") retry scheduled in ",
                                  retry_delay.count(), " ms (attempt ",
                                  fail_count, ")");
                    }
                }

                if (all_push_ok) {
                    bool all_synced = true;
                    for (const auto& node_id : connected_nodes) {
                        if (synced_nodes.find(node_id) == synced_nodes.end()) {
                            all_synced = false;
                            break;
                        }
                    }

                    if (all_synced) {
                        replay_pending = false;
                        TLOG_INFO(
                            "Schedule replay completed for all connected nodes");
                    }
                }
            }
        }
    }

    return EXIT_SUCCESS;
}
