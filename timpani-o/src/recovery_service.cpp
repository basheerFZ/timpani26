/*
 * SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
 * SPDX-License-Identifier: MIT
 */

#include "recovery_service.h"
#include "orchestrator_service.h"
#include "tlog.h"

extern std::unique_ptr<timpani::orchestrator::OrchestratorServiceImpl> g_orchestrator_service;

RecoveryServiceImpl::RecoveryServiceImpl(SchedInfoServer* schedinfo_server)
    : schedinfo_server_(schedinfo_server)
{
}

Status RecoveryServiceImpl::EnforceRecoveryPolicy(ServerContext* context,
                                                  const RecoveryCommand* request,
                                                  Response* reply)
{
    TLOG_INFO("Received RecoveryCommand: workload '", request->workload_id(),
              "' policy: ", schedinfo::v1::RecoveryPolicy_Name(request->recovery_policy()));

    if (request->recovery_policy() == schedinfo::v1::RecoveryPolicy::RECOVERY_STOP) {
        TLOG_INFO("RecoveryPolicy is STOP for workload '", request->workload_id(), "'. Removing workload and Broadcasting RecoverySignal.");

        std::string actual_wid = request->workload_id();
        if (schedinfo_server_) {
            if (!schedinfo_server_->RemoveWorkload(request->workload_id(), &actual_wid, false)) {
                TLOG_WARN("Failed to remove workload '", request->workload_id(), "' from schedule.");
            }
        } else {
            TLOG_ERROR("SchedInfoServer is not initialized. Cannot remove workload.");
        }

        bool broadcast_ok = false;
        if (g_orchestrator_service) {
            broadcast_ok = g_orchestrator_service->broadcast_recovery_signal(
                actual_wid,
                timpani::node::v1::RecoverySignal::ACTION_STOP);
        } else {
            TLOG_ERROR("OrchestratorService is not initialized. Cannot broadcast STOP RecoverySignal.");
        }

        if (!broadcast_ok) {
            TLOG_WARN("Broadcast RecoverySignal(ACTION_STOP) reported failure for workload '",
                      actual_wid, "'.");
            reply->set_status(-1);
            return Status::OK;
        }
    }

    reply->set_status(0);
    return Status::OK;
}
