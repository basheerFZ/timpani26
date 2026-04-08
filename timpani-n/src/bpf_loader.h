// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#pragma once

#include "bpf/maps.h"
#include <cstdint>

namespace timpani {
namespace node {

class BpfLoader {
public:
    BpfLoader();
    ~BpfLoader();

    bool load_programs();
    void unload_programs();

    bool update_partition(int cgroup_id, const PartitionInfo& info);
    bool update_tt_slot(const TtSlotKey& key, const TtSlotBpf& slot);
    bool update_cbs_state(uint64_t hash, const CbsState& state);

private:
    // struct timpani_bpf* skel_; // To be generated via bpftool
};

} // namespace node
} // namespace timpani
