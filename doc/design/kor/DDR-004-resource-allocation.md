<!--
SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
SPDX-License-Identifier: MIT
-->

# DDR-004: TIMPANI-O Resource Allocation Algorithm

**날짜:** 2026-04-24 (Last Updated: 2026-06-15)
**상태:** Accepted
**작성:** Human (Lead Architect) + AI

---

## 1. 결정 요약

| 항목 | 결정 |
|:--|:--|
| 스케줄링 방식 | Partitioned Scheduling (task migration 금지) |
| 노드 선택 | **Pullpiri 책임** — timpani는 `target_node`를 수신 (§6 참조) |
| CPU 할당 | timpani-o가 지정된 노드 내에서 CPU 할당 |
| U_bound | **TBD** — L1~L4 계층별 차등, 값 미정 |
| Hyperperiod 제어 | Period 제약 조건 정의 필요 (§8.A 참조) |
| 실패 정책 | **TBD** — 논의 필요 |

---

## 2. 컨텍스트

TIMPANI-O는 `WorkloadSpec[]`과 `NodeTopology`를 받아 노드별 `HierarchicalScheduleTable`을 생성한다.
Partitioned Scheduling 선택 이유: task migration은 cache miss + jitter를 유발, 차량 RT 환경에 부적합.

---

## 3. 알고리즘 흐름

```
입력: WorkloadSpec[] (target_node 포함) + NodeTopology
  ↓
Phase 1: L1~L4 분류 (DDR-001 2축 매핑)
  ↓
Phase 2: 사전 Feasibility 체크 (task 단위 utilization)
  ↓
Phase 3: 노드 검증 (target_node 존재 확인)
  ↓
Phase 4: CPU 배정 (지정된 노드 내에서)
  ↓
Phase 5: 스케줄 테이블 생성
  5-A: Period 제약 검증 (§8.A)
  5-B: TT 슬롯 배열 (L1) (§8.B)
  5-C: CBS 예산 (L2) (§8.C)
  ↓
Phase 6: Epoch 계산 (PTP 기반 epoch_ns)
  ↓
출력: HierarchicalScheduleTable × M (노드별)
```

---

## 4. Phase 1: L1~L4 분류

[DDR-001](DDR-001-workload-model.md)의 2축 모델 사용:

| TemporalClass | Criticality | L1~L4 | 스케줄링 타입 | CPU |
|:--|:--|:--|:--|:--|
| Periodic | SafetyCritical | L1 | TT_SLOT | Isolated (전용) |
| Sporadic | SafetyCritical | L2 | CBS | Isolated (pool 공유) |
| Any | NonSafety | L3/L4 | BEST_EFFORT | Non-Isolated |

**검증**:
- L1/L2: `task_specs` 필수. 비어있으면 `ValidationError`.
- L3/L4: `task_specs` 무시 (cgroup quota만).

---

## 5. Phase 2: 사전 Feasibility 체크

```
L1~L4 계층별:
  required_util  = Σ (task_j.wcet_us / task_j.period_us)
  available_util = total_cpus_in_layer × U_bound(layer)

  required_util > available_util → 즉시 거부
```

### U_bound (TBD)

> U_bound 값은 아직 확정되지 않았다. 아래는 논의를 위한 초기 후보값이다.

| Layer | U_bound (후보) | Headroom 근거 |
|:--|:--|:--|
| L1 | 0.80 | OS 인터럽트 + PTP + 안전 여유 |
| L2 | 0.85 | OS overhead + CBS 오버헤드 |
| L3/L4 | 0.90 | cgroup quota 오차 |

---

## 6. Phase 3: 노드 검증 (역할 명확화)

### timpani가 하지 않는 것: 노드 선택

> **아키텍처 결정**: 워크로드가 어느 노드에서 실행될지 결정하는 것은 **timpani의 책임이 아닙니다**.

SDV/차량 환경에서 워크로드는 빌드 시점 또는 배포 시점에 특정 노드에서 실행되도록 설계됨:
- 브레이크 컨트롤러 → Brake ECU
- 카메라 처리 → Vision ECU
- 크로스 노드 마이그레이션은 RT 워크로드에 무의미 (cache miss, 레이턴시)

| 역할 | 담당 | 설명 |
|:--|:--|:--|
| **노드 선택** | Pullpiri / 빌드 시스템 | 워크로드가 실행될 노드 결정 |
| **노드 정보 전달** | Pullpiri → timpani-o | `WorkloadSpec.target_node` (필수 필드) |
| **노드 내 CPU 할당** | timpani-o | 지정된 노드 내에서 L1/L2 CPU 할당 |

### timpani가 하는 것: 노드 내 CPU 할당

timpani-o는 Pullpiri로부터 노드 정보를 수신하여:
1. gRPC `NodeReady`를 통해 노드 존재 확인
2. L1 워크로드: Isolated CPU에 독점 할당
3. L2 워크로드: Isolated CPU pool 공유
4. L3/L4: timpani 관리 대상 외

