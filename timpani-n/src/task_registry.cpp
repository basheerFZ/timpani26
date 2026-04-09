// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#include "task_registry.h"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <sys/stat.h>

namespace timpani {
namespace node {

TaskRegistry::TaskRegistry(BpfLoader& bpf_loader) : bpf_loader_(bpf_loader) {}

TaskRegistry::~TaskRegistry() {
}

void TaskRegistry::scan_cgroups() {
    std::string base_dir = "/sys/fs/cgroup/";
    if (!std::filesystem::exists(base_dir)) return;

    for (const auto& entry : std::filesystem::directory_iterator(base_dir)) {
        if (!entry.is_directory()) continue;
        
        struct stat st;
        if (stat(entry.path().c_str(), &st) != 0) continue;
        uint64_t cgroup_id = st.st_ino;

        std::string procs_file = entry.path().string() + "/cgroup.procs";
        std::ifstream ifs(procs_file);
        if (!ifs) continue;

        int pid;
        while (ifs >> pid) {
            std::string task_dir = "/proc/" + std::to_string(pid) + "/task";
            if (!std::filesystem::exists(task_dir)) continue;

            for (const auto& tid_entry : std::filesystem::directory_iterator(task_dir)) {
                std::string comm_file = tid_entry.path().string() + "/comm";
                std::ifstream comm_ifs(comm_file);
                std::string comm_name;
                if (comm_ifs >> comm_name) {
                    auto it = expected_tasks_.find(comm_name);
                    if (it != expected_tasks_.end()) {
                        uint32_t tid = std::stoi(tid_entry.path().filename());
                        
                        TaskMeta meta = {};
                        meta.task_id_hash = it->second;
                        meta.scheduling_type = 0; // Default TT for PoC
                        bpf_loader_.update_task_meta(tid, meta);
                        
                        PartitionInfo pinfo = {};
                        pinfo.task_id_hash = it->second;
                        pinfo.cgroup_id = cgroup_id;
                        bpf_loader_.update_partition(cgroup_id, pinfo);
                        
                        std::cout << "[TaskRegistry] Registered " << comm_name << " (TID: " << tid << ", CGROUP: " << cgroup_id << ")\n";
                    }
                }
            }
        }
    }
}

void TaskRegistry::register_task(const char* comm, unsigned long task_id) {
    expected_tasks_[std::string(comm)] = task_id;
}

} // namespace node
} // namespace timpani
