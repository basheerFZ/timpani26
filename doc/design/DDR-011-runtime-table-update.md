<!--
SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
SPDX-License-Identifier: MIT
-->

# DDR-011: Runtime Schedule Table Update (Zero-Downtime)

**Date:** 2026-06-15
**Status:** Draft
**Author:** Human (Lead Architect) + AI
**Related:** DDR-005 (BPF Scheduler), DDR-006 (Communication Architecture)

---

## 1. Overview

This document describes the **zero-downtime schedule table update** mechanism in TIMPANI. The goal is to add, remove, or modify workloads without restarting any daemon or interrupting other running workloads.

### Design Goals

| Goal | Description |
|:--|:--|
| **Zero-Downtime** | Workload changes applied without process restart |
| **Determinism** | Table swap occurs only at hyperperiod boundaries |
| **Isolation** | Changes to one workload do not affect others (FFI) |
| **Atomicity** | Updates take effect only at a hyperperiod boundary; consistency across the userspace↔BPF↔app boundary is provided by in-place map upserts plus an SHM generation handshake (magic-last publish) |

### 1.1 How Zero-Downtime Is Achieved (Implemented — Phase 2)

As shipped in `timpani26`, zero-downtime updates are performed **in userspace**, not via a BPF-map double buffer:

- **Per-CPU timer threads**: TIMPANI-N runs one timer thread per isolated CPU (`TimerMaster::cpu_thread_loop`), each firing only its own CPU's TT slots on the shared `epoch_ns` hyperperiod grid. This replaced the earlier single-thread master, which could not correctly fire slots scheduled on multiple CPUs at the same instant (`46a301d`).
- **In-place BPF map upserts**: `set_schedule_table()` / `remove_workload()` rebuild the timer threads and upsert the live BPF maps (`tt_table_map`, `current_slot_map`, `cbs_map`) directly; there is a single active table, not two.
- **SHM generation handshake**: the timer master bumps a `generation` counter in the app-shared SHM and writes `magic` last, forcing `libttsched` clients to re-look up their task slots on change (`8369e01`). SHM slot indices are kept stable for existing tasks to avoid wakeup distortion (`42f12f5`), and per-thread SHM state is thread-local for multi-threaded apps (`bf8ba5c`).
- **Determinism**: changes take effect at the next hyperperiod boundary; a late start (where `epoch_ns` is already in the past) catches up to the next boundary on the same global grid.

> **Note**: BPF-map **double buffering** (`active_map_idx`, §2) is a *deferred* mechanism reserved for the atomic replacement of an entire BPF table map. It is **not yet implemented** (Open Item B-2.1) and is not exercised by the incremental update path above.

> **SHM layout change** (`8369e01`, breaking): `timpani_ttsched_shm` gained a `generation` field and the `tasks[]` array offset moved from byte 8 to 16. `libttsched.h` consumers (timpani-n, sample-apps, external TT workloads) must be recompiled.

---

## 2. Double Buffering Architecture (Deferred — Not Yet Implemented)

> **Status**: This section describes a **planned** mechanism that is **not implemented** in `timpani26` (Open Item B-2.1). The maps `tt_table_map_0` / `tt_table_map_1` and `active_map_idx` below do **not** exist in the current source — there is a single `tt_table_map`, and `ops.dispatch()` selects the active slot via `current_slot_map` (driven by the per-CPU timer threads, §1.1). Double buffering is reserved for a future case requiring **atomic replacement of an entire BPF table map**; the shipped incremental update path (§1.1, §4) does not need it.

### 2.1 Concept

```
┌─────────────────────────────────────────────────────────────┐
│  BPF Map Double Buffer Structure                            │
│                                                             │
│  ┌──────────────────┐    ┌──────────────────┐              │
│  │  tt_table_map[0] │    │  tt_table_map[1] │              │
│  │  (Shadow)        │    │  (Active)        │              │
│  └──────────────────┘    └──────────────────┘              │
│           ↑                       ↑                        │
│      Prepare next table      Currently executing           │
│                                                             │
│  active_map_idx = 1  (Current Active is [1])               │
│                                                             │
│  At Hyperperiod Boundary:                                   │
│    active_map_idx = 0  (Atomic swap → [0] becomes Active)  │
└─────────────────────────────────────────────────────────────┘
```

