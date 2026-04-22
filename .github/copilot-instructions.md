```instructions
#<!--
#* SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
#* SPDX-License-Identifier: MIT
#-->

# GitHub Copilot Instructions — TIMPANI Project

## Session Startup Checklist (MANDATORY)

**Every time a new VS Code Copilot chat session opens, you MUST complete the
following steps before responding to any request.**

Failure to complete these steps means you are operating without the current
source state and design intent — all responses will be unreliable.

### Step 1 — Sync the mainline source (`origin/timpani26`)

```bash
git fetch origin timpani26
git log --oneline -20 origin/timpani26
```

This gives you an overview of recent changes and the current direction
of the codebase. Do **not** read source files upfront — read them
on demand as the user's request requires. Use
`git show origin/timpani26:<filepath>` to read a specific file from
the remote branch without switching your local checkout.

### Step 2 — Read the design documents (`origin/dev/design-docs`)

```bash
git fetch origin dev/design-docs
git show origin/dev/design-docs --name-only | grep "^doc/design"
```

Read every DDR in `doc/design/` from that branch:

```bash
git show origin/dev/design-docs:doc/design/DDR-001-workload-model.md
git show origin/dev/design-docs:doc/design/DDR-002-scheduling-architecture.md
git show origin/dev/design-docs:doc/design/DDR-003-interface-protocol.md
git show origin/dev/design-docs:doc/design/DDR-004-resource-allocation.md
git show origin/dev/design-docs:doc/design/DDR-005-sched-ext-bpf-scheduler.md
git show origin/dev/design-docs:doc/design/DDR-006-communication-architecture.md
git show origin/dev/design-docs:doc/design/DDR-007-tt-cbs-integrated-scheduling.md
git show origin/dev/design-docs:doc/design/HSF-concept.md
```

> **Why**: `dev/design-docs` is maintained by PL separately from the
> implementation branch. It may contain newer decisions not yet merged
> into `timpani26`. Always treat these documents as the authoritative
> design specification.

### Step 3 — Confirm readiness

After completing Steps 1 and 2, briefly state:
- The HEAD commit hash and message of `origin/timpani26`
- The HEAD commit hash and message of `origin/dev/design-docs`
- Any DDRs that are in Draft status (action items for the session)

Only then proceed to answer the user's request.

---

## Project at a Glance

TIMPANI is a **real-time execution engine** for SDV (Software Defined Vehicle) systems that guarantees temporal determinism for workloads requiring it.

TIMPANI operates as a cooperative partner of **Pullpiri** (Workload Orchestrator):
- **Pullpiri** (developed by a separate team): Decides *what* runs *where* — manages the full workload lifecycle (deploy, start, stop, monitor)
- **TIMPANI**: Guarantees *when* and *within what CPU budget* — handles only L1/L2 workloads that require determinism

Two main components:
- **Timpani-O** (`timpani-o/`): Generates the global static Schedule Table from workload info received from Pullpiri, then streams it to Timpani-N via gRPC (`node_control.proto`, `OrchestratorService.NodeStream`). Table generation will migrate to an offline tool in the future.
- **Timpani-N** (`timpani-n/`): Node executor — receives the schedule table from Timpani-O via gRPC and enforces it at runtime via eBPF (`scx_timpani`). Manages container-based workloads prepared by Pullpiri, ensuring they run according to the schedule table.

### Team Scope

Our team develops **timpani-n/, timpani-o/, and sample-apps/** (C/C++ implementation).
- `timpani_rust/` is planned as part of Timpani 26 but will be developed by a separate team — NOT our scope.
- `pullpiri/` is developed by another separate team.

### Project Leader (PL)

The human Project Leader is the **final decision authority** for all design, implementation, and release decisions:
- PL approves proto file changes, new dependencies, and interface modifications
- When in doubt, ask PL — do not proceed autonomously on cross-component decisions

---

## Architecture: Core Concepts

### Pullpiri + Timpani Role Separation

| Component | Role | Scope |
|:--|:--|:--|
| **Pullpiri** | Workload Orchestrator (separate team) | Receives workload description (YAML), starts container-based workloads on each node, sends workload info to Timpani-O (proto format) |
| **Timpani-O** | Schedule Table Generator | Currently: receives workload info from Pullpiri + node resource YAML → generates global static schedule table + CBS budgets. Future: table generation migrates to offline tool; Timpani-O may merge into Pullpiri as table relay. |
| **Timpani-N** | RT Execution Engine | Receives schedule table (source-agnostic), enforces it via eBPF (scx_timpani), executes each task of container-based workloads per schedule table on Isolated CPUs |
| **scx_timpani** | Kernel Scheduler | sched_ext/eBPF scheduler — executes table commands at kernel level |

### Current Runtime Flow

```
① Pullpiri (separate team)
    │  receives workload description (YAML)
    │  starts container-based workloads on each node
    │  sends workload info to Timpani-O (proto format)
    ▼