```protobuf
message WorkloadSpec {
  string target_node = ...;  // 필수, Pullpiri가 지정
  ...
}
```

> **폐기됨**: `preferred_node`는 제거되었습니다. "soft hint" 개념은 RT 워크로드에 부적합.
> 노드는 Pullpiri가 명시적으로 지정해야 하며, timpani-o는 그대로 사용.

---

## 7. Phase 4: CPU 배정

| Layer | 배정 방식 |
|:--|:--|
| L1 | 워크로드당 전용 CPU (cpuset isolated, 독점) |
| L2 | Isolated CPU pool 공유 (sched_ext 런타임 선택) |
| L3/L4 | Non-Isolated CPU, cgroup quota만 |

---

## 8. Phase 5: 스케줄 테이블 생성

### 8.A: Period 제약과 Harmonic Period

#### 문제: Hyperperiod 폭발

Hyperperiod는 모든 태스크 주기의 최소공배수(LCM)입니다. 제약 없이 임의의 주기를 허용하면 **hyperperiod 폭발**이 발생할 수 있습니다:

| 태스크 주기 | Hyperperiod (LCM) | 스케줄 테이블 크기 |
|:--|:--|:--|
| 10ms, 20ms, 40ms | 40ms | 작음 (4 슬롯) |
| 10ms, 33ms | 330ms | 큼 (33 슬롯) |
| 7ms, 11ms, 13ms | 1001ms | 매우 큼 (143 슬롯) |

**Hyperperiod 폭발의 문제점**:
- 스케줄 테이블 메모리 사용량 증가
- 테이블 생성 시간 증가
- BPF 맵 크기 제한 초과 가능성
- 런타임 예측성 저하

#### 해결책: Harmonic Period 제약

**Harmonic period**란 모든 태스크 주기가 base tick의 **2의 거듭제곱 배수**인 것을 의미합니다.

```
base_tick = 100μs (예시)
허용 주기: 100μs, 200μs, 400μs, 800μs, 1600μs, ...
           = base_tick × 2^k  (k = 0, 1, 2, ...)
```

**Harmonic Period의 수학적 특성**:
```
모든 period_i = base_tick × 2^k_i 이면:
  → LCM(period_1, period_2, ...) = max(period_i)
```

| 예시 | 주기 | Hyperperiod |
|:--|:--|:--|
| **Harmonic** | 1ms, 2ms, 4ms | 4ms (= max) |
| **Harmonic** | 100μs, 400μs, 1600μs | 1600μs (= max) |
| **Non-Harmonic** | 3ms, 5ms | 15ms (= 3 × 5) |

#### 제약 적용 정책 (TBD)

> 적용 정책과 base_tick 값은 아직 확정되지 않았습니다.

| 정책 옵션 | 설명 | 트레이드오프 |
|:--|:--|:--|
| **Strict** | Non-harmonic 주기 거부 | 안전하지만 제한적 |
| **Warn** | 경고만, 계속 진행 | 유연하지만 폭발 가능 |
| **Limit** | Hyperperiod 임계값 초과 시 거부 | 타협점 (예: 10s 제한) |

**현재 구현 상태**:
- `HyperperiodManager`: LCM 계산, 1시간 초과 시 경고
- Harmonic 검증 로직: **미구현** (WBS O-3.6)

### 8.B: TT 슬롯 배열 (L1 Periodic Tasks)

> **현재 상태**: 알고리즘 개요 정의됨. **상세 구현은 Open** (WBS O-3.5)

#### TT vs ET 스케줄링

| 측면 | Event-Triggered (ET) | **Time-Triggered (TT)** |
|:--|:--|:--|
| **결정 시점** | 런타임 (이벤트 도착 시) | **오프라인** (사전 계산) |
| **알고리즘** | RM, DM, EDF (우선순위 기반) | **슬롯 오프셋 계산** (정적 배치) |
| **지터 원인** | 선점, 간섭, 도착 분산 | **거의 0** (고정 오프셋) |
| **대상** | L2~L4 | **L1 (Strict Deterministic)** |

**핵심 차이**:
- **ET**: 런타임 결정 "누가 먼저?" 우선순위 기반 → 지터 발생 가능
- **TT**: 오프라인 계산 "언제 실행?" 고정 오프셋 → 지터 최소화

> L1 워크로드는 지터 최소화를 위해 **TT 스케줄링** 적용.
> **런타임 스케줄링**으로 사용되는 RM/DM/EDF는 L1에 부적합.
> 다만, RM/DM 원칙은 **슬롯 배치 휴리스틱**으로 적용 가능.

#### 알고리즘 개요

```
① 워크로드별 hyperperiod: W.hp = LCM(task periods)
② 글로벌 hyperperiod (CPU별): global_hp = LCM(W.hp for all workloads on CPU)
③ 태스크 정렬 (배치 순서 — RM/DM 휴리스틱 적용 가능) → 슬롯 생성
④ 슬롯 오프셋 할당 (충돌 방지)
```

#### TT 슬롯 배치 상세 (TBD)

