// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#include "schedstat_monitor.h"

#include <cerrno>
#include <cstdint>
#include <iostream>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>

#ifdef CONFIG_TRACE_BPF_EVENT
#include "schedstat.skel.h"
#endif

namespace {

constexpr int kRingbufPollTimeoutMs = 100;

}  // namespace

namespace timpani {
namespace node {

SchedstatMonitor::SchedstatMonitor()
    : running_(false),
      pid_filter_map_fd_(-1)
#ifdef CONFIG_TRACE_BPF_EVENT
      ,
      skel_(nullptr),
      ring_buffer_(nullptr)
#endif
{
}

SchedstatMonitor::~SchedstatMonitor()
{
    stop();
}

bool SchedstatMonitor::start(EventCallback callback)
{
#ifdef CONFIG_TRACE_BPF_EVENT
    std::lock_guard<std::mutex> lock(mutex_);

    if (running_.load()) {
        return true;
    }

    callback_ = std::move(callback);
    if (!callback_) {
        std::cerr << "[gpdata] Schedstat callback is not set" << std::endl;
        return false;
    }

    skel_ = schedstat_bpf__open();
    if (!skel_) {
        std::cerr << "[gpdata] Failed to open schedstat BPF skeleton"
                  << std::endl;
        return false;
    }

    int ret = schedstat_bpf__load(skel_);
    if (ret < 0) {
        std::cerr << "[gpdata] Failed to load schedstat BPF skeleton: " << ret
                  << std::endl;
        schedstat_bpf__destroy(skel_);
        skel_ = nullptr;
        return false;
    }

    ring_buffer_ = ring_buffer__new(bpf_map__fd(skel_->maps.buffer),
                                    ring_buffer_callback, this, nullptr);
    if (!ring_buffer_) {
        std::cerr << "[gpdata] Failed to create schedstat ring buffer"
                  << std::endl;
        schedstat_bpf__destroy(skel_);
        skel_ = nullptr;
        return false;
    }

    ret = schedstat_bpf__attach(skel_);
    if (ret < 0) {
        std::cerr << "[gpdata] Failed to attach schedstat BPF skeleton: "
                  << ret << std::endl;
        ring_buffer__free(ring_buffer_);
        ring_buffer_ = nullptr;
        schedstat_bpf__destroy(skel_);
        skel_ = nullptr;
        return false;
    }

    pid_filter_map_fd_ = bpf_map__fd(skel_->maps.pid_filter_map);
    if (pid_filter_map_fd_ < 0) {
        std::cerr << "[gpdata] Failed to get pid_filter_map fd" << std::endl;
        ring_buffer__free(ring_buffer_);
        ring_buffer_ = nullptr;
        schedstat_bpf__destroy(skel_);
        skel_ = nullptr;
        return false;
    }

    running_.store(true);
    poll_thread_ = std::thread(&SchedstatMonitor::poll_loop, this);
    return true;
#else
    (void)callback;
    std::cerr << "[gpdata] CONFIG_TRACE_BPF_EVENT is disabled at build time"
              << std::endl;
    return false;
#endif
}

void SchedstatMonitor::stop()
{
    running_.store(false);

    if (poll_thread_.joinable()) {
        poll_thread_.join();
    }

#ifdef CONFIG_TRACE_BPF_EVENT
    std::lock_guard<std::mutex> lock(mutex_);

    callback_ = nullptr;
    pid_filter_map_fd_ = -1;

    if (ring_buffer_) {
        ring_buffer__free(ring_buffer_);
        ring_buffer_ = nullptr;
    }

    if (skel_) {
        schedstat_bpf__destroy(skel_);
        skel_ = nullptr;
    }
#else
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = nullptr;
    pid_filter_map_fd_ = -1;
#endif
}

bool SchedstatMonitor::add_pid(pid_t pid)
{
#ifdef CONFIG_TRACE_BPF_EVENT
    if (pid <= 0 || !is_active()) {
        return false;
    }

    uint8_t value = 1;
    return bpf_map_update_elem(pid_filter_map_fd_, &pid, &value, BPF_ANY) == 0;
#else
    (void)pid;
    return false;
#endif
}

bool SchedstatMonitor::remove_pid(pid_t pid)
{
#ifdef CONFIG_TRACE_BPF_EVENT
    if (pid <= 0 || pid_filter_map_fd_ < 0) {
        return false;
    }

    if (bpf_map_delete_elem(pid_filter_map_fd_, &pid) == 0) {
        return true;
    }

    return errno == ENOENT;
#else
    (void)pid;
    return false;
#endif
}

bool SchedstatMonitor::is_active() const
{
    return running_.load() && pid_filter_map_fd_ >= 0;
}

int SchedstatMonitor::ring_buffer_callback(void* ctx, void* data, size_t len)
{
    if (!ctx || !data || len < sizeof(schedstat_event)) {
        return 0;
    }

    auto* self = static_cast<SchedstatMonitor*>(ctx);
    const auto* event = static_cast<const schedstat_event*>(data);

    EventCallback callback;
    {
        std::lock_guard<std::mutex> lock(self->mutex_);
        callback = self->callback_;
    }

    if (callback) {
        callback(*event);
    }

    return 0;
}

void SchedstatMonitor::poll_loop()
{
#ifdef CONFIG_TRACE_BPF_EVENT
    while (running_.load()) {
        const int ret = ring_buffer__poll(ring_buffer_, kRingbufPollTimeoutMs);
        if (ret < 0 && ret != -EINTR) {
            std::cerr << "[gpdata] ring_buffer__poll failed: " << ret
                      << std::endl;
            break;
        }
    }
#endif
}

}  // namespace node
}  // namespace timpani