② Timpani-O
    │  receives workload info from Pullpiri
    │  reads node resource info from YAML config (available CPUs, etc.)
    │  generates global static schedule table + CBS budgets
    │  sends schedule table to Timpani-N (currently libtrpc/D-Bus, migrating to gRPC per DDR-006)
    ▼
③ Timpani-N
    │  receives schedule table
    │  enforces it via eBPF (scx_timpani)
    │  executes each task of container-based workloads per schedule table
    ▼
L1/L2 Container Workloads (deployed by Pullpiri, task-scheduled by Timpani-N)
```

### Future Architecture (Offline Tool Migration)

```
① Offline Schedule Table Generation Tool (outside Pullpiri)
    │  has pre-configured node resource info (CPUs available for Timpani)
    │  receives workload description (same format as current Pullpiri input)
    │  generates global static schedule table
    ▼
② Pullpiri (Timpani-O may become part of Pullpiri)
    │  receives pre-computed schedule table from offline tool
    │  starts container-based workloads on each node
    │  forwards schedule table to Timpani-N
    ▼
③ Timpani-N
    │  receives schedule table
    │  enforces it via eBPF (scx_timpani)
    │  executes each task of container-based workloads per schedule table
    ▼
L1/L2 Container Workloads
```

**Key changes in future architecture:**
- Schedule table generation moves from online (Timpani-O at runtime) to offline (pre-deployment tool)
- Timpani-O's role may merge into Pullpiri as a table relay/adapter
- Timpani-N remains unchanged — receives and enforces schedule table regardless of source
- Offline tool + WCET Analyzer provide a streamlined workflow for system designers

### L1~L4 Workload Classification

| Layer | Name | Guarantee | CPU Placement |
|:--|:--|:--|:--|
| **L1** | Strict Deterministic | Execution Jitter < 1ms target | Isolated CPU |
| **L2** | Budget-Bounded | CPU budget cap enforced; preempted on overrun | Isolated CPU |
| **L3** | Best-Effort | Response within available slack | Non-Isolated CPU |
| **L4** | Background | No guarantee | Non-Isolated CPU |

- **L1/L2**: Managed by Timpani. Assigned to Isolated CPUs via static table or CBS slots.
- **L3/L4**: Outside Timpani's scope. Delegated to Linux CFS on Non-Isolated CPUs.

### HSF (Hierarchical Scheduling Framework) Tree

```
Level 0 (Root / Offline Tool)
└── Generates static Schedule Table from L1~L4 workload parameters

    Level 1 (Timpani-N / Intermediate Node)
    └── Receives table from Timpani-O; enforces budgets via eBPF
        - FFI: Preempts on budget overrun to prevent interference
        - CBS: Manages Sporadic L1/L2 workloads within reserved slots

        Level 2 (Leaf / Workloads)
        └── Runs within delegated budget on Isolated CPUs
