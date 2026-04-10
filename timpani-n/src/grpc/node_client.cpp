// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#include "node_client.h"
#include <iostream>
#include <unistd.h>
#include <sys/utsname.h>

namespace timpani {
namespace node {

using namespace timpani::node::v1;

NodeClient::NodeClient(const std::string& server_address)
    : server_address_(server_address), running_(false), connected_(false) {
    char hostname_buf[256] = {};
    gethostname(hostname_buf, sizeof(hostname_buf) - 1);
    node_id_ = std::string(hostname_buf);
    std::cout << "[NodeClient] node_id: " << node_id_ << std::endl;
}

NodeClient::~NodeClient() {
    disconnect();
}

void NodeClient::set_table_callback(TableCallback cb) { table_callback_ = std::move(cb); }
void NodeClient::set_update_callback(UpdateCallback cb) { update_callback_ = std::move(cb); }
void NodeClient::set_shutdown_callback(ShutdownCallback cb) { shutdown_callback_ = std::move(cb); }

void NodeClient::connect() {
    running_ = true;
    channel_ = grpc::CreateChannel(server_address_, grpc::InsecureChannelCredentials());
    stub_ = OrchestratorService::NewStub(channel_);
    stream_thread_ = std::make_unique<std::thread>(&NodeClient::stream_thread_func, this);
}

void NodeClient::disconnect() {
    running_ = false;
    if (context_) {
        context_->TryCancel();
    }
    if (stream_thread_ && stream_thread_->joinable()) {
        stream_thread_->join();
    }
}

void NodeClient::stream_thread_func() {
    reconnect_loop();
}

void NodeClient::reconnect_loop() {
    int backoff = 1;
    while (running_) {
        context_ = std::make_unique<grpc::ClientContext>();
        stream_ = stub_->NodeStream(context_.get());
        connected_ = true;
        
        send_ready();
        
        ControlCommand command;
        while (running_ && stream_->Read(&command)) {
            backoff = 1; // Reset backoff on successful read
            if (command.has_full_table() && table_callback_) {
                table_callback_(command.full_table());
            } else if (command.has_update() && update_callback_) {
                update_callback_(command.update());
            } else if (command.has_shutdown() && shutdown_callback_) {
                shutdown_callback_(command.shutdown().grace_period_ms());
            }
        }
        connected_ = false;
        
        if (!running_) break;
        
        std::cerr << "[NodeClient] Connection lost. Reconnecting in " << backoff << "s..." << std::endl;
        sleep(backoff);
        backoff = std::min(backoff * 2, 30);
    }
}

void NodeClient::send_ready() {
    if (!connected_) return;
    
    NodeEvent event;
    event.set_node_id(node_id_);
    auto* ready = event.mutable_ready();

    ready->set_cpu_count(std::thread::hardware_concurrency());
    ready->set_memory_mb(4096); // Dummy
    
    // Kernel info
    struct utsname uts;
    uname(&uts);
    ready->set_kernel_version(uts.release);
    ready->set_preempt_rt(false); // Dummy for now
    ready->set_sched_ext(false);
    
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (stream_) {
        stream_->Write(event);
    }
}

void NodeClient::send_status() {
    if (!connected_) return;
    NodeEvent event;
    event.set_node_id(node_id_);
    event.mutable_status()->set_active_workloads(1);
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (stream_) stream_->Write(event);
}

void NodeClient::send_fault(const FaultInfo& fault) {
    if (!connected_) return;
    NodeEvent event;
    event.set_node_id(node_id_);
    *event.mutable_fault() = fault;
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (stream_) stream_->Write(event);
}

void NodeClient::send_table_applied(const std::string& table_id, bool success, const std::string& error) {
    if (!connected_) return;
    NodeEvent event;
    event.set_node_id(node_id_);
    auto* applied = event.mutable_applied();
    applied->set_table_id(table_id);
    applied->set_success(success);
    applied->set_error(error);
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (stream_) stream_->Write(event);
}

} // namespace node
} // namespace timpani
