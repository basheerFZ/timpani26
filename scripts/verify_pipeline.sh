#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
# SPDX-License-Identifier: MIT
#
# verify_pipeline.sh — 1차 E2E 파이프라인 검증 스크립트
#
# 사용법 (TIMPANI root에서):
#   bash scripts/verify_pipeline.sh [task_comm...]
#
# 기본 확인 태스크: task_A task_B

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
VENV_PYTHON="$ROOT_DIR/.venv/bin/python"
MOCK_SCRIPT="$SCRIPT_DIR/mock_pullpiri.py"

TASKS=("${@:-task_A task_B}")
if [[ $# -eq 0 ]]; then
    TASKS=(task_A task_B)
fi

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
ok()   { echo -e "${GREEN}[✓]${NC} $*"; }
warn() { echo -e "${YELLOW}[!]${NC} $*"; }
fail() { echo -e "${RED}[✗]${NC} $*"; }

# ── 1. 사전 조건 확인 ────────────────────────────────────────────────────────

echo "=== [1] Pre-flight checks ==="

if [[ ! -x "$VENV_PYTHON" ]]; then
    fail ".venv not found. Run: python3 -m venv .venv && .venv/bin/pip install grpcio grpcio-tools"
    exit 1
fi
ok "Python venv: $VENV_PYTHON"

# timpani-o 실행 중인지 확인 (port 50052)
if ss -tlnp 2>/dev/null | grep -q ':50052'; then
    ok "timpani-o SchedInfoService listening on :50052"
else
    warn "timpani-o NOT detected on :50052 — please start it first"
    echo "  cd timpani-o/build && sudo ./timpani-o --node-config ../examples/node_configurations.yaml"
    exit 1
fi

# timpani-o OrchestratorService (port 50060)
if ss -tlnp 2>/dev/null | grep -q ':50060'; then
    ok "timpani-o OrchestratorService listening on :50060"
else
    warn "OrchestratorService NOT detected on :50060"
fi

# timpani-n 실행 중인지 확인 (process name)
if pgrep -x "timpani-n" > /dev/null 2>&1; then
    ok "timpani-n is running (pid=$(pgrep -x timpani-n))"
else
    warn "timpani-n NOT running — please start it first"
    echo "  cd timpani-n/build && sudo ./timpani-n"
fi

# ── 2. mock_pullpiri.py 호출 ─────────────────────────────────────────────────

echo ""
echo "=== [2] Sending AddSchedInfo (mock Pullpiri) ==="
"$VENV_PYTHON" "$MOCK_SCRIPT" "$@" || { fail "mock_pullpiri.py failed"; exit 1; }
ok "AddSchedInfo sent"

# ── 3. 잠시 대기 (timpani-n이 콜백 처리할 시간) ────────────────────────────

echo ""
echo "=== [3] Waiting 2s for timpani-n to apply schedule table ==="
sleep 2

# ── 4. 태스크별 스케줄링 정책 + CPU affinity 확인 ──────────────────────────

echo ""
echo "=== [4] Verifying task scheduling policy & CPU affinity ==="

all_ok=true
for comm in "${TASKS[@]}"; do
    pid=$(pgrep -n -x "$comm" 2>/dev/null || true)
    if [[ -z "$pid" ]]; then
        warn "Task '$comm' not found in /proc — is sample_apps running?"
        all_ok=false
        continue
    fi

    echo ""
    echo "  --- Task: $comm (pid=$pid) ---"

    # 스케줄링 정책 (chrt)
    if command -v chrt &>/dev/null; then
        sched_info=$(chrt -p "$pid" 2>/dev/null || echo "chrt failed")
        echo "    $sched_info"
        if echo "$sched_info" | grep -q "SCHED_FIFO"; then
            ok "  SCHED_FIFO applied to $comm"
        else
            fail "  SCHED_FIFO NOT applied to $comm"
            all_ok=false
        fi
    else
        warn "  chrt not available — checking /proc/$pid/sched"
        grep "^policy" /proc/"$pid"/sched 2>/dev/null || true
    fi

    # CPU affinity
    if command -v taskset &>/dev/null; then
        affinity=$(taskset -p "$pid" 2>/dev/null || echo "taskset failed")
        echo "    $affinity"
    fi

    # /proc/<pid>/status에서 Cpus_allowed
    cpus_allowed=$(grep "^Cpus_allowed:" /proc/"$pid"/status 2>/dev/null | awk '{print $2}')
    echo "    Cpus_allowed: $cpus_allowed"
done

# ── 5. 결과 요약 ─────────────────────────────────────────────────────────────

echo ""
echo "=== [5] Summary ==="
if $all_ok; then
    ok "All tasks verified — 1차 검증 PASS"
else
    fail "Some tasks failed verification — check timpani-n logs"
    exit 1
fi
