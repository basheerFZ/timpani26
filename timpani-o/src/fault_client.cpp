/*
 * SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
 * SPDX-License-Identifier: MIT
 */

#include <iostream>

#include "tlog.h"
#include "fault_client.h"

FaultServiceClient& FaultServiceClient::GetInstance()
{
    static FaultServiceClient instance;
    return instance;

}

bool FaultServiceClient::Initialize(const std::string& server_address)
{
    if (initialized_) {
        TLOG_WARN("FaultServiceClient already initialized");
        return true;
    }

    if (server_address.empty()) {
        TLOG_ERROR("Server address cannot be empty");
        return false;
    }

    if (!CreateChannel(server_address)) {
        TLOG_ERROR("Failed to create gRPC channel to Pullpiri");
        return false;
    }

    initialized_ = true;
    return true;
}

bool FaultServiceClient::IsInitialized() const
{
    return initialized_;
}

bool FaultServiceClient::NotifyFault(const std::string &workload_id,
                                     const std::string &node_id,
                                     const std::string &task_name,
                                     FaultType fault_type,
                                     uint32_t cumulative_dmiss)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_) {
        TLOG_ERROR("FaultServiceClient not initialized");
        return false;
    }

    FaultInfo request;
    request.set_workload_id(workload_id);
    request.set_node_id(node_id);
    request.set_task_name(task_name);
    request.set_type(fault_type);
    request.set_cumulative_dmiss(cumulative_dmiss);

    if (!FlushRetryQueueLocked()) {
        TLOG_WARN("Retry queue flush failed. New fault will be queued on send failure.");
    }

    if (!SendFaultRpc(request)) {
        EnqueueRetryLocked(request);
        TLOG_WARN("Fault queued for retry. queue_size=", retry_queue_.size());
        return false;
    }

    return true;
}

bool FaultServiceClient::SendFaultRpc(const FaultInfo& request)
{
    Response reply;
    ClientContext context;

    TLOG_INFO("Notifying Pullpiri - Workload: ", request.workload_id(),
              ", Node: ", request.node_id(), ", Task: ", request.task_name(),
              ", Fault Type: ", FaultTypeToStr(request.type()));

    Status status = stub_->NotifyFault(&context, request, &reply);

    if (!status.ok()) {
        TLOG_ERROR("NotifyFault failed: ", status.error_code(), ": ",
                   status.error_message());
        return false;
    }

    if (reply.status() != 0) {
        TLOG_ERROR("NotifyFault: Pullpiri returned error: ", reply.status());
        return false;
    }

    return true;
}

bool FaultServiceClient::FlushRetryQueueLocked()
{
    while (!retry_queue_.empty()) {
        const FaultInfo& pending = retry_queue_.front();
        if (!SendFaultRpc(pending)) {
            return false;
        }
        retry_queue_.pop_front();
    }

    return true;
}

void FaultServiceClient::EnqueueRetryLocked(const FaultInfo& request)
{
    if (retry_queue_.size() >= kMaxRetryQueue) {
        retry_queue_.pop_front();
        TLOG_WARN("Retry queue full. Dropping oldest fault event.");
    }

    retry_queue_.push_back(request);
}

FaultServiceClient::FaultServiceClient()
    : initialized_(false)
{
}

FaultServiceClient::~FaultServiceClient()
{
    if (channel_) {
        // gRPC channel and stub will be cleaned up automatically
    }
}

bool FaultServiceClient::CreateChannel(const std::string& server_address)
{
    try {
        // Create gRPC channel with insecure credentials for now
        // TODO: Consider using secure credentials in production
        channel_ = grpc::CreateChannel(server_address,
                                       grpc::InsecureChannelCredentials());
        if (!channel_) {
            TLOG_ERROR("grpc::CreateChannel failed");
            return false;
        }

        // Create the stub
        stub_ = FaultService::NewStub(channel_);
        if (!stub_) {
            TLOG_ERROR("FaultService::NewStub failed");
            return false;
        }

        return true;
    }
    catch (const std::exception& e) {
        TLOG_ERROR("Exception while creating gRPC channel: ", e.what());
        return false;
    }
}

const char* FaultServiceClient::FaultTypeToStr(FaultType type)
{
    switch (type) {
        case FaultType::UNKNOWN:
            return "UNKNOWN";
        case FaultType::DMISS:
            return "DMISS";
        default:
            return "INVALID";
    }
}