### 2.2 BPF Map Definitions

```c
/* B-2.1: Shadow map index management */
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, u32);
    __type(value, u32);  // 0 or 1
} active_map_idx SEC(".maps");

/* Double buffer: Two tt_table_maps */
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, MAX_TT_SLOTS);
    __type(key, struct TtSlotKey);
    __type(value, struct TtSlotBpf);
} tt_table_map_0 SEC(".maps");  // Shadow or Active

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, MAX_TT_SLOTS);
    __type(key, struct TtSlotKey);
    __type(value, struct TtSlotBpf);
} tt_table_map_1 SEC(".maps");  // Active or Shadow

/* Supporting data structures */
struct TtSlotKey {
    u32 cpu;
    u32 slot_idx;
};

struct TtSlotBpf {
    u64 workload_id_hash;
    u64 task_id_hash;
    u32 offset_us;
    u32 duration_us;
    u32 deadline_us;
    u32 cpu;
};
```

### 2.3 ops.dispatch() Implementation

```c
void BPF_STRUCT_OPS(timpani_dispatch, s32 cpu, struct task_struct *prev)
{
    /* Read active map index (atomic read) */
    u32 key = 0;
    u32 *active = bpf_map_lookup_elem(&active_map_idx, &key);
    u32 idx = active ? *active : 0;

    /* Look up current slot in active table */
    u32 *slot_idx = bpf_map_lookup_elem(&current_slot_map, &cpu);
    if (!slot_idx || *slot_idx == SLOT_NONE)
        goto fallback;

    struct TtSlotKey slot_key = { .cpu = cpu, .slot_idx = *slot_idx };
    struct TtSlotBpf *slot;

    /* Reference active map based on index */
    if (idx == 0)
        slot = bpf_map_lookup_elem(&tt_table_map_0, &slot_key);
    else
        slot = bpf_map_lookup_elem(&tt_table_map_1, &slot_key);

    if (slot) {
        /* Dispatch L1 TT task from its wait queue */
        scx_bpf_consume(slot->task_id_hash);
        return;
    }

fallback:
    /* L2 CBS → L3/L4 BE */
    if (scx_bpf_consume(DSQ_CBS))
        return;
    scx_bpf_consume(DSQ_BE);
}
```

---

## 3. Update Scenarios

### 3.1 Add Workload (Independent)

**Trigger:** Pullpiri sends `RegisterWorkload` request

```
Timeline →
                    Hyperperiod = 100ms
├────────────────────────────────────────────────────────────┤
│  Slot 0      │  Slot 1      │  Slot 2      │  Slot 3      │
│  Task A      │  Task B      │  (empty)     │  Task A      │
│  0~25ms      │  25~50ms     │  50~75ms     │  75~100ms    │
├────────────────────────────────────────────────────────────┤
                     ↑
              Current execution (t = 30ms)

Pullpiri → RegisterWorkload(Task C, L2 Sporadic)

┌─────────────────────────────────────────────────────────────┐
│  timpani-n Internal Processing                              │
│                                                             │
│  Step 1: timpani-o receives request, recalculates table     │
│  Step 2: timpani-o sends ScheduleTableUpdate to timpani-n   │
│  Step 3: timpani-n loads new table into Shadow map          │
│                                                             │
│  [Active Map: tt_table_map_1] ← ops.dispatch() references   │
│    Slot 0: Task A                                           │
│    Slot 1: Task B                                           │
│    Slot 2: (empty)                                          │
│    Slot 3: Task A                                           │
│                                                             │
│  [Shadow Map: tt_table_map_0] ← Background update           │
│    Slot 0: Task A                                           │
│    Slot 1: Task B                                           │
│    Slot 2: Task C (NEW!)  ← CBS slot added                  │
│    Slot 3: Task A                                           │
│                                                             │
│  Step 4: Set shadow_ready = true                            │
└─────────────────────────────────────────────────────────────┘

                    Hyperperiod Boundary (t = 100ms)
                              ↓
┌─────────────────────────────────────────────────────────────┐
│  Atomic Swap                                                │
│                                                             │
│  active_map_idx = 0  ← Single u32 write (atomic)            │
│                                                             │
│  → ops.dispatch() now references tt_table_map_0             │
│  → Task C can execute in Slot 2                             │
│  → Task A, B continue without interruption                  │
└─────────────────────────────────────────────────────────────┘
```