```

### Static Table + CBS Extension

- **Periodic workloads**: Placed in static time-triggered slots by Timpani-O.
- **Data-Flow workloads (Dependent Periodic)**: DAG pipeline analyzed offline; CPU stages unrolled into sequential periodic slots.
- **Sporadic workloads (Event-Driven, L1/L2 only)**: CBS (Constant Bandwidth Server) slots reserved in the static table. At runtime, Timpani-N enforces the CBS budget per MIT/WCET. L3/L4 workloads are NOT CBS targets.

---

## Workspace Structure

```
TIMPANI/
├── timpani-n/                  # Node executor (C++ + BPF)
│   ├── CMakeLists.txt
│   └── src/
│       ├── main.cpp            # Entry point, signal handling, option parsing
│       ├── timer_master.cpp/h  # Main RT loop — SCHED_FIFO timer dispatch
│       ├── bpf_loader.cpp/h    # BPF program load/unload (libbpf CO-RE)
│       ├── fault_monitor.cpp/h # Budget overrun detection via BPF ring buffer
│       ├── task_registry.cpp/h # cgroup scan, task_id → pid mapping
│       ├── grpc/
│       │   └── node_client.cpp/h  # gRPC client — connects to Timpani-O NodeStream
│       └── bpf/
│           ├── timpani.bpf.c   # sched_ext BPF scheduler (TT + CBS + BE DSQs)
│           └── maps.h          # Shared BPF map definitions
│
├── timpani-o/                  # Schedule table generator (C++)
│   ├── CMakeLists.txt
│   ├── proto/
│   │   ├── schedinfo.proto         # Pullpiri ↔ Timpani-O protocol
│   │   └── node_control.proto      # Timpani-O ↔ Timpani-N protocol (NodeStream)
│   └── src/
│       ├── main.cpp
│       ├── global_scheduler.cpp/h      # GlobalScheduler: table generation
│       ├── scheduler_utils.cpp/h       # Feasibility analysis, EDF/RM utils
│       ├── hyperperiod_manager.cpp/h   # LCM-based hyperperiod calculation
│       ├── node_config.cpp/h           # Node YAML config loading
│       ├── schedinfo_service.cpp/h     # gRPC handler for Pullpiri input
│       ├── orchestrator_service.cpp/h  # gRPC NodeStream server (→ Timpani-N)
│       ├── table_builder.cpp/h         # Schedule table → protobuf serialization
│       ├── fault_client.cpp/h          # Fault reporting to Pullpiri
│       ├── sched_info.h                # Internal scheduling data structures
│       ├── task.h                      # Task / TaskSet type definitions
│       └── tlog.h                      # Logging utilities
│
├── sample-apps/                # Validation workload apps (C)
│   ├── CMakeLists.txt
│   └── src/
│       ├── sample_apps.c/h     # Workload implementations (L1~L4 examples)
│       ├── sched.c             # Scheduling policy helpers for sample apps
│       ├── libttsched.c/h      # Time-triggered scheduling API
│       └── version.h.in        # Build-time version info
│
├── libtrpc/                    # tRPC IPC library (unused by timpani-n/o; kept for reference)
├── libbpf/                     # libbpf submodule (BPF CO-RE support)
├── timpani_rust/               # Separate team's scope — NOT our target
└── .github/
    ├── copilot-instructions.md  # This file
    └── commit-message-instructions.md
```

---

## Code Style Requirements

### C / BPF (`timpani-n/src/bpf/`, `sample-apps/src/`)
- **Style**: Linux kernel coding style (`scripts/checkpatch.pl` standard)
- **Indentation**: 8-space tabs
- **Naming**: `snake_case` for functions and variables; `UPPER_CASE` for macros/constants
- **Error handling**: Always check return values. Use `goto cleanup` pattern for resource cleanup.
- **Comments**: Doxygen-style `/** ... */` above function definitions
- **Headers**: `#pragma once` or `#ifndef _HEADER_NAME_H_` guards

### C++ (`timpani-n/src/`, `timpani-o/src/`)
- **Style**: 4-space indentation
- **Naming**: `snake_case` functions/variables; `PascalCase` classes; `UPPER_CASE` constants
- **Error handling**: Use RAII and explicit return code checks. No bare `new`/`delete` — prefer smart pointers.
- **No exceptions** in hot paths
- **No gRPC/async** in the RT critical path (Timer Master thread)

### SPDX header (required on every new source file)
```c
// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT
```

---

## Dependencies Reference

| Library | Purpose | Location |
|:--|:--|:--|
| `libbpf` | BPF CO-RE support for eBPF programs | `libbpf/` |
| `protobuf` / gRPC | Pullpiri ↔ Timpani-O (`schedinfo.proto`), Timpani-O ↔ Timpani-N (`node_control.proto`) | `timpani-o/proto/` |
| `libc` / POSIX RT | `clock_nanosleep`, `SCHED_FIFO` (Timer Master thread only) | System |

Do not add new external dependencies without explicit instruction.

---

## What NOT to Do

