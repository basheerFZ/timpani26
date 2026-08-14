<!--
SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
SPDX-License-Identifier: MIT
-->

# DDR-007: L1(TT) + L2(CBS) Integrated Scheduling — Implementation on Isolated CPUs

**Date:** 2026-04-27
**Status:** Proposal
**Authors:** Human (Lead Architect) + AI

---

## 0. Scope and Prerequisites

This document **assumes** the following decisions established in
[DDR-001](DDR-001-workload-model.md) ~ [DDR-006](DDR-006-communication-architecture.md)
and [HSF-concept](HSF-concept.md).

| Prerequisite | Source |
|:--|:--|
| L1 = Periodic + SafetyCritical, **TT (Time-Triggered) as the sole mechanism** | DDR-001 §4 |
| L2 = Sporadic + SafetyCritical, **CBS (Constant Bandwidth Server) as the sole mechanism** | DDR-001 §4 |
| L3/L4 = NonSafety, delegated to CFS on Non-Isolated CPUs (out of scope of this DDR) | DDR-001 §4 |
| Partitioned Scheduling, task migration prohibited | DDR-004 §2 |
| sched_ext (`SCX_OPS_SWITCH_PARTIAL`) + Timer Master pattern | DDR-002, DDR-005 |
| HSF 3-Level (Root / Timpani-N / Workload) | HSF-concept §7 |

> **Key simplification (vs. earlier drafts):**
> This DDR **does not adopt** the L1 dual-mode (TT-floor + CBS) or the L2 Periodic (TT_CAPPED) concepts.
> - L1 is **always Periodic**, hence scheduled by **TT only**.
> - L2 is **always Sporadic**, hence scheduled by **CBS only**.
> - "L1 Sporadic" and "L2 Periodic" combinations are not defined by the workload model (DDR-001).
> - As a result, there is a single TT slot type and a single CBS DSQ — no further splitting is needed.

---

## 1. Problem Statement

DDR-001~006 define L1 (TT) and L2 (CBS) as **independent mechanisms**, but the
**integrated behavior of the two mechanisms sharing the same Isolated CPU** is
not yet specified. The following five questions must be answered.

| Question | Owner |
|:--|:--|
| Q1. How do L1 TT slots and L2 CBS servers share time on the same CPU? | Integrated model §2 |
| Q2. How does Timpani-O compute CBS available bandwidth after placing TT slots? | Timpani-O §3 |
| Q3. How does Timpani-N BPF arbitrate between TT firing and CBS execution at runtime? | Timpani-N §4 |
| Q4. How is a CBS budget replenishment that occurs during a TT slot handled? | §4.5 |
| Q5. How are L1/L2 placed across multiple Isolated CPUs? | §6 |

---

## 2. Integrated Scheduling Model

### 2.1 Time-Sharing Principle — "TT-first, CBS-in-gaps"

The time axis of an Isolated CPU is determined in two stages.

```
Step A (offline, Timpani-O):
  L1 TT slots are placed at fixed positions within the hyperperiod.
  → These slots are "reserved time" that never moves.

Step B (offline, Timpani-O):
  The empty intervals (Gaps) between TT slots are exactly the L2 CBS
  servers' executable time.
  → CBS feasibility is verified "within the Gap intervals".

Step C (runtime, Timpani-N):
  At every slot boundary, the Timer Master signals the BPF scheduler.
  TT slot active     → unconditionally dispatch the L1 task.
  TT slot inactive   → dispatch an L2 CBS task that has positive budget.
  (= Gap)
```

### 2.2 Time Axis Visualization

```
Isolated CPU, hyperperiod = 20ms (example)

  0ms     2ms    5ms    7ms   10ms   12ms   15ms   17ms   20ms
  │       │      │      │      │      │      │      │      │
  ├──TT──┤       ├──TT─┤       ├──TT──┤       ├──TT─┤      │
  │ L1-A │       │L1-B │       │ L1-A │       │L1-B │      │
  │      │       │     │       │      │       │     │      │
  │      ├──CBS──┤     ├─CBS──┤       ├──CBS──┤     ├─CBS─┤
  │      │ L2-X  │     │ L2-Y │       │ L2-X  │     │idle │
  │      │       │     │      │       │       │     │     │

  TT slot (L1):  offset/duration fixed offline by Timpani-O
  Gap (L2 CBS):  empty intervals between TT slots — CBS execution region
  idle:          when no CBS tasks are runnable or all budgets exhausted → CPU power-save
```

### 2.3 Dispatch Priority Chain

```
[Highest priority]
  Is a TT slot currently active? ──Yes──→ Dispatch L1 task immediately (non-preemptible)
        │
        No (= Gap)
        ↓
  Any task in CBS_DSQ with remaining budget? ──Yes──→ Dispatch L2 CBS task
        │
        No
        ↓
  Isolated CPU idle (L3/L4 run on different CPUs)
[Lowest priority]
```

### 2.4 HSF 3-Level Mapping

| HSF Level | Role in this DDR | Implementation site |
|:--|:--|:--|
| Level 0 (Root, offline) | Pre-compute L1 TT slots + L2 CBS budgets | Timpani-O §3 |
| Level 1 (Timpani-N) | Fire TT slots, enforce CBS budgets | BPF + Timer Master §4 |
| Level 2 (Workload) | Run within delegated slot/budget | DDR-005 User App Interface |

---

## 3. Timpani-O — Offline Integrated Schedule Generation

### 3.1 Overall Flow (5 Steps)

```
Input: WorkloadSpec[] (L1/L2 only, with task_specs)
       + NodeTopology (list of Isolated CPUs)
        │
        ▼
Step 1: Workload classification (L1 → TT / L2 → CBS)
        │
        ▼
Step 2: Isolated CPU assignment (Partitioned, see DDR-004 §6)
        │
        ▼
Step 3: TT slot placement (L1 Periodic, DM ordering, harmonic-period assumption)
        │
        ▼
Step 4: Gap analysis + CBS budget allocation (L2 Sporadic, feasibility check)
        │
        ▼
Step 5: Emit HierarchicalScheduleTable (DDR-003 messages)
```

### 3.2 Step 1 — Classification

The DDR-001 mapping rules apply directly. This DDR handles only L1/L2.

