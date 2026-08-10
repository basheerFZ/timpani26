<!--
SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
SPDX-License-Identifier: MIT
-->

# DDR-005: TIMPANI sched_ext BPF Scheduler Detailed Design

**Date:** 2026-04-24 (Last Updated: 2026-06-16)
**Status:** Draft — Under Discussion
**Author:** Human (Lead Architect) + AI
**Related:** DDR-011 (Runtime Table Update) for double buffering/shadow map architecture

---

## Goals

Defines the internal structure of the sched_ext BPF custom scheduler running in TIMPANI-N.
This scheduler:
- Enforces the HSF hierarchy of `HierarchicalScheduleTable` at the kernel level
- Precise TT slot firing (tens of μs precision)
- CBS budget enforcement (for L2 Sporadic workloads only)
- CPU isolation based on L1–L4 classification (Isolated / Non-Isolated)
- Deadline miss detection → fault_ringbuf → daemon

---

## Phase 2 Scheduling Hierarchy & DSQ Structure

TIMPANI Phase 2 operates as a **Partial BPF Scheduler** (`SCX_OPS_SWITCH_PARTIAL` mode). L1/L2 safety-critical tasks are explicitly scheduled via `scx_timpani`, while L3/L4 non-safety tasks are fully delegated to the standard Linux CFS, avoiding BPF-level overhead for background processes.

**Linux System-Wide Scheduling Hierarchy (Top-Down Priority):**
```text
[Highest Priority]
  SCHED_STOP
      ↓
  SCHED_DEADLINE
      ↓
  SCHED_FIFO / SCHED_RR  (Timer Master runs here)
      ↓
  SCHED_EXT (scx_timpani)
      ├─ SCX_DSQ_LOCAL_ON  (L1 TT — dispatched directly in enqueue())
      ├─ DSQ_CBS           (L2 Sporadic)
      ├─ [DSQ_THROTTLED]   (Holding Queue - Not Dispatched)
      ├─ DSQ_BE            (Legacy BE wrapper)
      └─ SCX_DSQ_GLOBAL    (Unregistered fallback)
      ↓
  SCHED_NORMAL (EEVDF)     (L3/L4 Native Delegation)
      ↓
  SCHED_IDLE
[Lowest Priority]
```

### Dispatch Queue (DSQ) Definitions

1. **SCX_DSQ_LOCAL_ON (Kernel-internal per-CPU local DSQ)**
   - **ID**: `SCX_DSQ_LOCAL_ON | assigned_cpu` (kernel built-in)
   - **Target**: L1 TT tasks. Dispatched directly in `ops.enqueue()` — bypasses any custom intermediate DSQ and `ops.dispatch()`. Timer Master calls `scx_bpf_kick_cpu(cpu, SCX_KICK_IDLE)` to wake the CPU immediately if idle.

2. **DSQ_CBS**
   - **ID**: `(1ULL << 61)` (Global Custom DSQ)
   - **Target**: L2 Sporadic. Consumed immediately after TT slots if budget > 0.

3. **DSQ_THROTTLED**
   - **ID**: `(1ULL << 61) | 1` (Global Custom DSQ)
   - **Target**: holding queue for budget-exhausted L2 tasks. **Never dispatched**. Waits for `bpf_timer` replenishment.

4. **DSQ_BE**
   - **Target**: Legacy representation for L3/L4. Subsumed by direct CFS delegation in Phase 2.

5. **SCX_DSQ_GLOBAL (System Fallback)**
   - **Target**: Unregistered SCHED_EXT tasks. Provides automatic global Load Balancing by allowing Idle CPUs to dynamically pull fallback tasks.

---

## sched_ext Basic Concepts

```
sched_ext (SCX): officially merged in Linux 6.12+
  → Completely replaces kernel scheduler behavior with a BPF program
  → Implements callbacks in the ops struct via BPF

Core ops callbacks:
  ops.select_cpu()  : selects which CPU to run a task on
  ops.enqueue()     : decides which dispatch queue to place a task in
  ops.dispatch()    : selects a task to run when CPU is idle
  ops.running()     : when a task acquires a CPU
  ops.stopping()    : when a task releases a CPU
  ops.init_task()   : initializes BPF-side metadata when a task is first created
```

---

## HSF 3-Level Tree and sched_ext ops Mapping

The 3-Level structure of HSF (DDR-002) maps to sched_ext ops as follows:

