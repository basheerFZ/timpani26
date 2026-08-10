<!--
SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
SPDX-License-Identifier: MIT
-->

# DDR-002: Scheduling Architecture (HSF + sched_ext)

**Date:** 2026-04-24
**Status:** Accepted
**Author:** Human (Lead Architect) + AI

---

## 1. Decision

Linux RT scheduling policies (SCHED_FIFO/RR/DEADLINE) are not used directly.
Instead, **HSF (Hierarchical Scheduling Framework)** is implemented as a **sched_ext BPF scheduler**,
with **cgroup v2 cpuset isolated partition** for hardware isolation.
TT slot precision is achieved via the **Timer Master pattern**.

---

## 2. Context

### Problems with the Existing Approach

| Problem | Cause |
|:--|:--|
| Per-thread policy complexity | Each thread must be individually configured as SCHED_FIFO |
| Jitter | User-level SCHED_DEADLINE/CBS implementation causes unavoidable latency |
| App code modification | Applications must set thread priorities themselves |
| Mixed-criticality isolation impossible | Simple priority is insufficient for FFI requirements |

### Why sched_ext

- Kernel scheduler replaced with BPF → **no app code changes required**
- Automatic `cgroup → sched_ext domain` mapping → no per-thread configuration
- Full dispatch timing control in `ops.dispatch()`
- CBS, TT, budget enforcement all implemented within BPF

---

## 3. Design Principles

1. **Pullpiri-led role separation**: Timpani guarantees real-time behavior only for L1/L2 workloads. All other control authority belongs to Pullpiri.
2. **Offline/online separation**: Static schedule table generation (offline) and budget enforcement (online) are separated to minimize runtime complexity.
3. **Single lightweight Execution Engine**: The kernel engine (`scx_timpani`) performs no heuristic computation — it enforces pre-generated table commands only. This is a design choice for ISO 26262 FFI.

---

## 4. HSF 3-Level Tree

```
Level 0 (Root)
└── Static Schedule Table — generated offline by Timpani-O

    Level 1 (Intermediate)
    └── Timpani-N — receives table, enforces budgets on workloads

        Level 2 (Leaf)
        └── Workload — operates its own scheduler within delegated budget
```

| Level | Role | HSF Rationale |
|:--|:--|:--|
| Level 0 | Static Schedule Table (offline) | Global resource allocation pre-confirmed |
| Level 1 | Timpani-N (online enforcement) | FFI: preempts on budget overrun |
| Level 2 | Workload (black box) | Compositionality: integrated without knowing internals |

---

## 5. CPU Isolation

```
┌──────────────────────────┬──────────────────────────┐
│     Isolated CPU         │    Non-Isolated CPU      │
│    (Timpani-N managed)   │    (Linux CFS managed)   │
│                          │                          │
│  L1: Periodic + Safety   │  L3: Best-Effort         │
│  L2: Sporadic + Safety   │  L4: Background          │
│                          │                          │
│  cgroup v2 cpuset        │  System tasks included   │
│  isolated partition,     │                          │
│  nohz_full, rcu_nocbs    │                          │
└──────────────────────────┴──────────────────────────┘
```

L1/L2 and L3/L4 resource contention is fully prevented at the hardware level.

### cgroup Mapping

| L1–L4 | cgroup Implementation | Isolation |
|:--|:--|:--|
| L1 | cpuset isolated, dedicated CPU | Exclusive core |
| L2 | cpuset isolated, shared CPU pool | Budget enforced by sched_ext |
| L3 | Non-Isolated CPU, cgroup weight | CFS delegation |
| L4 | Non-Isolated CPU, lowest queue | CFS, tens of ms latency allowed |

---

## 6. TIMPANI-N Runtime Architecture

```
TIMPANI-N Daemon (C++, userspace)
│
├── [Timer Master Thread]
│     SCHED_FIFO, dedicated CPU pin
│     clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, epoch_ns + slot_offset)
│     → updates current_slot_map → scx_bpf_kick_cpu()
│
├── [BPF Loader]
│     Receives HierarchicalScheduleTable from TIMPANI-O
│     Updates BPF maps:
│       tt_table_map     — TT slot table
│       cbs_map          — CBS budget/state (Cs, Ts)
│       partition_map    — cgroup_id → L1–L4
│       current_slot_map — active TT slot
│
└── [Fault Monitor]
      fault_ringbuf polling → FaultNotification to TIMPANI-O

sched_ext BPF Scheduler (kernel)
│
├── ops.select_cpu()    L1/L2 → Isolated CPU, L3/L4 → Non-Isolated CPU
├── ops.enqueue()       L1: TT slot check, L2: CBS budget check, L3/L4: BE queue
├── ops.dispatch()      TT task → immediate dispatch, CBS → budget-based dispatch
├── ops.running()       CBS: record execution start
└── ops.stopping()      CBS: deduct budget, check deadline → fault_ringbuf on miss
```

---

## 7. Sporadic + Safety Dual-Mode (CBS + TT Floor)

`Sporadic + SafetyCritical` workloads are classified as **L2** (see [DDR-001](DDR-001-workload-model.md)). CBS alone may not fully satisfy ISO 26262 FFI. The **dual-mode** policy addresses this:

1. **CBS mode (default)**: On event arrival, run within CBS budget.
2. **TT Floor mode (supplement)**: Reserve a minimum guaranteed slot in the static table based on `min_inter_arrival_us`.
   - Floor slot size = WCET, period = `min_inter_arrival_us`
   - If event arrives before floor slot → run via CBS immediately
   - If no event arrives → watchdog inspection in floor slot

**Effect**: Periodic monitoring is guaranteed even when no event arrives (sensor dropout), satisfying FFI. Combines CBS flexibility with TT determinism.

---

## 8. Key Decisions Summary

| Item | Decision | Reason |
|:--|:--|:--|
| TT precision | Timer Master (SCHED_FIFO + ABSTIME) | BPF timer alone has excessive jitter |
| CBS implementation | BPF `ops.running/stopping` + `bpf_timer` replenishment | Avoids user-level CBS complexity |
| CBS scope | **L2 Sporadic workloads** | L1 uses TT, L3/L4 delegated to CFS |
| CPU isolation | cgroup v2 `cpuset.cpus.partition = "isolated"` | HW-level isolation for ISO 26262 FFI |
| Kernel requirement | PREEMPT_RT | Tens-of-μs precision |

---

## 9. Deployment Requirements

- Linux 6.12+ (sched_ext merged)
- PREEMPT_RT patch
- cgroup v2 mounted
- Yocto: `meta-realtime` layer

---

## 10. Affected Components

- `timpani-n/src/` — Runtime loop (Timer Master + BPF Loader + Fault Monitor)
- `timpani-n/src/bpf/` — sched_ext BPF scheduler
- `timpani-o/src/` — HierarchicalScheduleTable generation