```rust
enum Mechanism {
    Tt,   // L1 Periodic only
    Cbs,  // L2 Sporadic only
}

struct ClassifiedTask {
    workload_id: String,
    task_id:     String,
    mechanism:   Mechanism,
    period_us:   u32,   // L1: period / L2: min_inter_arrival
    wcet_us:     u32,
    deadline_us: u32,
}

fn classify(spec: &WorkloadSpec) -> Result<Vec<ClassifiedTask>, ValidationError> {
    let mech = match (spec.temporal_class, spec.criticality) {
        (Periodic, SafetyCritical) => Mechanism::Tt,    // L1
        (Sporadic, SafetyCritical) => Mechanism::Cbs,   // L2
        _ => return Err(ValidationError::OutOfScope),    // L3/L4 or Aperiodic
    };
    Ok(spec.task_specs.iter().map(|t| ClassifiedTask {
        workload_id: spec.workload_id.clone(),
        task_id:     t.task_id.clone(),
        mechanism:   mech,
        period_us:   t.period_us,
        wcet_us:     t.wcet_us,
        deadline_us: t.deadline_us,
    }).collect())
}
```

### 3.3 Step 2 — Isolated CPU Assignment

Follows DDR-004 §7. Additional integrated criteria from this DDR:

```
Assignment priority (Partitioned Scheduling):

① L1 workload (TT)  → dedicated Isolated CPU per workload
   (DDR-002 §5: cpuset isolated, exclusive core ownership)

② L2 workload (CBS) → shared Isolated CPU pool
   (DDR-002 §5: cpuset isolated, sched_ext enforces budgets)

L2 task CPU selection (current Timpani-O policy):
  ─ CBS Us = wcet/period
  ─ For each candidate CPU c:
       residual_cap(c) = U_bound − U_tt(c) − U_cbs_assigned(c) − U_overhead
  ─ Among CPUs with residual_cap(c) ≥ Us, pick the one with the largest residual
       (Worst-Fit Decreasing — minimizes gap fragmentation)
```

### 3.4 Step 3 — L1 TT Slot Placement

This DDR uses **a single Phase with Deadline Monotonic (DM) ordering**.
All L1 tasks are SafetyCritical, so Phase splitting (Safety vs RT) is unnecessary.

#### Prerequisite — Harmonic Periods

This DDR **requires** the harmonic-period rule discussed in DDR-004 §8 (5-A).
Under this assumption, hyperperiod = max(period_i), and TT slot conflicts cannot occur.

```
Harmonic condition:
  ∀ task_i, task_j:  period_i divides period_j  OR  period_j divides period_i

Violation:
  Timpani-O reports InfeasibleError::NonHarmonicPeriod
  → Pullpiri rejects the workload
```

#### Placement Algorithm

```rust
fn place_l1_tt_slots(
    cpu: CpuId,
    tt_tasks: &[ClassifiedTask],   // L1 only
    hp_us: u64,                    // = max(period_i) under harmonic assumption
) -> Result<Vec<TtSlot>, InfeasibleError> {

    let mut timeline = Timeline::new(hp_us);  // tracks occupancy of [0, hp_us)

    // Deadline Monotonic ordering — shorter deadline gets the slot first
    let sorted: Vec<_> = tt_tasks.iter()
        .sorted_by_key(|t| t.deadline_us)
        .collect();

    for task in sorted {
        let repeats = hp_us / task.period_us as u64;
        for k in 0..repeats {
            // Default position: at the start of each period
            let preferred = (k as u64) * task.period_us as u64;

            // Under the harmonic assumption, placement should land exactly on `preferred`.
            // On conflict, search for the nearest free slot.
            let offset = timeline
                .place_at_or_nearest(preferred, task.wcet_us as u64)
                .ok_or_else(|| InfeasibleError::TtSlotConflict {
                    cpu,
                    task: task.task_id.clone(),
                })?;

            timeline.insert(TtSlot {
                workload_id: task.workload_id.clone(),
                task_id:     task.task_id.clone(),
                offset_us:   offset as u32,
                duration_us: task.wcet_us,
                deadline_us: task.deadline_us,
                cpu,
            });
        }
    }

    Ok(timeline.slots())
}
```

### 3.5 Step 4 — Gap Analysis + L2 CBS Budget Allocation

#### 4-A: Gap Extraction

A Gap is a time interval not occupied by any TT slot.

```rust
struct GapInterval { start_us: u32, end_us: u32, length_us: u32 }

fn compute_gaps(slots: &[TtSlot], hp_us: u64) -> Vec<GapInterval> {
    let mut gaps = Vec::new();
    let mut cursor = 0u32;
    let sorted: Vec<_> = slots.iter().sorted_by_key(|s| s.offset_us).collect();

    for slot in sorted {
        if cursor < slot.offset_us {
            gaps.push(GapInterval {
                start_us:  cursor,
                end_us:    slot.offset_us,
                length_us: slot.offset_us - cursor,
            });
        }
        cursor = slot.offset_us + slot.duration_us;
    }
    if (cursor as u64) < hp_us {
        gaps.push(GapInterval {
            start_us:  cursor,
            end_us:    hp_us as u32,
            length_us: hp_us as u32 - cursor,
        });
    }
    gaps
}
```

#### 4-B: Feasibility Conditions (per CPU)

```
Definitions (CPU k):
  U_tt(k)       = Σ (wcet_i / period_i)   ∀ L1 TT task i on CPU k
  U_cbs(k)      = Σ (Cs_j  / Ts_j)        ∀ L2 CBS server j on CPU k
  U_overhead    = 0.02                    (Timer Master + BPF dispatch)
  U_bound       = 0.80                    (when L1 is present, conservative — DDR-004 §5)

Feasibility:
  U_tt(k) + U_cbs(k) + U_overhead ≤ U_bound

Additional condition (sufficient gap):
  min_gap(k) ≥ CBS_MIN_EXEC_US   (e.g., 100μs)
    └ Gaps that are too short are unusable due to context-switch overhead
```

#### 4-C: CBS Parameter Derivation

Follows DDR-004 §8 (5-C).

```
For every L2 CBS task j:
  Cs_j = task.wcet_us               (server budget = WCET per arrival)
  Ts_j = task.min_inter_arrival_us  (replenishment period = MIT)
  Us_j = Cs_j / Ts_j

deadline_us  = task.deadline_us  (used by BPF runtime to detect dmiss)
```

#### 4-D: Allocation Algorithm (Safety CBS — non-rejectable)

L2 is also SafetyCritical, so budget reduction is not allowed.
On bandwidth shortage, an `InfeasibleError` is reported immediately so that
Pullpiri rejects the workload.