```
HSF Level 0 (Root / Offline Tool)
  → Pre-computes static schedule table + CBS budget
  → Injected into BPF maps (tt_table_map, cbs_map, partition_map)
  → The "answer key" that sched_ext references at runtime

HSF Level 1 (Timpani-N / Intermediate node)
  → ops.select_cpu():  Enforces Isolated/Non-Isolated CPU per L1–L4 classification
  → ops.enqueue():     L1(TT) → TT wait queue, L2(CBS) → CBS queue, L3/L4 → BE queue
  → ops.dispatch():    TT task dispatched with highest priority, then CBS after checking balance
  → ops.running/stopping(): budget deduction, deadline miss detection

HSF Level 2 (Workload / Leaf)
  → Workloads run within the budget delegated to them
  → BPF preempts on budget overrun (FFI guaranteed)
```

---

## Overall Structure

```
┌─────────────────────────────────────────────────────────────┐
│  TIMPANI sched_ext BPF (kernel)                             │
│                                                             │
│  ┌─────────────────────────────────────────────────────┐   │
│  │  BPF Maps (injected by TIMPANI-N daemon)            │   │
│  │                                                     │   │
│  │  partition_map   : cgroup_id → L1–L4 class + CPU mask│  │
│  │  tt_table_map    : (cpu, slot_idx) → TtSlot         │   │
│  │  cbs_map         : workload_id → CbsState           │   │
│  │  current_slot_map: cpu → current active TT slot_idx │   │
│  │  task_meta_map   : pid → TaskMeta                   │   │
│  │  fault_ringbuf   : deadline miss events → daemon    │   │
│  └─────────────────────────────────────────────────────┘   │
│                                                             │
│  ops.select_cpu()                                           │
│    → look up partition_map[cgroup_id]                       │
│    → L1: force dedicated Isolated CPU                       │
│    → L2: select within Isolated CPU pool                    │
│    → L3/L4: select from Non-Isolated CPUs                   │
│                                                             │
│  ops.enqueue()                                              │
│    → TT_SLOT (L1 Periodic): wait in TT_WAIT_QUEUE (Custom) │
│    → CBS (L2 Sporadic): check cbs_map balance              │
│                  balance > 0 → DSQ_CBS                     │
│                  balance = 0 → DSQ_THROTTLED (bpf_timer)   │
│    → BEST_EFFORT (L3/L4): DSQ_BE (low priority)            │
│                                                            │
│  ops.dispatch()                                            │
│    → check current_slot_map[cpu] (updated by Timer Master) │
│    → if L1 TT task for that slot exists: dispatch immediately│
│    → otherwise: DSQ_CBS → DSQ_BE order                     │
│                                                            │
│  ops.running()                                              │
│    → L1 TT task: record start time (for deadline miss check) │
│    → L2 CBS task: record exec_start_ns, begin deducting     │
│                                                             │
│  ops.stopping()                                             │
│    → L1 TT task: completion time - (epoch_ns+offset+deadline)│
│               if exceeded → record in fault_ringbuf         │
│    → L2 CBS task: measure exec time → budget deduct         │
│               budget ≤ 0 → move to throttled_queue         │
│               set bpf_timer (replenish budget after Ts)     │
└─────────────────────────────────────────────────────────────┘
          ↑ calls scx_bpf_kick_cpu()
┌─────────────────────────────────────────────────────────────┐
│  TIMPANI-N Timer Master Thread (C++, userspace)              │
│  Highest SCHED_FIFO priority, CPU pinned                    │
│                                                             │
│  loop:                                                      │
│    next_slot = compute_next_tt_slot(current_time, table)    │
│    clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME,           │
│                    epoch_ns + next_slot.offset_us * 1000)   │
│    // wakes up                                              │
│    bpf_map_update(current_slot_map, cpu, next_slot.idx)    │
│    scx_bpf_kick_cpu(next_slot.cpu)                         │
└─────────────────────────────────────────────────────────────┘
```

---

## BPF Map Detailed Definition

