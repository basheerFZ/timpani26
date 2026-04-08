// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#include "node_client.h"

namespace timpani {
namespace node {

NodeClient::NodeClient(const std::string& server_address)
    : server_address_(server_address) {
}

NodeClient::~NodeClient() {
    disconnect();
}

void NodeClient::connect() {
}

void NodeClient::disconnect() {
}

void NodeClient::send_ready() {
}

void NodeClient::send_fault() {
}

} // namespace node
} // namespace timpani
