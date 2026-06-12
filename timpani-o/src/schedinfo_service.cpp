/*
 * SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
 * SPDX-License-Identifier: MIT
 */

#include <cstring>
#include <iomanip>

#include "tlog.h"
#include "schedinfo_service.h"
#include "orchestrator_service.h"

extern std::unique_ptr<timpani::orchestrator::OrchestratorServiceImpl> g_orchestrator_service;

// ---------------------------------------------------------------------------
// SchedInfoServiceImpl
// ---------------------------------------------------------------------------

SchedInfoServiceImpl::SchedInfoServiceImpl(
    std::shared_ptr<NodeConfigManager> node_config_manager)
    : node_config_manager_(node_config_manager),
      schedule_changed_(false)
{
    TLOG_INFO("SchedInfoServiceImpl created with GlobalScheduler (DDR-007)");

    global_scheduler_ =
        std::make_shared<GlobalScheduler>(node_config_manager_);

    if (node_config_manager_ && node_config_manager_->IsLoaded()) {
        TLOG_INFO("Node configuration loaded with ",
                  node_config_manager_->GetAllNodes().size(), " nodes");
    } else {
        TLOG_INFO("Using default node configuration");
    }
}

Status SchedInfoServiceImpl::AddSchedInfo(ServerContext* context,
                                          const SchedInfo* request,
                                          Response* reply)
{
    TLOG_INFO("Received SchedInfo: ", request->workload_id(), " with ",
              request->tasks_size(), " tasks");

    // Print detailed task information
    for (int i = 0; i < request->tasks_size(); i++) {
        const auto& task = request->tasks(i);
        TLOG_DEBUG("Task ", i, ": ", task.name());
        TLOG_DEBUG("  Priority: ", task.priority());
        TLOG_DEBUG("  Policy: ", task.policy());
        TLOG_DEBUG("  CPU Affinity: 0x", std::setfill('0'), std::setw(16),
                   std::hex, task.cpu_affinity(), std::dec);
        TLOG_DEBUG("  Period: ", task.period());
        TLOG_DEBUG("  Runtime: ", task.runtime());
        TLOG_DEBUG("  Deadline: ", task.deadline());
        TLOG_DEBUG("  Release Time: ", task.release_time());
        TLOG_DEBUG("  Max Deadline Misses: ", task.max_dmiss());
        TLOG_DEBUG("  Node ID: ", task.node_id());
    }

    // Schedule table generation logic remains below...

    // ── Step 1: Classify workload by TemporalClass (DDR-007 §3.2) ──
    Mechanism mechanism;
    switch (request->temporal_class()) {
        case TemporalClass::TEMPORAL_PERIODIC:
            mechanism = Mechanism::TT;
            TLOG_INFO("Workload '", request->workload_id(),
                      "' classified as L1 (TT — Time-Triggered)");
            break;
        case TemporalClass::TEMPORAL_SPORADIC:
            mechanism = Mechanism::CBS;
            TLOG_INFO("Workload '", request->workload_id(),
                      "' classified as L2 (CBS — Constant Bandwidth Server)");
            break;
        default:
            TLOG_ERROR("Unknown temporal_class for workload '",
                       request->workload_id(), "'");
            reply->set_status(-1);
            return Status::OK;
    }

    // Convert gRPC TaskInfo → ClassifiedTask
    std::vector<ClassifiedTask> classified =
        ConvertToClassifiedTasks(request, mechanism);

    if (classified.empty()) {
        TLOG_ERROR("No valid tasks for workload: ", request->workload_id());
        reply->set_status(-1);
        return Status::OK;
    }

    // Determine target nodes from tasks
    std::set<std::string> target_nodes;
    for (const auto& task : request->tasks()) {
        if (!task.node_id().empty()) {
            target_nodes.insert(task.node_id());
        }
    }

    if (target_nodes.empty()) {
        // If no node specified, use all configured nodes
        if (node_config_manager_ && node_config_manager_->IsLoaded()) {
            for (const auto& [nid, _] : node_config_manager_->GetAllNodes()) {
                target_nodes.insert(nid);
            }
        }
        if (target_nodes.empty()) {
            target_nodes.insert("default");
        }
    }

    std::unique_lock<std::shared_mutex> lock(schedule_mutex_);

    // Store/replace classified tasks for this workload
    WorkloadEntry entry;
    entry.target_nodes = target_nodes;
    entry.tasks = std::move(classified);
    workload_tasks_[request->workload_id()] = std::move(entry);

    // Regenerate schedule tables for ALL workloads combined per node
    std::string error_detail;
    if (!RegenerateAllSchedules(error_detail)) {
        // Roll back: remove the workload that caused the failure
        workload_tasks_.erase(request->workload_id());
        // Attempt regeneration without the failed workload
        RegenerateAllSchedules(error_detail);

        TLOG_ERROR("Scheduling infeasible after adding workload '",
                   request->workload_id(), "': ", error_detail);
        reply->set_status(-1);
        return Status::OK;
    }

    schedule_changed_ = true;

    TLOG_INFO("Successfully scheduled workload '", request->workload_id(),
              "' (", workload_tasks_.size(), " total workload(s), ",
              schedule_tables_.size(), " node(s))");

    reply->set_status(0);
    return Status::OK;
}

