// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>

#include <sys/types.h>

#include "trace_bpf.h"

struct ring_buffer;
struct schedstat_bpf;

namespace timpani {
namespace node {

class SchedstatMonitor {
public:
    using EventCallback = std::function<void(const schedstat_event&)>;

    SchedstatMonitor();
    ~SchedstatMonitor();

    bool start(EventCallback callback);
    void stop();

    bool add_pid(pid_t pid);
    bool remove_pid(pid_t pid);

    bool is_active() const;

private:
    static int ring_buffer_callback(void* ctx, void* data, size_t len);
    void poll_loop();

    EventCallback callback_;
    std::atomic<bool> running_;
    std::thread poll_thread_;
    int pid_filter_map_fd_;
    std::mutex mutex_;

#ifdef CONFIG_TRACE_BPF_EVENT
    struct schedstat_bpf* skel_;
    struct ring_buffer* ring_buffer_;
#endif
};

}  // namespace node
}  // namespace timpani