### 3.2 Remove Workload (STOP)

**Trigger:** Pullpiri sends `RemoveWorkload` request (FaultAction = STOP)

```
Current State:
├────────────────────────────────────────────────────────────┤
│  Slot 0      │  Slot 1      │  Slot 2      │  Slot 3      │
│  Task A      │  Task B      │  Task C      │  Task A      │
├────────────────────────────────────────────────────────────┤

Pullpiri → RemoveWorkload(Task B)

┌─────────────────────────────────────────────────────────────┐
│  Shadow Map Preparation                                     │
│                                                             │
│  [Active: tt_table_map_0]                                   │
│    Slot 0: Task A                                           │
│    Slot 1: Task B  ← Still executing                        │
│    Slot 2: Task C                                           │
│    Slot 3: Task A                                           │
│                                                             │
│  [Shadow: tt_table_map_1] ← Prepare table without Task B    │
│    Slot 0: Task A                                           │
│    Slot 1: (empty)  ← Task B removed                        │
│    Slot 2: Task C                                           │
│    Slot 3: Task A                                           │
└─────────────────────────────────────────────────────────────┘

                    Hyperperiod Boundary
                         ↓
┌─────────────────────────────────────────────────────────────┐
│  Atomic Swap + BPF Map Cleanup                              │
│                                                             │
│  1. active_map_idx = 1                                      │
│  2. Delete Task B's pid from task_meta_map                  │
│  3. Delete Task B's budget from cbs_budget_map (if L2)      │
│                                                             │
│  → Task B is no longer dispatched                           │
│  → Task A, C continue without interruption                  │
└─────────────────────────────────────────────────────────────┘
```

### 3.3 Full Table Replacement (Mode Transition)

**Trigger:** Driving mode change (e.g., Parking → Highway)

```
[Parking Mode Table]
├──────────────────────────────────────────────────────────────┤
│  Slot 0       │  Slot 1       │  Slot 2       │  Slot 3     │
│  Parking-Cam  │  Ultrasonic   │  Parking-AI   │  Display    │
│  Period: 50ms │  Period: 25ms │  Period: 50ms │  Period: 50ms│
├──────────────────────────────────────────────────────────────┤
Hyperperiod = 50ms

Mode Transition Request → Full table replacement required

[Highway Mode Table] ← Prepared in Shadow map
├──────────────────────────────────────────────────────────────┤
│  Slot 0       │  Slot 1       │  Slot 2       │  Slot 3     │
│  Front-Cam    │  Radar-Fusion │  ADAS-AI      │  Dashboard  │
│  Period: 20ms │  Period: 10ms │  Period: 20ms │  Period: 33ms│
├──────────────────────────────────────────────────────────────┤
Hyperperiod = 660ms (LCM recalculated)

┌─────────────────────────────────────────────────────────────┐
│  Transition Process                                          │
│                                                             │
│  1. timpani-o: Generate new table + feasibility check       │
│  2. timpani-n: Load Highway table into Shadow map           │
│  3. Timer Master: Wait for current hyperperiod (max 50ms)   │
│  4. At Hyperperiod Boundary:                                 │
│     - active_map_idx swap (atomic)                          │
│     - Reconfigure Timer Master with new hyperperiod (660ms) │
│  5. New workloads begin execution immediately               │
│                                                             │
│  ★ Transition jitter: 0 (swap only at hyperperiod boundary) │
│  ★ Existing workload downtime: 0ms                          │
└─────────────────────────────────────────────────────────────┘
```

