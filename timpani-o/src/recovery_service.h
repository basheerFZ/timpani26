/*
 * SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
 * SPDX-License-Identifier: MIT
 */

#ifndef RECOVERY_SERVICE_H
#define RECOVERY_SERVICE_H

#include <grpcpp/grpcpp.h>
#include <memory>
#include <string>

#include "proto/schedinfo.grpc.pb.h"
#include "schedinfo_service.h"

using namespace grpc;
using namespace schedinfo::v1;

/**
 * @brief Implementation of the RecoveryService gRPC service
 *
 * This service handles recovery policy deliveries from Pullpiri to Timpani-O.
 */
class RecoveryServiceImpl final : public RecoveryService::Service
{
  public:
    explicit RecoveryServiceImpl(SchedInfoServer* schedinfo_server);
    ~RecoveryServiceImpl() = default;

    Status EnforceRecoveryPolicy(ServerContext* context,
                                 const RecoveryCommand* request,
                                 Response* reply) override;

  private:
    SchedInfoServer* schedinfo_server_;
};

#endif  // RECOVERY_SERVICE_H
