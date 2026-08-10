<!--
SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
SPDX-License-Identifier: MIT
-->

# DDR-003: Interface / Protocol Design

**Date:** 2026-04-24 (Last Updated: 2026-06-15)
**Status:** Accepted
**Author:** Human (Lead Architect) + AI

---

## 1. Decision

TIMPANI's interfaces are separated into three layers:
1. **WorkloadSpec** (Pullpiri → TIMPANI-O): workload attribute declaration
2. **HierarchicalScheduleTable** (TIMPANI-O → TIMPANI-N): computed execution table
3. **FaultNotification** (TIMPANI-O → Pullpiri): fault reporting

Workload lifecycle actions supported:
- **STOP**: Graceful workload termination (priority implementation)
- **RESTART**: Workload restart after fault or manual request (future)

Time synchronization is based on **PTP (IEEE 1588)** with a shared `epoch_ns`.

---

## 2. Context

### Interface Separation Rationale

| Layer | Knows | Produces |
|:--|:--|:--|
| Pullpiri | Workload **attributes** | WorkloadSpec |
| TIMPANI-O | Resource allocation + schedule **computation** | HierarchicalScheduleTable |
| TIMPANI-N | Table **enforcement** only | FaultNotification |

### Deployment Flow

```
1. Pullpiri: Deploy workload (same for L1~L4)
   └─ Container creation, cgroup cpuset setup (Pullpiri's responsibility)

2. Pullpiri → timpani-o: WorkloadSpec (gRPC)
   └─ Response: success/failure only (WorkloadResponse)
   └─ No need to return classification result (L1~L4) — Pullpiri already specified in YAML

3. timpani-o: Processes L1/L2 workloads only
   ├─ L1/L2: Generate Schedule Table → apply to timpani-n
   └─ L3/L4: Ignored (not managed by timpani)

4. timpani-n → timpani-o → Pullpiri: FaultNotification (async)
   └─ Runtime events: deadline miss, budget overrun, etc.
```

### CPU Isolation Responsibility

| Role | Owner | Notes |
|:--|:--|:--|
| **cgroup cpuset setup** | Pullpiri | Per-container CPU assignment |
| **Isolated CPU range definition** | System config | Pullpiri is aware of this |
| **Schedule Table application** | timpani | L1/L2 workloads only |

> **Design Principle**: timpani-o does not need to return cpuset info to Pullpiri.
> Pullpiri, as the cluster resource manager, already knows the CPU topology.

> **cgroup cpuset usage**: Using cgroup cpuset instead of `isolcpus` kernel parameter enables runtime dynamic management.

### Runtime Flow

```
Pullpiri → WorkloadSpec → TIMPANI-O → HierarchicalScheduleTable → TIMPANI-N → eBPF enforcement
                                                                        ↓
                                                               FaultNotification → TIMPANI-O → Pullpiri
```

### Workload Lifecycle

```
[Add Workload]
Pullpiri → AddWorkload(WorkloadSpec) → timpani-o
  → L1/L2: Schedule Table 생성 → timpani-n
  → L3/L4: Ignored (Pullpiri deploys directly via CFS)

[Stop Workload] (Priority Implementation)
Pullpiri → RemoveWorkload(WorkloadId) → timpani-o
  → timpani-o: Remove workload from Schedule Table
  → timpani-o: Recalculate hyperperiod (if needed)
  → timpani-o → timpani-n: Send ScheduleTableUpdate
  → timpani-n: Remove BPF map entries (task_meta_map, tt_table_map)
  → timpani-n: Release CBS budget (L2)
  → timpani-n: Confirm slot release

[Restart Workload] (Future)
Fault → FaultPolicy.action = RESTART
  → timpani-n → timpani-o: FaultNotification
  → timpani-o → Pullpiri: RestartRequest
  → Pullpiri: Container restart
  → Pullpiri → timpani-o: AddWorkload (re-register)
```