```rust
fn allocate_l2_cbs_budgets(
    cpu: CpuId,
    cbs_tasks: &[ClassifiedTask],   // L2 only
    u_tt: f64,
    gaps: &[GapInterval],
) -> Result<Vec<CbsConfig>, InfeasibleError> {

    let u_overhead = 0.02;
    let u_bound    = 0.80;
    let u_avail    = u_bound - u_tt - u_overhead;

    if u_avail <= 0.0 {
        return Err(InfeasibleError::NoCbsBandwidth { cpu, u_tt });
    }

    let min_gap = gaps.iter().map(|g| g.length_us).min().unwrap_or(0);
    if min_gap < CBS_MIN_EXEC_US {
        warn!("CPU {}: minimum gap {}μs < {}μs", cpu, min_gap, CBS_MIN_EXEC_US);
    }

    let mut configs = Vec::new();
    let mut u_alloc = 0.0f64;

    // Allocate in decreasing utilization order (fail fast)
    let sorted: Vec<_> = cbs_tasks.iter()
        .sorted_by(|a, b| {
            let ua = a.wcet_us as f64 / a.period_us as f64;
            let ub = b.wcet_us as f64 / b.period_us as f64;
            ub.partial_cmp(&ua).unwrap()
        })
        .collect();

    for task in sorted {
        let us = task.wcet_us as f64 / task.period_us as f64;
        if u_alloc + us > u_avail {
            // L2 is also Safety → cannot reject silently → fail immediately
            return Err(InfeasibleError::SafetyCbsExceeded {
                cpu,
                task: task.task_id.clone(),
                requested_us: us,
                available_us: u_avail - u_alloc,
            });
        }
        configs.push(CbsConfig {
            workload_id: task.workload_id.clone(),
            task_id:     task.task_id.clone(),
            budget_us:   task.wcet_us,
            period_us:   task.period_us,
            deadline_us: task.deadline_us,
        });
        u_alloc += us;
    }

    Ok(configs)
}
```

### 3.6 Step 5 — Emit HierarchicalScheduleTable

The DDR-003 message definitions are used as-is (no field additions needed).
TT slot type splitting (`TtSlotType`) and CBS dual-mode fields are not adopted.

```protobuf
// Identical to DDR-003 — no slot_type / dual_mode fields
message TtSlot {
  string workload_id = 1;
  string task_id     = 2;
  uint32 offset_us   = 3;
  uint32 duration_us = 4;
  uint32 deadline_us = 5;
  uint32 cpu         = 6;
}

message CbsConfig {
  string workload_id = 1;
  string task_id     = 2;
  uint32 budget_us   = 3;  // Cs
  uint32 period_us   = 4;  // Ts
  uint32 deadline_us = 5;
}
```

#### Output Example (JSON)

```json
{
  "table_id": "sched-001",
  "node_id": "ecu-front",
  "hyperperiod_us": 20000,
  "epoch_ns": 1809500000000000000,
  "partitions": [{
    "partition_id": "isolated-0",
    "cpuset": { "cpus": [2], "isolated": true },
    "tt_slots": [
      { "workload_id": "brake",         "task_id": "brake_ctrl",
        "offset_us": 0,     "duration_us": 2000, "deadline_us": 5000, "cpu": 2 },
      { "workload_id": "brake",         "task_id": "brake_ctrl",
        "offset_us": 10000, "duration_us": 2000, "deadline_us": 5000, "cpu": 2 },
      { "workload_id": "steer",         "task_id": "steer_ctrl",
        "offset_us": 5000,  "duration_us": 2000, "deadline_us": 10000, "cpu": 2 }
    ],
    "cbs_entries": [
      { "workload_id": "lidar_proc",    "task_id": "lidar_main",
        "budget_us": 2000, "period_us": 5000, "deadline_us": 5000 },
      { "workload_id": "collision",     "task_id": "col_detect",
        "budget_us": 1500, "period_us": 10000, "deadline_us": 10000 }
    ]
  }]
}
```

---

## 4. Timpani-N — Runtime Integrated Dispatch

### 4.1 BPF Map Definitions (simplified vs. DDR-005)

```c
/* TtSlotBpf — no slot_type field (single type) */
struct TtSlotBpf {
    u64 workload_id_hash;
    u64 task_id_hash;
    u32 offset_us;
    u32 duration_us;
    u32 deadline_us;
    u32 cpu;
};

/* CbsState — no dual_mode/floor_slot_idx fields.
   This DDR does not adopt bpf_timer-based replenishment, so the timer field is also removed.
   Replenishment is done via Lazy (BPF ops path) + Backup (Replenisher Thread) — see §4.5. */
struct CbsState {
    u32 budget_us;          /* Cs */
    u32 period_us;          /* Ts */
    u32 remaining_us;       /* remaining */
    u32 deadline_us;
    u64 replenish_at_ns;    /* next scheduled replenishment time (referenced by both lazy/backup) */
    u64 exec_start_ns;      /* updated in ops.running() */
};

/* TaskMeta — 2 scheduling types */
struct TaskMeta {
    u64 workload_id_hash;
    u64 task_id_hash;
    u8  scheduling_type;    /* 0 = STYPE_TT (L1) , 1 = STYPE_CBS (L2) */
    u8  layer;              /* 1 = L1 , 2 = L2 (informational) */
    u64 activation_ns;
    u64 dsq_id;             /* For TT tasks, the per-task DSQ ID = task_id_hash */
};

/* current_slot_map: cpu → active slot_idx (updated by Timer Master) */
/* SLOT_NONE (0xFFFFFFFF) → currently in a Gap */
```

### 4.2 DSQ Layout

```
DSQ                       Priority    Target          ID
────────────────────────────────────────────────────────────────────
task-specific DSQ (L1)    Highest    L1 TT task     meta->task_id_hash (dynamic)
CBS_DSQ                   Mid        L2 CBS task    (1ULL << 61)
THROTTLED_DSQ             None       Budget-spent L2 (1ULL << 61) | 1
SCX_DSQ_GLOBAL            Lowest     unregistered    fallback (DDR-005)
```

> **Single CBS DSQ:** Splitting into `CBS_L1_DSQ` / `CBS_L2_DSQ` (as in earlier drafts)
> is not adopted. Since CBS applies only to L2, a single `CBS_DSQ` is sufficient
> and reduces BPF dispatch branches to one.

### 4.3 Timer Master Event Timeline

The Timer Master fires **TT_START only**. Gap entry happens naturally when
a TT task completes its work and calls `ttsched_wait_next_period()` → `FUTEX_WAIT`,
voluntarily yielding the CPU. `ops.dispatch()` then automatically consumes CBS tasks
from `DSQ_CBS` — no explicit TT_END kick is required.

