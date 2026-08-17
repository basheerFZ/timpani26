# SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
# SPDX-License-Identifier: MIT
"""
Pullpiri mock: AddSchedInfo gRPC call to timpani-o SchedInfoService.

Usage (from TIMPANI root):
    .venv/bin/python scripts/mock_pullpiri.py [--host HOST] [--port PORT]

Default: localhost:50052
"""

import sys
import os
import argparse
import socket

# Generated stubs live in scripts/
sys.path.insert(0, os.path.dirname(__file__))
import schedinfo_pb2
import schedinfo_pb2_grpc
import grpc


def build_request() -> schedinfo_pb2.SchedInfo:
    node_id = socket.gethostname()
    req = schedinfo_pb2.SchedInfo()
    req.workload_id = "verify_wl"

    # task_A: 10ms period, 2ms runtime, FIFO prio=20, CPU 0, node=hostname
    t_a = req.tasks.add()
    t_a.name         = "task_A"
    t_a.priority     = 20
    t_a.policy       = schedinfo_pb2.FIFO   # 1
    t_a.cpu_affinity = 0x1                  # CPU 0
    t_a.period       = 10000               # us
    t_a.runtime      = 2000                # us
    t_a.deadline     = 10000              # us
    t_a.release_time = 0
    t_a.max_dmiss    = 0
    t_a.node_id      = node_id

    # task_B: 20ms period, 3ms runtime, FIFO prio=19, CPU 0, node=hostname
    t_b = req.tasks.add()
    t_b.name         = "task_B"
    t_b.priority     = 19
    t_b.policy       = schedinfo_pb2.FIFO
    t_b.cpu_affinity = 0x1
    t_b.period       = 20000
    t_b.runtime      = 3000
    t_b.deadline     = 20000
    t_b.release_time = 0
    t_b.max_dmiss    = 0
    t_b.node_id      = node_id

    print(f"[mock_pullpiri] node_id: '{node_id}'")
    return req


def main():
    parser = argparse.ArgumentParser(description="Pullpiri mock — AddSchedInfo")
    parser.add_argument("--host", default="localhost", help="timpani-o host")
    parser.add_argument("--port", type=int, default=50052, help="SchedInfoService port")
    args = parser.parse_args()

    addr = f"{args.host}:{args.port}"
    print(f"[mock_pullpiri] Connecting to {addr} ...")

    channel = grpc.insecure_channel(addr)
    stub = schedinfo_pb2_grpc.SchedInfoServiceStub(channel)

    req = build_request()
    print(f"[mock_pullpiri] Sending AddSchedInfo: workload_id='{req.workload_id}' "
          f"tasks={[t.name for t in req.tasks]}")

    try:
        resp = stub.AddSchedInfo(req, timeout=5)
        if resp.status == 0:
            print(f"[mock_pullpiri] ✓ Success (status=0)")
        else:
            print(f"[mock_pullpiri] ✗ Failed (status={resp.status})", file=sys.stderr)
            sys.exit(1)
    except grpc.RpcError as e:
        print(f"[mock_pullpiri] ✗ gRPC error: {e.code()} — {e.details()}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