bool SchedInfoServiceImpl::RegenerateAllSchedules(std::string& error_detail)
{
    // Collect all target nodes across all workloads
    std::set<std::string> all_nodes;
    for (const auto& [wl_id, entry] : workload_tasks_) {
        all_nodes.insert(entry.target_nodes.begin(), entry.target_nodes.end());
    }

    ScheduleTableMap new_tables;

    for (const auto& node_id : all_nodes) {
        // Gather ALL classified tasks destined for this node
        std::vector<ClassifiedTask> all_tasks;
        for (const auto& [wl_id, entry] : workload_tasks_) {
            if (entry.target_nodes.count(node_id)) {
                all_tasks.insert(all_tasks.end(),
                                 entry.tasks.begin(), entry.tasks.end());
            }
        }

        if (all_tasks.empty()) continue;

        auto result = global_scheduler_->generate_schedule(node_id, all_tasks);

        if (std::holds_alternative<InfeasibleError>(result)) {
            const auto& err = std::get<InfeasibleError>(result);
            error_detail = "node '" + node_id + "': " + err.details;
            return false;
        }

        new_tables[node_id] =
            std::move(std::get<timpani::node::v1::HierarchicalScheduleTable>(result));
    }

    schedule_tables_ = std::move(new_tables);
    return true;
}

std::vector<ClassifiedTask> SchedInfoServiceImpl::ConvertToClassifiedTasks(
    const SchedInfo* request, Mechanism mechanism)
{
    std::vector<ClassifiedTask> tasks;
    tasks.reserve(request->tasks_size());

    for (int i = 0; i < request->tasks_size(); i++) {
        const auto& grpc_task = request->tasks(i);

        ClassifiedTask ct;
        ct.workload_id = request->workload_id();
        ct.task_id     = grpc_task.name();
        ct.mechanism   = mechanism;
        ct.period_us   = static_cast<uint32_t>(grpc_task.period());
        ct.wcet_us     = static_cast<uint32_t>(grpc_task.runtime());
        ct.deadline_us = static_cast<uint32_t>(grpc_task.deadline());
        ct.assigned_cpu = -1;

        // Use period as deadline if deadline is not set
        if (ct.deadline_us == 0 && ct.period_us > 0) {
            ct.deadline_us = ct.period_us;
        }

        tasks.push_back(ct);
    }

    return tasks;
}

ScheduleTableMap SchedInfoServiceImpl::GetScheduleTables(bool* changed)
{
    std::shared_lock<std::shared_mutex> lock(schedule_mutex_);
    if (changed) {
        *changed = schedule_changed_;
        schedule_changed_ = false;
    }
    return schedule_tables_;
}

int SchedInfoServiceImpl::SchedPolicyToInt(SchedPolicy policy)
{
    switch (policy) {
        case SchedPolicy::NORMAL: return 0;
        case SchedPolicy::FIFO:   return 1;
        case SchedPolicy::RR:     return 2;
        default:                  return -1;
    }
}

// ---------------------------------------------------------------------------
// SchedInfoServer
// ---------------------------------------------------------------------------

SchedInfoServer::SchedInfoServer(std::shared_ptr<NodeConfigManager> node_config_manager)
    : service_(node_config_manager), server_(nullptr), server_thread_(nullptr)
{
    TLOG_INFO("SchedInfoServer created with node configuration");
}

SchedInfoServer::~SchedInfoServer() { Stop(); }

bool SchedInfoServer::Start(int port)
{
    std::string server_addr = "0.0.0.0:" + std::to_string(port);

    ServerBuilder builder;
    builder.AddListeningPort(server_addr, grpc::InsecureServerCredentials());
    builder.AddChannelArgument(GRPC_ARG_ALLOW_REUSEPORT, 0);
    builder.RegisterService(&service_);

    server_ = builder.BuildAndStart();
    if (!server_) {
        TLOG_ERROR("Failed to start SchedInfoService on ", server_addr);
        return false;
    }

    server_thread_ =
        std::make_unique<std::thread>([this]() { server_->Wait(); });
    return true;
}