```c
// partition_map: cgroup_id → L1–L4 classification + CPU mask
// Key: u64 (cgroup_id)
// Value:
struct PartitionInfo {
    u8  layer;             // 1=L1, 2=L2, 3=L3, 4=L4
    u64 cpu_mask;          // allowed CPU bitmask
};

// tt_table_map: (cpu, slot_idx) → TtSlot
// Key: struct { u32 cpu; u32 slot_idx; }
// Value:
struct TtSlotBpf {
    u64 workload_id_hash;  // hash of workload_id
    u64 task_id_hash;      // hash of task_id (TaskSpec.task_id)
    u32 offset_us;
    u32 duration_us;
    u32 deadline_us;
    u32 cpu;
};

// cbs_map: task_key (workload+task hash) → CbsState (exclusive to L2 Sporadic tasks)
// Key: u64 (workload_id_hash ^ task_id_hash)
// Value:
struct CbsState {
    u32 budget_us;         // Cs: maximum budget
    u32 period_us;         // Ts: replenishment period
    u32 remaining_us;      // current balance (deducted at runtime)
    u64 replenish_at_ns;   // next replenishment time (bpf_timer reference)
    u32 deadline_us;
};

// current_slot_map: cpu → current active slot index
// Key: u32 (cpu)
// Value: u32 (slot_idx, 0xFFFFFFFF = none)

// task_meta_map: pid → TaskMeta
// Key: u32 (pid)
// Value:
struct TaskMeta {
    u64 workload_id_hash;
    u64 task_id_hash;      // hash of TaskSpec.task_id
    u8  scheduling_type;   // 0=TT_SLOT, 1=CBS, 2=BEST_EFFORT
    u8  layer;             // 1=L1, 2=L2, 3=L3, 4=L4
    u64 activation_ns;     // recorded in ops.running()
};

// fault_ringbuf: deadline miss events → userspace
struct FaultEvent {
    u64 workload_id_hash;
    u32 cpu;
    u64 expected_deadline_ns;
    u64 actual_completion_ns;
    u8  fault_type;        // 0=DMISS, 1=BUDGET_EXCEED
};
```

---

## Core ops Implementation Pseudocode

### ops.select_cpu()

```c
s32 BPF_STRUCT_OPS(timpani_select_cpu, struct task_struct *p,
                   s32 prev_cpu, u64 wake_flags)
{
    u64 cgroup_id = /* look up from task_meta_map (see Q1 below) */;
    struct PartitionInfo *part = bpf_map_lookup_elem(&partition_map, &cgroup_id);
    if (!part)
        return prev_cpu;  // unknown task → keep existing CPU

    // L1: force dedicated Isolated CPU
    if (part->layer == 1) {
        s32 cpu = bpf_cpumask_any_and(p->cpus_ptr, part->cpu_mask);
        return cpu >= 0 ? cpu : -ENOENT;
    }

    // L2: prefer idle CPU within Isolated CPU pool
    if (part->layer == 2) {
        s32 cpu = scx_bpf_pick_idle_cpu(part->cpu_mask, 0);
        return cpu >= 0 ? cpu : bpf_cpumask_any(part->cpu_mask);
    }

    // L3/L4: select from Non-Isolated CPUs
    s32 cpu = scx_bpf_pick_idle_cpu(part->cpu_mask, 0);
    return cpu >= 0 ? cpu : bpf_cpumask_any(part->cpu_mask);
}
```

### ops.enqueue()

```c
void BPF_STRUCT_OPS(timpani_enqueue, struct task_struct *p, u64 enq_flags)
{
    struct TaskMeta *meta = bpf_map_lookup_elem(&task_meta_map, &p->pid);
    if (!meta) {
        // unregistered task → global fallback
        scx_bpf_dispatch(p, SCX_DSQ_GLOBAL, SCX_SLICE_DFL, enq_flags);
        return;
    }

    if (meta->scheduling_type == SCHED_TYPE_TT) {
        // L1 TT: dispatch directly to the assigned CPU's local DSQ.
        // No intermediate custom DSQ — task is immediately at its final
        // destination. kick_cpu wakes the CPU if it is currently idle.
        u32 acpu = meta->assigned_cpu;
        if (acpu < 64 && (isolated_cpu_mask & (1ULL << acpu))) {
            scx_bpf_dispatch(p, SCX_DSQ_LOCAL_ON | acpu, SCX_SLICE_DFL, enq_flags);
            scx_bpf_kick_cpu(acpu, SCX_KICK_IDLE);
        } else {
            scx_bpf_dispatch(p, SCX_DSQ_GLOBAL, SCX_SLICE_DFL, enq_flags);
        }
        return;
    }

    if (meta->scheduling_type == SCHED_TYPE_CBS) {
        // L2 Sporadic: check CBS budget balance
        struct CbsState *cbs = bpf_map_lookup_elem(&cbs_map,
                                                    &meta->task_id_hash);
        if (cbs) {
            u64 now = bpf_ktime_get_ns();
            if (cbs->remaining_us == 0 && now >= cbs->replenish_at_ns) {
                cbs->remaining_us = cbs->budget_us;
                cbs->replenish_at_ns = now + (u64)cbs->period_us * 1000ULL;
            }
            if (cbs->remaining_us > 0)
                scx_bpf_dispatch(p, DSQ_CBS, SCX_SLICE_DFL, enq_flags);
            else
                scx_bpf_dispatch(p, DSQ_THROTTLED, SCX_SLICE_DFL, enq_flags);
        } else {
            scx_bpf_dispatch(p, DSQ_CBS, SCX_SLICE_DFL, enq_flags);
        }
        return;
    }

    // L3/L4: BE queue
    scx_bpf_dispatch(p, DSQ_BE, SCX_SLICE_DFL, enq_flags);
}
```