```
Event         Meaning                                  Action
─────────────────────────────────────────────────────────────────────
TT_START      TT slot start                            current_slot_map[cpu] = idx
                                                      futex_wake(task)  /* wake exactly this task */
                                                      scx_bpf_kick_cpu(cpu, SCX_KICK_IDLE)
```

> **Gap entry via natural yield, not kick**: After completing its work, the TT task
> calls `ttsched_wait_next_period()` → `FUTEX_WAIT`. The CPU becomes idle and
> `ops.dispatch()` runs automatically, consuming `DSQ_CBS`. No TT_END event or
> SLOT_NONE write is needed in the current implementation.
> `current_slot_map` is written only at TT_START and is read only by `ops.stopping()`
> for deadline miss detection — not by `ops.dispatch()`.

#### Timer Master Pseudocode

```cpp
void TimerMaster::wake_task(uint32_t task_id_hash)
{
    int idx = task_hash_to_shm_idx_[task_id_hash];
    // Monotonic counter increment + FUTEX_WAKE on per-task SHM address
    __atomic_fetch_add(&ttsched_shm_->tasks[idx].counter, 1u, __ATOMIC_SEQ_CST);
    syscall(SYS_futex, &ttsched_shm_->tasks[idx].counter,
            FUTEX_WAKE, 1, nullptr, nullptr, 0);
}

void TimerMaster::run(const ScheduleTable& table)
{
    // Only TT_START events; no TT_END
    uint64_t hp_ns = table.hyperperiod_us() * 1000ULL;
    uint64_t cycle_start = table.epoch_ns();

    while (running_) {
        for (const auto& slot : slot_table_) {
            uint64_t target = cycle_start + slot.offset_ns;
            struct timespec ts = ns_to_timespec(target);
            clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &ts, nullptr);

            // Update current_slot_map (used only by ops.stopping for deadline check)
            bpf_map_update_elem(current_slot_fd_, &slot.cpu, &slot.slot_idx, BPF_ANY);
            // Wake the TT task via futex + kick idle CPU
            wake_task(slot.task_id_hash);
            scx_bpf_kick_cpu(slot.cpu, SCX_KICK_IDLE);
        }
        cycle_start += hp_ns;
    }
}
```

### 4.4 BPF ops Implementation — Integrated Dispatch

#### ops.select_cpu()

Same as DDR-005. L1 → dedicated Isolated CPU; L2 → Isolated CPU pool.

#### ops.enqueue() — 2-way Branch

This DDR performs CBS replenishment with a **Lazy + Backup** approach (§4.5).
The `enqueue` path is one of the primary entry points for lazy replenishment —
on event arrival, it tests whether `replenish_at_ns` has elapsed and bypasses
the throttled state.

```c
void BPF_STRUCT_OPS(timpani_enqueue, struct task_struct *p, u64 enq_flags)
{
    struct TaskMeta *meta = bpf_map_lookup_elem(&task_meta_map, &p->pid);
    if (!meta) {
        scx_bpf_dispatch(p, SCX_DSQ_GLOBAL, SCX_SLICE_DFL, enq_flags);
        return;
    }

    if (meta->scheduling_type == STYPE_TT) {
        /* L1: dispatch directly to the assigned CPU's kernel-internal local DSQ.
         * SCX_DSQ_LOCAL_ON bypasses any intermediate custom DSQ;
         * the task is immediately at its final destination.
         * SCX_KICK_IDLE wakes the CPU only if it is currently idle. */
        u32 acpu = meta->assigned_cpu;
        if (acpu < 64 && (isolated_cpu_mask & (1ULL << acpu))) {
            scx_bpf_dispatch(p, SCX_DSQ_LOCAL_ON | acpu, SCX_SLICE_DFL, enq_flags);
            scx_bpf_kick_cpu(acpu, SCX_KICK_IDLE);
        } else {
            scx_bpf_dispatch(p, SCX_DSQ_GLOBAL, SCX_SLICE_DFL, enq_flags);
        }
        return;
    }

    /* L2 CBS */
    u64 cbs_key = meta->workload_id_hash ^ meta->task_id_hash;
    struct CbsState *cbs = bpf_map_lookup_elem(&cbs_map, &cbs_key);
    if (!cbs) {
        scx_bpf_dispatch(p, SCX_DSQ_GLOBAL, SCX_SLICE_DFL, enq_flags);
        return;
    }

    /* Lazy replenish — bypass throttling if replenishment time has elapsed */
    cbs_lazy_replenish(cbs, bpf_ktime_get_ns());

    if (cbs->remaining_us > 0)
        scx_bpf_dispatch(p, CBS_DSQ, SCX_SLICE_DFL, enq_flags);
    else
        scx_bpf_dispatch(p, THROTTLED_DSQ, SCX_SLICE_DFL, enq_flags);
}
```

#### ops.dispatch() — Gap → CBS

