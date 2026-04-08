// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#pragma once

#include <grpcpp/grpcpp.h>
#include "proto/node_control.grpc.pb.h"
#include <map>
#include <mutex>
#include <string>

namespace timpani {
namespace orchestrator {

using namespace timpani::node::v1;

class OrchestratorServiceImpl final : public OrchestratorService::Service {
public:
    OrchestratorServiceImpl();
    ~OrchestratorServiceImpl() override;

    grpc::Status NodeStream(
        grpc::ServerContext* context,
        grpc::ServerReaderWriter<ControlCommand, NodeEvent>* stream) override;

    bool push_full_table(const std::string& node_id, const HierarchicalScheduleTable& table);
    bool push_update(const std::string& node_id, const ScheduleTableUpdate& update);

private:
    struct ConnectedNode {
        std::string node_id;
        grpc::ServerReaderWriter<ControlCommand, NodeEvent>* stream;
    };

    std::mutex nodes_mutex_;
    std::map<std::string, ConnectedNode*> connected_nodes_;

    void handle_node_ready(const NodeReady& ready, ConnectedNode& node);
    void handle_node_status(const NodeStatus& status, ConnectedNode& node);
    void handle_fault_info(const FaultInfo& fault, ConnectedNode& node);
    void handle_table_applied(const TableApplied& applied, ConnectedNode& node);
};

} // namespace orchestrator
} // namespace timpani
