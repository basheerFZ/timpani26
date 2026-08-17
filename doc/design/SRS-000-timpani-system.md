<!--
SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
SPDX-License-Identifier: MIT
-->

# SRS-000: TIMPANI Software Requirements Specification (SRS)

**Date:** 2026-08-10
**Status:** Draft
**Author:** Human + AI

> This is the **system-wide requirements baseline** for the TIMPANI design set. It is a *requirements specification*, not a Design Decision Record (DDR), and therefore lives in the `SRS-*` namespace rather than under a `DDR-NNN` number. Per-feature requirement specs (e.g. `SRS-008-…`) elaborate the requirements of a specific decision record.
>
> **Keyword convention**: **SHALL** = mandatory, **SHOULD** = recommended.

---

## 1. Introduction

This document defines the system, functional, and non-functional requirements that the Timpani-Next (2026) framework must satisfy. TIMPANI is a real-time scheduling engine that must guarantee **Dual Determinism** — temporal determinism + functional determinism — in Software-Defined-Vehicle (SDV) and AI-Native environments.

- **Reference documents**: DDR-001 ~ DDR-012, `TIMPANI-on-QNX.md`

---

## 2. Environment Constraints

| ID | Requirement | Priority | Mapping |
|:--|:--|:-:|:--|
| **REQ-ENV-001** | The kernel SHALL be Linux 6.12+ with `sched_ext` support. | High | DDR-005 |
| **REQ-ENV-002** | `PREEMPT_RT` SHALL be applied to achieve tens-of-microsecond timing precision. | High | DDR-002, DDR-005 |
| **REQ-ENV-003** | `cgroup v2` SHALL be used for workload resource isolation (isolated partitions). | High | DDR-002, DDR-004 |

---

## 3. Functional Requirements

### 3.1 Workload & Scheduling Model
| ID | Requirement | Mapping |
|:--|:--|:--|
| **REQ-FUN-001** | The system SHALL classify workloads by TemporalClass (Periodic/Sporadic) and CriticalityClass (Safety/NonSafety). | DDR-001 |
| **REQ-FUN-002** | Classified workloads SHALL be scheduled across four HSF (Hierarchical Scheduling Framework) layers, L1 (Strict RT) ~ L4 (Background). | DDR-001, DDR-002 |
| **REQ-FUN-003** | L1 (Periodic + Safety) workloads SHALL run from a statically generated Schedule Table (TT slots) produced by Timpani-O, ensuring consistency. | DDR-002 |
| **REQ-FUN-004** | L2 (Sporadic + Safety) workloads SHALL run event-driven, with CPU budget controlled by a CBS (Constant Bandwidth Server) mechanism. | DDR-002, DDR-005, DDR-008 |
| **REQ-FUN-005** | L3/L4 (NonSafety) workloads SHALL be fully delegated to the Linux default scheduler (CFS) with no Timpani-N BPF-runtime intervention. | DDR-002 |

### 3.2 Runtime Control & Safety
| ID | Requirement | Mapping |
|:--|:--|:--|
| **REQ-FUN-006** | To guarantee a minimum monitoring cadence for L2 Sporadic Safety workloads, a Dual-Mode schedule combining CBS with a TT floor SHALL be supported. | DDR-002, DDR-007 |
| **REQ-FUN-007** | An API (`ttsched_wait_next_period()`, low-latency/futex-based) SHALL be provided so an application's period aligns exactly with its TT slot. | DDR-005 |
| **REQ-FUN-008** | When an L1/L2 workload overruns its budget or misses its deadline, the system SHALL detect it and report to upper layers via a fault ring buffer (`fault_ringbuf`). | DDR-005, DDR-008 |
| **REQ-FUN-009** | Event-driven (L2) workloads SHALL be identified/observed at the middleware dispatch boundary, transport-independently, and their MIT (Minimum Inter-arrival Time) violations and latency SHALL be monitored. | DDR-008 |

### 3.3 Interface & Communication
| ID | Requirement | Mapping |
|:--|:--|:--|
| **REQ-FUN-010** | The Timpani-O ↔ Timpani-N control channel SHALL use a gRPC bidirectional stream (`OrchestratorService.NodeStream`), superseding the legacy libtrpc/D-Bus transport. | DDR-003, DDR-006 |
| **REQ-FUN-011** | Pullpiri SHALL deliver schedule/workload information to Timpani-O (`SchedInfoService`) and enforce recovery through `RecoveryService`. | DDR-003, DDR-006, DDR-012 |