```c
void BPF_STRUCT_OPS(timpani_dispatch, s32 cpu, struct task_struct *prev)
{
    /* TT tasks (L1) are dispatched via SCX_DSQ_LOCAL_ON in ops.enqueue().
     * They are already on this CPU's local DSQ — no consume needed here.
     * Gap entry happens naturally when the TT task yields (futex_wait);
     * ops.dispatch() is called automatically when the CPU becomes idle. */

    /* Gap → L2 CBS dispatch */
    if (scx_bpf_consume(CBS_DSQ))
        return;

    /* idle: L3/L4 do not run on Isolated CPUs */
}
```

    /* ─── Phase 2: Gap → L2 CBS dispatch ─── */
    if (scx_bpf_consume(CBS_DSQ))
        return;

    /* idle: L3/L4 do not run on Isolated CPUs */
}
```

#### ops.running() — CBS Slice Capping

A CBS task's time slice is capped at the **time remaining until the next TT slot start**.
This way it naturally yields at TT slot boundaries (even a kick simply lands in stopping immediately).

```c
void BPF_STRUCT_OPS(timpani_running, struct task_struct *p)
{
    struct TaskMeta *meta = bpf_map_lookup_elem(&task_meta_map, &p->pid);
    if (!meta) return;

    u64 now = bpf_ktime_get_ns();
    meta->activation_ns = now;

    if (meta->scheduling_type != STYPE_CBS)
        return;

    u64 cbs_key = meta->workload_id_hash ^ meta->task_id_hash;
    struct CbsState *cbs = bpf_map_lookup_elem(&cbs_map, &cbs_key);
    if (!cbs) return;

    cbs->exec_start_ns = now;

    /* slice = min(remaining budget, time to next TT) */
    u32 us_to_next_tt = compute_us_to_next_tt(bpf_get_smp_processor_id(), now);
    u32 effective_us  = min_u32(cbs->remaining_us, us_to_next_tt);
    p->scx.slice = (u64)effective_us * 1000ULL;
}
```

#### ops.stopping() — TT dmiss / CBS Budget Decrement

```c
void BPF_STRUCT_OPS(timpani_stopping, struct task_struct *p, bool runnable)
{
    struct TaskMeta *meta = bpf_map_lookup_elem(&task_meta_map, &p->pid);
    if (!meta) return;

    u64 now = bpf_ktime_get_ns();

    if (meta->scheduling_type == STYPE_TT) {
        /* L1 TT: deadline-miss check */
        u64 deadline_abs = meta->activation_ns +
                           (u64)get_tt_deadline(meta) * 1000ULL;
        if (now > deadline_abs) {
            struct FaultEvent ev = {
                .workload_id_hash     = meta->workload_id_hash,
                .cpu                  = bpf_get_smp_processor_id(),
                .expected_deadline_ns = deadline_abs,
                .actual_completion_ns = now,
                .fault_type           = FAULT_DMISS,
            };
            bpf_ringbuf_output(&fault_ringbuf, &ev, sizeof(ev), 0);
        }
        return;
    }

    /* L2 CBS: budget decrement */
    u64 cbs_key = meta->workload_id_hash ^ meta->task_id_hash;
    struct CbsState *cbs = bpf_map_lookup_elem(&cbs_map, &cbs_key);
    if (!cbs) return;

    u64 exec_ns = now - cbs->exec_start_ns;
    u32 exec_us = (u32)(exec_ns / 1000ULL);

    if (cbs->remaining_us > exec_us) {
        cbs->remaining_us -= exec_us;
    } else {
        cbs->remaining_us = 0;
        /* Only record the next replenishment time. The actual replenishment is
           done by §4.5's Lazy (BPF ops path) or Backup (Replenisher Thread).
           No bpf_timer_start() — avoids PREEMPT_RT constraints. */
        cbs->replenish_at_ns += (u64)cbs->period_us * 1000ULL;
        /* On the next enqueue this task will route into THROTTLED_DSQ */
    }

    /* CBS deadline-miss check (optional) */
    u64 deadline_abs = meta->activation_ns + (u64)cbs->deadline_us * 1000ULL;
    if (now > deadline_abs) {
        struct FaultEvent ev = {
            .workload_id_hash     = meta->workload_id_hash,
            .cpu                  = bpf_get_smp_processor_id(),
            .expected_deadline_ns = deadline_abs,
            .actual_completion_ns = now,
            .fault_type           = FAULT_DMISS,
        };
        bpf_ringbuf_output(&fault_ringbuf, &ev, sizeof(ev), 0);
    }
}
```

### 4.5 CBS Budget Replenishment — Unified Lazy + Backup

#### 4.5.1 Design Decision — `bpf_timer` Not Adopted

`bpf_timer` callbacks run in **softirq context** (the hrtimer callback path) on
mainline kernels. Under PREEMPT_RT the following constraints apply.

```
① Softirq handling change
   On PREEMPT_RT, softirqs are forced into the ksoftirqd kthread context.
   → hrtimer/bpf_timer callbacks no longer run immediately after hardirq
   → adds jitter equal to ksoftirqd's scheduling latency (tens to hundreds of μs)

② BPF context restrictions
   On certain PREEMPT_RT builds, bpf_timer is either disabled or only allowed in
   sleepable mode. (Differs by kernel version — 6.12 mainline allows it partially,
   varies with the RT patchset.)
```

To meet the precision requirement (tens of μs, DDR-002 §8) together with the
mandatory PREEMPT_RT deployment (DDR-005 §Q3), `bpf_timer` is unsuitable.
Therefore this DDR **does not adopt `bpf_timer`** and unifies CBS replenishment
on the combination of the two mechanisms below.

```
[Primary]  Lazy Replenishment in BPF
  → At every BPF ops entry point (enqueue/dispatch), compare the clock and
    replenish in place.
  → No timer needed. Self-clocking.

[Backup]   Replenisher Thread (userspace)
  → Wakes at the replenishment time using the same idiom as Timer Master
    (SCHED_FIFO + clock_nanosleep ABSTIME), updates cbs_map, and calls
    scx_bpf_kick_cpu().
  → Guarantees wake-up of throttled tasks that are otherwise sleeping
    (covers the gap left by lazy alone).
```

**Role assignment:**

| Situation | Owner |
|:--|:--|
| CBS task traverses a BPF ops path (enqueue/dispatch) | Lazy in-place replenishment (BPF) |
| CBS task is asleep in throttled state (no events arriving) | Replenisher Thread kicks it |
| Right after a TT slot ends, check whether CBS_DSQ has waiters | dispatch performs lazy replenish then consume |

#### 4.5.2 Primary — Lazy Replenishment

CBS replenishment does not need to fire "exactly at a callback time". It
suffices that "after the replenishment time has passed, the next dispatch
finds the budget restored". A single `now ≥ replenish_at_ns` check at each
key BPF ops entry point is enough.

##### Replenishment Helper

```c
/* Replenish for as many elapsed Ts periods as needed and update replenish_at_ns.
   Catches up multiple missed periods at once (e.g., when the task has been
   asleep for a while). Cost: O(1) — pure arithmetic. */
static __always_inline void cbs_lazy_replenish(struct CbsState *cbs, u64 now)
{
    if (now < cbs->replenish_at_ns)
        return;  /* not yet time to replenish */

    u64 ts_ns     = (u64)cbs->period_us * 1000ULL;
    u64 elapsed   = now - cbs->replenish_at_ns;
    u64 n_periods = elapsed / ts_ns + 1;       /* current + missed */

    /* Per CBS semantics, remaining budget never exceeds budget_us */
    cbs->remaining_us    = cbs->budget_us;
    cbs->replenish_at_ns += n_periods * ts_ns;
}
```

##### Entry Point 1 — `ops.enqueue()`

§4.4's enqueue already includes the `cbs_lazy_replenish(cbs, now)` call.
When an event arrives after the replenishment time, the throttled state is
bypassed instantly (the most common path).

##### Entry Point 2 — `ops.dispatch()` (before CBS consume)

Promote tasks accumulated in `THROTTLED_DSQ` whose replenishment time has
elapsed back to `CBS_DSQ`.

```c
void BPF_STRUCT_OPS(timpani_dispatch, s32 cpu, struct task_struct *prev)
{
    /* TT tasks handled in ops.enqueue() via SCX_DSQ_LOCAL_ON — no Phase 1 here. */

    /* Promote replenished tasks from throttled queue to CBS_DSQ */
    promote_throttled_if_replenished(cpu, bpf_ktime_get_ns());

    if (scx_bpf_consume(CBS_DSQ))
        return;
    /* idle */
}

