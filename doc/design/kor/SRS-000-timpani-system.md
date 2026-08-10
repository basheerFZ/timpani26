<!--
SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
SPDX-License-Identifier: MIT
-->

# SRS-000: TIMPANI 소프트웨어 요구사항 명세 (SRS)

**Date:** 2026-08-10
**Status:** Draft
**Author:** Human + AI

> 본 문서는 TIMPANI 설계 문서 세트의 **시스템 전체 요구사항 기준선(baseline)**이다. 이는 Design Decision Record(DDR)가 아니라 *요구사항 명세*이므로, `DDR-NNN` 번호가 아니라 `SRS-*` 네임스페이스에 속한다. 기능별 요구사항 명세(예: `SRS-008-…`)는 특정 결정 기록의 요구사항을 구체화한다.
>
> **키워드 규약**: **SHALL** = 필수, **SHOULD** = 권장.

---

## 1. 개요 (Introduction)

본 문서는 Timpani-Next (2026) 프레임워크가 달성해야 하는 시스템·기능적·비기능적 요구사항을 정의한다. TIMPANI는 SDV(Software Defined Vehicle) 및 AI-Native 환경에서 **Dual Determinism**(시간적 결정론 + 기능적 결정론)을 보장해야 하는 실시간 스케줄링 엔진이다.

- **참조 문서**: DDR-001 ~ DDR-012, `TIMPANI-on-QNX.md`

---

## 2. 운영 환경 제약사항 (Environment Constraints)

| ID | 요구사항 | 우선순위 | 매핑 |
|:--|:--|:-:|:--|
| **REQ-ENV-001** | 커널은 `sched_ext`를 지원하는 Linux 6.12 이상이어야 한다(SHALL). | High | DDR-005 |
| **REQ-ENV-002** | 수십 μs 단위 정밀 타이밍을 위해 `PREEMPT_RT`가 적용되어야 한다(SHALL). | High | DDR-002, DDR-005 |
| **REQ-ENV-003** | 워크로드 자원 격리를 위해 `cgroup v2`를 사용해야 한다(SHALL). Isolated partition 필요. | High | DDR-002, DDR-004 |

---

## 3. 기능적 요구사항 (Functional Requirements)

### 3.1 워크로드 및 스케줄링 모델
| ID | 요구사항 | 매핑 |
|:--|:--|:--|
| **REQ-FUN-001** | 시스템은 워크로드를 TemporalClass(Periodic/Sporadic)와 CriticalityClass(Safety/NonSafety)로 분류해야 한다. | DDR-001 |
| **REQ-FUN-002** | 분류된 워크로드는 HSF(Hierarchical Scheduling Framework)의 4계층 L1(Strict RT)~L4(Background)로 스케줄링되어야 한다. | DDR-001, DDR-002 |
| **REQ-FUN-003** | L1(Periodic+Safety) 워크로드는 일관성 보장을 위해 Timpani-O가 정적으로 생성한 Schedule Table(TT Slot) 기반으로 동작해야 한다. | DDR-002 |
| **REQ-FUN-004** | L2(Sporadic+Safety) 워크로드는 이벤트 기반으로 동작하되, CBS(Constant Bandwidth Server)로 CPU 예산이 통제되어야 한다. | DDR-002, DDR-005, DDR-008 |
| **REQ-FUN-005** | L3/L4(NonSafety) 워크로드는 Timpani-N BPF 런타임 개입 없이 Linux 기본 스케줄러(CFS)에 완전히 위임되어야 한다. | DDR-002 |

### 3.2 런타임 제어 및 안전성
| ID | 요구사항 | 매핑 |
|:--|:--|:--|
| **REQ-FUN-006** | L2 Sporadic Safety 워크로드의 최소 감시 주기 보장을 위해 CBS와 TT Floor를 결합한 Dual-Mode 스케줄링을 지원해야 한다. | DDR-002, DDR-007 |
| **REQ-FUN-007** | 애플리케이션 주기가 TT 슬롯과 정확히 동기화되도록 저지연(Futex 기반) API `ttsched_wait_next_period()`를 제공해야 한다. | DDR-005 |
| **REQ-FUN-008** | L1/L2 워크로드가 예산(Budget)을 초과하거나 데드라인을 놓치면, 이를 감지하고 예외 링버퍼(`fault_ringbuf`)를 통해 상위에 리포트해야 한다. | DDR-005, DDR-008 |
| **REQ-FUN-009** | 이벤트 구동형(L2) 워크로드는 전송 계층과 무관하게 미들웨어 디스패치 경계에서 식별/관측되고, MIT(Minimum Inter-arrival Time) 위반 및 지연이 감시되어야 한다. | DDR-008 |

### 3.3 인터페이스 및 통신
| ID | 요구사항 | 매핑 |
|:--|:--|:--|
| **REQ-FUN-010** | Timpani-O ↔ Timpani-N 제어 채널은 gRPC 양방향 스트림(`OrchestratorService.NodeStream`)을 사용해야 하며, 기존 libtrpc/D-Bus 전송을 대체한다. | DDR-003, DDR-006 |
| **REQ-FUN-011** | Pullpiri는 Timpani-O에 스케줄/워크로드 정보를 전달(`SchedInfoService`)하고, `RecoveryService`를 통해 복구를 집행해야 한다. | DDR-003, DDR-006, DDR-012 |

