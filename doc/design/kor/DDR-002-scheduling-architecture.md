<!--
SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
SPDX-License-Identifier: MIT
-->

# DDR-002: Scheduling Architecture (HSF + sched_ext)

**날짜:** 2026-04-24
**상태:** Accepted
**작성:** Human (Lead Architect) + AI

---

## 1. 결정

Linux RT 스케줄링 정책(SCHED_FIFO/RR/DEADLINE)을 직접 사용하지 않는다.
대신 **HSF (Hierarchical Scheduling Framework)**를 **sched_ext BPF 스케줄러**로 구현하고,
**cgroup v2 cpuset isolated partition**으로 하드웨어 격리를 제공한다.
TT 슬롯의 정밀 발화는 **Timer Master 패턴**으로 처리한다.

---

## 2. 컨텍스트

### 기존 접근의 문제

| 문제 | 원인 |
|:--|:--|
| Thread 단위 policy 설정 복잡도 | 각 thread를 개별적으로 SCHED_FIFO 설정 |
| Jitter | User-level SCHED_DEADLINE/CBS 구현의 불가피한 지연 |
| 앱 코드 수정 필요 | 앱이 thread priority를 직접 설정해야 함 |
| Mixed-criticality 격리 불가 | 단순 priority로는 FFI 요건 불충분 |

### sched_ext 선택 이유

- 커널 스케줄러 자체를 BPF로 교체 → **앱 코드 무수정**
- `cgroup → sched_ext domain` 자동 매핑 → thread별 설정 불필요
- `ops.dispatch()`에서 발화 타이밍 완전 제어
- CBS, TT, 예산 강제 모두 BPF 내에서 구현

---

## 3. 설계 원칙

1. **Pullpiri 주도의 역할 분리**: Timpani는 L1/L2 워크로드의 실시간성만 보장. 나머지 제어권은 Pullpiri 소관.
2. **오프라인/온라인 분리**: 정적 스케줄 테이블 생성(오프라인)과 예산 강제 집행(온라인)을 분리하여 런타임 복잡도 최소화.
3. **단일 경량 Execution Engine**: 커널 엔진(`scx_timpani`)은 휴리스틱 연산 없이 사전 생성된 테이블 명령만 집행. ISO 26262 FFI를 위한 설계.

---

## 4. HSF 3-Level 트리

```
Level 0 (Root)
└── 정적 스케줄 테이블 — Timpani-O가 오프라인으로 생성

    Level 1 (Intermediate)
    └── Timpani-N — 테이블 수신, 워크로드에 예산 강제

        Level 2 (Leaf)
        └── 워크로드 — 위임받은 예산 내에서 자체 스케줄러 운영
```

| Level | 역할 | HSF 근거 |
|:--|:--|:--|
| Level 0 | 정적 스케줄 테이블 (오프라인) | 글로벌 자원 분배 사전 확정 |
| Level 1 | Timpani-N (온라인 집행) | FFI: 예산 초과 시 선점 |
| Level 2 | 워크로드 (블랙박스) | Compositionality: 내부 구조 몰라도 통합 |

---

## 5. CPU 격리

```
┌──────────────────────────┬──────────────────────────┐
│     Isolated CPU         │    Non-Isolated CPU      │
│    (Timpani-N 관할)      │    (Linux CFS 관할)      │
│                          │                          │
│  L1: Periodic + Safety   │  L3: Best-Effort         │
│  L2: Sporadic + Safety   │  L4: Background          │
│                          │                          │
│  cgroup v2 cpuset        │  시스템 태스크 포함       │
│  isolated partition,     │                          │
│  nohz_full, rcu_nocbs    │                          │
└──────────────────────────┴──────────────────────────┘
```

L1/L2와 L3/L4의 자원 경합(Contention)을 HW 수준에서 원천 차단.

### cgroup 매핑