/* Walk THROTTLED_DSQ and move replenished tasks to CBS_DSQ.
   Verifier requires an iteration cap (e.g., PROMOTE_BUDGET = 16). */
static __always_inline
void promote_throttled_if_replenished(s32 cpu, u64 now)
{
    int budget = PROMOTE_BUDGET;
    bpf_for_each(scx_dsq, p, THROTTLED_DSQ, 0) {
        if (--budget == 0) break;
        struct TaskMeta *m = bpf_map_lookup_elem(&task_meta_map, &p->pid);
        if (!m) continue;
        u64 k = m->workload_id_hash ^ m->task_id_hash;
        struct CbsState *cbs = bpf_map_lookup_elem(&cbs_map, &k);
        if (!cbs) continue;
        cbs_lazy_replenish(cbs, now);
        if (cbs->remaining_us > 0)
            scx_bpf_dispatch_from_dsq(BPF_FOR_EACH_ITER, p, CBS_DSQ, 0);
    }
}
```

##### `ops.stopping()` Change

The `bpf_timer_start()` call has already been removed in §4.4's stopping.
On budget exhaustion only `replenish_at_ns` is recorded; actual replenishment
is performed by either lazy or backup paths.

#### 4.5.3 Backup — Replenisher Thread (userspace)

**Why needed:** Lazy works only if a BPF ops path is invoked. The following
case is not covered by lazy alone.

```
Scenario: collision_det exhausts its budget at t=3ms → THROTTLED_DSQ
          No event arrives → no enqueue
          The CPU's TT slot belongs to another task → low dispatch frequency
          Replenishment time t=10ms passes but budget is not restored
          Event arrives at t=11ms → enqueue → only then lazy replenish

Issue: latency from replenishment to dispatch is several μs ~ several hundred μs.
       Soft behavior, but a weakness from a "determinism" standpoint.
```

The **Replenisher Thread** addresses this gap using the same idiom as Timer Master.

##### Daemon-side Implementation

```cpp
/* timpani-n/src/replenisher.cpp */

class Replenisher {
public:
    void run(const ScheduleTable& table) {
        /* (1) Build a min-heap of (cbs_key, cpu, period_ns, next_at_ns) for every CBS server */
        auto pq = build_priority_queue(table);

        while (running_) {
            auto top = pq.top();
            struct timespec ts = ns_to_timespec(top.next_at_ns);
            clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &ts, nullptr);

            /* (2) Atomic update of cbs_map — idempotent with the lazy helper */
            CbsState s;
            bpf_map_lookup_elem(cbs_fd_, &top.key, &s);
            cbs_apply_replenish_userspace(&s, top.next_at_ns);
            bpf_map_update_elem(cbs_fd_, &top.key, &s, BPF_EXIST);

            /* (3) Kick the assigned CPU → re-enter dispatch() */
            scx_bpf_kick_cpu_from_user(top.cpu);

            /* (4) Advance to the next replenishment time */
            top.next_at_ns += top.period_ns;
            pq.pop(); pq.push(top);
        }
    }
};
```

##### Thread Properties

| Property | Value | Rationale |
|:--|:--|:--|
| Scheduling policy | `SCHED_FIFO`, prio = Timer Master − 1 | TT precision takes priority |
| CPU pin | Non-Isolated CPU, or the same CPU as Timer Master | Separate from RT critical path (DDR-006 §4) |
| Wake-up precision | `clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME)` | Same as Timer Master |
| RT-safe | No gRPC/allocation, only atomic map updates | DDR-006 §4 |

#### 4.5.4 Consistency of Lazy / Backup

Both mechanisms use the same semantic helper
(`cbs_lazy_replenish` / `cbs_apply_replenish_userspace`) and are therefore
**idempotent**. Races are safe.

```
Race scenario:
  t=10000μs (replenishment time)
    BPF enqueue path → cbs_lazy_replenish() runs →
        remaining_us = budget_us, replenish_at_ns = 20000μs
    (almost simultaneously) Replenisher Thread wakes →
        BPF map read → remaining_us = budget_us already applied →
        cbs_apply_replenish_userspace() yields the same result → no-op
        kick still fires, but dispatch re-entry is harmless

Result: regardless of order, the final state is identical.
        Duplicate kicks are absorbed by sched_ext (kicks to idle CPUs are free).
```

#### 4.5.5 Handling Conflicts with TT Slots

##### Conflict Scenario — Replenishment Just Before a TT Slot

```
t=2ms                t=4ms              t=5ms
├──── CBS exec ──────┤                   ┤ TT_START
                     │ Replenisher kick │ kick (Timer Master)
                     │ remaining_us=Cs  │
                     │                  │
→ The previously THROTTLED task moves to CBS_DSQ on next enqueue
→ Phase 1 (TT) takes priority, so CBS cannot run at t=5ms
→ After the TT slot ends (TT_END), CBS dispatch happens automatically (Phase 2)

Result: no conflict. Replenishment time and dispatch time are independent.
```

##### Preemption Scenario — TT Slot Arriving Mid-CBS

```
t=2ms     t=4.5ms (≈)    t=5ms
│         │              │
├── CBS ──┤              │ TT_START
          │ slice expires │ kick (already idle)
          │ stopping()    │
          │ budget −=     │
          │              │
          └─ idle ───────┤ → ops.dispatch() Phase 1 (TT) fires

