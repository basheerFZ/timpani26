/*
 * SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
 * SPDX-License-Identifier: MIT
 */

#include "global_scheduler.h"
#include "tlog.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>
#include <set>

using namespace timpani::node::v1;

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

GlobalScheduler::GlobalScheduler(
    std::shared_ptr<NodeConfigManager> node_config_manager)
    : node_config_manager_(node_config_manager)
{
    TLOG_INFO("GlobalScheduler created (DDR-007 TT+CBS pipeline)");
}

// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------

ScheduleResult GlobalScheduler::generate_schedule(
    const std::string& node_id,
    const std::vector<ClassifiedTask>& tasks)
{
    if (tasks.empty()) {
        return InfeasibleError(InfeasibleReason::UtilizationExceeded, -1, "",
                               "No tasks provided");
    }

    TLOG_INFO("=== GlobalScheduler: generating schedule for node '",
              node_id, "' with ", tasks.size(), " tasks ===");

    // ── Step 1: Separate by mechanism (already classified by caller) ──
    std::vector<ClassifiedTask> tt_tasks;
    std::vector<ClassifiedTask> cbs_tasks;

    for (const auto& t : tasks) {
        if (t.mechanism == Mechanism::TT) {
            tt_tasks.push_back(t);
        } else {
            cbs_tasks.push_back(t);
        }
    }

    TLOG_INFO("  L1 TT tasks: ", tt_tasks.size(),
              ", L2 CBS tasks: ", cbs_tasks.size());

    // ── Step 2: CPU assignment ──
    std::map<int, PerCpuSchedule> cpu_schedules;
    if (!assign_cpus(node_id, tt_tasks, cbs_tasks, cpu_schedules)) {
        return InfeasibleError(InfeasibleReason::UtilizationExceeded, -1, "",
                               "CPU assignment failed — no isolated CPUs available for node '"
                               + node_id + "'");
    }

    // ── Step 3: TT slot placement (per CPU) ──
    for (auto& [cpu_id, sched] : cpu_schedules) {
        // Collect TT tasks assigned to this CPU
        std::vector<ClassifiedTask> cpu_tt;
        for (const auto& t : tt_tasks) {
            if (t.assigned_cpu == cpu_id) {
                cpu_tt.push_back(t);
            }
        }

        if (cpu_tt.empty()) continue;

        // Collect periods for hyperperiod calculation
        std::vector<uint64_t> periods;
        for (const auto& t : cpu_tt) {
            if (t.period_us > 0) {
                periods.push_back(t.period_us);
            }
        }

        // Validate harmonic periods
        if (!validate_harmonic(periods)) {
            std::string detail = "Non-harmonic periods on CPU " + std::to_string(cpu_id) + ":";
            for (auto p : periods) detail += " " + std::to_string(p);
            return InfeasibleError(InfeasibleReason::NonHarmonicPeriod,
                                   cpu_id, "", detail);
        }

        uint64_t hp_us = calculate_hyperperiod(periods);

        InfeasibleError err;
        if (!place_tt_slots(cpu_id, cpu_tt, hp_us, sched, err)) {
            return err;
        }
    }

    // ── Compute global hyperperiod across all CPUs ──
    std::vector<uint64_t> all_periods;
    for (const auto& t : tt_tasks) {
        if (t.period_us > 0) all_periods.push_back(t.period_us);
    }
    for (const auto& t : cbs_tasks) {
        if (t.period_us > 0) all_periods.push_back(t.period_us);
    }
    uint64_t global_hp = all_periods.empty() ? 0 : calculate_hyperperiod(all_periods);

    // ── Step 4: Gap analysis + CBS allocation (per CPU) ──
    for (auto& [cpu_id, sched] : cpu_schedules) {
        // Use global hyperperiod for gap computation
        uint64_t hp_us = global_hp;
        if (hp_us == 0) {
            // If no periods, use a default; shouldn't happen with valid tasks
            hp_us = 10000; // 10ms default
        }

        sched.gaps = compute_gaps(sched.tt_slots, hp_us);

        // Collect CBS tasks assigned to this CPU
        std::vector<ClassifiedTask> cpu_cbs;
        for (const auto& t : cbs_tasks) {
            if (t.assigned_cpu == cpu_id) {
                cpu_cbs.push_back(t);
            }
        }

        if (!cpu_cbs.empty()) {
            InfeasibleError err;
            if (!allocate_cbs_budgets(cpu_id, cpu_cbs, sched.u_tt,
                                       sched.gaps, sched, err)) {
                return err;
            }
        }
    }

    // ── Step 5: Build protobuf table ──
    auto table = build_table(node_id, cpu_schedules, global_hp);
    return table;
}

