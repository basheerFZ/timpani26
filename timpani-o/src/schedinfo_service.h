/*
 * SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
 * SPDX-License-Identifier: MIT
 */

#ifndef SCHEDINFO_SERVICE_H
#define SCHEDINFO_SERVICE_H

#include <map>
#include <memory>
#include <set>
#include <shared_mutex>
#include <thread>
#include <grpcpp/grpcpp.h>

#include "proto/schedinfo.grpc.pb.h"
#include "node_config.h"
#include "global_scheduler.h"

using namespace grpc;
using namespace schedinfo::v1;

/**
* @brief Implementation of the SchedInfoService gRPC service
*
* This service handles scheduling information deliveries from Pullpiri to Timpani-O.
* Uses the DDR-007 GlobalScheduler to generate TT+CBS schedule tables.
*/
class SchedInfoServiceImpl final : public SchedInfoService::Service
{
  public:
    explicit SchedInfoServiceImpl(std::shared_ptr<NodeConfigManager> node_config_manager = nullptr);
    ~SchedInfoServiceImpl() = default;

    Status AddSchedInfo(ServerContext* context, const SchedInfo* request,
                        Response* reply) override;

    grpc::Status EnforceRecoveryPolicy(grpc::ServerContext* context,
                                       const schedinfo::v1::RecoveryCommand* request,
                                       schedinfo::v1::Response* reply) override;

    /**
     * @brief Get all schedule tables (workload_id → node_id → table).
     * @param changed  If non-null, set to true if tables changed since last call.
     * @return Current schedule table map.
     */
    ScheduleTableMap GetScheduleTables(bool* changed = nullptr);

  private:
    static int SchedPolicyToInt(SchedPolicy policy);

    // Convert gRPC TaskInfo → ClassifiedTask using TemporalClass
    std::vector<ClassifiedTask> ConvertToClassifiedTasks(
        const SchedInfo* request, Mechanism mechanism);

    // Regenerate schedule tables for all workloads on every target node
    bool RegenerateAllSchedules(std::string& error_detail);

    // Per-workload classified tasks: workload_id → (target_nodes, tasks)
    struct WorkloadEntry {
        std::set<std::string> target_nodes;
        std::vector<ClassifiedTask> tasks;
    };
    std::map<std::string, WorkloadEntry> workload_tasks_;

    // Schedule tables: node_id → combined HierarchicalScheduleTable (all workloads)
    ScheduleTableMap schedule_tables_;
    mutable std::shared_mutex schedule_mutex_;
    bool schedule_changed_;

    std::shared_ptr<NodeConfigManager> node_config_manager_;
    std::shared_ptr<GlobalScheduler> global_scheduler_;
};

/**
 * @brief SchedInfoServer class for managing the SchedInfoService gRPC server
 */
class SchedInfoServer
{
  public:
    explicit SchedInfoServer(std::shared_ptr<NodeConfigManager> node_config_manager = nullptr);
    ~SchedInfoServer();
    bool Start(int port);
    void Stop();

    ScheduleTableMap GetScheduleTables(bool* changed = nullptr);
    void DumpSchedInfo();

 private:
    SchedInfoServiceImpl service_;
    std::unique_ptr<Server> server_;
    std::unique_ptr<std::thread> server_thread_;
};

#endif  // SCHEDINFO_SERVICE_H