→ The slice cap in §4.4 ops.running() yields the CPU before the TT slot starts.
→ Even if a kick arrives, it just causes a benign dispatch re-entry.
```

#### 4.5.6 Precision / Overhead Profile

| Aspect | This DDR (Lazy + Backup) |
|:--|:--|
| Replenishment firing precision | `clock_nanosleep` precision (~few μs) — same as Timer Master |
| BPF code complexity | One-line helper at ops entry points; no timer registration/callback |
| Userspace dependency | One Replenisher Thread |
| Throttled wake-up | Replenisher kick (deterministic) or lazy at enqueue (early recovery) |
| Multi-period catch-up | One-shot correction via `n_periods` |
| PREEMPT_RT suitability | ○ (Same idiom as Timer Master — no `bpf_timer` dependency) |

---

## 5. End-to-End Sequence

### 5.1 Sample Workloads

```
brake_ctrl    (L1, Periodic, period=10ms,    wcet=2ms,   deadline=5ms)
steer_ctrl    (L1, Periodic, period=20ms,    wcet=2ms,   deadline=10ms)
collision_det (L2, Sporadic, MIT=10ms,       wcet=1.5ms, deadline=10ms)
lidar_proc    (L2, Sporadic, MIT=5ms,        wcet=2ms,   deadline=5ms)
```

### 5.2 Timpani-O Processing (offline)

```
Step 1 Classification:
  brake_ctrl    → Mechanism::Tt   (L1)
  steer_ctrl    → Mechanism::Tt   (L1)
  collision_det → Mechanism::Cbs  (L2)
  lidar_proc    → Mechanism::Cbs  (L2)

Step 2 CPU assignment (Isolated cpu=2):
  hyperperiod = LCM(10, 20) = 20ms
  U_tt  = 2/10 + 2/20         = 0.30
  U_cbs = 1.5/10 + 2/5        = 0.55
  U_total = 0.30 + 0.55 + 0.02 = 0.87  > 0.80 → InfeasibleError

Adjustment: move lidar_proc to cpu=3
  cpu=2: U_tt=0.30, U_cbs=0.15 (collision_det), U=0.47 ≤ 0.80 ✓
  cpu=3: U_tt=0,    U_cbs=0.40 (lidar_proc),    U=0.42 ≤ 0.80 ✓

Step 3 TT slot placement (cpu=2, DM order):
  brake_ctrl(d=5ms)  → offset=0,  10000  duration=2000
  steer_ctrl(d=10ms) → offset=5000        duration=2000

Step 4 Gap analysis (cpu=2):
  Gap intervals: [2000, 5000], [7000, 10000], [12000, 20000]
  → total 14ms / 20ms

Step 5 CBS budgets:
  collision_det → Cs=1500, Ts=10000, deadline=10000
  lidar_proc    → Cs=2000, Ts=5000,  deadline=5000  (placed on cpu=3)
```

### 5.3 Timpani-N Runtime Behavior (cpu=2, one hyperperiod)

```
t=0ms      TT_START(cpu=2, brake_ctrl)
           → current_slot_map[2] = idx_brake_0
           → kick → dispatch Phase 1 → consume(brake.task_id_hash)
           → brake_ctrl runs [0, 2ms]

t=2ms      TT_END(cpu=2)
           → current_slot_map[2] = SLOT_NONE
           → kick → dispatch Phase 2 → consume(CBS_DSQ)
           → if collision_det is waiting, run it; otherwise idle

t=5ms      TT_START(cpu=2, steer_ctrl)
           → if a CBS task is running, slice expiration (or kick) drives stopping
           → CBS budget decremented → dispatch Phase 1 → steer_ctrl runs [5, 7ms]

t=7ms      TT_END(cpu=2) → Gap [7, 10ms]
           → CBS may dispatch

t=10ms     TT_START(cpu=2, brake_ctrl)  /* second instance */
           → ...

t=20ms     hyperperiod ends, next cycle repeats
```

Event arrival flow (collision_det):

```
App: ttsched_signal_arrival("col_detect");   /* DDR-005 User App API */
  → futex_wake → BPF observes task wake-up
  → ops.enqueue(): cbs->remaining_us > 0 → CBS_DSQ
  → dispatched in the next Gap
```

---

## 6. Multi-Isolated-CPU Placement Strategy

### 6.1 Design Principle — Separate TT-heavy and CBS-heavy CPUs

Mixing many L1 tasks with L2 CBS on the same CPU accumulates the following costs:
- Gap fragmentation → frequent CBS context switches
- Possible min_gap < CBS_MIN_EXEC_US
- Increased complexity of feasibility analysis

When possible, **dedicate separate CPUs to L1 and L2** workloads.

```
Available Isolated CPUs: [cpu2, cpu3, cpu4, cpu5]

  cpu2, cpu3 (TT-primary):  L1 workloads
    U_tt ≈ 0.50~0.70, U_cbs ≈ 0 ~ 0.10 (only minor spillover)

  cpu4, cpu5 (CBS-primary): L2 workloads
    U_tt ≈ 0,        U_cbs ≈ 0.50~0.78
```

### 6.2 Assignment Algorithm

```rust
fn assign_isolated_cpus(
    node: &Node,
    classified: &[ClassifiedTask],
) -> Result<CpuAssignment, InfeasibleError> {

    let isolated = node.isolated_cpus();
    let mut assignment = CpuAssignment::new();

    /* ① L1 TT: prefer per-workload dedicated CPUs; if exhausted, share within the cpuset */
    for task in classified.iter().filter(|t| t.mechanism == Tt) {
        let cpu = pick_tt_cpu(&isolated, &assignment, task)?;
        assignment.add(cpu, task.clone(), Mechanism::Tt);
    }

    /* ② L2 CBS: try to fit into the Gaps of a TT-light CPU */
    for task in classified.iter().filter(|t| t.mechanism == Cbs) {
        let us = task.wcet_us as f64 / task.period_us as f64;

        /* Candidate 1: a TT CPU with enough residual capacity and sufficient min_gap */
        if let Some(cpu) = pick_tt_cpu_with_gap(&assignment, task, us) {
            assignment.add(cpu, task.clone(), Mechanism::Cbs);
            continue;
        }

        /* Candidate 2: allocate a separate CBS-primary CPU */
        let cpu = pick_or_alloc_cbs_cpu(&isolated, &assignment, us)
            .ok_or(InfeasibleError::NoCbsCpu(task.task_id.clone()))?;
        assignment.add(cpu, task.clone(), Mechanism::Cbs);
    }

    Ok(assignment)
}
```

### 6.3 Post-Assignment Verification

For every assigned CPU, verify the following before emitting the table:

```
∀ cpu ∈ assigned_cpus:
  U_tt(cpu) + U_cbs(cpu) + U_overhead ≤ U_bound (= 0.80)
  min_gap(cpu) ≥ CBS_MIN_EXEC_US (when CBS tasks are present)
  No TT slot placement conflicts (harmonic condition holds)