// ---------------------------------------------------------------------------
// Step 2: CPU Assignment (DDR-007 §3.3 + §6)
// ---------------------------------------------------------------------------

bool GlobalScheduler::assign_cpus(
    const std::string& node_id,
    std::vector<ClassifiedTask>& tt_tasks,
    std::vector<ClassifiedTask>& cbs_tasks,
    std::map<int, PerCpuSchedule>& cpu_schedules)
{
    // Get available isolated CPUs from node config
    std::vector<int> available_cpus;
    if (node_config_manager_ && node_config_manager_->IsLoaded()) {
        available_cpus = node_config_manager_->GetAvailableCpus(node_id);
    }

    if (available_cpus.empty()) {
        // Fallback: use default CPUs [2, 3] for isolated scheduling
        available_cpus = {2, 3};
        TLOG_WARN("No CPU config for node '", node_id,
                  "', using default isolated CPUs [2, 3]");
    }

    // Sort CPUs for deterministic assignment
    std::sort(available_cpus.begin(), available_cpus.end());

    // Initialize PerCpuSchedule for each available CPU
    for (int cpu : available_cpus) {
        cpu_schedules[cpu] = PerCpuSchedule(cpu);
    }

    // ① L1 TT tasks: assign to dedicated CPUs per workload
    //    Group TT tasks by workload_id, assign each workload to a separate CPU
    std::map<std::string, std::vector<size_t>> tt_workload_groups;
    for (size_t i = 0; i < tt_tasks.size(); ++i) {
        tt_workload_groups[tt_tasks[i].workload_id].push_back(i);
    }

    size_t cpu_idx = 0;
    std::map<std::string, int> tt_workload_cpu;  // workload_id → assigned CPU

    for (const auto& [wl_id, task_indices] : tt_workload_groups) {
        if (cpu_idx >= available_cpus.size()) {
            // Ran out of CPUs — share with the last one
            cpu_idx = available_cpus.size() - 1;
            TLOG_WARN("Not enough isolated CPUs for dedicated TT assignment; "
                      "sharing CPU ", available_cpus[cpu_idx],
                      " for workload '", wl_id, "'");
        }

        int cpu = available_cpus[cpu_idx];
        tt_workload_cpu[wl_id] = cpu;

        double wl_util = 0.0;
        for (size_t idx : task_indices) {
            tt_tasks[idx].assigned_cpu = cpu;
            if (tt_tasks[idx].period_us > 0) {
                wl_util += static_cast<double>(tt_tasks[idx].wcet_us) /
                           tt_tasks[idx].period_us;
            }
        }
        cpu_schedules[cpu].u_tt += wl_util;

        TLOG_INFO("  TT workload '", wl_id, "' → CPU ", cpu,
                  " (", task_indices.size(), " tasks, U_tt=",
                  cpu_schedules[cpu].u_tt, ")");
        ++cpu_idx;
    }

    // ② L2 CBS tasks: worst-fit decreasing by residual capacity (DDR-007 §3.3)
    //    Sort CBS tasks by utilization (decreasing)
    std::vector<size_t> cbs_indices(cbs_tasks.size());
    std::iota(cbs_indices.begin(), cbs_indices.end(), 0);
    std::sort(cbs_indices.begin(), cbs_indices.end(),
        [&cbs_tasks](size_t a, size_t b) {
            double ua = (cbs_tasks[a].period_us > 0)
                ? static_cast<double>(cbs_tasks[a].wcet_us) / cbs_tasks[a].period_us
                : 0.0;
            double ub = (cbs_tasks[b].period_us > 0)
                ? static_cast<double>(cbs_tasks[b].wcet_us) / cbs_tasks[b].period_us
                : 0.0;
            return ua > ub;  // decreasing
        });

    for (size_t idx : cbs_indices) {
        double us = (cbs_tasks[idx].period_us > 0)
            ? static_cast<double>(cbs_tasks[idx].wcet_us) / cbs_tasks[idx].period_us
            : 0.0;

        // Find CPU with the most residual capacity (worst-fit)
        int best_cpu = -1;
        double best_residual = -1.0;

        for (int cpu : available_cpus) {
            double residual = U_BOUND - cpu_schedules[cpu].u_tt -
                              cpu_schedules[cpu].u_cbs - U_OVERHEAD;
            if (residual >= us && residual > best_residual) {
                best_residual = residual;
                best_cpu = cpu;
            }
        }

        if (best_cpu < 0) {
            TLOG_ERROR("No CPU can accommodate CBS task '",
                       cbs_tasks[idx].task_id, "' (U_s=", us, ")");
            return false;
        }

        cbs_tasks[idx].assigned_cpu = best_cpu;
        cpu_schedules[best_cpu].u_cbs += us;

        TLOG_INFO("  CBS task '", cbs_tasks[idx].task_id, "' → CPU ", best_cpu,
                  " (U_s=", us, ", residual=", best_residual - us, ")");
    }

    return true;
}

