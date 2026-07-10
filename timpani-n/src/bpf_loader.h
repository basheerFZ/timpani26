// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#pragma once

#include "bpf/maps.h"
#include <cstdint>

struct timpani_bpf;

namespace timpani {
namespace node {

class BpfLoader {
public:
    BpfLoader();
    ~BpfLoader();

    bool load_programs();
    void unload_programs();

    bool update_tt_slot(const TtSlotKey& key, const TtSlotBpf& slot);
    bool update_cbs_state(uint64_t hash, const CbsState& state);
    bool update_task_meta(uint32_t pid, const TaskMeta& meta);
    bool delete_task_meta(uint32_t pid);
    bool delete_tt_slot(const TtSlotKey& key);
    bool delete_cbs_state(uint64_t hash);
    bool update_current_slot(uint32_t cpu, uint32_t slot_idx);
    bool update_next_tt_start(uint32_t cpu, uint64_t next_start_ns);


    int get_fault_ringbuf_fd() const;

    // Shadow Map Swapping for hot updates
    void swap_shadow_maps();
    int get_active_map_idx() const;
    int get_shadow_map_idx() const;
    void apply_table_update();

private:
    int active_map_idx_;
    struct timpani_bpf* skel_;
};

} // namespace node
} // namespace timpani
