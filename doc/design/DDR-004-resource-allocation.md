<!--
SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
SPDX-License-Identifier: MIT
-->

# DDR-004: TIMPANI-O Resource Allocation Algorithm

**Date:** 2026-04-24 (Last Updated: 2026-06-15)
**Status:** Accepted
**Author:** Human (Lead Architect) + AI

---

## 1. Decision Summary

| Item | Decision |
|:--|:--|
| Scheduling mode | Partitioned Scheduling (no task migration) |
| Node selection | **Pullpiri's responsibility** — timpani receives `target_node` (see §6) |
| CPU assignment | timpani-o assigns CPUs within the specified node |
| U_bound | **TBD** — tiered by L1–L4, values to be determined |
| Hyperperiod control | Period constraints to be defined (see §8.A) |
| Failure policy | **TBD** — to be discussed |

---

## 2. Context

TIMPANI-O receives `WorkloadSpec[]` and `NodeTopology`, generates `HierarchicalScheduleTable` per node.
Partitioned Scheduling chosen because task migration causes cache misses and jitter unsuitable for vehicle RT.

---

## 3. Algorithm Flow

```
Input: WorkloadSpec[] (with target_node) + NodeTopology
  ↓
Phase 1: L1–L4 Classification (DDR-001 2-axis mapping)
  ↓
Phase 2: Pre-Feasibility Check (per-task utilization)
  ↓
Phase 3: Node Validation (verify target_node exists)
  ↓
Phase 4: CPU Assignment (within specified node)
  ↓
Phase 5: Schedule Table Generation
  5-A: Period constraint validation (§8.A)
  5-B: TT slot array for L1 (§8.B)
  5-C: CBS budget for L2 (§8.C)
  ↓
Phase 6: Epoch Computation (PTP-based epoch_ns)
  ↓
Output: HierarchicalScheduleTable × M (per node)
```

---

## 4. Phase 1: L1–L4 Classification

Uses the 2-axis model from [DDR-001](DDR-001-workload-model.md):

| TemporalClass | Criticality | L1–L4 | Scheduling Type | CPU |
|:--|:--|:--|:--|:--|
| Periodic | SafetyCritical | L1 | TT_SLOT | Isolated (dedicated) |
| Sporadic | SafetyCritical | L2 | CBS | Isolated (shared pool) |
| Any | NonSafety | L3/L4 | BEST_EFFORT | Non-Isolated |

**Validation**:
- L1/L2: `task_specs` required. Returns `ValidationError` if empty.
- L3/L4: `task_specs` ignored (cgroup quota only).

---

## 5. Phase 2: Pre-Feasibility Check

```
Per L1–L4 layer:
  required_util  = Σ (task_j.wcet_us / task_j.period_us)
  available_util = total_cpus_in_layer × U_bound(layer)

  if required_util > available_util → reject immediately
```

### U_bound (TBD)

> U_bound values are not yet finalized. The following are initial candidates for discussion.

| Layer | U_bound (candidate) | Headroom rationale |
|:--|:--|:--|
| L1 | 0.80 | OS interrupts + PTP + safety margin |
| L2 | 0.85 | OS overhead + CBS overhead |
| L3/L4 | 0.90 | cgroup quota tolerance |

---

## 6. Phase 3: Node Validation (Role Clarification)

### What timpani Does NOT Do: Node Selection

> **Architecture Decision**: Deciding which node a workload runs on is **NOT timpani's responsibility**.

In SDV/vehicle environments, workloads are designed to run on specific nodes at build-time or deployment:
- Brake controller → Brake ECU
- Camera processing → Vision ECU
- Cross-node migration is meaningless for RT workloads (cache misses, latency)

| Role | Owner | Description |
|:--|:--|:--|
| **Node selection** | Pullpiri / Build system | Decides which node runs a workload |
| **Node info delivery** | Pullpiri → timpani-o | `WorkloadSpec.target_node` (required field) |
| **CPU assignment within node** | timpani-o | Assigns L1/L2 CPUs within the specified node |

### What timpani Does: CPU Assignment Within Node

timpani-o receives node information from Pullpiri and:
1. Validates node exists via gRPC `NodeReady`
2. L1 workloads: Exclusive assignment to isolated CPUs
3. L2 workloads: Shared isolated CPU pool
4. L3/L4: Not managed by timpani

```protobuf
message WorkloadSpec {
  string target_node = ...;  // Required, specified by Pullpiri
  ...
}
```

> **Deprecated**: `preferred_node` has been removed. The "soft hint" concept is unsuitable for RT workloads.
> Nodes must be explicitly specified by Pullpiri; timpani-o uses them as-is.

---

## 7. Phase 4: CPU Assignment

| Layer | Assignment |
|:--|:--|
| L1 | Dedicated CPU per workload (cpuset isolated, exclusive) |
| L2 | Shared Isolated CPU pool (sched_ext selects at runtime) |
| L3/L4 | Non-Isolated CPU, cgroup quota only |

