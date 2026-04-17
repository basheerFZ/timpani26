# Changelog

All notable changes to the TIMPANI project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Calendar Versioning](https://calver.org/) (YYYY.MM.PATCH).

## [Unreleased]

## [2026.04.1] - 2026-04-17

Patch release with multi-node support enhancements and build improvements.

### timpani-n

#### Added
- CLI options: `-n <node_id>` override, `-p <port>` configuration (79c9a83)
- **Version tracking**: `-V` option shows version, Git commit, build timestamp
- `version.h.in` template for compile-time version embedding

### sample-apps

#### Added
- **Version tracking**: `--version` option shows version, Git commit, build timestamp
- `version.h.in` template for compile-time version embedding

### timpani-o

#### Added
- Schedule replay for reconnected nodes — automatically resend schedule table when timpani-n reconnects (f2fa4ce)

### Build

#### Added
- CMakeLists.txt: VERSION file fallback for container builds
  - Try local VERSION → parent VERSION → default 0.0.0-dev
- CMakeLists.txt: Git commit hash and build timestamp injection via `configure_file()`
- Dockerfile: `--build-arg VERSION` and `--build-arg GIT_COMMIT_HASH` support

#### Changed
- Remove duplicate `timpani-o/VERSION` and `timpani-o/CHANGELOG.md`
- All components now reference root `/VERSION` file

### Documentation

#### Added
- [VERSIONING.md](doc/VERSIONING.md): Version management guide
  - CalVer format explanation
  - Release process
  - Commit message convention
- README.md: Versioned build and container build instructions

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
- `2026.05.0` - First release in May 2026
