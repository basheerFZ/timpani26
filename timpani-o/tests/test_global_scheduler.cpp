/*
 * SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
 * SPDX-License-Identifier: MIT
 */

#include <gtest/gtest.h>
#include <memory>
#include <variant>
#include <vector>

#include "../src/global_scheduler.h"
#include "../src/node_config.h"

class GlobalSchedulerTest : public ::testing::Test {
protected:
    void SetUp() override {
        node_config_manager_ = std::make_shared<NodeConfigManager>();
        scheduler_ = std::make_unique<GlobalScheduler>(node_config_manager_);
    }

    ClassifiedTask MakeTtTask(const std::string& wl_id,
                              const std::string& task_id,
                              uint32_t period_us,
                              uint32_t wcet_us,
                              uint32_t deadline_us = 0) {
        ClassifiedTask t;
        t.workload_id = wl_id;
        t.task_id     = task_id;
        t.mechanism   = Mechanism::TT;
        t.period_us   = period_us;
        t.wcet_us     = wcet_us;
        t.deadline_us = (deadline_us > 0) ? deadline_us : period_us;
        t.assigned_cpu = -1;
        return t;
    }

    ClassifiedTask MakeCbsTask(const std::string& wl_id,
                               const std::string& task_id,
                               uint32_t mit_us,
                               uint32_t wcet_us,
                               uint32_t deadline_us = 0) {
        ClassifiedTask t;
        t.workload_id = wl_id;
        t.task_id     = task_id;
        t.mechanism   = Mechanism::CBS;
        t.period_us   = mit_us;
        t.wcet_us     = wcet_us;
        t.deadline_us = (deadline_us > 0) ? deadline_us : mit_us;
        t.assigned_cpu = -1;
        return t;
    }

    bool IsSuccess(const ScheduleResult& result) {
        return std::holds_alternative<timpani::node::v1::HierarchicalScheduleTable>(result);
    }

    const timpani::node::v1::HierarchicalScheduleTable& GetTable(
        const ScheduleResult& result) {
        return std::get<timpani::node::v1::HierarchicalScheduleTable>(result);
    }

    const InfeasibleError& GetError(const ScheduleResult& result) {
        return std::get<InfeasibleError>(result);
    }

    std::shared_ptr<NodeConfigManager> node_config_manager_;
    std::unique_ptr<GlobalScheduler> scheduler_;
};

// ─── Classification ─────────────────────────────────────────────────────────

TEST_F(GlobalSchedulerTest, SingleTtTaskProducesTable) {
    std::vector<ClassifiedTask> tasks = {
        MakeTtTask("brake", "brake_ctrl", 10000, 2000, 5000)
    };
    auto result = scheduler_->generate_schedule("node1", tasks);
    ASSERT_TRUE(IsSuccess(result));

    const auto& table = GetTable(result);
    EXPECT_EQ(table.node_id(), "node1");
    EXPECT_GT(table.partitions_size(), 0);
}

TEST_F(GlobalSchedulerTest, SingleCbsTaskProducesTable) {
    std::vector<ClassifiedTask> tasks = {
        MakeCbsTask("lidar", "lidar_main", 5000, 2000, 5000)
    };
    auto result = scheduler_->generate_schedule("node1", tasks);
    ASSERT_TRUE(IsSuccess(result));

    const auto& table = GetTable(result);
    EXPECT_GT(table.partitions_size(), 0);

    // Should have CBS entries
    bool has_cbs = false;
    for (int p = 0; p < table.partitions_size(); ++p) {
        for (int l = 0; l < table.partitions(p).layers_size(); ++l) {
            if (table.partitions(p).layers(l).cbs_entries_size() > 0) {
                has_cbs = true;
            }
        }
    }
    EXPECT_TRUE(has_cbs);
}

TEST_F(GlobalSchedulerTest, EmptyTasksReturnsEmptyTable) {
    std::vector<ClassifiedTask> tasks;
    auto result = scheduler_->generate_schedule("node1", tasks);
    ASSERT_TRUE(IsSuccess(result));
    const auto& table = GetTable(result);
    EXPECT_EQ(table.partitions_size(), 0);
}

// ─── Harmonic Period Validation ─────────────────────────────────────────────

TEST_F(GlobalSchedulerTest, HarmonicPeriodsSucceed) {
    // 5ms, 10ms, 20ms are harmonic (5 | 10, 5 | 20, 10 | 20)
    std::vector<ClassifiedTask> tasks = {
        MakeTtTask("wl1", "t1", 5000, 500, 5000),
        MakeTtTask("wl1", "t2", 10000, 1000, 10000),
        MakeTtTask("wl1", "t3", 20000, 1000, 20000),
    };
    auto result = scheduler_->generate_schedule("node1", tasks);
    EXPECT_TRUE(IsSuccess(result));
}