---

## 4. Timer Master Implementation

> **Status**: The pseudo-code in §4.1 reflects the **original single-thread double-buffer design** and does **not** match `timpani26`. As shipped, `TimerMaster` runs **one timer thread per isolated CPU** (`cpu_thread_loop`), applies table changes via in-place BPF map upserts plus the SHM generation handshake (no `performAtomicSwap()` / `active_map_idx`), and aligns to the shared `epoch_ns` hyperperiod grid with catch-up for late starts. See §1.1 for the implemented model; §4.1 is retained as the reference design for the deferred double-buffer path (§2).

### 4.1 Userspace Thread (C++)

```cpp
class TimerMaster {
public:
    void run() {
        while (running_) {
            // Wait until next hyperperiod boundary
            uint64_t next_boundary = epoch_ns_ +
                ((current_hp_ + 1) * hyperperiod_ns_);

            struct timespec ts = ns_to_timespec(next_boundary);
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);

            // Check if shadow map is ready
            if (shadow_ready_.load()) {
                performAtomicSwap();
            }

            // Advance to next hyperperiod
            current_hp_++;

            // Fire TT slots for this hyperperiod
            dispatchTtSlots();
        }
    }

private:
    void performAtomicSwap() {
        // Atomic swap: single u32 write
        uint32_t key = 0;
        uint32_t new_idx = 1 - current_active_idx_;

        bpf_map_update_elem(active_map_idx_fd_, &key, &new_idx, BPF_ANY);

        current_active_idx_ = new_idx;
        shadow_ready_.store(false);

        // Apply new hyperperiod if changed
        if (new_hyperperiod_ns_ != hyperperiod_ns_) {
            hyperperiod_ns_ = new_hyperperiod_ns_;
            LOG_INFO("Hyperperiod updated to {} ns", hyperperiod_ns_);
        }

        LOG_INFO("Schedule table swapped at hyperperiod {}", current_hp_);
    }

    void dispatchTtSlots() {
        // Iterate through TT slots for current hyperperiod
        for (const auto& slot : tt_slots_) {
            uint64_t fire_time = epoch_ns_ +
                (current_hp_ * hyperperiod_ns_) +
                (slot.offset_us * 1000);

            struct timespec ts = ns_to_timespec(fire_time);
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);

            // Update current_slot_map and kick CPU
            updateCurrentSlot(slot.cpu, slot.slot_idx);
            kickCpu(slot.cpu);
        }
    }

    std::atomic<bool> shadow_ready_{false};
    uint32_t current_active_idx_ = 0;
    uint64_t hyperperiod_ns_;
    uint64_t new_hyperperiod_ns_;
    uint64_t current_hp_ = 0;
    int active_map_idx_fd_;
};
```

### 4.2 Shadow Map Update (gRPC Handler)

```cpp
Status GrpcHandler::HandleScheduleTableUpdate(
    const ScheduleTableUpdate& update) {

    // Determine which map is shadow (inactive)
    uint32_t shadow_idx = 1 - timer_master_->currentActiveIdx();
    int shadow_fd = (shadow_idx == 0) ?
        tt_table_map_0_fd_ : tt_table_map_1_fd_;

    // Clear shadow map
    clearBpfMap(shadow_fd);

    // Load new table into shadow map
    for (const auto& slot : update.tt_slots()) {
        struct TtSlotKey key = {
            .cpu = slot.cpu(),
            .slot_idx = slot.slot_idx()
        };
        struct TtSlotBpf value = {
            .workload_id_hash = hashWorkloadId(slot.workload_id()),
            .task_id_hash = hashTaskId(slot.task_id()),
            .offset_us = slot.offset_us(),
            .duration_us = slot.duration_us(),
            .deadline_us = slot.deadline_us(),
            .cpu = slot.cpu()
        };
        bpf_map_update_elem(shadow_fd, &key, &value, BPF_ANY);
    }

    // Update hyperperiod if changed
    if (update.hyperperiod_us() != timer_master_->hyperperiodUs()) {
        timer_master_->setNewHyperperiod(update.hyperperiod_us() * 1000);
    }

    // Signal shadow map is ready
    timer_master_->setShadowReady(true);

    return Status::OK;
}
```

