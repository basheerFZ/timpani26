<!--
SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.

SPDX-License-Identifier: Apache-2.0
-->

# DDR-012: Fault Management 아키텍처 (Fault Monitor Integration v4)

**작성일:** 2026-07-13  
**최종 수정:** 2026-07-15  
**상태:** 승인 / 구현 완료 (Approved / Implemented)  
**작성자:** 김재현 (Jaehyun Kim)  
**관련 문서:** DDR-002 (Scheduling Architecture), DDR-003 (Interface / Protocol), DDR-006 (Communication Architecture), DDR-011 (Runtime Table Update)

---

## 1. 개요

본 문서는 `feat/fault-monitor-integration_v4` 브랜치에서 구현된 **Fault Management 경로**를 정의한다.  
핵심 범위는 아래 두 가지다.

1. Fault Monitor가 Deadline miss 이벤트를 오탐지 없이 수집하고 보고하는 경로
2. Fault 정책(`FaultAction`)에 따라 복구 액션을 트리거하는 경로

---

## 2. 용어 통일 (DDR-001~011 기준)

본 문서는 기존 DDR 용어를 다음과 같이 따른다.

| 용어 | 본 문서 표기 | 기준 문서 |
|:--|:--|:--|
| 데드라인 미스 | **Deadline miss (DMISS)** | DDR-003, DDR-005 |
| 장애 보고 메시지 | **FaultInfo / FaultNotification** | DDR-003 |
| 복구 액션 | **FaultAction.NOTIFY / RESTART / STOP** | DDR-003 |
| 타이머 컴포넌트 | **Timer Master** | DDR-002, DDR-005, DDR-011 |
| 기준 시각 필드 | **epoch_ns** | DDR-003, DDR-004 |
| 반복 주기 | **Hyperperiod** | DDR-001, DDR-004, DDR-011 |
| 사용자 영역 | **userspace** | DDR-002, DDR-006 |

용어 일관성을 위해 본 문서에서는 `ACTION_STOP`, `TimerMaster`, `유저 공간`, `DMISS-only` 같은 변형 표현을 사용하지 않는다.

---

## 3. Fault Management 데이터 경로

```
sched_ext (kernel)
  fault_ringbuf (Deadline miss / budget 관련 이벤트)
        ↓
Fault Monitor (timpani-n, userspace)
  - 임계값(current_limit) 조회
  - 과도기 게이트(dmiss_counting_enabled_) 적용
  - 조건 충족 시 FaultInfo 생성
        ↓
timpani-o
        ↓
Pullpiri
  - FaultAction 정책 적용 (NOTIFY / RESTART / STOP)
```

핵심 목표는 다음과 같다.

- 과도기 이벤트를 무시하여 오탐지 차단
- 임계값 초과에 대해서만 보고하여 신호 품질 유지
- 정책 기반 복구 동작과의 명확한 인터페이스 유지

---

## 4. 설계 결정

### 4.1 Pre-Pass 임계값 동기화 (BPF 등록 이전)

기존에는 BPF 등록 후 `current_limit`를 채우는 순서였고, 이 구간에서 Deadline miss가 먼저 도착하면 오탐지가 발생할 수 있었다.

v4에서는 스케줄 테이블 적용 시 아래 순서를 강제한다.

1. `TtSlot` / `CbsEntry`의 `current_limit`를 Fault Monitor에 선등록
2. 이후 task/BPF map 등록 및 `sched_ext` 집행 시작

아래 예시는 설명용 pseudo-code이며, 실제 구현을 그대로 복사한 코드가 아님을 명시한다.
```cpp
// 1) Pre-Pass: current_limit 선등록
for (const auto& partition : table.partitions()) {
    for (const auto& layer : partition.layers()) {
        for (const auto& tt_slot : layer.tt_slots()) {
            fault_monitor.update_task_limit(tt_slot.task_id(), tt_slot.current_limit());
        }
        for (const auto& cbs_entry : layer.cbs_entries()) {
            fault_monitor.update_task_limit(cbs_entry.task_id(), cbs_entry.current_limit());
        }
    }
}

// 2) Main Pass: BPF 등록 및 스케줄링 적용
for (const auto& partition : table.partitions()) {
  for (const auto& layer : partition.layers()) {
    for (const auto& tt_slot : layer.tt_slots()) {
      bpf_loader.upsert_task_meta(tt_slot.task_id(), tt_slot.workload_id());
      bpf_loader.upsert_tt_slot(tt_slot);
    }
    for (const auto& cbs_entry : layer.cbs_entries()) {
      bpf_loader.upsert_task_meta(cbs_entry.task_id(), cbs_entry.workload_id());
      bpf_loader.upsert_cbs_budget(cbs_entry);
    }
  }
}

timer_master.apply_schedule_table(table);
timer_master.start_or_update_dispatch_loop();
```

### 4.2 이벤트 기반 과도기 게이트 (`dmiss_counting_enabled_`)

고정 시간 warm-up(예: 500ms) 방식은 결정론이 낮아 제거했다.  
대신 Timer Master 정렬 완료 이벤트와 연동한 게이트를 사용한다.

1. 새 테이블 수신 직후: `set_dmiss_counting_enabled(false)`
2. Timer Master가 새 기준 시각 정렬 완료 후: `timing_ready_callback_()`
3. 콜백에서 카운팅 재활성화: `set_dmiss_counting_enabled(true)`

이 방식으로 과도기 동안의 이벤트는 차단되고, 정렬 완료 이후 이벤트만 집계된다.

### 4.3 보고 조건 정규화

Fault Monitor는 카운팅 활성 상태에서만 아래 조건을 검사한다.

$$
\text{report} \iff (\text{dmiss\_count} > \text{current\_limit})
$$

즉, 임계값 이내 지터는 노이즈로 처리하고 초과 시에만 `FaultInfo`를 상위로 전달한다.

---

## 5. FaultAction.STOP 연동

본 DDR에서 정의하는 범위는 STOP 액션의 **fault 연동 지점**이다.

- Pullpiri가 `FaultAction.STOP` 정책을 적용하면 대상 워크로드 제거 흐름으로 진입
- timpani-o/timpani-n은 STOP 대상 워크로드의 스케줄 항목 및 런타임 상태를 정리

STOP 세부 시퀀스(테이블 갱신, BPF map 정리, 경계 동기화)는 아래 문서가 기준이다.

- 인터페이스/정책 필드: DDR-003
- 런타임 테이블 교체 및 정리: DDR-011
- 통신 전달 구조: DDR-006

---

## 6. 구현 모듈 요약

| 컴포넌트 | 파일 / 모듈 | Fault Management 관점 역할 |
|:--|:--|:--|
| **Protobuf** | `proto/node_control.proto`<br>`proto/schedinfo.proto` | `FaultAction`, `FaultInfo`, `current_limit` 연관 필드 |
| **Timpani-N** | `src/fault_monitor.h/.cpp` | Pre-Pass limit 동기화, 이벤트 게이트, 임계값 초과 판정 |
| **Timpani-N** | `src/main.cpp` | 테이블 수신 시 fault 게이트 on/off 및 콜백 연결 |
| **Timpani-O** | `src/recovery_service.h/.cpp` | Fault 수신 후 정책에 따른 제어 흐름 연결 |

---

## 7. 검증 요약

### 7.1 자동화 테스트

`ctest` 기준 주요 테스트가 정상 통과했고, fault 경로 회귀가 없음을 확인했다.

- `SchedInfoServiceTest`
- `FaultServiceClientTest`
- `NodeConfigTest`
- `GlobalSchedulerTest`
- `HierarchicalTableBuilderTest`
