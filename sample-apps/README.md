<!--
* SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
* SPDX-License-Identifier: MIT
-->

# Sample Applications

**Sample-apps** provides real-time workload generators for TIMPANI system validation. It supports periodic execution, deadline monitoring, and runtime statistics collection with 8 different workload algorithms.

| | |
|---|---|
| **Version** | See `./sample_apps --version` |
| **License** | MIT |

---

## Quick Start

### Container (Recommended)

```bash
cd sample-apps

# Build with version info
VERSION=$(cat ../VERSION)
GIT_HASH=$(git rev-parse --short HEAD)
podman build --build-arg VERSION=$VERSION --build-arg GIT_COMMIT_HASH=$GIT_HASH \
  -t sample-apps:$VERSION .

# Verify version
podman run --rm sample-apps:$VERSION --version
# sample_apps version 2026.4.1
#   Git commit: abc1234
#   Build time: 2026-04-17 06:00:00 UTC

# Run (requires timpani-n running with /dev/shm/timpani_ttsched)
podman run -d --privileged --pid=host --ipc=host --name task_A \
  sample-apps:$VERSION \
  -p 10 -d 9 -a 3 -l 2000 --bpf -s task_A
```

### Native Build

```bash
cd sample-apps

# Build
mkdir build && cd build
cmake .. && make

# Verify version (includes Git commit and build time)
./sample_apps --version
# sample_apps version 2026.4.1
#   Git commit: abc1234
#   Build time: 2026-04-17 06:00:00 UTC

# Run (from build directory)
sudo ./sample_apps -p 10 -d 9 -a 3 -l 2000 --bpf -s task_A
```

---

## Build

### Prerequisites

**Ubuntu:**
```bash
sudo apt install -y build-essential cmake
```

**CentOS:**
```bash
sudo dnf install -y gcc gcc-c++ cmake make
```

### Native Build

```bash
git clone https://github.com/MCO-PICCOLO/TIMPANI.git
cd TIMPANI/sample-apps
mkdir build && cd build
cmake ..
make
```

### Container Build

```bash
cd sample-apps

# Build with version info
VERSION=$(cat ../VERSION)
GIT_HASH=$(git rev-parse --short HEAD)
podman build --build-arg VERSION=$VERSION --build-arg GIT_COMMIT_HASH=$GIT_HASH \
  -t sample-apps:$VERSION .

# Verify version inside container
podman run --rm sample-apps:$VERSION --version
```

---

## Run

### Native

```bash
# From build directory (requires sudo for SCHED_FIFO)

# BPF mode (with timpani-n)
sudo ./sample_apps -p 10 -d 9 -a 3 -l 2000 --bpf -s task_A

# Timer mode (standalone)
sudo ./sample_apps -p 100 -d 90 -a 1 -l 5 -t mytask

# Help
./sample_apps -h
```

### Container

Container execution requires special permissions for real-time scheduling and IPC.

```bash
# BPF mode (with timpani-n)
podman run -d --privileged --pid=host --ipc=host --name task_A \
  sample-apps:latest \
  -p 10 -d 9 -a 3 -l 2000 --bpf -s task_A

podman run -d --privileged --pid=host --ipc=host --name task_B \
  sample-apps:latest \
  -p 20 -d 18 -a 3 -l 3000 --bpf -s task_B

# Timer mode (standalone)
podman run -d --privileged --name mytask \
  sample-apps:latest \
  -p 100 -d 90 -a 1 -l 5 -t mytask

# Using compose (Phase 1 verification tasks)
podman-compose up -d
```

**Required container options:**

| Option | Purpose |
|--------|---------|
| `--privileged` | SCHED_FIFO + BPF permissions |
| `--pid=host` | Host PID namespace (timpani-n task detection) |
| `--ipc=host` | Host IPC namespace (`/dev/shm/timpani_ttsched` shared memory) |

---

## Options

| Option | Description | Default |
|--------|-------------|---------|
| `-p, --period` | Period in milliseconds | 100 |
| `-d, --deadline` | Deadline in milliseconds | Same as period |
| `-r, --runtime` | Expected runtime in microseconds | 50000 |
| `-P, --priority` | Real-time priority (1-99) | 50 |
| `-a, --algorithm` | Algorithm selection (1-8) | 1 |
| `-l, --loops` | Loop count/parameter | 10 |
| `-s, --stats` | Task name for statistics | - |
| `-t, --timer` | Timer-based periodic execution | Signal-based |
| `-b, --bpf` | BPF/TT-Sched futex-based wakeup | Signal-based |
| `-h, --help` | Display help | - |

---

## Workload Algorithms

| # | Name | Description |
|---|------|-------------|
| 1 | NSQRT | Newton-Raphson square root (CPU-intensive) |
| 2 | Fibonacci | Fibonacci sequence (sequential integer ops) |
| 3 | Busy loop | Pure CPU time consumption |
| 4 | Matrix | Matrix multiplication (FPU-intensive) |
| 5 | Memory | Random memory access (cache stress) |
| 6 | Crypto | Cryptographic hash simulation |
| 7 | Mixed | Combined workload types |
| 8 | Prime | Prime number calculation |

---

## Execution Modes

| Mode | Option | Description |
|------|--------|-------------|
| **BPF** | `--bpf` | TimerMaster futex wakeup via `/dev/shm/timpani_ttsched`. Requires timpani-n running. |
| **Timer** | `-t` | Internal timer-based periodic execution. Standalone operation. |
| **Signal** | (default) | Waits for external SIGRTMIN+2 signal. |

---

## Development

### Coding Style

- C99 standard
- 4 spaces indentation

---

## Registry (Optional)

```bash
podman login sdv.lge.com
podman tag sample-apps:latest sdv.lge.com/timpani/sample-apps:v0.1.0
podman push sdv.lge.com/timpani/sample-apps:v0.1.0
```
