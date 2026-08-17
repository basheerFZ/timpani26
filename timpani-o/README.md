<!--
* SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
* SPDX-License-Identifier: MIT
-->

# TIMPANI-O

**Timpani-O** is the orchestrator component of the TIMPANI real-time scheduling system. It generates and distributes schedule tables to Timpani-N instances via gRPC.

| | |
|---|---|
| **Version** | 2026.04.1 ([CalVer](https://calver.org/)) |
| **Changelog** | [CHANGELOG.md](CHANGELOG.md) |
| **License** | MIT |

## Architecture

```
Pullpiri (Workload Orchestrator)
    │
    │ WorkloadSpec (gRPC :50052)
    ▼
┌─────────────────────────────────────┐
│           TIMPANI-O                 │
│  ┌─────────────────────────────┐    │
│  │ SchedInfoServer (:50052)    │────┤◄─ Pullpiri
│  │ OrchestratorServer (:50060) │────┤◄─► Timpani-N (bidirectional)
│  │ FaultClient (→:50053)       │────┤──► Pullpiri FaultService
│  └─────────────────────────────┘    │
└─────────────────────────────────────┘
    │
    │ HierarchicalScheduleTable (gRPC :50060)
    ▼
TIMPANI-N (Node Executor)
```

| Port | Service | Direction |
|------|---------|-----------|
| 50052 | SchedInfoServer | Pullpiri → timpani-o |
| 50060 | OrchestratorServer | timpani-n ↔ timpani-o |
| 50053 | FaultService (client) | timpani-o → Pullpiri |

---

## Quick Start

### Container (Recommended)

```bash
cd timpani-o

# Build with version info
VERSION=$(cat ../VERSION)
podman build --build-arg VERSION=$VERSION \
  -t timpani-o:$VERSION .

# Verify version label
podman inspect timpani-o:$VERSION --format '{{index .Config.Labels "version"}}'
# 2026.04.1

# Run (mount examples/node_configurations.yaml)
podman run -d --name timpani-o \
  -p 50052:50052 -p 50060:50060 \
  -v $(pwd)/examples/node_configurations.yaml:/config/node_configurations.yaml:ro \
  timpani-o:$VERSION \
  -c /config/node_configurations.yaml
```

### Native Build

```bash
cd timpani-o

# Prerequisites (Ubuntu)
sudo apt install -y libgrpc++-dev libprotobuf-dev protobuf-compiler-grpc

# Build
mkdir build && cd build
cmake .. && make

# Run (from build directory)
./timpani-o -c ../examples/node_configurations.yaml
```

---

## Build

### Prerequisites

**Ubuntu:**
```bash
sudo apt install -y libgrpc++-dev libprotobuf-dev protobuf-compiler-grpc
```

**CentOS:**
```bash
sudo dnf install -y protobuf-devel protobuf-compiler epel-release
sudo dnf install -y grpc-devel
```

### Native Build

```bash
git clone https://github.com/eclipse-timpani/timpani26
cd TIMPANI/timpani-o
mkdir build && cd build
cmake ..
make
```

**Cross-compilation (ARM64):**
```bash
# From build directory
cmake -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain-aarch64-gcc.cmake ..
make
```

**Packaging:**
```bash
# From build directory
cpack -G DEB   # or RPM, TGZ
```

### Container Build

timpani-o embeds version as a container image **label** (not in the binary).
`GIT_COMMIT_HASH` is not supported — pass `VERSION` only.

```bash
cd timpani-o

# Build with version info
VERSION=$(cat ../VERSION)
podman build --build-arg VERSION=$VERSION \
  -t timpani-o:$VERSION .

# Verify version label
podman inspect timpani-o:$VERSION --format '{{index .Config.Labels "version"}}'
```

---

## Run

### Native

```bash
# From build directory (default config)
./timpani-o -c ../examples/node_configurations.yaml

# Without config
./timpani-o

# Help
./timpani-o -h
```

### Container

```bash
# Default (mount host config)
podman run -d --name timpani-o \
  -p 50052:50052 -p 50060:50060 \
  -v $(pwd)/examples/node_configurations.yaml:/config/node_configurations.yaml:ro \
  timpani-o:latest \
  -c /config/node_configurations.yaml

# Without config
podman run -d --name timpani-o \
  -p 50052:50052 -p 50060:50060 \
  timpani-o:latest

# Connect to Pullpiri on host
podman run -d --name timpani-o \
  --network=host \
  -v $(pwd)/examples/node_configurations.yaml:/config/node_configurations.yaml:ro \
  timpani-o:latest \
  -c /config/node_configurations.yaml -f localhost -p 50053

# Using compose
podman-compose up -d
```

---

## Testing

```bash
# From timpani-o directory
mkdir build && cd build
cmake -DBUILD_TESTS=ON ..
make

# Run all tests
make test

# Run individual tests
./tests/test_schedinfo_service
./tests/test_fault_client
./tests/test_global_scheduler
./tests/test_node_config
```

**Test prerequisites:**
```bash
# Ubuntu
sudo apt install -y libgtest-dev

# CentOS
sudo dnf install -y gtest-devel
```

---

## Development

### Coding Style

- [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html) with modifications:
  - 4 spaces indentation
  - Line break before opening brace (functions/classes)
- Format with `clang-format -i <file>`

---

## Registry (Optional)

For deployment to internal registry:

```bash
podman login sdv.lge.com
podman tag timpani-o:latest sdv.lge.com/timpani/timpani-o:v0.1.0
podman push sdv.lge.com/timpani/timpani-o:v0.1.0
```

