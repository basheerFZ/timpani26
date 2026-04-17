<!--
* SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
* SPDX-License-Identifier: MIT
-->

# TIMPANI-N

**Timpani-N** is the node executor component of the TIMPANI real-time scheduling system. It uses eBPF/sched_ext for deterministic task execution with microsecond precision.

## Architecture

```
TIMPANI-O (Orchestrator)
    │
    │ HierarchicalScheduleTable (gRPC :50060)
    ▼
┌─────────────────────────────────────────────┐
│              TIMPANI-N                      │
│  ┌───────────────────────────────────────┐  │
│  │ NodeClient (→:50060)                  │──┤──► timpani-o
│  │ TimerMaster (CLOCK_REALTIME ABSTIME)  │  │
│  │ BPF Loader (sched_ext scheduler)      │  │
│  │ FaultMonitor (ringbuf → gRPC)         │  │
│  └───────────────────────────────────────┘  │
│                    │                        │
│         ┌─────────┴─────────┐               │
│         ▼                   ▼               │
│  ┌─────────────┐    ┌──────────────┐        │
│  │ BPF Maps    │    │ /dev/shm/    │        │
│  │ tt_table    │    │ timpani_ttsched│       │
│  │ task_meta   │    │ (futex)      │        │
│  └─────────────┘    └──────────────┘        │
└─────────────────────────────────────────────┘
    │
    │ Futex wakeup / sched_ext dispatch
    ▼
Time-Triggered Workloads (sample-apps, etc.)
```

| Component | Description |
|-----------|-------------|
| NodeClient | gRPC client connecting to timpani-o |
| TimerMaster | High-precision timer using CLOCK_REALTIME |
| BPF Loader | Loads sched_ext BPF scheduler |
| FaultMonitor | Monitors deadline misses via BPF ringbuf |

---

## Quick Start

### Package Installation (Recommended)

```bash
# Debian/Ubuntu
sudo apt install ./timpani-n_2026.04.1_x86_64.deb

# RedHat/CentOS
sudo dnf install ./timpani-n-2026.04.1-1.x86_64.rpm

# Configure and start
sudo vi /etc/timpani/timpani-n.conf
sudo systemctl start timpani-n
```

### Native Build

```bash
cd TIMPANI/timpani-n

# Prerequisites (Ubuntu 22.04+)
sudo apt install -y libelf-dev zlib1g-dev clang linux-tools-$(uname -r) \
    libgrpc++-dev libprotobuf-dev protobuf-compiler-grpc

# Build
mkdir build && cd build
cmake .. && make

# Verify version (includes Git commit and build time)
./timpani-n -V
# timpani-n version 2026.4.1
#   Git commit: abc1234
#   Build time: 2026-04-17 06:00:00 UTC

# Run (requires root for BPF)
sudo ./timpani-n 192.168.1.100  # timpani-o host
```

---

## Build

### Prerequisites

**Ubuntu 22.04+:**
```bash
# BPF dependencies
sudo apt install -y libelf-dev zlib1g-dev clang linux-tools-$(uname -r)

# gRPC dependencies
sudo apt install -y libgrpc++-dev libprotobuf-dev protobuf-compiler-grpc
```

**CentOS Stream 10:**
```bash
# Development tools
sudo dnf group install -y "Development Tools"
sudo dnf install -y cmake

# BPF dependencies
sudo dnf install -y clang bpftool elfutils-libelf-devel zlib-devel

# gRPC dependencies
sudo dnf install -y grpc-devel protobuf-devel
```

### Native Build

```bash
git clone --recurse-submodules https://github.com/MCO-PICCOLO/TIMPANI.git
cd TIMPANI/timpani-n
mkdir build && cd build

# Local build — Git hash is auto-detected from the repo
cmake .. && make

# CI / package build — no git history available, pass explicitly
VERSION=$(cat ../../VERSION)
GIT_HASH=$(git rev-parse --short HEAD)
cmake -DGIT_COMMIT_HASH=$GIT_HASH .. && make

# Verify
./timpani-n -V
# timpani-n version 2026.4.1
#   Git commit: abc1234
#   Build time: 2026-04-17 06:00:00 UTC
```

### Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `CONFIG_TRACE_BPF` | ON | Enable eBPF sched_ext scheduler |
| `CONFIG_TRACE_EVENT` | ON | Enable ftrace for sched/timer events |
| `CONFIG_TRACE_BPF_EVENT` | OFF | Track sched_switch/waking events |

```bash
# Disable BPF (fallback to timer-only mode)
cmake -DCONFIG_TRACE_BPF=OFF ..
```

### Cross-compilation (ARM64)

```bash
mkdir build-arm64 && cd build-arm64
cmake -DCMAKE_TOOLCHAIN_FILE=../toolchain-aarch64-gcc.cmake ..
make
```

---

## Packaging

Build DEB, RPM, or TGZ packages:

```bash
cd build

# Debian/Ubuntu
cpack -G DEB
# Output: timpani-n_2026.04.1_x86_64.deb

# RedHat/CentOS
cpack -G RPM
# Output: timpani-n-2026.04.1-1.x86_64.rpm

# Tarball
cpack -G TGZ
```

The version in the package filename and the version embedded in the binary both come from the root `VERSION` file at build time. Verify after installation:

```bash
timpani-n -V
# timpani-n version 2026.4.1
#   Git commit: abc1234
#   Build time: 2026-04-17 06:00:00 UTC
```

The package includes:
- `/usr/bin/timpani-n` - Binary
- `/lib/systemd/system/timpani-n.service` - Systemd unit
- `/etc/timpani/timpani-n.conf` - Configuration

---

## Installation

### Package Installation (Recommended)

**Debian/Ubuntu:**
```bash
sudo apt install ./timpani-n_2026.04.1_x86_64.deb
```

**RedHat/CentOS:**
```bash
sudo dnf install ./timpani-n-2026.04.1-1.x86_64.rpm
```

The package automatically:
- Installs binary and systemd service
- Creates `/etc/timpani/` configuration directory
- Runs `systemctl daemon-reload`
- Enables the service (doesn't start automatically)

### Manual Installation

```bash
# Binary
sudo cp build/timpani-n /usr/bin/

# Configuration
sudo mkdir -p /etc/timpani
sudo cp timpani-n.conf /etc/timpani/

# Systemd service
sudo cp timpani-n.service /lib/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable timpani-n
```

---

## Configuration

Edit `/etc/timpani/timpani-n.conf`:

```bash
# Orchestrator (timpani-o) connection
ORCHESTRATOR_HOST=192.168.1.100
ORCHESTRATOR_PORT=50060

# Node identity (empty = use hostname)
NODE_ID=node1
```

---

## Systemd Service

### Service Management

```bash
# Start/stop/restart
sudo systemctl start timpani-n
sudo systemctl stop timpani-n
sudo systemctl restart timpani-n

# Status and logs
sudo systemctl status timpani-n
journalctl -u timpani-n -f
```

### Required Capabilities

timpani-n requires elevated privileges for BPF and real-time scheduling:

| Capability | Purpose |
|------------|---------|
| `CAP_BPF` | Load BPF programs |
| `CAP_SYS_ADMIN` | sched_ext, BPF map operations |
| `CAP_SYS_NICE` | Real-time priority (SCHED_FIFO) |
| `CAP_SYS_RESOURCE` | setrlimit for memlock |

These are configured in the systemd unit file with security hardening:
- `ProtectSystem=strict`
- `PrivateTmp=yes`
- `NoNewPrivileges=yes`

---

## Command Line Options

```
Usage: timpani-n [options] [orchestrator_host]

Options:
  -n <name>   Node ID override (default: hostname)
  -p <port>   Orchestrator gRPC port (default: 50060)
  -V          Print version, Git commit, and build timestamp
  -h          Show help
```

**Example:**
```bash
# Connect to timpani-o at 192.168.1.100:50060 as "node1"
sudo ./timpani-n -n node1 -p 50060 192.168.1.100
```
