<!--
SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
SPDX-License-Identifier: MIT
-->

# DDR-006: Communication Architecture & Runtime Update

**Date:** 2026-04-24 (Last Updated: 2026-06-16)
**Status:** Draft
**Author:** Human (Lead Architect) + AI
**Related:** DDR-011 (Runtime Table Update) for zero-downtime schedule update mechanism

---

## 0. Implementation Status (Note for AI)

> **IMPORTANT**: The gRPC bidirectional communication and C++ daemon rework described in this document were **fully implemented in Phase 1**.
> **AI INSTRUCTION**: If you are an AI assistant, treat these components as existing infrastructure. Unless the User explicitly asks you to work on the "Open Items" (Section 7), **stop reading here and ignore the rest of this document** to save context and avoid generating redundant code.

---

## 1. Decision Summary

| Item | Decision |
|:--|:--|
| O↔N Protocol | **gRPC Bidirectional Stream** (replaces libtrpc) |
| timpani-n stack | **C++ daemon** (gRPC integration) + Pure C BPF |
| Runtime tables | Supported (hot updates without restart) |
| libtrpc | Fully deprecated |

---

## 2. Limits of Timpani 25 (libtrpc)

Timpani 25 relied on libtrpc (D-Bus), which is unidirectional (Client → Server only).
This caused critical limitations:
- **No push updates**: Server cannot notify nodes of workload changes.
- **Restart chains**: Any YAML change requires restarting pullpiri → timpani-o → every timpani-n.
- **No status**: Nodes could only report faults, not current health/utilization.

---

## 3. Interfaces (Illustrative)

timpani-o exposes a bidirectional stream. timpani-n connects to it.

```protobuf
syntax = "proto3";
package timpani.node.v1;

service OrchestratorService {
  rpc NodeStream (stream NodeEvent) returns (stream ControlCommand) {}
}

// timpani-n → timpani-o
message NodeEvent {
  string node_id = 1;
  oneof event {
    NodeReady    ready   = 2;  // Topology, kernel version, PREEMPT_RT/sched_ext status
    NodeStatus   status  = 3;  // Health, uptime, utilization
    FaultInfo    fault   = 4;  // Deadline misses (from DDR-003)
    TableApplied applied = 5;  // Ack of table push
  }
}

// timpani-o → timpani-n
message ControlCommand {
  oneof command {
    HierarchicalScheduleTable full_table = 1;  // Full swap
    ScheduleTableUpdate       update     = 2;  // Add/Remove/Modify
    ShutdownCommand           shutdown   = 3;  // Graceful exit
  }
}
```

---

## 4. C++ Rework Architecture

To support gRPC, the `timpani-n` userspace daemon is rewritten in C++. The BPF kernel code remains in C. The RT critical path is strictly isolated.

| Thread | Role | gRPC access |
|:--|:--|:--|
| **Timer Master** | RT slot firing (SCHED_FIFO) | ❌ Forbidden |
| **gRPC Client** | O↔N stream management | ✅ Dedicated |
| **BPF Loader** | Injects gRPC updates to BPF maps | Indirect |
| **Fault Monitor** | Polls `fault_ringbuf` | ✅ Reports |

---

## 5. Runtime Table Updates

Changes from Pullpiri are recalculated by timpani-o and **pushed** to timpani-n nodes, taking effect without process restarts.

### Transition Policy

| Update Type | Application Timing |
|:--|:--|
| Add/Remove independent workload | **Immediately** |
| Pipeline workloads / Full tables | **Next hyperperiod boundary** (swap shadow/active maps) |

### Independence & Graceful Degradation
- If `timpani-n` starts first: retries connection until `timpani-o` is up.
- If `timpani-o` restarts: `timpani-n` continues using the last loaded schedule, then syncs.

---

## 6. Deployment

| Component | Packaging | Rationale |
|:--|:--|:--|
| **timpani-o** | OCI Container | Standard microservice, no host kernel privileges required. |
| **timpani-n** | systemd service | Needs CAP_BPF, CAP_SYS_NICE, cgroup monitoring, PID namespaces. Starts before workload containers. |

---

## 7. Open Items

- [ ] gRPC Unix socket vs TCP (Single ECU vs Multi-ECU)
- [ ] BPF atomic swap feasibility for shadow map
- [ ] Exact NodeStatus polling rate