### ops.dispatch()

```c
void BPF_STRUCT_OPS(timpani_dispatch, s32 cpu, struct task_struct *prev)
{
    // TT tasks (L1): dispatched via SCX_DSQ_LOCAL_ON in ops.enqueue().
    // They are already on this CPU's local DSQ — no consume needed here.
    // Gap entry happens naturally: when a TT task calls futex_wait and
    // yields, the CPU becomes idle and ops.dispatch() runs automatically.

    // 1st priority: L2 CBS Sporadic tasks with remaining budget
    if (scx_bpf_consume(DSQ_CBS))
        return;

    // 2nd priority: L3/L4 BE queue
    scx_bpf_consume(DSQ_BE);
}
```

### ops.running() / ops.stopping()

```c
void BPF_STRUCT_OPS(timpani_running, struct task_struct *p)
{
    struct TaskMeta *meta = bpf_map_lookup_elem(&task_meta_map, &p->pid);
    if (!meta) return;

    u64 now = bpf_ktime_get_ns();
    meta->activation_ns = now;

    // L2 CBS: begin budget deduction (record execution start time)
    if (meta->scheduling_type == SCHED_TYPE_CBS) {
        struct CbsState *cbs = bpf_map_lookup_elem(&cbs_map,
                                                    &meta->workload_id_hash);
        if (cbs) cbs->exec_start_ns = now;
    }
}

void BPF_STRUCT_OPS(timpani_stopping, struct task_struct *p, bool runnable)
{
    struct TaskMeta *meta = bpf_map_lookup_elem(&task_meta_map, &p->pid);
    if (!meta) return;

    u64 now = bpf_ktime_get_ns();

    if (meta->scheduling_type == SCHED_TYPE_TT) {
        // L1 TT: Deadline miss check
        u64 deadline_abs = meta->activation_ns +
                           (u64)get_tt_deadline(meta) * 1000;
        if (now > deadline_abs) {
            struct FaultEvent evt = {
                .workload_id_hash     = meta->workload_id_hash,
                .cpu                  = bpf_get_smp_processor_id(),
                .expected_deadline_ns = deadline_abs,
                .actual_completion_ns = now,
                .fault_type           = FAULT_DMISS,
            };
            bpf_ringbuf_output(&fault_ringbuf, &evt, sizeof(evt), 0);
        }
    }

    if (meta->scheduling_type == SCHED_TYPE_CBS) {
        // L2 CBS: deduct budget
        struct CbsState *cbs = bpf_map_lookup_elem(&cbs_map,
                                                    &meta->workload_id_hash);
        if (cbs) {
            u64 exec_ns = now - cbs->exec_start_ns;
            u32 exec_us = exec_ns / 1000;
            if (cbs->remaining_us > exec_us)
                cbs->remaining_us -= exec_us;
            else {
                cbs->remaining_us = 0;
                // budget exhausted → schedule replenishment after Ts via bpf_timer
                bpf_timer_start(&cbs->replenish_timer, cbs->period_us * 1000,
                                BPF_F_TIMER_ABS);
                // move to throttled_queue (handled at next enqueue)
            }
        }
    }
}
```

---



## Open Design Questions

