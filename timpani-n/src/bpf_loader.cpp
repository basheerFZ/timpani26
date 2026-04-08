// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#include "bpf_loader.h"

namespace timpani {
namespace node {

BpfLoader::BpfLoader() : active_map_idx_(0) {
}

BpfLoader::~BpfLoader() {
    unload_programs();
}

bool BpfLoader::load_programs() {
    // TODO: Init libbpf and load timpani.bpf.o skeleton.
    return false;
}

void BpfLoader::unload_programs() {
    // TODO: Cleanup BPF skeleton.
}

bool BpfLoader::update_partition(int /* cgroup_id */, const PartitionInfo& /* info */) {
    return false;
}

bool BpfLoader::update_tt_slot(const TtSlotKey& /* key */, const TtSlotBpf& /* slot */) {
    return false;
}

bool BpfLoader::update_cbs_state(uint64_t /* hash */, const CbsState& /* state */) {
    return false;
}

void BpfLoader::swap_shadow_maps() {
    active_map_idx_ = 1 - active_map_idx_;
}

int BpfLoader::get_active_map_idx() const {
    return active_map_idx_;
}

int BpfLoader::get_shadow_map_idx() const {
    return 1 - active_map_idx_;
}

void BpfLoader::apply_table_update() {
    swap_shadow_maps();
    // TODO: Write active_map_idx_ to BPF map (e.g. active_config_map)
}

} // namespace node
} // namespace timpani
