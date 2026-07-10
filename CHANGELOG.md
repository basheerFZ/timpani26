# Changelog

All notable changes to the TIMPANI project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Calendar Versioning](https://calver.org/) (YYYY.MM.PATCH).

## [Unreleased]

## [2026.07.0] - 2026-07-10

Major feature release introducing CBS scheduling Phase 2, Fault Monitor integration, and various stability fixes.

### timpani-n

#### Added
- **TT+CBS Scheduling Phase 2**: Implemented advanced TT+CBS scheduling logic and CBS deadline miss detection in BPF.
- **Fault Monitor Integration**: 
  - Added threshold-based fault filtering and max_dmiss propagation.
  - Implemented TT/CBS Execution Domain Eviction upon receiving `ACTION_STOP` recovery signals.
  - Handled RecoverySignal Stop Action and encapsulated dmiss count tracking.
- **TimerMaster Improvements**: Implemented logic to catch up stale epochs for accurate scheduling.
- **Routing & Lifecycles**:
  - Route TT tasks to assigned CPUs using `SCX_DSQ_LOCAL_ON`.
  - Added deletion interfaces to BpfLoader and TimerMaster.

#### Fixed
- Broadcast shutdown on TimerMaster exit to prevent hanging processes.
- Removed stale `partition_map` update from TaskRegistry.
- Enhanced CPU kick for replenished throttled tasks.

### timpani-o

#### Added
- **Fault Management**: Forward faults, send RecoverySignal, and enforce stop-state sync with fault forward retry queue.
- **TT+CBS Support**: Introduced TT+CBS scheduling table support and added grpcurl test scripts.
- **Build & CI**: Added Dockerfile for Alpine-based builds.

#### Fixed
- Prevented full schedule push during `ACTION_STOP`.
- Clarified hyperperiod computation in the global scheduler.

### sample-apps

#### Added
- Added `cbstask` for CBS workload simulation.

#### Fixed
- Set `SCHED_EXT` policy correctly and fixed SHM lifecycle.
- Skip workload execution when `ttsched` is not ready.
- Updated `ALGO_BUSY` behavior for simulations.
## [2026.04.2] - 2026-04-27

Patch release for sample-apps IPC compatibility and DDR-005 BPF scheduler compliance.

### timpani-n

#### Added
- BPF scheduler: Apply DDR-005 partial compliance requirements (Gap 3, 4, 7, 8, 9)
  - Route unregistered tasks to `SCX_DSQ_GLOBAL`
  - Enforce `SCX_OPS_SWITCH_PARTIAL`
  - Utilize `BPF_STRUCT_OPS_SLEEPABLE(init)`
  - Track `actual_completion_ns` for deadline misses

#### Fixed
- `dispatch()`: Added `SLOT_NONE` handling to prevent invalid slot dispatches
- `sample_apps`: Disabled stdout buffering for correct logging in container IPC environments

## [2026.04.1] - 2026-04-17

Patch release with multi-node support enhancements, version traceability, and build improvements.

### timpani-n

#### Added
- Version tracking: `-V` shows version, Git commit hash, and build timestamp
- `version.h.in`: compile-time version embedding via CMake `configure_file()`

### sample-apps

#### Added
- Version tracking: `--version` shows version, Git commit hash, and build timestamp
- `version.h.in`: compile-time version embedding via CMake `configure_file()`

### timpani-o

#### Added
- Schedule replay for reconnected nodes — resend schedule table on timpani-n reconnect (f2fa4ce)

### Build

#### Added
- CMakeLists.txt: VERSION file fallback chain for container/CI builds
  (local VERSION → parent VERSION → `0.0.0-dev`)
- CMakeLists.txt: Git commit hash and build timestamp injection via `configure_file()`
- Dockerfile (`sample-apps`, `timpani-o`): `--build-arg VERSION` and
  `--build-arg GIT_COMMIT_HASH` support
- Dockerfile: `LABEL version` in runtime stage (fixes multi-stage build label propagation)

#### Changed
- Remove duplicate `timpani-o/VERSION` and `timpani-o/CHANGELOG.md`
- All components reference root `VERSION` file as single source of truth

### Documentation

#### Added
- [VERSIONING.md](doc/VERSIONING.md): Version management guide (CalVer policy, release process)
- Component READMEs: version build and verification guidance
  - Local build: Git hash auto-detected; CI/package: explicit `-DGIT_COMMIT_HASH`
  - Container verify: `podman run --rm ... --version` (sample-apps) /
    `podman inspect ... Labels` (timpani-o)

---

## [2026.04.0] - 2026-04-16

First unified versioned release with container and systemd deployment support.

### timpani-o

#### Added
- **Container Deployment**: Docker and Podman support
  - Multi-stage Dockerfile (Ubuntu 22.04 build → Alpine 3.21 runtime)
  - docker-compose.yml for orchestration
  - gRPC ports exposed: 50052 (SchedInfo), 50060 (Orchestrator)

#### Changed
- Remove libtrpc/D-Bus dependencies (gRPC migration complete)
- Add `get_connected_node_ids()` for multi-node support

#### Fixed
- Push schedule table to connected nodes instead of self hostname (aa797bc)

### timpani-n

#### Added
- **Systemd Service**: Production deployment support
  - `timpani-n.service`: systemd unit with fine-grained capabilities
  - `timpani-n.conf`: Environment configuration file
  - Security hardening (ProtectSystem, PrivateTmp, NoNewPrivileges)
- **DEB/RPM Packaging**: CPack-based package generation
  - Post-install script: systemctl daemon-reload, enable service
  - Dependencies: libelf, zlib, protobuf, grpc
- **Documentation**: README rewritten in timpani-o style
  - Architecture diagram, Quick Start guide
  - Removed deprecated README.Ubuntu20.md, README.CentOS.md
- CLI options: `-n <node_id>`, `-p <port>` (79c9a83)
- Schedule replay for reconnected nodes (d12760c)

### sample-apps

#### Added
- **Container Deployment**: Docker and Podman support
  - Multi-stage Dockerfile (Ubuntu 22.04 build → Alpine 3.21 runtime)
  - docker-compose.yml with Phase 1 verification tasks
  - Requires `--privileged --pid=host --ipc=host` for BPF mode

---

## Pre-release History

The following changes were made before formal version management was introduced.

### Phase 1 Implementation (2026-04)

| Component | Feature | Commit |
|:--|:--|:--|
| timpani-n | C++ rework + gRPC | G2, G3 |
| timpani-n | sched_ext BPF scheduler PoC | G1 |
| timpani-n | Timer Master (ABSTIME + jitter histogram) | G1 |
| timpani-n | select_cpu partition_map lookup | N2 |
| timpani-n | kick_cpu via BPF kick_map | N3 |
| timpani-n | CBS budget enforcement in BPF | N1 |
| timpani-n | Dynamic deadline from tt_table_map | N4 |
| timpani-o | table_builder (NodeSchedInfoMap → HierarchicalScheduleTable) | V1 |
| all | Legacy C/libtrpc code removal | 011f520 |

### Earlier Development

- **feat**: Implement HyperperiodManager for workload hyperperiod calculations
- **feat**: Enhance workload management and task reporting
- **feat**: Support for Apex.OS SchedInfo
- **refactor**: Improve scheduling info buffer management
- **fix**: gRPC detection with pkg-config fallback on Ubuntu 22.04

---

## Version Format

This project uses [Calendar Versioning](https://calver.org/):

```
YYYY.MM.PATCH
```

- **YYYY**: Full year (e.g., 2026)
- **MM**: Month (1-12, without leading zero)
- **PATCH**: Patch number within the month (starting from 0)

Examples:
- `2026.04.0` - First release in April 2026
- `2026.04.1` - First patch in April 2026
- `2026.04.2` - Second patch in April 2026
- `2026.05.0` - First release in May 2026
