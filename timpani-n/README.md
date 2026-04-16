<!--
* SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
* SPDX-License-Identifier: MIT
-->

# Timpani-N

## Getting started

## Prerequisites

For CentOS, refer to [README.CentOS.md](README.CentOS.md).

libelf-dev and zlib1g-dev required for libbpf submodule

> NOTE: libbpf has been integrated as a git submodule since
```
sudo apt install -y libelf-dev zlib1g-dev
```

clang and linux-tools(bpftool) required for bpf feature

> NOTE: For Ubuntu 20, skip these commands and follow [prerequisites for Ubuntu 20.04](README.Ubuntu20.md) instead.

```
sudo apt install -y clang
sudo apt install -y linux-tools-$(uname -r)
```

pkg-config and libsystemd-dev required for libtrpc submodule
```
sudo apt install -y pkg-config
sudo apt install -y libsystemd-dev
```

libyaml required for dummy_server program

```
sudo apt install -y libyaml-dev
```

## Build

```
git clone https://github.com/MCO-PICCOLO/TIMPANI.git
cd TIMPANI
git submodule add https://github.com/libbpf/libbpf.git libbpf
git submodule update --init --recursive
cd timpani-n
mkdir build
cd build
cmake ..
make
```
### Build options

- CONFIG_TRACE_EVENT (ON by default)

  - Gets ftrace dump for sched, timer, signal, and trace_marker events

- CONFIG_TRACE_BPF (ON by default)

  - Activates a bpf program to trace sigwait system call entry/exit of time-triggered tasks
  - Makes it possible to detect deadline misses

- CONFIG_TRACE_BPF_EVENT (OFF by default)

  - Loads a bpf program to keep track of sched_switch and sched_waking events of time-triggered tasks
  - Calculates on-cpu time and scheduling latency

### Cross-compilation for ARM64

```
cd build-arm64
cmake -DCMAKE_TOOLCHAIN_FILE=../toolchain-aarch64-gcc.cmake ..
make
```

### Packaging

```
cd build
cpack -G DEB
or
cpack -G RPM
or
cpack -G TGZ
```

## How to use

execute sample wakee1 process in terminal 1
```
cd build
sudo ./exprocs wakee1 10000
```

execute sample wakee2 process in terminal 2
```
cd build
sudo ./exprocs wakee2 50000
```

execute sample wakee3 process in terminal 3
```
cd build
sudo ./exprocs wakee3 20000
```

execute dummy server, and modify schedinfo.yaml before running if task info is different
```
cd build
./dummy_server
```

execute time trigger in other terminal
```
cd build
sudo ./timpani-n
```

## Systemd Service (Production Deployment)

For production deployment, timpani-n should run as a systemd service.

### Installation

```bash
# Install binary
sudo cp build/timpani-n /usr/local/bin/

# Install configuration
sudo mkdir -p /etc/timpani
sudo cp timpani-n.conf /etc/timpani/

# Install and enable service
sudo cp timpani-n.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable timpani-n
```

### Configuration

Edit `/etc/timpani/timpani-n.conf`:

```bash
# Orchestrator connection
ORCHESTRATOR_HOST=192.168.1.100   # timpani-o host
ORCHESTRATOR_PORT=50060

# Node identity (empty = use hostname)
NODE_ID=node1
```

### Service Management

```bash
# Start/stop/restart
sudo systemctl start timpani-n
sudo systemctl stop timpani-n
sudo systemctl restart timpani-n

# Check status and logs
sudo systemctl status timpani-n
journalctl -u timpani-n -f
```

### Required Capabilities

timpani-n requires the following Linux capabilities:

| Capability | Purpose |
|:--|:--|
| `CAP_BPF` | Load BPF programs |
| `CAP_SYS_ADMIN` | sched_ext, BPF map operations |
| `CAP_SYS_NICE` | Real-time priority (SCHED_FIFO) |
| `CAP_SYS_RESOURCE` | setrlimit for memlock |

These are configured in the systemd unit file.

***