### Q1. How to obtain cgroup_id in BPF? ✅ Resolved

```
→ **Decision: Option B**
  TIMPANI-N daemon registers pids in task_meta_map
  at container startup (including cgroup_id, scheduling_type, L1–L4 classification)

  Implementation: handled by the BPF Loader component of DDR-006 C++ rework.
  partition_map is also initialized simultaneously based on topology reported in DDR-006 NodeReady.

Note — alternatives considered:
  Option A: bpf_task_cgroup_id() — does not exist (no BPF API currently)
  Option C: traverse cgroup hierarchy via BPF iterator (complex, rejected)
```

### Q2. Selecting the correct task from TT_WAIT_DSQ

```
Problem: When multiple TT task threads are waiting in TT_WAIT_DSQ,
      scx_bpf_consume(TT_WAIT_DSQ) in ops.dispatch()
      pulls the front task — cannot guarantee it matches the active slot

Solutions:
  Option A: use a dedicated DSQ per task thread
            DSQ ID = task_id_hash → consume from that DSQ
  Option B: look up pid directly from task_meta_map, then
            use scx_bpf_dispatch_vtime() to prioritize that specific task

→ Option A is simpler for BPF implementation:
  Create a dedicated DSQ per task thread (up to the number of L1/L2 tasks)
  → on slot activation, consume only the DSQ for that task
```

### Q3. PREEMPT_RT + sched_ext Compatibility

```
The combination of PREEMPT_RT patch and sched_ext:
  Linux 6.12: sched_ext officially included
  PREEMPT_RT: mainline merge in progress as of 6.12 (some archs)

  aarch64 (primary vehicle ECU): PREEMPT_RT 6.12 patch available
  → Can be applied via Yocto meta-realtime layer

  Needs verification: actual jitter measurement for sched_ext + PREEMPT_RT combination
  → Delegated to Antigravity PoC Task #1
```

---

## Antigravity PoC Task #1 — Delegation Ready

Once design discussion for this DDR is complete, delegate the following PoC to Antigravity:

```
ANTIGRAVITY TASK: sched_ext + PREEMPT_RT Jitter Measurement PoC

Context:
  TIMPANI-N uses sched_ext-based BPF scheduler + PREEMPT_RT kernel
  TT slot precision requirement: tens of μs

Goal:
  Implement minimal BPF scheduler based on scx_minimal
  Implement Timer Master pattern (SCHED_FIFO + clock_nanosleep ABSTIME)
  Measure two timings:
    1. Timer Master wakeup jitter (clock_nanosleep precision)
    2. BPF ops.dispatch() call latency (kick_cpu → actual execution)
  Comparative measurement with/without PREEMPT_RT

Reference:
  timpani-n/src/core.c (existing C implementation — see runtime loop)
  timpani-n/src/sched.c (scheduling API wrappers)
  scx/scx_minimal.bpf.c (Linux kernel tools/sched_ext)

Output:
  Minimal PoC code (C userspace + BPF C)
  Jitter measurement result report (table + analysis)
  Comparison with/without PREEMPT_RT
```

---

## User App Interface

### Task Thread Identification

TIMPANI-N identifies task threads using a **cgroup + thread name** combination.

| Identification step | Method | Auto/Manual |
|:--|:--|:--|
| Workload identification | cgroup_id (automatically created at container start) | Auto |
| Task thread identification | `task_struct->comm` (thread name) | Set by app |

**App constraint**: Each task thread must set its thread name via `pthread_setname_np()` or `prctl(PR_SET_NAME)`. This name must match `WorkloadSpec.task_specs[].task_id`.

```c
// App code example — the only required constraint
pthread_setname_np(pthread_self(), "sensor_read");  // must match task_id
```

This is not a TIMPANI-specific constraint but a formalization of general best practices in RT app development (debugging, Perfetto tracing, logging).

**Identification behavior**:

```
① Pullpiri: container start → cgroup created (automatic)
② App: pthread_setname_np(task_id) in each task thread (1 line)
③ Timpani-N: detects new thread appearing in cgroup
   → reads /proc/<pid>/task/<tid>/comm
   → matches against WorkloadSpec.task_specs[].task_id
   → registers task_meta_map[tid] = { workload_id, task_id_hash, layer, ... }
④ BPF: applies scheduling policy to registered tids
```

### Re-identification on Restart

pid/tid changes on Container stop → start. Name-based identification makes restart safe:

