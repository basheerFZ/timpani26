<!--
SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
SPDX-License-Identifier: MIT
-->

# DDR-006: Communication Architecture & Runtime Update

**날짜:** 2026-04-24 (최종 수정: 2026-06-16)
**상태:** Draft
**작성:** Human (Lead Architect) + AI
**관련 문서:** [DDR-011 (런타임 테이블 업데이트)](DDR-011-runtime-table-update.md) — zero-downtime 스케줄 업데이트 메커니즘

---

## 0. 구현 상태 (AI를 위한 참고 사항)

> **IMPORTANT**: 이 문서에 기술된 gRPC 양방향 통신 및 C++ 데몬 재작성(Rework)은 **Phase 1에서 이미 구현이 완료되었습니다**.
> **AI INSTRUCTION**: 문서를 읽는 AI 어시스턴트는 이 컴포넌트들을 기존 인프라로 간주해야 합니다. 사용자가 "미결 항목(Open Items)"(섹션 7)에 관련된 작업을 명시적으로 지시하지 않는 한, **여기서 읽기를 멈추고 문서의 나머지 부분은 무시하십시오**. 이를 통해 컨텍스트를 절약하고 중복 코드 생성을 방지할 수 있습니다.

---

## 1. 결정 요약

| 항목 | 결정 |
|:--|:--|
| O↔N 프로토콜 | **gRPC 양방향 스트림** (libtrpc 대체) |
| timpani-n 스택 | **C++ daemon** (gRPC 연동) + 순수 C BPF |
| 런타임 테이블 | 지원 (재시작 없는 hot update) |
| libtrpc | 완전 대체 (사용 중단) |

---

## 2. 기존 Timpani 25 (libtrpc)의 한계

Timpani 25는 단방향(Client → Server)인 libtrpc (D-Bus)에 의존했다.
이로 인해 다음과 같은 치명적 한계가 발생했다:
- **Push 불가**: 서버가 워크로드 변경 사항을 노드에 통보할 수 없음.
- **재시작 연쇄**: YAML 변경 시 pullpiri → timpani-o → 모든 timpani-n 순차 재시작 필수.
- **상태 비보고**: 노드는 현재 상태/자원 사용량을 보고할 수 없고 결함(fault)만 보고.

---

## 3. 인터페이스 (반영 예시)

timpani-o는 양방향 스트림을 개방하며, timpani-n이 이에 연결한다.

```protobuf
syntax = "proto3";
package timpani.node.v1;

service OrchestratorService {
  rpc NodeStream (stream NodeEvent) returns (stream ControlCommand) {}
}

// timpani-n → timpani-o
message NodeEvent {
  string node_id = 1;
  oneof event {
    NodeReady    ready   = 2;  // Topology, 커널 버전, PREEMPT_RT/sched_ext 상태
    NodeStatus   status  = 3;  // 자원 사용량, uptime 등 상태 보고
    FaultInfo    fault   = 4;  // Deadline misses (DDR-003 참조)
    TableApplied applied = 5;  // 테이블 push 수신/적용 확인
  }
}

// timpani-o → timpani-n
message ControlCommand {
  oneof command {
    HierarchicalScheduleTable full_table = 1;  // 전체 교체
    ScheduleTableUpdate       update     = 2;  // 추가/삭제/변경
    ShutdownCommand           shutdown   = 3;  // 정상 종료
  }
}
```

---

## 4. C++ Rework 아키텍처

gRPC 연동을 위해 `timpani-n`의 userspace daemon을 C++로 다시 작성한다. BPF 커널 코드는 C를 유지한다. RT critical path는 엄격히 분리된다.

| 스레드 | 역할 | gRPC 접근 |
|:--|:--|:--|
| **Timer Master** | RT 슬롯 발화 (SCHED_FIFO) | ❌ 금지 |
| **gRPC Client** | O↔N 스트림 관리 | ✅ 전담 |
| **BPF Loader** | 수신된 업데이트를 BPF maps에 주입 | 간접 |
| **Fault Monitor** | `fault_ringbuf` 관찰 및 전송 | ✅ 보고 |

---

## 5. 런타임 테이블 업데이트

Pullpiri의 변경 사항은 timpani-o가 재계산 후 timpani-n 노드들에 직접 **push**하며, 프로세스 재시작 없이 즉각 반영된다.

### 전환 정책

| 업데이트 유형 | 적용 시점 |
|:--|:--|
| 독립 워크로드 추가/제거 | **즉시** |
| 파이프라인 워크로드 / 전체 교체 | **다음 hyperperiod 경계** (shadow/active map 교체) |

### 독립 구동 & Graceful Degradation
- `timpani-n`이 먼저 시작 시: `timpani-o` 연결 전까지 재시도.
- `timpani-o` 재시작 시: `timpani-n`은 기존 스케줄로 계속 동작하며, 복구 후 새 테이블 동기화.

---

## 6. 배포

| 컴포넌트 | 패키징 | 근거 |
|:--|:--|:--|
| **timpani-o** | OCI Container | 표준 마이크로서비스, 호스트 권한 불필요. Pullpiri 컨테이너 생태계 소속. |
| **timpani-n** | systemd service | CAP_BPF, CAP_SYS_NICE, cgroup 감시, PID namespace 필요. 워크로드 컨테이너보다 선행 구동 필수. |

---

## 7. 미결 항목

- [ ] gRPC Unix socket vs TCP (단일 ECU vs 다중 ECU)
- [ ] BPF shadow map atomic swap의 구현 가능성 검증
- [ ] 구체적인 NodeStatus 송신 주기