### 3.4 Runtime Schedule Update
| ID | Requirement | Mapping |
|:--|:--|:--|
| **REQ-FUN-012** | The system SHALL add, remove, and modify workloads at runtime without restarting any daemon, applying changes deterministically at a hyperperiod boundary (zero-downtime). | DDR-011 |

### 3.5 Resource Allocation
| ID | Requirement | Mapping |
|:--|:--|:--|
| **REQ-FUN-013** | Timpani-O SHALL compute a feasible CPU/partition placement for L1/L2 workloads and reject an infeasible schedule (schedulability / feasibility check). | DDR-004, DDR-007 |

### 3.6 Fault & Recovery
| ID | Requirement | Mapping |
|:--|:--|:--|
| **REQ-FUN-014** | The system SHALL detect and report the scheduling-anomaly classes — deadline miss, CBS budget overrun, MIT violation, wakeup-latency exceedance — distinguishing normal isolation control (e.g. budget exhaustion) from true faults. | DDR-008, DDR-012 |
| **REQ-FUN-015** | On a recovery decision, the system SHALL enforce STOP/RESTART via `RecoveryService.EnforceRecoveryPolicy` → `RecoverySignal`, evicting the workload's schedule-table entries and BPF state. | DDR-003, DDR-011, DDR-012 |

### 3.7 Time Synchronization
| ID | Requirement | Mapping |
|:--|:--|:--|
| **REQ-FUN-016** | Multi-node deployments SHALL align to a shared PTP-derived `epoch_ns` so hyperperiod phases are consistent across nodes. | DDR-003, DDR-011 |

---

## 4. Non-Functional Requirements

| ID | Requirement | Class | Mapping |
|:--|:--|:--|:--|
| **REQ-NFR-001** | **Temporal determinism**: timer-firing jitter for L1 TT workloads SHALL be bounded to tens of μs. | Performance | DDR-002, DDR-011 |
| **REQ-NFR-002** | **Freedom From Interference (FFI)**: hardware-level CPU isolation between Safety (L1/L2) and NonSafety (L3/L4) SHALL be guaranteed (toward ISO 26262). | Safety | DDR-002, DDR-004 |
| **REQ-NFR-003** | **Application transparency**: workloads SHALL be scheduled via cgroup mapping without setting kernel thread scheduling attributes (SCHED_FIFO, priority) themselves. | Usability | DDR-002, DDR-005 |
| **REQ-NFR-004** | **Restart resilience**: a workload's identity SHALL be resolved primarily from Pullpiri deployment metadata + cgroup; thread name (`comm`) is a PoC/diagnostic fallback only. On container restart (pid change), policy SHALL be re-applied via this identity. | Reliability | DDR-005, DDR-008 (Decision 3B) |
| **REQ-NFR-005** | **Multi-core scalability**: the timer subsystem SHALL scale across isolated CPUs (per-CPU timer threads) so that slots scheduled at the same instant on different CPUs fire correctly. | Performance | DDR-011 |

---

## 5. Roadmap & Future Requirements

Forward-looking requirements that are not yet detailed at the current development stage. *(Specific timelines and productization targets are tracked internally and intentionally omitted here.)*

### 5.1 Near-term
| ID | Requirement |
|:--|:--|
| **REQ-AI-001** | Derive concrete control requirements for AI workloads (e.g. NPU allocation, chunk-based execution). *(future scope)* |
| **REQ-ED-001** | Provide dispatch-boundary observation + CBS-linked control (Event Monitor) for sporadic (L2) workloads, validated with a SOME/IP ADAS (FCW) PoC. *(future scope; see DDR-008)* |
| **REQ-DD-001** | Provide pipeline/DAG data-flow control with deadline tracking, validated with a Data-Driven (sensor-fusion) PoC. *(future scope)* |

### 5.2 Longer-term
| ID | Requirement |
|:--|:--|
| **REQ-SAF-001** | The system SHOULD be architected to support functional-safety compliance (e.g. ISO 26262 ASIL-B) via a fault handler and safety architecture. *(planned)* |
| **REQ-ENV-004** | The architecture SHOULD support portability to commercial RTOSes such as QNX via a hardware/OS abstraction layer (HAL). *(planned; see `TIMPANI-on-QNX.md`)* |
| **REQ-AD-001** | Subject to a feasibility study, define an AD-framework abstraction layer (FAL) for platforms such as ROS2 / Autoware. *(planned)* |

---