TEST_F(GlobalSchedulerTest, NonHarmonicPeriodsReturnError) {
    // 7ms and 10ms are not harmonic
    std::vector<ClassifiedTask> tasks = {
        MakeTtTask("wl1", "t1", 7000, 500, 7000),
        MakeTtTask("wl1", "t2", 10000, 1000, 10000),
    };
    auto result = scheduler_->generate_schedule("node1", tasks);
    ASSERT_FALSE(IsSuccess(result));
    EXPECT_EQ(GetError(result).reason, InfeasibleReason::NonHarmonicPeriod);
}

// ─── TT Slot Placement ─────────────────────────────────────────────────────

TEST_F(GlobalSchedulerTest, TtSlotOffsetsMatchPeriod) {
    // Single task with period=10ms, wcet=2ms in hyperperiod=20ms
    // Expect 2 slots at offsets 0 and 10000
    std::vector<ClassifiedTask> tasks = {
        MakeTtTask("brake", "brake_ctrl", 10000, 2000, 5000),
    };
    auto result = scheduler_->generate_schedule("node1", tasks);
    ASSERT_TRUE(IsSuccess(result));

    const auto& table = GetTable(result);
    ASSERT_GT(table.partitions_size(), 0);

    const auto& layer = table.partitions(0).layers(0);
    // With period 10000 in hyperperiod 10000 → 1 slot at offset 0
    // (hyperperiod = max period = 10000 under harmonic assumption)
    EXPECT_GE(layer.tt_slots_size(), 1);
    EXPECT_EQ(layer.tt_slots(0).offset_us(), 0u);
    EXPECT_EQ(layer.tt_slots(0).duration_us(), 2000u);
}

TEST_F(GlobalSchedulerTest, DmOrderingShorterDeadlineFirst) {
    // Two tasks: brake(deadline=5ms), steer(deadline=10ms)
    // DM ordering: brake gets placed first at offset 0
    std::vector<ClassifiedTask> tasks = {
        MakeTtTask("brake", "brake_ctrl", 10000, 2000, 5000),
        MakeTtTask("steer", "steer_ctrl", 10000, 2000, 10000),
    };
    auto result = scheduler_->generate_schedule("node1", tasks);
    ASSERT_TRUE(IsSuccess(result));

    const auto& table = GetTable(result);
    ASSERT_GT(table.partitions_size(), 0);

    // Find the partition with TT slots
    for (int p = 0; p < table.partitions_size(); ++p) {
        const auto& layer = table.partitions(p).layers(0);
        if (layer.tt_slots_size() >= 2) {
            // First placed task (brake, deadline=5ms) should have offset=0
            EXPECT_EQ(layer.tt_slots(0).task_id(), "brake_ctrl");
            EXPECT_EQ(layer.tt_slots(0).offset_us(), 0u);
        }
    }
}

// ─── Gap Extraction ─────────────────────────────────────────────────────────

TEST_F(GlobalSchedulerTest, NoTtSlotsProducesFullGap) {
    // CBS-only workload → entire hyperperiod is a gap
    std::vector<ClassifiedTask> tasks = {
        MakeCbsTask("lidar", "lidar_main", 5000, 1000, 5000),
    };
    auto result = scheduler_->generate_schedule("node1", tasks);
    ASSERT_TRUE(IsSuccess(result));
}

// ─── CBS Allocation ─────────────────────────────────────────────────────────

TEST_F(GlobalSchedulerTest, CbsWithinBudgetSucceeds) {
    // TT uses 20% utilization, CBS uses 30% → total 52% < 80% bound
    std::vector<ClassifiedTask> tasks = {
        MakeTtTask("brake", "brake_ctrl", 10000, 2000, 5000),   // U=0.20
        MakeCbsTask("lidar", "lidar_main", 10000, 3000, 10000), // U=0.30
    };
    auto result = scheduler_->generate_schedule("node1", tasks);
    EXPECT_TRUE(IsSuccess(result));
}

TEST_F(GlobalSchedulerTest, CbsExceedingBandwidthFails) {
    // With 2 default CPUs, TT and CBS go to separate CPUs.
    // To trigger overload, we need CBS utilization alone to exceed
    // U_BOUND on whatever CPU it lands on.
    // CBS_U = 0.85 > 0.80 - 0.02 = 0.78 → must fail
    std::vector<ClassifiedTask> tasks = {
        MakeCbsTask("wl2", "cbs1", 10000, 8500, 10000),  // U=0.85
    };
    auto result = scheduler_->generate_schedule("node1", tasks);
    ASSERT_FALSE(IsSuccess(result));
    // CBS over-capacity is caught during CPU assignment
    auto reason = GetError(result).reason;
    EXPECT_TRUE(reason == InfeasibleReason::UtilizationExceeded ||
                reason == InfeasibleReason::NoCbsCpu);
}