```
① Container Stop:
   Timpani-N: detects cgroup deletion event
   → invalidates all task_meta_map entries for that workload
   → that TT slot enters "no task" state → dispatch skip + missing event report

② Container Start (new pid):
   Timpani-N: detects new cgroup + new thread appearing
   → reads comm → re-matches with same task_id
   → registers new tid in task_meta_map
   → resumes dispatching new task from next slot
```

### Periodic Execution Synchronization: ttsched_wait_next_period()

**Required for all L1/L2.**

Although sched_ext dispatches tasks at TT slot timings at the kernel level, if the app manages its period with its own timer/sleep, a phase difference arises between the app's wakeup time and the TT slot start time, increasing jitter.

```
Problem: when app uses its own sleep

  TT slot:     |──────|         |──────|
  App sleep:         |──sleep──|──work──|──sleep──|
                                ↑
                  app wakes at its own timing
                  → mismatch with slot start → tens to hundreds of μs extra jitter
```

`ttsched_wait_next_period()` is a lightweight synchronization API provided by TIMPANI that precisely synchronizes the app's main loop to the TT slot start time:

```c
// libttsched.h — lightweight library provided by TIMPANI
#include <libttsched.h>

void* task_thread(void* arg) {
    pthread_setname_np(pthread_self(), "sensor_read");  // required: task identification
    ttsched_init();                                      // initialize

    while (1) {
        ttsched_wait_next_period();  // wait until TT slot start (required)
        do_periodic_work();           // perform periodic work
    }
}
```

**Implementation mechanisms** (candidates):

| Method | Description | Latency |
|:--|:--|:--|
| **futex (implemented)** | Timer Master increments per-task SHM counter + FUTEX_WAKE; task waits with FUTEX_WAIT on its own counter address | **~1μs** |
| eventfd | Timer Master calls eventfd_write() → app waits on read() | ~2μs |
| shared memory flag + busy poll | app polls a shared memory flag | <1μs (CPU cost) |

Futex was chosen for its low latency, absence of file-descriptor overhead, and per-task address granularity (FUTEX_WAKE on `tasks[i].counter` wakes exactly that task). The monotonic counter pattern also prevents lost wakeups if Timer Master fires before FUTEX_WAIT is entered (EAGAIN path).

### App Constraints Summary by L1–L4

| L1–L4 | pthread_setname_np | ttsched_wait_next_period() | sigwait | Notes |
|:--|:--|:--|:--|:--|
| **L1** | **Required** | **Required** | Not needed | μs-precision synchronization |
| **L2** | **Required** | **Required** | Not needed | Increased jitter and wasted budget if not used |
| **L3/L4** | Not needed | Not needed | Not needed | Outside TIMPANI scope |

> **Change from Timpani 25**: `sigwait()`-based main loop enforcement is **completely removed**. The app's main loop structure is free; replaced by a single `ttsched_wait_next_period()` call.

### App Migration from Timpani 25 to 26

```c
// Existing Timpani 25
prctl(PR_SET_NAME, "sensor_read");
sigset_t set;
sigemptyset(&set);
sigaddset(&set, SIGRTMIN);
while (1) {
    sigwait(&set, &sig);       // ← remove
    do_periodic_work();
}

// New Timpani 26
pthread_setname_np(pthread_self(), "sensor_read");  // keep
ttsched_init();
while (1) {
    ttsched_wait_next_period();  // ← replace (sigwait → ttsched)
    do_periodic_work();
}
```

Migration cost: **2-line change** (`sigwait` → `ttsched_wait_next_period`, remove signal setup code).

---

## Affected Components

| File | Changes |
|------|---------|
| `timpani-n/src/core.c` | Implement Timer Master loop (redesign existing loop to sched_ext-based) |
| `timpani-n/src/bpf/` | New — TIMPANI sched_ext BPF scheduler directory |
| `timpani-n/src/bpf/timpani.bpf.c` | New — sched_ext ops implementation |
| `timpani-n/src/bpf/maps.h` | New — BPF map definitions |
| `timpani-n/src/task_registry.c` | New — cgroup monitoring + thread name matching → task_meta_map registration |
| `timpani-n/src/trace_bpf.c` | Integrate fault_ringbuf polling |
| `sample-apps/src/libttsched.h` | New — ttsched_wait_next_period() lightweight library |
