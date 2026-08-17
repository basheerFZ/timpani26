<!--
SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
SPDX-License-Identifier: MIT
-->

# DDR-001: TIMPANI Workload Model

**날짜:** 2026-04-06 (Last Updated: 2026-06-15)
**상태:** Accepted
**작성:** Human (Lead Architect) + AI

---

## 1. 결정

TIMPANI는 모든 워크로드를 **TemporalClass**와 **CriticalityClass** 2개의 축으로 분류하고, 그 결과를 **L1~L4 계층**에 매핑한다. `WorkloadSpec` 데이터 모델이 이 속성들을 담는다.

분산 DAG 시나리오를 위해 **Pipeline** 개념을 도입하여 여러 워크로드를 공통 hyperperiod로 묶는다.

---

## 2. 컨텍스트

### Pullpiri + Timpani 역할 분리

| 컴포넌트 | 역할 |
|:--|:--|
| **Pullpiri** | Workload Orchestrator — 무엇을 어디서 실행할 것인가 (생명주기, 분류) |
| **Timpani** | RT Execution Engine — 결정론이 필요한 워크로드를 정확히 언제, 얼마만큼의 CPU 시간 안에 실행할 것인가 |

Pullpiri가 워크로드를 L1~L4로 분류하여 TIMPANI에 전달하면, TIMPANI-O가 스케줄 테이블을 생성하고, TIMPANI-N이 커널 수준(eBPF/sched_ext)에서 강제 집행한다.

### 워크로드 정의

워크로드는 **OCI Container**에 RT 시간적 속성을 확장한 것(`WorkloadSpec`)이다. TIMPANI는 워크로드를 **블랙박스**로 취급한다 (HSF Compositionality). 하나의 워크로드는 **하나의 노드**에 바인딩된다.

### Pipeline

분산 노드에 걸친 데이터 흐름 파이프라인은 **Pipeline**으로 묶어 공통 hyperperiod, DAG 의존성, E2E deadline을 정의한다. 시간 동기화는 gPTP (IEEE 802.1AS) 기반.

---

## 3. 워크로드 분류 (2축)

### 축 1: TemporalClass (timpani 관리 대상만)

| 분류 | 정의 | 파라미터 |
|:--|:--|:--|
| `Periodic` | 고정 주기 반복 실행 | `period_us` |
| `Sporadic` | 이벤트 활성화, 최소 도착 간격 보장 | `min_inter_arrival_us` |

> **Aperiodic 제외**: Aperiodic 워크로드는 MIT (Minimum Inter-arrival Time)가 없어 CBS를 적용할 수 없습니다.
> timpani에서 관리하지 않으며, Pullpiri가 L3/L4 (CFS)로 직접 배포합니다.

### 축 2: CriticalityClass

| 분류 | 정의 | L1~L4 범위 |
|:--|:--|:--|
| `SafetyCritical` | ISO 26262 기능 안전 대상 | **L1 또는 L2만 가능** (Isolated CPU) |
| `NonSafety` | 안전 의무 없음 (QM) | **L3 또는 L4만 가능** (Non-Isolated CPU) |

---

## 4. L1~L4 매핑

```
                │  SafetyCritical          │  NonSafety
────────────────┼──────────────────────────┼──────────────────────
Periodic        │  L1 (TT slot, Isolated)  │  (timpani 미관리)
Sporadic        │  L2 (CBS, Isolated)      │  (timpani 미관리)
```

| Layer | 명칭 | Safety | 스케줄링 | CPU |
|:--|:--|:--|:--|:--|
| **L1** | Strict Deterministic | Safety | 정적 TT 슬롯 | Isolated |
| **L2** | Budget-Bounded | Safety | CBS 예산 | Isolated |
| **L3** | Best-Effort | Non-Safety | CFS | Non-Isolated |
| **L4** | Background | Non-Safety | CFS (최하위) | Non-Isolated |

**규칙:**
- **timpani 관리 대상**: SafetyCritical 워크로드만 (L1/L2)
- **timpani 미관리**: NonSafety 워크로드 (L3/L4) — Pullpiri가 직접 배포 (CFS, cgroup cpuset)
- Periodic + SafetyCritical → L1 (TT). Sporadic + SafetyCritical → L2 (CBS).

> `Sporadic + SafetyCritical` (L2)의 경우, CBS만으로는 ISO 26262 FFI를 만족하기 어렵다. 듀얼모드(TT_FLOOR 워치독)는 DDR-002에서 정의한다.

---

## 5. WorkloadSpec 데이터 모델

### Workload vs Task

- **Workload**: 예산 격리 경계 (cgroup). 내부에 여러 task thread를 포함할 수 있음.
- **Task**: 커널 스케줄링 단위 (`task_struct`). TT 슬롯은 task thread 단위로 생성.

| L1~L4 | `task_specs` | 이유 |
|:--|:--|:--|
| L1 | 필수 | thread별 deadline 보장이 안전 요건 |
| L2 | 필수 | Isolated CPU에서 task 단위 예산 제어 |
| L3/L4 | 불필요 | cgroup quota만 적용 |

### Hyperperiod

- 워크로드 내부: `LCM(내부 모든 task의 period)`
- 글로벌: `LCM(모든 L1/L2 워크로드의 hyperperiod)`

### Proto 정의 (예시 — 변경 가능)

> 아래는 예시이다. 최종 proto 정의는 DDR-003에서 확정한다.

```protobuf
// 예시 — 미확정
message WorkloadSpec {
  string           workload_id      = 1;
  string           name             = 2;
  TemporalClass    temporal_class   = 3;  // Periodic / Sporadic / Aperiodic
  CriticalityClass criticality      = 4;  // SafetyCritical / NonSafety → L1-L4 결정
  repeated TaskSpec task_specs       = 5;  // L1/L2: 필수 (task별 스케줄링), L3/L4: 무시
  string           pipeline_id      = 6;  // 분산 DAG Pipeline 소속 시 설정 (optional)
}

message TaskSpec {
  string task_id     = 1;
  uint32 period_us   = 2;   // Periodic: period / Sporadic: min_inter_arrival
  uint32 wcet_us     = 3;
  uint32 deadline_us = 4;
}
```

---

## 6. AI 워크로드

**상태: TBD.** AI 워크로드 분류는 아직 정의되지 않았으며, NPU/GPU 스케줄링 통합이 성숙됨에 따라 향후 DDR에서 다룬다.

---

## 7. 영향받는 컴포넌트

- `timpani-o/src/` — WorkloadSpec 처리 및 L1~L4 분류
- `timpani-o/proto/` — WorkloadSpec proto 정의 (DDR-003)
- `timpani-n/src/` — L1~L4 분류에 따른 실행 정책 분기
