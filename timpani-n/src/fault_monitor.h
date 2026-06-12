// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#pragma once

#include "bpf/maps.h"
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

namespace timpani {
namespace node {

class FaultMonitor {
public:
    using FaultCallback = std::function<void(const FaultEvent&, uint32_t dmiss_count)>;

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
    std::map<std::pair<uint64_t, uint64_t>, uint32_t> task_dmiss_counts_;
    std::mutex dmiss_mutex_;
};

} // namespace node
} // namespace timpani
