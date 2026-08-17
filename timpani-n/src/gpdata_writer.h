// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

#include "trace_bpf.h"

namespace timpani {
namespace node {

class GpdataWriter {
public:
    explicit GpdataWriter(const std::string& node_id);
    ~GpdataWriter();

    bool start();
    void stop();
    void write_event(const schedstat_event& event, const std::string& task_name);

private:
    static constexpr uint64_t kNsPerUs = 1000ULL;

    bool ensure_file_opened_locked();
    bool calibrate_ktime_offset_locked();
    uint64_t to_realtime_ns(uint64_t monotonic_ns) const;
    static uint64_t ns_to_us_round_up(uint64_t ns);

    std::string node_id_;
    FILE* file_;
    uint64_t ktime_offset_ns_;
    bool started_;
    std::mutex mutex_;
};

} // namespace node
} // namespace timpani
