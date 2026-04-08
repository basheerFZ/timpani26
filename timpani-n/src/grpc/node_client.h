// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <memory>
// #include <grpcpp/grpcpp.h> // Will be included when proto is generated

namespace timpani {
namespace node {

class NodeClient {
public:
    NodeClient(const std::string& server_address);
    ~NodeClient();

    void connect();
    void disconnect();

    // Sends NodeReady event including CPU topology
    void send_ready();

    // Sends FaultInfo event
    void send_fault();

private:
    std::string server_address_;
    // std::unique_ptr<OrchestratorService::Stub> stub_;
};

} // namespace node
} // namespace timpani