### Workload STOP Processing (timpani Internal)

| Component | Action | Details |
|:--|:--|:--|
| **timpani-o** | Remove from Schedule Table | Delete TT slots (L1) or CBS config (L2) |
| **timpani-o** | Hyperperiod recalculation | Recalculate if workload removal affects LCM |
| **timpani-o** | Send table update | `ScheduleTableUpdate.remove` to timpani-n |
| **timpani-n** | BPF map cleanup | Remove entries from `task_meta_map`, `tt_table_map`, `cbs_budget_map` |
| **timpani-n** | Slot release confirmation | Ensure no dangling references |
| **timpani-n** | Report completion | Ack to timpani-o |

> **Note**: STOP is processed immediately at hyperperiod boundary to ensure determinism.

> In the future, table generation may move offline. TIMPANI-N is unchanged — it enforces the table regardless of source. The message formats remain the same.

---

## 3. Interface 1: WorkloadSpec (Pullpiri → TIMPANI-O)

> Proto definitions below are illustrative examples. Final definitions are subject to change.

```protobuf
syntax = "proto3";
package timpani.workload.v1;

service WorkloadService {
  rpc AddWorkload    (WorkloadSpec)   returns (WorkloadResponse) {}
  rpc RemoveWorkload (WorkloadId)     returns (WorkloadResponse) {}
}

message WorkloadSpec {
  string           workload_id      = 1;
  string           name             = 2;
  TemporalClass    temporal_class   = 3;  // Periodic / Sporadic / Aperiodic
  CriticalityClass criticality      = 4;  // SafetyCritical / NonSafety → determines L1-L4
  repeated TaskSpec task_specs       = 5;  // L1/L2: required, L3/L4: ignored
  ContainerSpec    container        = 6;
  FaultPolicy      fault_policy     = 7;
  string           pipeline_id      = 8;  // optional — set when part of a Pipeline
}

message TaskSpec {
  string task_id              = 1;   // per-app identifier (e.g., "sensor_read")
  uint32 period_us            = 2;   // L1 only: TT period
  uint32 min_inter_arrival_us = 3;   // L2 only: minimum inter-arrival time (MIT)
  uint32 wcet_us              = 4;   // Worst-Case Execution Time (μs)
  uint32 deadline_us          = 5;   // relative deadline (μs)
}

enum TemporalClass {
  PERIODIC  = 0;  // L1: TT slot
  SPORADIC  = 1;  // L2: CBS
}

enum CriticalityClass {
  SAFETY_CRITICAL = 0;
  NON_SAFETY      = 1;
}

message ContainerSpec {
  string image       = 1;
  string target_node = 2;  // Required: execution node specified by Pullpiri (timpani does not select nodes)
}

message FaultPolicy {
  uint32      max_dmiss          = 1;
  FaultAction action_on_miss     = 2;
  uint32      watchdog_period_us = 3;  // 0 = disabled
}

enum FaultAction {
  NOTIFY  = 0;  // Notify only, no automatic action
  STOP    = 1;  // Graceful workload stop (priority implementation)
  RESTART = 2;  // Workload restart (future)
}

// Note: STOP vs RESTART
// - STOP: Graceful termination, release slots, remove from schedule table
// - RESTART: Stop + re-register workload (requires Pullpiri coordination)

message WorkloadId {
  string workload_id = 1;
}

message WorkloadResponse {
  int32  status  = 1;  // 0 = success, non-zero = error code
  string message = 2;  // error description (if any)
}
```

**WorkloadResponse Design Principles**:
- Returns success/failure only (no need to return L1~L4 classification result)
- Pullpiri already specified `temporal_class` + `criticality` in WorkloadSpec, so timpani does not "re-classify"
- L3/L4 workloads are ignored by timpani-o (no Schedule Table generation)

**Field Validation Rules**:

