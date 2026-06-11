// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#pragma once

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "node_control.grpc.pb.h"

namespace timpani {
namespace node {

class NodeClient
{
  public:
    using TableCallback = std::function<void(
        const timpani::node::v1::HierarchicalScheduleTable&)>;
    using UpdateCallback =
        std::function<void(const timpani::node::v1::ScheduleTableUpdate&)>;
    using ShutdownCallback = std::function<void(uint32_t grace_period_ms)>;
    using RecoveryCallback = std::function<void(const timpani::node::v1::RecoverySignal&)>;

    explicit NodeClient(const std::string& server_address,
              const std::string& node_id_override = "");
    ~NodeClient();

    void set_table_callback(TableCallback cb);
    void set_update_callback(UpdateCallback cb);
    void set_shutdown_callback(ShutdownCallback cb);
    void set_recovery_callback(RecoveryCallback cb);

    void connect();
    void disconnect();

    void send_ready();
    void send_status();
    void send_fault(const timpani::node::v1::FaultInfo& fault);
    void send_table_applied(const std::string& table_id, bool success,
                            const std::string& error = "");

  private:
    void stream_thread_func();
    void reconnect_loop();

    std::string server_address_;
    std::string node_id_;  // hostname — matches Pullpiri convention
    std::shared_ptr<grpc::Channel> channel_;
    std::unique_ptr<timpani::node::v1::OrchestratorService::Stub> stub_;
    std::unique_ptr<grpc::ClientContext> context_;
    std::unique_ptr<grpc::ClientReaderWriter<timpani::node::v1::NodeEvent,
                                             timpani::node::v1::ControlCommand>>
        stream_;

    std::unique_ptr<std::thread> stream_thread_;
    std::atomic<bool> running_;
    std::atomic<bool> connected_;

    TableCallback table_callback_;
    UpdateCallback update_callback_;
    ShutdownCallback shutdown_callback_;
    RecoveryCallback recovery_callback_;

    std::mutex write_mutex_;
};

}  // namespace node
}  // namespace timpani