```

---

## 7. Integrated Rules — Summary Table

| Mechanism | Workload class | Timpani-O role | Timpani-N role | DSQ | Preemption rule |
|:--|:--|:--|:--|:--|:--|
| **TT** | L1 (Periodic + Safety) | Single-Phase DM placement (harmonic assumption) | Timer Master kick → immediate dispatch | per-task DSQ | Non-preemptible (highest priority) |
| **CBS** | L2 (Sporadic + Safety) | Allocate budgets within Gaps; non-rejectable | Budget-based dispatch in Gap regions; replenishment every Ts | `CBS_DSQ` / `THROTTLED_DSQ` | Yields to TT slots (slice cap) |

### Dispatch Priority Chain

```
[1] L1 TT (active slot)
       ↓ (Gap)
[2] L2 CBS (remaining budget > 0)
       ↓ (no CBS task or all throttled)
[3] Idle  (L3/L4 do not run on Isolated CPUs)
```

### Fault Reporting

| Fault Type | Trigger | Where it is raised |
|:--|:--|:--|
| `FAULT_DMISS` (L1) | TT task fails to finish before its deadline_abs | ops.stopping() |
| `FAULT_DMISS` (L2) | CBS task fails to finish within deadline_us | ops.stopping() |
| `BUDGET_EXCEED` (L2) | CBS task exhausts budget_us (natural throttle) | ops.stopping() — informational, not a fault |
| `WATCHDOG` | Timpani-N daemon detects a missing Sporadic event | userspace fault_monitor (FaultPolicy.watchdog_period_us) |

---

## 8. Feasibility Analysis — Integrated Conditions

### 8.1 Per-CPU Sufficient Condition

```
∀ Isolated CPU k ∈ assigned_cpus:

  U_tt(k)    = Σ_i (wcet_i / period_i)         ∀ L1 task i on CPU k
  U_cbs(k)   = Σ_j (Cs_j  / Ts_j)              ∀ L2 CBS server j on CPU k
  U_overhead = 0.02
  U_bound    = 0.80   (when L1 is present, DDR-004 §5)

  Feasibility:
    U_tt(k) + U_cbs(k) + U_overhead ≤ U_bound        ─ (necessary 1)
    min_gap(k) ≥ CBS_MIN_EXEC_US                     ─ (necessary 2)
    All L1 task periods are harmonic                  ─ (necessary 3)
```

### 8.2 Qualitative Guarantees

| Guarantee | Basis |
|:--|:--|
| Determinism of L1 TT | Static slots + Phase 1 dispatch is the highest priority |
| Temporal isolation of L2 CBS | Budget (Cs) is replenished exactly once per Ts; enforced by BPF |
| Blocking L1 ↔ L2 interference (FFI) | While a TT slot is active, CBS cannot dispatch (Phase 1 wins) |
| Blocking L2 ↔ L2 interference | Each CBS server's `budget_us` is decremented independently — runaway is contained |
| Isolation from L3/L4 | cpuset isolated partition (DDR-002 §5) |
| Compositional analysis | L1 analysis ⊥ L2 analysis (separated by Gap analysis) |

---

## 9. ISO 26262 / FFI Perspective

FFI in this model is guaranteed at three layers.

```
Layer 1: Physical isolation (cpuset isolated)
  Isolated CPU ↔ Non-Isolated CPU
  → Complete isolation between L1/L2 and L3/L4

Layer 2: Temporal isolation (TT slots)
  L1 tasks run only in their fixed offset/duration slots
  → L1 slot positions are not affected by adding/removing L2 workloads
  → Compositional analysis: L1 can be analyzed in isolation

Layer 3: Budget isolation (CBS)
  Each L2 CBS server's budget is enforced independently by BPF
  → A runaway L2 task cannot affect another L2 task
  → L1 always wins via Phase 1 dispatch — independent of L2 runaway

Certification scope split:
  Timpani-N (BPF) — Pure execution logic; no Criticality branches → easy to verify
  Timpani-O      — Validates Safety resource allocation; verified separately as an offline tool
```

---

## 10. Affected Components / DDRs

| DDR | Impact | Change |
|:--|:--|:--|
| DDR-001 | None | This DDR adopts DDR-001's L1/L2 definitions verbatim |
| DDR-002 | Amendment | §7 Sporadic+Safety dual-mode → not adopted by this DDR (simplified to L1=Periodic only). DDR-002 §7 needs a future consistency update |
| DDR-003 | None | TtSlot/CbsConfig retain their existing definitions. No `slot_type`/`dual_mode` additions |
| DDR-004 | Phase 5 algorithm | Concretized by §3.4~3.5 (TT placement + Gap + CBS budget) |
| DDR-005 | BPF Map | `TtSlotBpf.slot_type`, `CbsState.dual_mode` etc. not adopted. Apply the simplified layout from §4.1 |
| DDR-005 | ops.dispatch() | Replaced by the 2-Phase integrated logic in §4.4 |
| DDR-005 | ops.enqueue() | 2-way branching (STYPE_TT / STYPE_CBS) |
| DDR-005 | ops.running() | Add the CBS slice cap in §4.4 |
| DDR-005 | Timer Master | Only TT_START/TT_END events used (no separate GAP_START) |
| DDR-005 | CBS replenishment | No `bpf_timer` upstreaming. Unified on Lazy (BPF ops) + Replenisher Thread (userspace) — §4.5 |
| DDR-006 | Replenisher Thread | Add to the RT critical-path thread table (§4): SCHED_FIFO, prio = Timer Master − 1 |
| DDR-006 | Runtime updates | None — DDR-006's hot-update mechanism is reused |

---

## 11. Open Items

- [ ] Decide `CBS_MIN_EXEC_US` (initial candidate: 100μs — Antigravity PoC measurement needed)
- [ ] Validate `U_overhead = 0.02` (PREEMPT_RT + sched_ext measurement, DDR-005 §Q3)
- [ ] L1 harmonic-period enforcement policy (linked with DDR-004 open items)
- [ ] L2 CBS deadline-miss handling policy (FaultPolicy applicability — linked with DDR-001 fault policy)
- [ ] CPU assignment: worst-fit vs best-fit (§3.3, §6.2)
- [ ] BPF implementation of `compute_us_to_next_tt()` (how to look up the next per-CPU TT_START time) — consider introducing a new `next_tt_start_map`
- [ ] §4.5 Lazy replenishment iteration cap (`THROTTLED_DSQ` walk limit, `PROMOTE_BUDGET`) — measure the largest verifier-passable value
- [ ] Decide whether to merge the Replenisher Thread with the Timer Master or keep it separate (event-queue merge vs split)
- [ ] Determine the Replenisher Thread priority (validate `Timer Master − 1` via PoC, §4.5.3)
