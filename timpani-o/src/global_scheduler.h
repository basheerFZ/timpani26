/*
 * SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
 * SPDX-License-Identifier: MIT
 */

#ifndef GLOBAL_SCHEDULER_H
#define GLOBAL_SCHEDULER_H

#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>
#include <stdint.h>

#include "node_config.h"
#include "proto/node_control.grpc.pb.h"

// ---------------------------------------------------------------------------
// DDR-007 workload classification types
// ---------------------------------------------------------------------------

/**
 * @brief DDR-007 scheduling mechanism — determined by TemporalClass.
 *
 * TEMPORAL_PERIODIC (L1) → TT (Time-Triggered)
 * TEMPORAL_SPORADIC (L2) → CBS (Constant Bandwidth Server)
 */
enum class Mechanism {
    TT,   // L1 Periodic — static time-triggered slots
    CBS,  // L2 Sporadic — Constant Bandwidth Server
};

/**
 * @brief Lightweight classified task for the scheduler pipeline.
 *
 * Produced by the service layer from gRPC TaskInfo + TemporalClass,
 * consumed by GlobalScheduler::generate_schedule().
 */
struct ClassifiedTask {
    std::string workload_id;
    std::string task_id;
    Mechanism   mechanism;
    uint32_t    period_us;      // L1: period / L2: min_inter_arrival
    uint32_t    wcet_us;        // worst-case execution time
    uint32_t    deadline_us;
    uint32_t    max_dmiss;
    int         assigned_cpu;

    ClassifiedTask()
        : mechanism(Mechanism::TT),
          period_us(0), wcet_us(0), deadline_us(0),
          assigned_cpu(-1), max_dmiss(0) {}
};

/**
 * @brief Schedule table storage: node_id → combined table (all workloads merged)
 */
using ScheduleTableMap =
    std::map<std::string, timpani::node::v1::HierarchicalScheduleTable>;

// ---------------------------------------------------------------------------
// Intermediate structures for the 5-step pipeline (DDR-007 §3)
// ---------------------------------------------------------------------------

/**
 * @brief A placed TT slot within a hyperperiod timeline.
 */
struct TtSlotPlacement {
    std::string workload_id;
    std::string task_id;
    uint32_t    offset_us;
    uint32_t    duration_us;
    uint32_t    deadline_us;
    uint32_t    max_dmiss;
    int         cpu;
};

/**
 * @brief A gap interval between TT slots (DDR-007 §3.5 4-A).
 */
struct GapInterval {
    uint32_t start_us;
    uint32_t end_us;
    uint32_t length_us;
};

/**
 * @brief CBS budget allocation for a single L2 task (DDR-007 §3.5 4-C).
 */
struct CbsAllocation {
    std::string workload_id;
    std::string task_id;
    uint32_t    budget_us;      // Cs — server budget = WCET per arrival
    uint32_t    period_us;      // Ts — replenishment period = MIT
    uint32_t    deadline_us;
    uint32_t    max_dmiss;
    int         cpu;
};

/**
 * @brief Reasons why a schedule is infeasible.
 */
enum class InfeasibleReason {
    NonHarmonicPeriod,   // L1 task periods are not harmonic
    TtSlotConflict,      // TT slot placement conflict on timeline
    NoCbsBandwidth,      // No residual bandwidth for CBS after TT placement
    SafetyCbsExceeded,   // L2 CBS bandwidth exceeds available budget
    NoCbsCpu,            // No isolated CPU available for CBS tasks
    UtilizationExceeded, // Total utilization exceeds bound
};

/**
 * @brief Error returned when schedule generation fails.
 */
struct InfeasibleError {
    InfeasibleReason reason;
    int              cpu;        // -1 if not CPU-specific
    std::string      task_id;    // empty if not task-specific
    std::string      details;    // human-readable explanation

    InfeasibleError() : reason(InfeasibleReason::UtilizationExceeded), cpu(-1) {}
    InfeasibleError(InfeasibleReason r, int c, const std::string& t, const std::string& d)
        : reason(r), cpu(c), task_id(t), details(d) {}
};

/**
 * @brief Per-CPU schedule produced during the pipeline.
 */
