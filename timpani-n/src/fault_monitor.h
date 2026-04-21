// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#pragma once

#include "bpf/maps.h"
#include <functional>
#include <thread>

namespace timpani {
namespace node {

class FaultMonitor {
public:
    using FaultCallback = std::function<void(const FaultEvent&)>;

    FaultMonitor();
    ~FaultMonitor();

    void set_ringbuf_fd(int fd);
    void set_callback(FaultCallback cb);
    void start();
    void stop();

private:
    void poll_loop();
    static int ring_buf_callback(void* ctx, void* data, size_t len);

    FaultCallback callback_;
    bool running_;
    int ringbuf_fd_;
    std::thread poll_thread_;
};

} // namespace node
} // namespace timpani