---

## 5. BPF Map Cleanup on STOP

When a workload is stopped, the following BPF maps must be cleaned up:

| Map | Cleanup Action |
|:--|:--|
| `tt_table_map` | Remove slots for this workload (via shadow swap) |
| `task_meta_map` | Delete pid → TaskMeta entry |
| `cbs_budget_map` | Delete workload → CbsState entry (L2 only) |
| `partition_map` | Delete cgroup_id → PartitionInfo entry |

```cpp
void BpfLoader::cleanupWorkload(const std::string& workload_id) {
    uint64_t wid_hash = hashWorkloadId(workload_id);

    // 1. Find and delete task_meta entries
    for (auto& [pid, meta] : iterateMap(task_meta_map_fd_)) {
        if (meta.workload_id_hash == wid_hash) {
            bpf_map_delete_elem(task_meta_map_fd_, &pid);
        }
    }

    // 2. Delete CBS budget entry (L2)
    bpf_map_delete_elem(cbs_budget_map_fd_, &wid_hash);

    // 3. Delete partition entry
    uint64_t cgroup_id = workload_cgroup_map_[workload_id];
    bpf_map_delete_elem(partition_map_fd_, &cgroup_id);

    LOG_INFO("Cleaned up BPF maps for workload {}", workload_id);
}
```

---

## 6. Timing Guarantees

### 6.1 Worst-Case Latency

| Operation | Latency | Notes |
|:--|:--|:--|
| Shadow map update | O(n) slots | Background, non-blocking |
| Atomic swap | < 1 μs | Single u32 write |
| Table activation delay | 0 ~ 1 hyperperiod | Wait for boundary |

### 6.2 Maximum Transition Delay

```
Max Delay = Current Hyperperiod Duration

Example:
- Hyperperiod = 100ms
- Request arrives at t = 10ms
- Swap occurs at t = 100ms (next boundary)
- Max delay = 90ms
```

### 6.3 Why Hyperperiod Boundary?

Swapping at arbitrary times would cause:
1. **Partial slot execution**: Task might be preempted mid-slot
2. **Deadline miss**: New deadlines might already be passed
3. **Budget inconsistency**: CBS budgets not aligned with periods

Waiting for hyperperiod boundary ensures:
- All current slots complete naturally
- New table starts fresh with aligned budgets
- Deterministic behavior maintained

---

## 7. Comparison: Restart vs Zero-Downtime

| Scenario | Restart Approach | Zero-Downtime Approach |
|:--|:--|:--|
| Add Workload | Full daemon restart (~seconds) | Hyperperiod boundary swap (~ms) |
| Mode Transition | System reboot (~tens of seconds) | Atomic swap (< 1 μs) |
| Fault Workload Removal | Other workloads affected | Only target workload isolated |
| SDV Safety Impact | Vehicle control loss during restart | Continuous operation |

---

## 8. Open Items

- [ ] **B-2.1**: Implement `active_map_idx` BPF map — deferred; needed only for atomic full-table BPF-map replacement. The shipped incremental update path (§1.1) does not require it.
- [ ] **B-3.1**: Verify BPF atomic swap feasibility
- [ ] Measure actual swap latency on target hardware
- [ ] Define maximum hyperperiod for acceptable transition delay

---

## 9. References

- DDR-005: sched_ext BPF Scheduler Design
- DDR-006: Communication Architecture
- DDR-007: TT + CBS Integrated Scheduling
- Linux sched_ext documentation: `tools/sched_ext/`