### 3.4 런타임 스케줄 갱신
| ID | 요구사항 | 매핑 |
|:--|:--|:--|
| **REQ-FUN-012** | 시스템은 데몬 재시작 없이 런타임에 워크로드를 추가/제거/수정할 수 있어야 하며, 변경은 hyperperiod 경계에서 결정적으로 적용되어야 한다(zero-downtime). | DDR-011 |

### 3.5 자원 할당
| ID | 요구사항 | 매핑 |
|:--|:--|:--|
| **REQ-FUN-013** | Timpani-O는 L1/L2 워크로드에 대해 실행 가능한 CPU/파티션 배치를 산출하고, 불가능한 스케줄은 거부해야 한다(스케줄링 가능성/feasibility 검사). | DDR-004, DDR-007 |

### 3.6 폴트 및 복구
| ID | 요구사항 | 매핑 |
|:--|:--|:--|
| **REQ-FUN-014** | 시스템은 스케줄링 이상 부류 — deadline miss, CBS budget overrun, MIT 위반, wakeup-latency 초과 — 를 감지·리포트하되, 정상 격리 제어(예: budget 소진)와 진짜 fault를 구분해야 한다. | DDR-008, DDR-012 |
| **REQ-FUN-015** | 복구 결정 시, 시스템은 `RecoveryService.EnforceRecoveryPolicy` → `RecoverySignal`로 STOP/RESTART를 집행하고, 해당 워크로드의 Schedule Table 및 BPF 상태를 evict해야 한다. | DDR-003, DDR-011, DDR-012 |

### 3.7 시간 동기화
| ID | 요구사항 | 매핑 |
|:--|:--|:--|
| **REQ-FUN-016** | 멀티노드 배치는 공유 PTP 기반 `epoch_ns`에 정렬되어 노드 간 hyperperiod 위상이 일관되어야 한다. | DDR-003, DDR-011 |

---

## 4. 비기능적 요구사항 (Non-Functional Requirements)

| ID | 요구사항 | 분류 | 매핑 |
|:--|:--|:--|:--|
| **REQ-NFR-001** | **시간적 결정론**: L1 TT 워크로드의 타이머 발화 지터(Jitter)는 수십 μs 이내로 보장되어야 한다. | Performance | DDR-002, DDR-011 |
| **REQ-NFR-002** | **상호 간섭 배제(FFI)**: Safety(L1/L2)와 NonSafety(L3/L4) 간 하드웨어 레벨 CPU 격리가 보장되어야 한다(ISO 26262 지향). | Safety | DDR-002, DDR-004 |
| **REQ-NFR-003** | **애플리케이션 투명성**: 워크로드는 커널 스케줄링 속성(SCHED_FIFO, priority)을 직접 설정하지 않고 cgroup 매핑으로 투명하게 스케줄링되어야 한다. | Usability | DDR-002, DDR-005 |
| **REQ-NFR-004** | **재시작 복원력**: 워크로드 식별은 우선적으로 Pullpiri 배포 메타데이터 + cgroup으로 해석되어야 하며, thread name(`comm`)은 PoC/진단용 fallback에 한한다. 컨테이너 재시작(pid 변경) 시 이 식별로 정책이 재적용되어야 한다. | Reliability | DDR-005, DDR-008 (결정 3B) |
| **REQ-NFR-005** | **멀티코어 확장성**: 타이머 서브시스템은 격리 CPU 전반으로 확장(per-CPU 타이머 스레드)되어, 서로 다른 CPU에서 같은 시각에 예약된 슬롯이 올바르게 발화되어야 한다. | Performance | DDR-011 |

---

## 5. 로드맵 및 향후 요구사항 (Roadmap & Future Requirements)

현재 개발 단계에서 아직 구체화되지 않은 향후 요구사항이다. *(구체적 일정 및 상용화 목표는 내부에서 관리하며 본 문서에서는 의도적으로 생략한다.)*

### 5.1 단기
| ID | 요구사항 |
|:--|:--|
| **REQ-AI-001** | AI 워크로드용 구체적 제어 요구사항(예: NPU 할당, chunk-based execution)을 도출한다. *(future scope)* |
| **REQ-ED-001** | Sporadic(L2) 워크로드를 위한 디스패치 경계 관측 + CBS 연계 제어(Event Monitor)를 제공하고, SOME/IP ADAS(FCW) PoC로 검증한다. *(future scope; DDR-008 참조)* |
| **REQ-DD-001** | 파이프라인/DAG 데이터 흐름 제어와 마감 시간 추적을 제공하고, Data-Driven(센서 퓨전) PoC로 검증한다. *(future scope)* |

### 5.2 중장기
| ID | 요구사항 |
|:--|:--|
| **REQ-SAF-001** | 시스템은 fault handler 및 기능안전 아키텍처를 통해 기능안전(예: ISO 26262 ASIL-B) 준수를 지원하도록 설계되어야 한다(SHOULD). *(planned)* |
| **REQ-ENV-004** | 아키텍처는 하드웨어/OS 추상화 계층(HAL)을 통해 QNX 등 상용 RTOS로의 이식성을 지원해야 한다(SHOULD). *(planned; `TIMPANI-on-QNX.md` 참조)* |
| **REQ-AD-001** | Feasibility 검토를 전제로, ROS2 / Autoware 등 플랫폼을 위한 AD 프레임워크 추상화 계층(FAL)을 정의한다. *(planned)* |

---