- Do not target `timpani_rust/` — it is NOT our team's development scope
- Do not modify proto files without explicit approval
- Do not put gRPC or async logic in the RT critical path (Timer Master thread in timpani-n)
- Do not use unordered containers where deterministic ordering matters — use sorted structures
- Do not create new public APIs without checking with the Project Leader (PL)
- Do not delete or rename existing public methods in `TimerMaster`, `BpfLoader`, `TaskRegistry`, `FaultMonitor`, or `GlobalScheduler`

---

## Git Workflow

### Branch Structure

| Branch | Purpose | Who uses it |
|:--|:--|:--|
| `timpani26` | Mainline — team-reviewed, stable | All developers |
| `dev/design-docs` | DDR documents (pending team review → MR to timpani26) | PL |
| `dev/sh-agent-md` | Agent instruction files (PL only, never merge to timpani26) | PL |
| `dev/timpani26-implementation-phase1` | Active implementation — Phase 1 | Developers + AI agents |

### Working Branch Rule

**Always work on `dev/timpani26-implementation-phase1`.**

```bash
git checkout dev/timpani26-implementation-phase1
git pull origin dev/timpani26-implementation-phase1
```

- Read task from `.agent/tasks/<task>.md`
- Implement, build, test
- Commit to `dev/timpani26-implementation-phase1`
- Push and request review from PL

### Task File Lifecycle

```bash
# Task assigned: file appears in .agent/tasks/
git pull origin dev/timpani26-implementation-phase1
cat .agent/tasks/<task>.md

# Task complete: PL removes file with dedicated commit
# chore(agent): remove completed task <task>
```

### What NOT to Do

- Do NOT push directly to `timpani26`
- Do NOT push to `dev/design-docs` or `dev/sh-agent-md`
- Do NOT merge branches — always use MR/PR via GitLab
- Do NOT force-push without explicit PL instruction

---

## Testing

All new functions must have accompanying unit tests.

- **timpani-n**: Tests go in `timpani-n/test/`
- **timpani-o**: Tests go in `timpani-o/tests/`
- **sample-apps**: Build and run on target hardware (Raspberry Pi 5) or via Docker

Build and run:
```bash
# timpani-n
cd timpani-n && mkdir -p build && cd build
cmake .. && make -j$(nproc)

# timpani-o
cd timpani-o && mkdir -p build && cd build
cmake .. && make -j$(nproc)
ctest --output-on-failure
```

---

## Design Decision Records

Design decisions for Timpani 26 are tracked in `doc/design/`:

| DDR | Title | Status |
|:--|:--|:--|
| DDR-001 | Workload Model (L1~L4, 3-axis classification) | ✅ Accepted |
| DDR-002 | Scheduling Architecture (HSF + sched_ext) | ✅ Accepted |
| DDR-003 | Interface / Protocol Design | ✅ Accepted |
| DDR-004 | Resource Allocation Algorithm | ✅ Accepted |
| DDR-005 | sched_ext BPF Scheduler Detail | 🔄 Draft |
| DDR-006 | Communication Architecture & Runtime Update | 🔄 Draft |
| DDR-007 | TT + CBS Integrated Scheduling | 🔄 Draft |

---

## Reference: Key Source Files

| Feature | Primary File |
|:--|:--|
| Entry point & signal handling | `timpani-n/src/main.cpp` |
| RT timer dispatch loop | `timpani-n/src/timer_master.cpp` |
| BPF program load/unload | `timpani-n/src/bpf_loader.cpp` |
| Budget overrun detection | `timpani-n/src/fault_monitor.cpp` |
| cgroup / task-id mapping | `timpani-n/src/task_registry.cpp` |
| gRPC client (→ Timpani-O) | `timpani-n/src/grpc/node_client.cpp` |
| sched_ext BPF scheduler | `timpani-n/src/bpf/timpani.bpf.c` |
| Schedule table generation | `timpani-o/src/global_scheduler.cpp` |
| Feasibility analysis | `timpani-o/src/scheduler_utils.cpp` |
| Table → protobuf serialization | `timpani-o/src/table_builder.cpp` |
| gRPC NodeStream server (→ Timpani-N) | `timpani-o/src/orchestrator_service.cpp` |
| gRPC handler (Pullpiri input) | `timpani-o/src/schedinfo_service.cpp` |
| Sample workloads | `sample-apps/src/sample_apps.c` |
```
