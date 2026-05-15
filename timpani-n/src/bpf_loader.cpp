// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#include "bpf_loader.h"
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>

#include "timpani.skel.h"

namespace {

/**
 * parse_cpu_range_to_mask() - Convert a cpuset range string (e.g. "2-3,5") to
 * a bitmask.  Returns 0 if the string is empty or unparseable.
 */
static uint64_t parse_cpu_range_to_mask(const std::string& range_str)
{
    uint64_t mask = 0;
    std::istringstream ss(range_str);
    std::string token;
    while (std::getline(ss, token, ',')) {
        // strip whitespace
        auto trim = [](std::string& s) {
            s.erase(0, s.find_first_not_of(" \t\r\n"));
            s.erase(s.find_last_not_of(" \t\r\n") + 1);
        };
        trim(token);
        if (token.empty()) continue;
        auto dash = token.find('-');
        if (dash == std::string::npos) {
            int cpu = std::stoi(token);
            if (cpu >= 0 && cpu < 64) mask |= (1ULL << cpu);
        } else {
            int lo = std::stoi(token.substr(0, dash));
            int hi = std::stoi(token.substr(dash + 1));
            for (int c = lo; c <= hi && c < 64; c++) mask |= (1ULL << c);
        }
    }
    return mask;
}

/**
 * read_file_line() - Read the first non-empty line of a file.
 * Returns empty string on failure.
 */
static std::string read_file_line(const std::string& path)
{
    std::ifstream f(path);
    if (!f.is_open()) return {};
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty()) return line;
    }
    return {};
}

/**
 * scan_cgroup_tree_for_isolated() - Walk /sys/fs/cgroup recursively and
 * aggregate cpuset.cpus.effective for all cgroups whose
 * cpuset.cpus.partition == "isolated".
 *
 * This is the Stage-2 fallback when the root-level cpuset.cpus.isolated knob
 * is unavailable or empty (older kernels, unusual configs).
 */
static uint64_t scan_cgroup_tree_for_isolated(const std::string& base)
{
    uint64_t mask = 0;
    DIR* dir = opendir(base.c_str());
    if (!dir) return 0;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') continue;
        if (entry->d_type != DT_DIR) continue;

        std::string sub = base + "/" + entry->d_name;

        // Check if this cgroup is an isolated partition
        std::string part_val = read_file_line(sub + "/cpuset.cpus.partition");
        if (part_val == "isolated") {
            std::string eff = read_file_line(sub + "/cpuset.cpus.effective");
            if (eff.empty()) eff = read_file_line(sub + "/cpuset.cpus");
            mask |= parse_cpu_range_to_mask(eff);
        }

        // Recurse
        mask |= scan_cgroup_tree_for_isolated(sub);
    }
    closedir(dir);
    return mask;
}

/**
 * detect_isolated_cpu_mask() - Detect isolated CPUs from cgroup v2.
 *
 * Stage 1: Read /sys/fs/cgroup/cpuset.cpus.isolated (root-level aggregate).
 *          This file is always accurate regardless of where child cgroup
 *          partitions were created.
 * Stage 2: If Stage 1 is unavailable or empty, scan the cgroup tree for
 *          cgroups with cpuset.cpus.partition == "isolated".
 *
 * Returns the bitmask of isolated CPUs, or 0 if none found.
 */
static uint64_t detect_isolated_cpu_mask()
{
    // Stage 1: root aggregate knob (kernel 5.14+, cgroup v2)
    static const std::string ISOLATED_KNOB = "/sys/fs/cgroup/cpuset.cpus.isolated";
    std::string val = read_file_line(ISOLATED_KNOB);
    if (!val.empty()) {
        uint64_t mask = parse_cpu_range_to_mask(val);
        if (mask != 0) {
            std::cout << "[BpfLoader] Isolated CPUs (Stage 1 — "
                      << ISOLATED_KNOB << "): " << val << std::endl;
            return mask;
        }
    }

    // Stage 2: cgroup tree scan
    std::cout << "[BpfLoader] cpuset.cpus.isolated empty or unavailable, "
                 "falling back to cgroup tree scan..." << std::endl;
    uint64_t mask = scan_cgroup_tree_for_isolated("/sys/fs/cgroup");
    if (mask != 0) {
        std::cout << "[BpfLoader] Isolated CPUs (Stage 2 — tree scan): "
                  << "mask=0x" << std::hex << mask << std::dec << std::endl;
    }
    return mask;
}

} // anonymous namespace

namespace timpani {
namespace node {

BpfLoader::BpfLoader() : active_map_idx_(0), skel_(nullptr) {
}

BpfLoader::~BpfLoader() {
    unload_programs();
}

bool BpfLoader::load_programs() {
    // Detect isolated CPUs before loading the BPF program so the mask can be
    // injected into the BPF global (skel->bss->isolated_cpu_mask) before
    // attach.  The BPF init() op uses this mask to create per-CPU TT DSQs
    // only for actually-isolated CPUs — no hardcoded CPU count needed.
    uint64_t iso_mask = detect_isolated_cpu_mask();
    if (iso_mask == 0) {
        // No isolated CPUs found: scx_timpani has no CPUs to manage.
        // This is a fatal misconfiguration for an RT system — abort rather
        // than silently running without RT guarantees.
        std::cerr << "[BpfLoader] FATAL: No isolated CPUs detected from cgroup.\n"
                  << "            Expected: /sys/fs/cgroup/cpuset.cpus.isolated\n"
                  << "                  or: a cgroup with cpuset.cpus.partition=isolated\n"
                  << "            Configure CPU isolation before starting timpani-n."
                  << std::endl;
        return false;
    }

    // Open skeleton (parse BPF ELF, do NOT load yet)
    skel_ = timpani_bpf__open();
    if (!skel_) {
        std::cerr << "[BpfLoader] Failed to open BPF skeleton" << std::endl;
        return false;
    }

    // Inject isolated CPU mask into BPF .rodata global BEFORE load+verifier.
    // isolated_cpu_mask is declared `volatile const` in BPF source, so the
    // skeleton exposes it via ->rodata (writable before load, read-only after).
    // The BPF init() callback reads this to create per-CPU TT DSQs.
    skel_->rodata->isolated_cpu_mask = iso_mask;
    std::cout << "[BpfLoader] Injecting isolated_cpu_mask=0x"
              << std::hex << iso_mask << std::dec
              << " into BPF global" << std::endl;

    // Load (verifier runs here) then attach (init() called here)
    if (timpani_bpf__load(skel_) != 0) {
        std::cerr << "[BpfLoader] Failed to load BPF skeleton" << std::endl;
        unload_programs();
        return false;
    }

    int err = timpani_bpf__attach(skel_);
    if (err) {
        std::cerr << "[BpfLoader] Failed to attach BPF skeleton" << std::endl;
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