// ---------------------------------------------------------------------------
// Step 3: TT Slot Placement (DDR-007 §3.4)
// ---------------------------------------------------------------------------

bool GlobalScheduler::place_tt_slots(
    int cpu,
    const std::vector<ClassifiedTask>& tt_tasks,
    uint64_t hyperperiod_us,
    PerCpuSchedule& schedule,
    InfeasibleError& error)
{
    if (hyperperiod_us == 0) {
        error = InfeasibleError(InfeasibleReason::TtSlotConflict, cpu, "",
                                "Hyperperiod is zero");
        return false;
    }

    // Sort by deadline (DM ordering — shorter deadline first)
    std::vector<ClassifiedTask> sorted = tt_tasks;
    std::sort(sorted.begin(), sorted.end(),
        [](const ClassifiedTask& a, const ClassifiedTask& b) {
            return a.deadline_us < b.deadline_us;
        });

    // Timeline occupancy tracking: set of occupied intervals [start, end)
    struct Interval {
        uint32_t start;
        uint32_t end;
    };
    std::vector<Interval> occupied;

    auto find_free_slot = [&occupied](uint32_t start, uint32_t duration,
                                       uint32_t deadline_end) -> int32_t {
        uint32_t candidate = start;
        // Scan forward for a gap that can hold 'duration' before deadline_end
        while (candidate + duration <= deadline_end) {
            bool conflict = false;
            for (const auto& occ : occupied) {
                if (candidate < occ.end && candidate + duration > occ.start) {
                    // Jump past this occupied interval
                    candidate = occ.end;
                    conflict = true;
                    break;
                }
            }
            if (!conflict) {
                return static_cast<int32_t>(candidate);
            }
        }
        return -1;  // No free slot found
    };

    for (const auto& task : sorted) {
        if (task.period_us == 0) continue;

        uint64_t repeats = hyperperiod_us / task.period_us;
        for (uint64_t k = 0; k < repeats; ++k) {
            uint32_t frame_start = static_cast<uint32_t>(k * task.period_us);
            uint32_t frame_end   = frame_start + task.deadline_us;
            if (frame_end > hyperperiod_us) {
                frame_end = static_cast<uint32_t>(hyperperiod_us);
            }

            int32_t offset = find_free_slot(frame_start, task.wcet_us, frame_end);
            if (offset < 0) {
                error = InfeasibleError(InfeasibleReason::TtSlotConflict, cpu,
                    task.task_id,
                    "TT slot conflict in frame [" + std::to_string(frame_start) +
                    ", " + std::to_string(frame_end) + ") on CPU " +
                    std::to_string(cpu));
                return false;
            }

            TtSlotPlacement slot;
            slot.workload_id = task.workload_id;
            slot.task_id     = task.task_id;
            slot.offset_us   = static_cast<uint32_t>(offset);
            slot.duration_us = task.wcet_us;
            slot.deadline_us = task.deadline_us;
            slot.cpu         = cpu;

            schedule.tt_slots.push_back(slot);
            occupied.push_back({static_cast<uint32_t>(offset),
                                static_cast<uint32_t>(offset) + task.wcet_us});
        }
    }

    // Sort slots by offset for gap computation
    std::sort(schedule.tt_slots.begin(), schedule.tt_slots.end(),
        [](const TtSlotPlacement& a, const TtSlotPlacement& b) {
            return a.offset_us < b.offset_us;
        });

    TLOG_INFO("  CPU ", cpu, ": placed ", schedule.tt_slots.size(),
              " TT slots in hyperperiod ", hyperperiod_us, "us");
    return true;
}

// ---------------------------------------------------------------------------
// Step 4-A: Gap Extraction (DDR-007 §3.5)
// ---------------------------------------------------------------------------

