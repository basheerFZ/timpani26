<!--
SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
SPDX-License-Identifier: MIT
-->

# DDR-001: TIMPANI Workload Model

**Date:** 2026-04-06 (Last Updated: 2026-06-15)
**Status:** Accepted
**Author:** Human (Lead Architect) + AI

---

## 1. Decision

TIMPANI classifies all workloads using two axes — **TemporalClass** and **CriticalityClass** — and maps the result to **L1–L4 layers**. A `WorkloadSpec` data model carries these attributes.

The **Pipeline** concept groups multiple workloads under a shared hyperperiod for distributed DAG scenarios.

---

## 2. Context

### Pullpiri + Timpani Role Separation

| Component | Role |
|:--|:--|
| **Pullpiri** | Workload Orchestrator — what runs where (lifecycle, classification) |
| **Timpani** | RT Execution Engine — when and within what CPU budget deterministic workloads run |

Pullpiri classifies workloads into L1–L4 and delivers them to TIMPANI. TIMPANI-O generates a schedule table, and TIMPANI-N enforces it at the kernel level (eBPF/sched_ext).

### Workload Definition

A workload is an **OCI Container** extended with RT temporal attributes (`WorkloadSpec`). TIMPANI treats a workload as a **black box** (HSF Compositionality). A single workload is bound to **one node**.

### Pipeline

Data-flow pipelines spanning multiple nodes are grouped into a **Pipeline** with a shared hyperperiod, DAG dependency, and E2E deadline. Time synchronization is based on gPTP (IEEE 802.1AS).

---

## 3. Workload Classification (Two-Axis)

### Axis 1: TemporalClass (timpani-managed only)

| Classification | Definition | Parameters |
|:--|:--|:--|
| `Periodic` | Fixed-period repeated execution | `period_us` |
| `Sporadic` | Event-activated, minimum inter-arrival guaranteed | `min_inter_arrival_us` |

> **Aperiodic Excluded**: Aperiodic workloads have no MIT (Minimum Inter-arrival Time), making CBS inapplicable.
> They are not managed by timpani; Pullpiri deploys them directly as L3/L4 (CFS).

### Axis 2: CriticalityClass

| Classification | Definition | L1–L4 Scope |
|:--|:--|:--|
| `SafetyCritical` | ISO 26262 functional safety subject | **L1 or L2 only** (Isolated CPU) |
| `NonSafety` | No safety obligation (QM) | **L3 or L4 only** (Non-Isolated CPU) |

---

## 4. L1–L4 Mapping

```
                │  SafetyCritical          │  NonSafety
────────────────┼──────────────────────────┼──────────────────────
Periodic        │  L1 (TT slot, Isolated)  │  (Not managed by timpani)
Sporadic        │  L2 (CBS, Isolated)      │  (Not managed by timpani)
```

| Layer | Name | Safety | Scheduling | CPU |
|:--|:--|:--|:--|:--|
| **L1** | Strict Deterministic | Safety | Static TT slot | Isolated |
| **L2** | Budget-Bounded | Safety | CBS budget | Isolated |
| **L3** | Best-Effort | Non-Safety | CFS | Non-Isolated |
| **L4** | Background | Non-Safety | CFS (lowest) | Non-Isolated |

**Rules:**
- **Managed by timpani**: SafetyCritical workloads only (L1/L2)
- **Not managed by timpani**: NonSafety workloads (L3/L4) — Pullpiri deploys directly (CFS, cgroup cpuset)
- Periodic + SafetyCritical → L1 (TT). Sporadic + SafetyCritical → L2 (CBS).

> For `Sporadic + SafetyCritical` (L2), CBS alone may not satisfy ISO 26262 FFI. The dual-mode approach (TT_FLOOR watchdog) is defined in DDR-002.

---

## 5. WorkloadSpec Data Model

### Workload vs Task

- **Workload**: Budget isolation boundary (cgroup). May contain multiple task threads.
- **Task**: Kernel scheduling unit (`task_struct`). TT slots are generated per task thread.

| L1–L4 | `task_specs` | Reason |
|:--|:--|:--|
| L1 | Required | Per-thread deadline is a safety requirement |
| L2 | Required | Per-task budget control on Isolated CPU |
| L3/L4 | Not needed | cgroup quota only |

### Hyperperiod

- Per-workload: `LCM(all internal task periods)`
- Global: `LCM(all L1/L2 workload hyperperiods)`

### Proto Definition (Example — subject to change)

> The following is an illustrative example. The final proto definition will be finalized in DDR-003.

```protobuf
// Example — not finalized
enum TemporalClass {
  PERIODIC = 0;   // L1: TT slot
  SPORADIC = 1;   // L2: CBS
}

enum CriticalityClass {
  SAFETY_CRITICAL = 0;  // L1/L2 (managed by timpani)
  NON_SAFETY      = 1;  // L3/L4 (not managed by timpani)
}

message WorkloadSpec {
  string           workload_id      = 1;
  string           name             = 2;
  TemporalClass    temporal_class   = 3;  // Periodic / Sporadic
  CriticalityClass criticality      = 4;  // SafetyCritical / NonSafety → determines L1-L4
  repeated TaskSpec task_specs      = 5;  // L1/L2: required, L3/L4: ignored
  string           pipeline_id      = 6;  // Set when part of a distributed DAG Pipeline (optional)
}

message TaskSpec {
  string task_id              = 1;
  uint32 period_us            = 2;  // L1 only: TT period
  uint32 min_inter_arrival_us = 3;  // L2 only: minimum inter-arrival time (MIT)
  uint32 wcet_us              = 4;
  uint32 deadline_us          = 5;
}
```

**Field Validation Rules**:

| Layer | `temporal_class` | `period_us` | `min_inter_arrival_us` |
|:--|:--|:--|:--|
| L1 | `PERIODIC` | **Required** | Ignored |
| L2 | `SPORADIC` | Ignored | **Required** |
| L3/L4 | — | Ignored | Ignored |

> **Design Principle**: `period_us` and `min_inter_arrival_us` have different semantics, so they are separate fields.
> Clarity is more important than proto buffer savings.

---

## 6. AI Workloads

**Status: TBD.** AI workload classification is not yet defined and will be addressed in a future DDR as NPU/GPU scheduling integration matures.

---

## 7. Affected Components

- `timpani-o/src/` — WorkloadSpec processing and L1–L4 classification
- `timpani-o/proto/` — WorkloadSpec proto definition (DDR-003)
- `timpani-n/src/` — Execution policy branching by L1–L4
