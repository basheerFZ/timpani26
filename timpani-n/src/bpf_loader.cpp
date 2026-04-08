// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#include "bpf_loader.h"

namespace timpani {
namespace node {

BpfLoader::BpfLoader() {
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

} // namespace node
} // namespace timpani