| L1~L4 | cgroup 구현 | 격리 수준 |
|:--|:--|:--|
| L1 | cpuset isolated, 전용 CPU | 전용 코어 독점 |
| L2 | cpuset isolated, CPU pool 공유 | sched_ext가 예산 강제 |
| L3 | Non-Isolated CPU, cgroup weight | CFS 위임 |
| L4 | Non-Isolated CPU, 최하위 큐 | CFS, 수십 ms 지연 허용 |

---

## 6. TIMPANI-N 런타임 아키텍처

```
TIMPANI-N Daemon (C++, userspace)
│
├── [Timer Master Thread]
│     SCHED_FIFO, dedicated CPU pin
│     clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, epoch_ns + slot_offset)
│     → current_slot_map 업데이트 → scx_bpf_kick_cpu()
│
├── [BPF Loader]
│     TIMPANI-O로부터 HierarchicalScheduleTable 수신
│     BPF maps 업데이트:
│       tt_table_map     — TT 슬롯 테이블
│       cbs_map          — CBS 예산/상태 (Cs, Ts)
│       partition_map    — cgroup_id → L1~L4
│       current_slot_map — 현재 활성 TT 슬롯
│
└── [Fault Monitor]
      fault_ringbuf polling → TIMPANI-O에 FaultNotification 전송

sched_ext BPF Scheduler (kernel)
│
├── ops.select_cpu()    L1/L2 → Isolated CPU, L3/L4 → Non-Isolated CPU
├── ops.enqueue()       L1: TT 슬롯 확인, L2: CBS 예산 확인, L3/L4: BE queue
├── ops.dispatch()      TT task → 즉시 dispatch, CBS → 예산 기반 dispatch
├── ops.running()       CBS: 실행 시작 기록
└── ops.stopping()      CBS: budget 차감, deadline 체크 → miss 시 fault_ringbuf 기록
```

---

## 7. Sporadic + Safety 듀얼모드 (CBS + TT Floor)

`Sporadic + SafetyCritical` 워크로드는 **L2**로 분류된다 ([DDR-001](DDR-001-workload-model.md) 참조). CBS 단독으로는 ISO 26262 FFI 충족이 어려우므로 **듀얼모드** 정책을 적용한다:

1. **CBS 모드 (기본)**: 이벤트 도착 시 CBS 예산 내에서 실행.
2. **TT Floor 모드 (보조)**: `min_inter_arrival_us` 기반으로 정적 테이블에 최소 보장 슬롯 예약.
   - Floor slot 크기 = WCET, 주기 = `min_inter_arrival_us`
   - 이벤트가 floor slot 이전에 도착 → CBS로 즉시 실행
   - 이벤트 미도착 → floor slot에서 워치독 검사

**효과**: 이벤트 미도착(센서 탈락) 시에도 주기적 감시 보장. CBS 유연성 + TT 결정론 결합.

---

## 8. 핵심 결정 요약

| 항목 | 결정 | 이유 |
|:--|:--|:--|
| TT 정밀도 | Timer Master (SCHED_FIFO + ABSTIME) | BPF timer 단독은 jitter 과다 |
| CBS 구현 | BPF `ops.running/stopping` + `bpf_timer` 보충 | user-level CBS 복잡도 회피 |
| CBS 대상 | **L2 Sporadic 워크로드** | L1은 TT, L3/L4는 CFS 위임 |
| CPU 격리 | cgroup v2 `cpuset.cpus.partition = "isolated"` | HW 수준 격리, ISO 26262 FFI |
| 커널 요건 | PREEMPT_RT | 수십 μs 정밀도 |

---

## 9. 배포 요건

- Linux 6.12+ (sched_ext merge)
- PREEMPT_RT 패치
- cgroup v2 마운트
- Yocto: `meta-realtime` 레이어

---

## 10. 영향받는 컴포넌트

- `timpani-n/src/` — 런타임 루프 (Timer Master + BPF Loader + Fault Monitor)
- `timpani-n/src/bpf/` — sched_ext BPF 스케줄러
- `timpani-o/src/` — HierarchicalScheduleTable 생성
