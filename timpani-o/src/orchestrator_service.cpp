// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#include "orchestrator_service.h"
#include "fault_client.h"
#include <iostream>

namespace timpani {
namespace orchestrator {

OrchestratorServiceImpl::OrchestratorServiceImpl() {}

OrchestratorServiceImpl::~OrchestratorServiceImpl() {}

grpc::Status OrchestratorServiceImpl::NodeStream(
    grpc::ServerContext* context,
    grpc::ServerReaderWriter<ControlCommand, NodeEvent>* stream) {
    
    auto current_node = std::make_shared<ConnectedNode>();
    current_node->stream = stream;

    NodeEvent event;
    while (stream->Read(&event)) {
        if (current_node->node_id.empty() && !event.node_id().empty()) {
            current_node->node_id = event.node_id();
            std::lock_guard<std::mutex> lock(nodes_mutex_);
            connected_nodes_[current_node->node_id] = current_node;
            std::cout << "[Orchestrator] Node connected: " << current_node->node_id << std::endl;
        }

        if (event.has_ready()) {
            handle_node_ready(event.ready(), *current_node);
        } else if (event.has_status()) {
            handle_node_status(event.status(), *current_node);
        } else if (event.has_fault()) {
            handle_fault_info(event.fault(), *current_node);
        } else if (event.has_applied()) {
            handle_table_applied(event.applied(), *current_node);
        }
    }

    if (!current_node->node_id.empty()) {
        std::lock_guard<std::mutex> lock(nodes_mutex_);
        connected_nodes_.erase(current_node->node_id);
        std::cout << "[Orchestrator] Node disconnected: " << current_node->node_id << std::endl;
    }

    return grpc::Status::OK;
}

void OrchestratorServiceImpl::handle_node_ready(const NodeReady& ready, ConnectedNode& node) {
    std::cout << "[Orchestrator] NodeReady from " << node.node_id 
              << ": CPUs=" << ready.cpu_count() 
              << " RAM=" << ready.memory_mb() << "MB" << std::endl;
}

void OrchestratorServiceImpl::handle_node_status(const NodeStatus& status, ConnectedNode& node) {
    // Log or forward stats
}

void OrchestratorServiceImpl::handle_fault_info(const FaultInfo& fault, ConnectedNode& node) {
    std::cerr << "[Orchestrator] Fault from " << node.node_id 
              << " task=" << fault.task_id() << " type=" << fault.fault_type() << std::endl;

    std::string workload_id = fault.workload_id();
    if (workload_id.empty()) {
        workload_id = std::to_string(fault.workload_id_hash());
    }
    
    std::string task_id = fault.task_id();
    if (task_id.empty()) {
        task_id = std::to_string(fault.task_id_hash());
    }
    
    schedinfo::v1::FaultType proto_fault_type;
    switch(fault.fault_type()) {
        case timpani::node::v1::FaultType::DMISS:
            proto_fault_type = schedinfo::v1::FaultType::DMISS;
            break;
        default:
            proto_fault_type = schedinfo::v1::FaultType::UNKNOWN;
            break;
    }

    bool forwarded = FaultServiceClient::GetInstance().NotifyFault(
        workload_id,
        node.node_id,
        task_id,
        proto_fault_type,
        fault.dmiss_count()
    );

    if (!forwarded) {
        std::cerr << "[Orchestrator] Fault forwarding failed (queued for retry if enabled): "
                  << "workload=" << workload_id << " task=" << task_id
                  << std::endl;
    }
}

void OrchestratorServiceImpl::handle_table_applied(const TableApplied& applied, ConnectedNode& node) {
    std::cout << "[Orchestrator] TableApplied from " << node.node_id 
              << " table=" << applied.table_id() << " success=" << applied.success() << std::endl;
}

bool OrchestratorServiceImpl::push_full_table(const std::string& node_id, const HierarchicalScheduleTable& table) {
    std::shared_ptr<ConnectedNode> target_node;
    {
        std::lock_guard<std::mutex> lock(nodes_mutex_);
        auto it = connected_nodes_.find(node_id);
        if (it != connected_nodes_.end()) {
            target_node = it->second;
        }
    }
    
    if (target_node) {
        ControlCommand cmd;
        *cmd.mutable_full_table() = table;
        std::lock_guard<std::mutex> stream_lock(target_node->write_mutex);
        return target_node->stream->Write(cmd);
    }
    return false;
}

bool OrchestratorServiceImpl::push_update(const std::string& node_id, const ScheduleTableUpdate& update) {
    std::shared_ptr<ConnectedNode> target_node;
    {
        std::lock_guard<std::mutex> lock(nodes_mutex_);
        auto it = connected_nodes_.find(node_id);
        if (it != connected_nodes_.end()) {
            target_node = it->second;
        }
    }
    
    if (target_node) {
        ControlCommand cmd;
        *cmd.mutable_update() = update;
        std::lock_guard<std::mutex> stream_lock(target_node->write_mutex);
        return target_node->stream->Write(cmd);
    }
    return false;
}

std::vector<std::string> OrchestratorServiceImpl::get_connected_node_ids() {
    std::lock_guard<std::mutex> lock(nodes_mutex_);
    std::vector<std::string> ids;
    ids.reserve(connected_nodes_.size());
    for (const auto& pair : connected_nodes_) {
        ids.push_back(pair.first);
    }
    return ids;
}

bool OrchestratorServiceImpl::broadcast_recovery_signal(const std::string& workload_id, RecoverySignal::Action action) {
    std::lock_guard<std::mutex> lock(nodes_mutex_);
    bool all_success = true;
    for (auto& pair : connected_nodes_) {
        ControlCommand cmd;
        RecoverySignal* rec_sig = cmd.mutable_recovery();
        rec_sig->set_workload_id(workload_id);
        rec_sig->set_action(action);
        
        auto& target_node = pair.second;
        std::lock_guard<std::mutex> stream_lock(target_node->write_mutex);
        if (!target_node->stream->Write(cmd)) {
            all_success = false;
        }
    }
    return all_success;
}

} // namespace orchestrator
} // namespace timpani
