// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#include "orchestrator_service.h"
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

} // namespace orchestrator
} // namespace timpani
