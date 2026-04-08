// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#pragma once

#include "bpf/maps.h"
#include <functional>

namespace timpani {
namespace node {

class FaultMonitor {
public:
    using FaultCallback = std::function<void(const FaultEvent&)>;

    FaultMonitor();
    ~FaultMonitor();

    void set_callback(FaultCallback cb);
    void start();
    void stop();

private:
    void poll_loop();

    FaultCallback callback_;
    bool running_;
};

} // namespace node
} // namespace timpani