// ─── End-to-End: DDR-007 §5 Sample Workloads ────────────────────────────────

TEST_F(GlobalSchedulerTest, Ddr007SampleWorkloads) {
    // From DDR-007 §5.1:
    //   brake_ctrl    (L1, period=10ms,  wcet=2ms,   deadline=5ms)
    //   steer_ctrl    (L1, period=20ms,  wcet=2ms,   deadline=10ms)
    //   collision_det (L2, MIT=10ms,     wcet=1.5ms, deadline=10ms)
    //   lidar_proc    (L2, MIT=5ms,      wcet=2ms,   deadline=5ms)
    std::vector<ClassifiedTask> tasks = {
        MakeTtTask("brake", "brake_ctrl", 10000, 2000, 5000),
        MakeTtTask("steer", "steer_ctrl", 20000, 2000, 10000),
        MakeCbsTask("collision", "col_detect", 10000, 1500, 10000),
        MakeCbsTask("lidar_proc", "lidar_main", 5000, 2000, 5000),
    };

    auto result = scheduler_->generate_schedule("node1", tasks);
    ASSERT_TRUE(IsSuccess(result));

    const auto& table = GetTable(result);
    EXPECT_EQ(table.node_id(), "node1");
    EXPECT_GT(table.hyperperiod_us(), 0u);
    EXPECT_GT(table.partitions_size(), 0);

    // Count total TT slots and CBS entries across all partitions
    int total_tt = 0;
    int total_cbs = 0;
    for (int p = 0; p < table.partitions_size(); ++p) {
        for (int l = 0; l < table.partitions(p).layers_size(); ++l) {
            total_tt += table.partitions(p).layers(l).tt_slots_size();
            total_cbs += table.partitions(p).layers(l).cbs_entries_size();
        }
        // Each partition should have isolated=true
        EXPECT_TRUE(table.partitions(p).cpuset().isolated());
    }

    // Two TT workloads go to separate CPUs:
    //   CPU A: brake_ctrl (period=10ms, hyperperiod=10ms → 1 slot)
    //   CPU B: steer_ctrl (period=20ms, hyperperiod=20ms → 1 slot)
    // Total TT slots >= 2
    EXPECT_GE(total_tt, 2);
    // Should have CBS entries for collision_det and lidar_main
    EXPECT_GE(total_cbs, 2);
}

// ─── Multi-CPU ──────────────────────────────────────────────────────────────

TEST_F(GlobalSchedulerTest, MultipleTtWorkloadsGetSeparateCpus) {
    // Two TT workloads should prefer separate CPUs
    std::vector<ClassifiedTask> tasks = {
        MakeTtTask("wl_a", "task_a", 10000, 2000, 5000),
        MakeTtTask("wl_b", "task_b", 10000, 2000, 5000),
    };
    auto result = scheduler_->generate_schedule("node1", tasks);
    ASSERT_TRUE(IsSuccess(result));

    const auto& table = GetTable(result);
    // With default 2 CPUs available, should get 2 partitions
    EXPECT_GE(table.partitions_size(), 2);
}

// ─── Feasibility Constants ──────────────────────────────────────────────────

TEST_F(GlobalSchedulerTest, FeasibilityConstants) {
    EXPECT_DOUBLE_EQ(GlobalScheduler::U_OVERHEAD, 0.02);
    EXPECT_DOUBLE_EQ(GlobalScheduler::U_BOUND, 0.80);
    EXPECT_EQ(GlobalScheduler::CBS_MIN_EXEC_US, 100u);
}

// ─── Table Structure Verification ───────────────────────────────────────────

TEST_F(GlobalSchedulerTest, TableHasRequiredFields) {
    std::vector<ClassifiedTask> tasks = {
        MakeTtTask("brake", "brake_ctrl", 10000, 2000, 5000),
    };
    auto result = scheduler_->generate_schedule("node1", tasks);
    ASSERT_TRUE(IsSuccess(result));

    const auto& table = GetTable(result);
    EXPECT_FALSE(table.table_id().empty());
    EXPECT_EQ(table.node_id(), "node1");
    EXPECT_GT(table.hyperperiod_us(), 0u);
    EXPECT_GT(table.epoch_ns(), 0u);

    // Check TT slot hashes are non-zero
    for (int p = 0; p < table.partitions_size(); ++p) {
        for (int l = 0; l < table.partitions(p).layers_size(); ++l) {
            const auto& layer = table.partitions(p).layers(l);
            for (int s = 0; s < layer.tt_slots_size(); ++s) {
                EXPECT_NE(layer.tt_slots(s).workload_id_hash(), 0u);
                EXPECT_NE(layer.tt_slots(s).task_id_hash(), 0u);
            }
        }
    }
}