| Layer | `temporal_class` | `period_us` | `min_inter_arrival_us` |
|:--|:--|:--|:--|
| L1 | `PERIODIC` | **Required** | Ignored |
| L2 | `SPORADIC` | Ignored | **Required** |
| L3/L4 | — | Ignored | Ignored |
```

---

## 4. Interface 2: HierarchicalScheduleTable (TIMPANI-O → TIMPANI-N)

> **Communication change (DDR-006)**: `NodeService` below is replaced by `OrchestratorService.NodeStream` (bidirectional streaming gRPC). The `HierarchicalScheduleTable` message is retained and delivered via `ControlCommand.full_table`.

L1–L4 mapping to table entries:
- **L1 (Periodic + Safety)**: TT slots
- **L2 (Sporadic + Safety)**: CBS budget entries
- **L3/L4**: Not in table. CFS delegation on Non-Isolated CPU.

```protobuf
syntax = "proto3";
package timpani.node.v1;

message HierarchicalScheduleTable {
  string   table_id         = 1;
  string   node_id          = 2;
  uint64   hyperperiod_us   = 3;  // slot table repeat interval (μs)
  uint64   epoch_ns         = 4;  // PTP-based absolute start time (CLOCK_REALTIME ns)
  repeated PartitionConfig partitions = 5;
}

message PartitionConfig {
  string           partition_id  = 1;
  CpuSetSpec       cpuset        = 2;
  repeated TtSlot    tt_slots    = 3;  // L1 Periodic
  repeated CbsConfig cbs_entries = 4;  // L2 Sporadic
}

message CpuSetSpec {
  repeated uint32 cpus      = 1;
  bool            isolated  = 2;  // cpuset.cpus.partition = "isolated"
}

message TtSlot {
  string workload_id  = 1;
  string task_id      = 2;
  uint32 offset_us    = 3;  // offset from hyperperiod start
  uint32 duration_us  = 4;  // slot size = WCET
  uint32 deadline_us  = 5;
  uint32 cpu          = 6;
}

message CbsConfig {
  string workload_id  = 1;
  string task_id      = 2;
  uint32 budget_us    = 3;  // Cs: server budget (μs)
  uint32 period_us    = 4;  // Ts: replenishment period (μs)
  uint32 deadline_us  = 5;
}
```

---

## 5. Interface 3: FaultNotification (TIMPANI-N → TIMPANI-O → Pullpiri)

```protobuf
syntax = "proto3";
package timpani.fault.v1;

message FaultInfo {
  string    workload_id  = 1;
  string    node_id      = 2;
  string    task_name    = 3;
  FaultType type         = 4;
  uint64    timestamp_ns = 5;  // PTP-based occurrence time
  uint32    dmiss_count  = 6;  // cumulative deadline miss count
}

enum FaultType {
  UNKNOWN       = 0;
  DMISS         = 1;  // deadline miss
  BUDGET_EXCEED = 2;  // CBS budget exceeded (L2 only)
  WATCHDOG      = 3;  // watchdog timeout
}
```

---

## 6. PTP Time Synchronization

All TIMPANI-N nodes share a common `epoch_ns` derived from PTP-synchronized `CLOCK_REALTIME`.

```
1. TIMPANI-O computes: epoch_ns = now() + propagation_margin (e.g., +500ms)
2. epoch_ns included in HierarchicalScheduleTable sent to each TIMPANI-N
3. Timer Master: clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, epoch_ns)
4. epoch_ns reached → all nodes start hyperperiod simultaneously
```

This replaces the `SyncTimer` gRPC barrier approach used in Timpani 25.

---

## 7. Affected Components

| Component | Changes |
|:--|:--|
| `timpani-o/proto/` | WorkloadSpec replacement, HierarchicalScheduleTable addition |
| `timpani-o/src/` | WorkloadSpec processing, table generation |
| `timpani-n/src/` | PTP epoch wait logic, table enforcement |
| `pullpiri/proto/` | Proto synchronization needed |