std::vector<GapInterval> GlobalScheduler::compute_gaps(
    const std::vector<TtSlotPlacement>& slots,
    uint64_t hyperperiod_us)
{
    std::vector<GapInterval> gaps;
    uint32_t cursor = 0;

    // Slots must be sorted by offset (done in place_tt_slots)
    for (const auto& slot : slots) {
        if (cursor < slot.offset_us) {
            GapInterval gap;
            gap.start_us  = cursor;
            gap.end_us    = slot.offset_us;
            gap.length_us = slot.offset_us - cursor;
            gaps.push_back(gap);
        }
        uint32_t slot_end = slot.offset_us + slot.duration_us;
        if (slot_end > cursor) {
            cursor = slot_end;
        }
    }

    // Trailing gap after last slot
    if (cursor < static_cast<uint32_t>(hyperperiod_us)) {
        GapInterval gap;
        gap.start_us  = cursor;
        gap.end_us    = static_cast<uint32_t>(hyperperiod_us);
        gap.length_us = static_cast<uint32_t>(hyperperiod_us) - cursor;
        gaps.push_back(gap);
    }

    return gaps;
}

// ---------------------------------------------------------------------------
// Step 4-B/C/D: CBS Budget Allocation (DDR-007 §3.5)
// ---------------------------------------------------------------------------

bool GlobalScheduler::allocate_cbs_budgets(
    int cpu,
    const std::vector<ClassifiedTask>& cbs_tasks,
    double u_tt,
    const std::vector<GapInterval>& gaps,
    PerCpuSchedule& schedule,
    InfeasibleError& error)
{
    double u_avail = U_BOUND - u_tt - U_OVERHEAD;

    if (u_avail <= 0.0) {
        error = InfeasibleError(InfeasibleReason::NoCbsBandwidth, cpu, "",
            "No residual bandwidth on CPU " + std::to_string(cpu) +
            " (U_tt=" + std::to_string(u_tt) + ")");
        return false;
    }

    // Check minimum gap condition
    if (!gaps.empty()) {
        uint32_t min_gap = gaps[0].length_us;
        for (const auto& g : gaps) {
            if (g.length_us < min_gap) min_gap = g.length_us;
        }
        if (min_gap < CBS_MIN_EXEC_US) {
            TLOG_WARN("CPU ", cpu, ": minimum gap ", min_gap,
                      "us < CBS_MIN_EXEC_US (", CBS_MIN_EXEC_US, "us)");
        }
    }

    // Sort CBS tasks by utilization (decreasing) for fail-fast (DDR-007 §3.5 4-D)
    std::vector<ClassifiedTask> sorted = cbs_tasks;
    std::sort(sorted.begin(), sorted.end(),
        [](const ClassifiedTask& a, const ClassifiedTask& b) {
            double ua = (a.period_us > 0)
                ? static_cast<double>(a.wcet_us) / a.period_us : 0.0;
            double ub = (b.period_us > 0)
                ? static_cast<double>(b.wcet_us) / b.period_us : 0.0;
            return ua > ub;
        });

    double u_alloc = 0.0;

    for (const auto& task : sorted) {
        double us = (task.period_us > 0)
            ? static_cast<double>(task.wcet_us) / task.period_us : 0.0;

        if (u_alloc + us > u_avail) {
            // L2 is SafetyCritical — cannot reject silently (DDR-007 §3.5 4-D)
            error = InfeasibleError(InfeasibleReason::SafetyCbsExceeded, cpu,
                task.task_id,
                "CBS task '" + task.task_id + "' requires U_s=" +
                std::to_string(us) + " but only " +
                std::to_string(u_avail - u_alloc) + " available on CPU " +
                std::to_string(cpu));
            return false;
        }

        CbsAllocation alloc;
        alloc.workload_id = task.workload_id;
        alloc.task_id     = task.task_id;
        alloc.budget_us   = task.wcet_us;      // Cs = WCET
        alloc.period_us   = task.period_us;     // Ts = MIT
        alloc.deadline_us = task.deadline_us;
        alloc.cpu         = cpu;

        schedule.cbs_allocations.push_back(alloc);
        u_alloc += us;
    }

    schedule.u_cbs = u_alloc;

    TLOG_INFO("  CPU ", cpu, ": allocated ", schedule.cbs_allocations.size(),
              " CBS budgets (U_cbs=", u_alloc,
              ", U_total=", u_tt + u_alloc + U_OVERHEAD, ")");
    return true;
}

// ---------------------------------------------------------------------------
// Step 5: Build HierarchicalScheduleTable (DDR-007 §3.6)
// ---------------------------------------------------------------------------