struct PerCpuSchedule {
    int                            cpu_id;
    std::vector<TtSlotPlacement>   tt_slots;
    std::vector<GapInterval>       gaps;
    std::vector<CbsAllocation>     cbs_allocations;
    double                         u_tt;    // Σ(wcet/period) for TT tasks
    double                         u_cbs;   // Σ(Cs/Ts) for CBS tasks

    PerCpuSchedule() : cpu_id(-1), u_tt(0.0), u_cbs(0.0) {}
    explicit PerCpuSchedule(int cpu) : cpu_id(cpu), u_tt(0.0), u_cbs(0.0) {}
};

/**
 * @brief Result type: either a schedule table or an infeasibility error.
 */
using ScheduleResult =
    std::variant<timpani::node::v1::HierarchicalScheduleTable, InfeasibleError>;

// ---------------------------------------------------------------------------
// GlobalScheduler — DDR-007 §3 five-step pipeline
// ---------------------------------------------------------------------------

/**
 * @brief Generates integrated TT+CBS schedule tables per DDR-007.
 *
 * Pipeline:
 *   Step 1: Classification (done by caller — TemporalClass → Mechanism)
 *   Step 2: Isolated CPU assignment (partitioned scheduling)
 *   Step 3: L1 TT slot placement (harmonic-period DM ordering)
 *   Step 4: Gap analysis + L2 CBS budget allocation (feasibility check)
 *   Step 5: Emit HierarchicalScheduleTable protobuf
 */
class GlobalScheduler {
public:
    explicit GlobalScheduler(std::shared_ptr<NodeConfigManager> node_config_manager);
    ~GlobalScheduler() = default;

    /**
     * @brief Generate a schedule table for a single node.
     *
     * @param node_id   Target node identifier
     * @param tasks     Pre-classified tasks (Mechanism already set)
     * @return ScheduleResult — HierarchicalScheduleTable on success,
     *                          InfeasibleError on failure
     */
    ScheduleResult generate_schedule(const std::string& node_id,
                                     const std::vector<ClassifiedTask>& tasks);

    // DDR-007 §3.5 feasibility constants
    static constexpr double U_OVERHEAD  = 0.02;   // Timer Master + BPF dispatch
    static constexpr double U_BOUND     = 0.80;   // Conservative utilization bound (DDR-004 §5)
    static constexpr uint32_t CBS_MIN_EXEC_US = 100; // Minimum usable gap (μs)
    static uint64_t fnv1a_hash(const char* s);

private:
    // ── Step 2: CPU assignment ──
    bool assign_cpus(const std::string& node_id,
                     std::vector<ClassifiedTask>& tt_tasks,
                     std::vector<ClassifiedTask>& cbs_tasks,
                     std::map<int, PerCpuSchedule>& cpu_schedules);

    // ── Step 3: TT slot placement (per CPU) ──
    bool place_tt_slots(int cpu,
                        const std::vector<ClassifiedTask>& tt_tasks,
                        uint64_t hyperperiod_us,
                        PerCpuSchedule& schedule,
                        InfeasibleError& error);

    // ── Step 4-A: Gap extraction ──
    static std::vector<GapInterval> compute_gaps(
        const std::vector<TtSlotPlacement>& slots,
        uint64_t hyperperiod_us);

    // ── Step 4-B/C/D: CBS budget allocation (per CPU) ──
    bool allocate_cbs_budgets(int cpu,
                              const std::vector<ClassifiedTask>& cbs_tasks,
                              double u_tt,
                              const std::vector<GapInterval>& gaps,
                              PerCpuSchedule& schedule,
                              InfeasibleError& error);

    // ── Step 5: Build protobuf table ──
    timpani::node::v1::HierarchicalScheduleTable build_table(
        const std::string& node_id,
        const std::map<int, PerCpuSchedule>& cpu_schedules,
        uint64_t hyperperiod_us);

    // ── Math utilities ──
    static uint64_t gcd(uint64_t a, uint64_t b);
    static uint64_t lcm(uint64_t a, uint64_t b);
    static uint64_t calculate_hyperperiod(const std::vector<uint64_t>& periods);
    static bool     validate_harmonic(const std::vector<uint64_t>& periods);

    std::shared_ptr<NodeConfigManager> node_config_manager_;
};

#endif // GLOBAL_SCHEDULER_H
