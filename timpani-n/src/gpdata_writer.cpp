// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#include "gpdata_writer.h"

#include <climits>
#include <ctime>
#include <iostream>

namespace {

uint64_t timespec_to_ns(const timespec& ts)
{
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}

}  // namespace

namespace timpani {
namespace node {

GpdataWriter::GpdataWriter(const std::string& node_id)
    : node_id_(node_id.empty() ? "node" : node_id),
      file_(nullptr),
      ktime_offset_ns_(0),
      started_(false)
{
}

GpdataWriter::~GpdataWriter()
{
    stop();
}

bool GpdataWriter::start()
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (started_) {
        return true;
    }

    if (!calibrate_ktime_offset_locked()) {
        std::cerr << "[gpdata] Failed to calibrate ktime offset" << std::endl;
        return false;
    }

    started_ = true;
    return true;
}

void GpdataWriter::stop()
{
    std::lock_guard<std::mutex> lock(mutex_);

    started_ = false;
    if (file_) {
        std::fflush(file_);
        std::fclose(file_);
        file_ = nullptr;
    }
}

void GpdataWriter::write_event(const schedstat_event& event,
                               const std::string& task_name)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (!started_) {
        return;
    }

    if (!ensure_file_opened_locked()) {
        started_ = false;
        return;
    }

    const uint64_t ts_wakeup =
        ns_to_us_round_up(to_realtime_ns(event.ts_wakeup));
    const uint64_t ts_start =
        ns_to_us_round_up(to_realtime_ns(event.ts_start));
    const uint64_t ts_stop = ns_to_us_round_up(to_realtime_ns(event.ts_stop));

    const int ret = std::fprintf(
        file_, "%-16s 0 0 %s-C%d 0 %lu %lu %lu 0\n", task_name.c_str(),
        node_id_.c_str(), event.cpu, static_cast<unsigned long>(ts_wakeup),
        static_cast<unsigned long>(ts_start), static_cast<unsigned long>(ts_stop));

    if (ret < 0) {
        std::cerr << "[gpdata] Failed to write event" << std::endl;
        started_ = false;
    }
}

bool GpdataWriter::ensure_file_opened_locked()
{
    if (file_) {
        return true;
    }

    const std::string file_name = node_id_ + ".gpdata";
    file_ = std::fopen(file_name.c_str(), "w+");
    if (!file_) {
        std::cerr << "[gpdata] Failed to open file: " << file_name
                  << std::endl;
        return false;
    }

    return true;
}

bool GpdataWriter::calibrate_ktime_offset_locked()
{
    uint64_t best_delta = ULLONG_MAX;
    uint64_t best_offset = 0;

    for (int i = 0; i < 20; ++i) {
        timespec t1 = {};
        timespec t2 = {};
        timespec t3 = {};

        if (clock_gettime(CLOCK_REALTIME, &t1) != 0) {
            return false;
        }
        if (clock_gettime(CLOCK_MONOTONIC, &t2) != 0) {
            return false;
        }
        if (clock_gettime(CLOCK_REALTIME, &t3) != 0) {
            return false;
        }

        const uint64_t ns_t1 = timespec_to_ns(t1);
        const uint64_t ns_t2 = timespec_to_ns(t2);
        const uint64_t ns_t3 = timespec_to_ns(t3);

        const uint64_t delta = ns_t3 - ns_t1;
        const uint64_t midpoint = (ns_t1 + ns_t3) / 2ULL;

        if (delta < best_delta) {
            best_delta = delta;
            best_offset = midpoint - ns_t2;
        }
    }

    ktime_offset_ns_ = best_offset;
    return true;
}

uint64_t GpdataWriter::to_realtime_ns(uint64_t monotonic_ns) const
{
    return ktime_offset_ns_ + monotonic_ns;
}

uint64_t GpdataWriter::ns_to_us_round_up(uint64_t ns)
{
    return (ns + (kNsPerUs - 1ULL)) / kNsPerUs;
}

}  // namespace node
}  // namespace timpani