void SchedInfoServer::Stop()
{
    if (server_) {
        server_->Shutdown();
    }
    if (server_thread_ && server_thread_->joinable()) {
        server_thread_->join();
    }
}

ScheduleTableMap SchedInfoServer::GetScheduleTables(bool* changed)
{
    return service_.GetScheduleTables(changed);
}

void SchedInfoServer::DumpSchedInfo()
{
    auto tables = service_.GetScheduleTables();

    if (tables.empty()) {
        TLOG_INFO("No schedule tables available");
        return;
    }

    TLOG_INFO("Dumping ScheduleTableMap:");
    for (const auto& [node_id, table] : tables) {
        TLOG_INFO("Node: ", node_id,
                  " partitions=", table.partitions_size(),
                  " hyperperiod=", table.hyperperiod_us(), "us");
        for (int p = 0; p < table.partitions_size(); ++p) {
            const auto& part = table.partitions(p);
            TLOG_DEBUG("  Partition: ", part.partition_id(),
                        " cpuset=", "[ ", [&part] {
                            std::string cpus;
                            for (int c = 0; c < part.cpuset().cpus_size(); ++c) {
                                cpus += std::to_string(part.cpuset().cpus(c)) + " ";
                            }
                            return cpus.empty() ? "none" : cpus;
                        }(), "]");
            for (int l = 0; l < part.layers_size(); ++l) {
                const auto& layer = part.layers(l);
                TLOG_DEBUG("    Layer ", l, ": model=", layer.model());
                TLOG_DEBUG("    TT slots: ", layer.tt_slots_size(),
                           ", CBS entries: ", layer.cbs_entries_size());
                for (int s = 0; s < layer.tt_slots_size(); ++s) {
                    const auto& slot = layer.tt_slots(s);
                    TLOG_DEBUG("      TT Slot: task_id=", slot.task_id(),
                               ", offset=", slot.offset_us(), "us",
                               ", duration=", slot.duration_us(), "us",
                               ", deadline=", slot.deadline_us(), "us",
                               ", CPU=", slot.cpu());
                }
                for (int c = 0; c < layer.cbs_entries_size(); ++c) {
                    const auto& cbs = layer.cbs_entries(c);
                    TLOG_DEBUG("      CBS Entry: task_id=", cbs.task_id(),
                               ", budget=", cbs.budget_us(), "us",
                               ", period=", cbs.period_us(), "us",
                               ", deadline=", cbs.deadline_us(), "us");
                }
            }
        }
    }
}

Status SchedInfoServiceImpl::EnforceRecoveryPolicy(ServerContext* context,
                                                   const schedinfo::v1::RecoveryCommand* request,
                                                   Response* reply)
{
    TLOG_INFO("Received RecoveryCommand: workload '", request->workload_id(),
              "' policy: ", schedinfo::v1::RecoveryPolicy_Name(request->recovery_policy()));

    if (request->recovery_policy() == schedinfo::v1::RecoveryPolicy::RECOVERY_STOP) {
        TLOG_INFO("RecoveryPolicy is STOP for workload '", request->workload_id(), "'. Broadcasting RecoverySignal.");

        {
            std::unique_lock<std::shared_mutex> lock(schedule_mutex_);
            auto existing_it = workload_tasks_.find(request->workload_id());
            if (existing_it != workload_tasks_.end()) {
                workload_tasks_.erase(existing_it);
                
                std::string error_detail;
                if (!RegenerateAllSchedules(error_detail)) {
                    TLOG_ERROR("Failed to regenerate schedules after removing workload '", request->workload_id(), "': ", error_detail);
                } else {
                    schedule_changed_ = true;
                    TLOG_INFO("Removed workload '", request->workload_id(), "' from local schedule state due to STOP policy.");
                }
            } else {
                TLOG_WARN("STOP policy received for unknown workload '", request->workload_id(), "'.");
            }
        }

        bool broadcast_ok = false;
        if (g_orchestrator_service) {
            broadcast_ok = g_orchestrator_service->broadcast_recovery_signal(
                request->workload_id(),
                timpani::node::v1::RecoverySignal::ACTION_STOP);
        } else {
            TLOG_ERROR("OrchestratorService is not initialized. Cannot broadcast STOP RecoverySignal.");
        }

        if (!broadcast_ok) {
            TLOG_WARN("Broadcast RecoverySignal(ACTION_STOP) reported failure for workload '",
                      request->workload_id(), "'.");
            reply->set_status(-1);
            return Status::OK;
        }
    }

    reply->set_status(0);
    return Status::OK;
}