---

## 8. Phase 5: Schedule Table Generation

### 8.A: Period Constraints and Harmonic Periods

#### Problem: Hyperperiod Explosion

The hyperperiod is the Least Common Multiple (LCM) of all task periods. Without constraints, arbitrary periods can cause **hyperperiod explosion**:

| Task Periods | Hyperperiod (LCM) | Schedule Table Size |
|:--|:--|:--|
| 10ms, 20ms, 40ms | 40ms | Small (4 slots) |
| 10ms, 33ms | 330ms | Large (33 slots) |
| 7ms, 11ms, 13ms | 1001ms | Very Large (143 slots) |

**Problems with Hyperperiod Explosion**:
- Increased schedule table memory usage
- Longer table generation time
- May exceed BPF map size limits
- Reduced runtime predictability

#### Solution: Harmonic Period Constraint

**Harmonic periods** means all task periods are **powers of 2 multiples** of a base tick.

```
base_tick = 100μs (example)
Allowed periods: 100μs, 200μs, 400μs, 800μs, 1600μs, ...
               = base_tick × 2^k  (k = 0, 1, 2, ...)
```

**Mathematical Property of Harmonic Periods**:
```
If all period_i = base_tick × 2^k_i, then:
  → LCM(period_1, period_2, ...) = max(period_i)
```

| Example | Periods | Hyperperiod |
|:--|:--|:--|
| **Harmonic** | 1ms, 2ms, 4ms | 4ms (= max) |
| **Harmonic** | 100μs, 400μs, 1600μs | 1600μs (= max) |
| **Non-Harmonic** | 3ms, 5ms | 15ms (= 3 × 5) |

#### Constraint Enforcement Policy (TBD)

> Enforcement policy and base_tick value are not yet finalized.

| Policy Option | Description | Trade-offs |
|:--|:--|:--|
| **Strict** | Reject non-harmonic periods | Safe but restrictive |
| **Warn** | Warn only, proceed | Flexible but may explode |
| **Limit** | Reject if hyperperiod exceeds threshold | Compromise (e.g., 10s limit) |

**Current Implementation Status**:
- `HyperperiodManager`: Computes LCM, warns if exceeds 1 hour
- Harmonic validation logic: **Not implemented** (WBS O-3.6)

#### Validation Algorithm (Target: O-3.6)

```cpp
bool is_harmonic_set(const std::vector<uint64_t>& periods, uint64_t base_tick) {
    for (uint64_t period : periods) {
        if (period < base_tick) return false;
        uint64_t ratio = period / base_tick;
        // Check if ratio is a power of 2
        if ((ratio & (ratio - 1)) != 0) return false;
    }
    return true;
}
```

### 8.B: TT Slot Array (L1 Periodic Tasks)

> **Current Status**: Algorithm outline defined. **Detailed implementation is Open** (WBS O-3.5)

#### TT vs ET Scheduling

| Aspect | Event-Triggered (ET) | **Time-Triggered (TT)** |
|:--|:--|:--|
| **Decision point** | Runtime (on event arrival) | **Offline** (pre-computed) |
| **Algorithm** | RM, DM, EDF (priority-based) | **Slot Offset Calculation** (static placement) |
| **Jitter source** | Preemption, interference, arrival variance | **Near zero** (fixed offsets) |
| **Target** | L2~L4 | **L1 (Strict Deterministic)** |

**Key Difference**:
- **ET**: Runtime decision "who runs first?" based on priority → Jitter possible
- **TT**: Offline calculation "when to run?" with fixed offsets → Jitter minimized

> For L1 workloads, **TT scheduling** is applied to minimize jitter.
> RM/DM/EDF used as **runtime scheduling** are unsuitable for L1.
> However, RM/DM principles can be applied as **slot placement heuristics**.

#### Algorithm Overview

```
① Per-workload hyperperiod: W.hp = LCM(task periods)
② Global hyperperiod (per CPU): global_hp = LCM(W.hp for all workloads on CPU)
③ Sort tasks (placement order — RM/DM heuristic applicable) → generate slots
④ Assign slot offsets ensuring no overlap (collision avoidance)
```

#### TT Slot Placement Details (TBD)

**Step 1: Task Sorting (Placement Order)**

> This is **NOT runtime scheduling priority**.
> It determines **in what order to place slots** during offline computation.
>
> **Placement order affects jitter**:
> - Shorter period tasks placed first → repetitions distributed evenly
> - Longer period tasks placed first → short period slots become uneven → jitter worsens

**Placement Order Heuristics (Applying RM/DM Principles)**:

| Heuristic | Sort Criterion | Jitter Impact | Description |
|:--|:--|:--|:--|
| **RM Principle** | Shortest period first | ✅ Favorable | High-frequency tasks get even distribution |
| **DM Principle** | Shortest deadline first | ✅ Favorable | Tightly constrained tasks get better positions |
| First-Fit | Input order | ❌ Unfavorable | Random placement |
| BFD | Largest WCET first | ⚠️ Neutral | Bin packing optimization (jitter-agnostic) |