timpani::node::v1::HierarchicalScheduleTable GlobalScheduler::build_table(
    const std::string& node_id,
    const std::map<int, PerCpuSchedule>& cpu_schedules,
    uint64_t hyperperiod_us)
{
    HierarchicalScheduleTable table;
    table.set_table_id("table_v1");
    table.set_node_id(node_id);
    table.set_hyperperiod_us(static_cast<uint32_t>(hyperperiod_us));

    // epoch_ns = current wall-clock time (CLOCK_REALTIME)
    auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    table.set_epoch_ns(static_cast<uint64_t>(now_ns));

    int total_tt = 0;
    int total_cbs = 0;

    for (const auto& [cpu_id, sched] : cpu_schedules) {
        // Skip CPUs with no assignments
        if (sched.tt_slots.empty() && sched.cbs_allocations.empty()) {
            continue;
        }

        // One PartitionConfig per isolated CPU (DDR-007 §3.6)
        PartitionConfig* partition = table.add_partitions();
        partition->set_partition_id("isolated-" + std::to_string(cpu_id));

        CpuSetSpec* cpuset = partition->mutable_cpuset();
        cpuset->add_cpus(static_cast<uint32_t>(cpu_id));
        cpuset->set_isolated(true);

        // Single layer with both TT slots and CBS entries
        LayerConfig* layer = partition->add_layers();
        layer->set_layer_index(1);
        layer->set_model(ExecutionModel::TIME_TRIGGERED);

        // Populate TT slots
        for (const auto& slot : sched.tt_slots) {
            TtSlot* tt = layer->add_tt_slots();
            tt->set_workload_id(slot.workload_id);
            tt->set_task_id(slot.task_id);
            tt->set_offset_us(slot.offset_us);
            tt->set_duration_us(slot.duration_us);
            tt->set_deadline_us(slot.deadline_us);
            tt->set_cpu(static_cast<uint32_t>(slot.cpu));
            tt->set_workload_id_hash(fnv1a_hash(slot.workload_id.c_str()));
            tt->set_task_id_hash(fnv1a_hash(slot.task_id.c_str()));
            ++total_tt;
        }

        // Populate CBS entries
        for (const auto& alloc : sched.cbs_allocations) {
            CbsConfig* cbs = layer->add_cbs_entries();
            cbs->set_workload_id(alloc.workload_id);
            cbs->set_task_id(alloc.task_id);
            cbs->set_budget_us(alloc.budget_us);
            cbs->set_period_us(alloc.period_us);
            cbs->set_deadline_us(alloc.deadline_us);
            cbs->set_workload_id_hash(fnv1a_hash(alloc.workload_id.c_str()));
            cbs->set_task_id_hash(fnv1a_hash(alloc.task_id.c_str()));
            ++total_cbs;
        }
    }

    TLOG_INFO("BuildScheduleTable: node='", node_id,
              "' partitions=", table.partitions_size(),
              " tt_slots=", total_tt,
              " cbs_entries=", total_cbs,
              " hyperperiod=", hyperperiod_us, "us");

    return table;
}

// ---------------------------------------------------------------------------
// Math utilities
// ---------------------------------------------------------------------------

uint64_t GlobalScheduler::gcd(uint64_t a, uint64_t b)
{
    while (b != 0) {
        uint64_t temp = b;
        b = a % b;
        a = temp;
    }
    return a;
}

uint64_t GlobalScheduler::lcm(uint64_t a, uint64_t b)
{
    if (a == 0 || b == 0) return 0;
    return (a / gcd(a, b)) * b;
}

uint64_t GlobalScheduler::calculate_hyperperiod(
    const std::vector<uint64_t>& periods)
{
    if (periods.empty()) return 0;

    uint64_t result = periods[0];
    for (size_t i = 1; i < periods.size(); ++i) {
        result = lcm(result, periods[i]);
        if (result > 3600000000ULL) {  // 1 hour in microseconds
            TLOG_WARN("Hyperperiod very large: ", result / 1000000, " seconds");
        }
    }
    return result;
}

bool GlobalScheduler::validate_harmonic(const std::vector<uint64_t>& periods)
{
    if (periods.size() <= 1) return true;

    // Deduplicate and sort
    std::set<uint64_t> unique_set(periods.begin(), periods.end());
    std::vector<uint64_t> sorted(unique_set.begin(), unique_set.end());
    std::sort(sorted.begin(), sorted.end());

    // Harmonic condition: for every pair, the larger must be divisible by the smaller
    for (size_t i = 0; i < sorted.size(); ++i) {
        for (size_t j = i + 1; j < sorted.size(); ++j) {
            if (sorted[j] % sorted[i] != 0) {
                TLOG_WARN("Non-harmonic periods: ", sorted[i], " and ", sorted[j]);
                return false;
            }
        }
    }
    return true;
}

uint64_t GlobalScheduler::fnv1a_hash(const char* s)
{
    uint64_t hash = 14695981039346656037ULL;
    while (*s) {
        hash ^= static_cast<uint64_t>(static_cast<unsigned char>(*s++));
        hash *= 1099511628211ULL;
    }
    return hash;
}
