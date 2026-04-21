// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#include "bpf_loader.h"
#include <iostream>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>

#include "timpani.skel.h"

namespace timpani {
namespace node {

BpfLoader::BpfLoader() : active_map_idx_(0), skel_(nullptr) {
}

BpfLoader::~BpfLoader() {
    unload_programs();
}

bool BpfLoader::load_programs() {
    skel_ = timpani_bpf__open_and_load();
    if (!skel_) {
        std::cerr << "Failed to open and load BPF skeleton" << std::endl;
        return false;
    }

    int err = timpani_bpf__attach(skel_);
    if (err) {
        std::cerr << "Failed to attach BPF skeleton" << std::endl;
        unload_programs();
        return false;
    }

    return true;
}

void BpfLoader::unload_programs() {
    if (skel_) {
        timpani_bpf__destroy(skel_);
        skel_ = nullptr;
    }
}

bool BpfLoader::update_partition(uint64_t cgroup_id, const PartitionInfo& info) {
    if (!skel_) return false;
    return bpf_map_update_elem(bpf_map__fd(skel_->maps.partition_map), &cgroup_id, &info, BPF_ANY) == 0;
}

bool BpfLoader::update_tt_slot(const TtSlotKey& key, const TtSlotBpf& slot) {
    if (!skel_) return false;
    return bpf_map_update_elem(bpf_map__fd(skel_->maps.tt_table_map), &key, &slot, BPF_ANY) == 0;
}

bool BpfLoader::update_cbs_state(uint64_t hash, const CbsState& state) {
    if (!skel_) return false;
    return bpf_map_update_elem(bpf_map__fd(skel_->maps.cbs_map), &hash, &state, BPF_ANY) == 0;
}

bool BpfLoader::update_task_meta(uint32_t pid, const TaskMeta& meta) {
    if (!skel_) return false;
    return bpf_map_update_elem(bpf_map__fd(skel_->maps.task_meta_map), &pid, &meta, BPF_ANY) == 0;
}

bool BpfLoader::update_current_slot(uint32_t cpu, uint32_t slot_idx) {
    if (!skel_) return false;
    return bpf_map_update_elem(bpf_map__fd(skel_->maps.current_slot_map), &cpu, &slot_idx, BPF_ANY) == 0;
}

bool BpfLoader::update_kick_cpu(uint32_t cpu) {
    if (!skel_) return false;
    uint32_t flag = 1;
    return bpf_map_update_elem(bpf_map__fd(skel_->maps.kick_map), &cpu, &flag, BPF_ANY) == 0;
}

int BpfLoader::get_fault_ringbuf_fd() const {
    if (!skel_) return -1;
    return bpf_map__fd(skel_->maps.fault_ringbuf);
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