**Example: Applying RM Principle**

```
Tasks:
  A: period=4ms, wcet=1ms
  B: period=2ms, wcet=0.5ms  ← shortest period → placed first
  C: period=8ms, wcet=2ms

Hyperperiod = 8ms

RM-order placement:
  1) B (period=2ms): offset=[0, 2, 4, 6] → evenly distributed
  2) A (period=4ms): offset=[0.5, 4.5]  → placed between B slots
  3) C (period=8ms): offset=[1.5]       → remaining space

Result:
  0    1    2    3    4    5    6    7    8
  B----A----B---------B----A----B---------
       C---------

→ B's slot spacing is uniform (2ms) → Jitter minimized
```

**Recommendation**: Apply **RM or DM principle** as slot placement heuristic

```cpp
// RM principle: shortest period first
tasks.sort_by(|a, b| a.period_us.cmp(&b.period_us));

// DM principle: shortest deadline first
tasks.sort_by(|a, b| a.deadline_us.cmp(&b.deadline_us));
```

**Step 2: Slot Offset Calculation (Core)**

```
for each task in sorted_tasks:  // tasks sorted by RM/DM order
    repetitions = global_hp / task.period_us
    for i in 0..repetitions:
        // base offset: period multiple
        base_offset = i * task.period_us
        // find non-overlapping offset
        slot.offset_us = find_non_overlapping_offset(base_offset, task.wcet_us)
        slot.duration_us = task.wcet_us
        slot.deadline_us = base_offset + task.deadline_us
        slots.push(slot)
```

**Step 3: Collision Avoidance (TBD)**

> When multiple slots overlap on the same CPU, handling strategy needs to be defined.

| Strategy | Description |
|:--|:--|
| **Shift** | Move overlapping slot later within deadline |
| **Reject** | Fail feasibility check |
| **Multi-CPU** | Distribute to another CPU |

**Open Items (Target: WBS O-3.5)**:
- [ ] Finalize slot offset calculation algorithm
- [ ] Decide collision avoidance strategy
- [ ] Finalize task sorting criterion
- [ ] Slack time utilization strategy

### 8.C: CBS Budget (L2 Sporadic Tasks)

CBS budget calculation is relatively straightforward.

```
For each Sporadic task in L2:
  Cs = wcet_us                   // Server budget
  Ts = min_inter_arrival_us      // Replenishment period
  → CbsConfig { budget_us: Cs, period_us: Ts, deadline_us }
```

**CBS Utilization Verification**:
```
U_cbs = Σ (Cs_i / Ts_i) for all L2 tasks on CPU
if U_cbs > U_bound(L2) → reject
```

> CBS is exclusively for **L2 Sporadic** workloads. L3/L4 are delegated to CFS.

---

## 9. Phase 6: Epoch Computation

```
epoch_ns = CLOCK_REALTIME_now_ns() + propagation_margin
```

**Why propagation_margin?** After TIMPANI-O computes the schedule table, it must be transmitted to all TIMPANI-N nodes before execution begins. The margin accounts for:
- gRPC transmission latency to all nodes
- BPF map loading time on each node
- Clock synchronization settling time (PTP)

A margin that is too small risks nodes not being ready; too large wastes startup time. The default value is TBD (initial candidate: 500ms).

---

## 10. Failure Policy (TBD)

> Failure policy has not been finalized and requires further discussion.

Candidate approaches under consideration:

| Failure | Candidate action |
|:--|:--|
| L1 (Safety) placement failure | Option A: Abort all. Option B: Retry with relaxed constraints. |
| L2–L4 placement failure | Reject only that workload, continue remaining. |

---

## 11. Affected Components

| Component | Changes |
|:--|:--|
| `timpani-o/src/` | Resource allocation algorithm, feasibility checks, config |
| `timpani-o/proto/` | WorkloadSpec (DDR-003) |

---

## 12. Open Items

- [x] ~~Node placement algorithm finalization~~ → Not timpani's role (see §6). Node selection is Pullpiri's responsibility.
- [ ] U_bound values determination
- [ ] Failure policy decision
- [ ] Period constraint policy decision: Strict / Warn / Limit (see §8.A)
- [ ] `base_tick` value (candidate: 100μs)
- [ ] `propagation_margin` value (candidate: 500ms)
- [ ] TT Slot placement algorithm (WBS O-3.5): Sorting criterion, collision avoidance (see §8.B)
- [ ] Harmonic period validation implementation (WBS O-3.6)
- [x] ~~NodeTopology collection~~ → DDR-006: `NodeReady` via gRPC `NodeStream`
- [x] ~~Harmonic period concept documentation~~ (§8.A)
- [x] ~~TT vs ET distinction and RM/DM heuristic documentation~~ (§8.B)