**Step 1: 태스크 정렬 (배치 순서)**

> 이것은 **런타임 스케줄링 우선순위가 아닙니다**.
> 오프라인 계산 시 **어떤 순서로 슬롯을 배치할지** 결정.
>
> **배치 순서가 지터에 영향**:
> - 짧은 주기 태스크 먼저 배치 → 반복이 균등 분산
> - 긴 주기 태스크 먼저 배치 → 짧은 주기 슬롯이 불균등 → 지터 증가

**배치 순서 휴리스틱 (RM/DM 원칙 적용)**:

| 휴리스틱 | 정렬 기준 | 지터 영향 | 설명 |
|:--|:--|:--|:--|
| **RM 원칙** | 짧은 주기 우선 | ✅ 유리 | 고빈도 태스크가 균등 분배 |
| **DM 원칙** | 짧은 deadline 우선 | ✅ 유리 | 엄격한 태스크가 좋은 위치 확보 |
| First-Fit | 입력 순서 | ❌ 불리 | 무작위 배치 |
| BFD | 큰 WCET 우선 | ⚠️ 중립 | Bin packing 최적화 (지터 무관) |

**권장**: 슬롯 배치 휴리스틱으로 **RM 또는 DM 원칙** 적용

**Step 2: 슬롯 오프셋 계산 (핵심)**

```
for each task in sorted_tasks:  // RM/DM 순서로 정렬된 태스크
    repetitions = global_hp / task.period_us
    for i in 0..repetitions:
        // 기본 오프셋: 주기 배수
        base_offset = i * task.period_us
        // 충돌하지 않는 오프셋 찾기
        slot.offset_us = find_non_overlapping_offset(base_offset, task.wcet_us)
        slot.duration_us = task.wcet_us
        slot.deadline_us = base_offset + task.deadline_us
        slots.push(slot)
```

**Step 3: 충돌 방지 (TBD)**

> 동일 CPU에서 여러 슬롯이 겹칠 경우, 처리 전략 정의 필요.

| 전략 | 설명 |
|:--|:--|
| **Shift** | 충돌 슬롯을 deadline 내에서 뒤로 이동 |
| **Reject** | feasibility 검사 실패 |
| **Multi-CPU** | 다른 CPU로 분배 |

**미결 항목 (WBS O-3.5)**:
- [ ] 슬롯 오프셋 계산 알고리즘 확정
- [ ] 충돌 방지 전략 결정
- [ ] 태스크 정렬 기준 확정
- [ ] Slack time 활용 전략

### 8.C: CBS 예산 (L2 Sporadic Tasks)

CBS 예산 계산은 비교적 단순합니다.

```
L2의 각 Sporadic task:
  Cs = wcet_us                   // 서버 예산
  Ts = min_inter_arrival_us      // 보충 주기
  → CbsConfig { budget_us: Cs, period_us: Ts, deadline_us }
```

**CBS Utilization 검증**:
```
U_cbs = Σ (Cs_i / Ts_i) for all L2 tasks on CPU
if U_cbs > U_bound(L2) → 거부
```

> CBS는 **L2 Sporadic** 워크로드 전용. L3/L4는 CFS 위임.

---

## 9. Phase 6: Epoch 계산

```
epoch_ns = CLOCK_REALTIME_now_ns() + propagation_margin
```

**왜 propagation_margin이 필요한가?** TIMPANI-O가 스케줄 테이블을 계산한 후, 실행 시작 전에 모든 TIMPANI-N 노드에 전송 완료되어야 한다. 이 마진은 다음을 고려한다:
- 모든 노드로의 gRPC 전송 지연
- 각 노드에서의 BPF map 로딩 시간
- 시계 동기화 안정화 시간 (PTP)

마진이 너무 작으면 노드가 준비되지 않을 위험, 너무 크면 기동 시간 낭비. 기본값은 TBD (초기 후보: 500ms).

---

## 10. 실패 정책 (TBD)

> 실패 정책은 아직 확정되지 않았으며, 추가 논의가 필요하다.

검토 중인 후보 접근:

| 실패 | 후보 조치 |
|:--|:--|
| L1 (Safety) 배치 실패 | 방안 A: 전체 중단. 방안 B: 완화된 조건으로 재시도. |
| L2~L4 배치 실패 | 해당 워크로드만 거부, 나머지 계속. |

---

## 11. 영향받는 컴포넌트

| 컴포넌트 | 변경 내용 |
|:--|:--|
| `timpani-o/src/` | 자원 할당 알고리즘, feasibility 체크, config |
| `timpani-o/proto/` | WorkloadSpec (DDR-003) |

---

## 12. 미결 항목

- [ ] 노드 배치 알고리즘 확정
- [ ] U_bound 값 결정
- [ ] 실패 정책 결정
- [ ] Period 제약 정책 (harmonic 강제 vs 유연)
- [ ] `base_tick`, `propagation_margin` 값
- [x] ~~NodeTopology 수집~~ → DDR-006: `NodeReady` (gRPC `NodeStream`)
