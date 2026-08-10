<!--
SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
SPDX-License-Identifier: MIT
-->

# DDR-003: Interface / Protocol Design

**날짜:** 2026-04-24 (Last Updated: 2026-06-15)
**상태:** Accepted
**작성:** Human (Lead Architect) + AI

---

## 1. 결정

TIMPANI의 인터페이스를 세 레이어로 분리한다:
1. **WorkloadSpec** (Pullpiri → TIMPANI-O): 워크로드 속성 선언
2. **HierarchicalScheduleTable** (TIMPANI-O → TIMPANI-N): 계산된 실행 테이블
3. **FaultNotification** (TIMPANI-O → Pullpiri): 장애 보고

워크로드 생명주기 액션:
- **STOP**: 워크로드 정상 종료 (우선 구현)
- **RESTART**: 장애 또는 수동 요청 후 워크로드 재시작 (미래)

시간 동기화는 **PTP (IEEE 1588)** 기반, 공유 `epoch_ns`를 사용한다.

---

## 2. 컨텍스트

### 인터페이스 분리 근거

| 레이어 | 아는 것 | 생산물 |
|:--|:--|:--|
| Pullpiri | 워크로드 **속성** | WorkloadSpec |
| TIMPANI-O | 자원 할당 + 스케줄 **계산** | HierarchicalScheduleTable |
| TIMPANI-N | 테이블 **집행**만 | FaultNotification |

### 배포 흐름

```
1. Pullpiri: 워크로드 배포 (L1~L4 동일)
   └─ 컨테이너 생성, cgroup cpuset 설정 (Pullpiri 책임)

2. Pullpiri → timpani-o: WorkloadSpec (gRPC)
   └─ 응답: 성공/실패만 (WorkloadResponse)
   └─ 분류 결과 (L1~L4) 반환 불필요 — Pullpiri가 YAML에 이미 명시

3. timpani-o: L1/L2 워크로드만 처리
   ├─ L1/L2: Schedule Table 생성 → timpani-n에 적용
   └─ L3/L4: 무시 (timpani 관리 대상 외)

4. timpani-n → timpani-o → Pullpiri: FaultNotification (비동기)
   └─ 런타임 이벤트: deadline miss, budget overrun 등
```

### CPU 격리 책임

| 역할 | 담당 | 비고 |
|:--|:--|:--|
| **cgroup cpuset 설정** | Pullpiri | 컨테이너별 CPU 할당 |
| **Isolated CPU 범위 정의** | 시스템 구성 | Pullpiri가 인지 |
| **Schedule Table 적용** | timpani | L1/L2 워크로드만 |

> **설계 원칙**: timpani-o는 cpuset 정보를 Pullpiri에 반환할 필요 없음.
> Pullpiri는 클러스터 자원 관리자로서 CPU 토폴로지를 이미 알고 있음.

> **cgroup cpuset 사용**: `isolcpus` 커널 파라미터 대신 cgroup cpuset을 사용하여 런타임 동적 관리 가능.

### 런타임 흐름

```
Pullpiri → WorkloadSpec → TIMPANI-O → HierarchicalScheduleTable → TIMPANI-N → eBPF 집행
                                                                        ↓
                                                               FaultNotification → TIMPANI-O → Pullpiri
```

### 워크로드 생명주기

```
[워크로드 추가]
Pullpiri → AddWorkload(WorkloadSpec) → timpani-o
  → L1/L2: Schedule Table 생성 → timpani-n
  → L3/L4: 무시 (Pullpiri가 CFS로 직접 배포)

[워크로드 중지] (우선 구현)
Pullpiri → RemoveWorkload(WorkloadId) → timpani-o
  → timpani-o: Schedule Table에서 워크로드 제거
  → timpani-o: hyperperiod 재계산 (필요 시)
  → timpani-o → timpani-n: ScheduleTableUpdate 전송
  → timpani-n: BPF 맵 엔트리 제거 (task_meta_map, tt_table_map)
  → timpani-n: CBS 예산 해제 (L2)
  → timpani-n: 슬롯 해제 확인

[워크로드 재시작] (미래)
Fault → FaultPolicy.action = RESTART
  → timpani-n → timpani-o: FaultNotification
  → timpani-o → Pullpiri: RestartRequest
  → Pullpiri: 컨테이너 재시작
  → Pullpiri → timpani-o: AddWorkload (재등록)
```

### 워크로드 STOP 처리 (timpani 내부)

| 컴포넌트 | 액션 | 세부사항 |
|:--|:--|:--|
| **timpani-o** | Schedule Table에서 제거 | TT 슬롯 (L1) 또는 CBS 설정 (L2) 삭제 |
| **timpani-o** | Hyperperiod 재계산 | 워크로드 제거가 LCM에 영향 시 재계산 |
| **timpani-o** | 테이블 업데이트 전송 | `ScheduleTableUpdate.remove`를 timpani-n에 전송 |
| **timpani-n** | BPF 맵 정리 | `task_meta_map`, `tt_table_map`, `cbs_budget_map`에서 엔트리 제거 |
| **timpani-n** | 슬롯 해제 확인 | 댄글링 참조 없음 보장 |
| **timpani-n** | 완료 보고 | timpani-o에 Ack |

> **참고**: STOP은 결정론 보장을 위해 hyperperiod 경계에서 즉시 처리됨.

> 향후 테이블 생성이 오프라인으로 이동할 수 있다. TIMPANI-N은 변경 없음 — 테이블 출처와 무관하게 집행. 메시지 포맷은 유지.

---

## 3. Interface 1: WorkloadSpec (Pullpiri → TIMPANI-O)

> 아래 Proto 정의는 예시이다. 최종 정의는 변경될 수 있다.

```protobuf
syntax = "proto3";
package timpani.workload.v1;

service WorkloadService {
  rpc AddWorkload    (WorkloadSpec)   returns (WorkloadResponse) {}
  rpc RemoveWorkload (WorkloadId)     returns (WorkloadResponse) {}
}

message WorkloadSpec {
  string           workload_id      = 1;
  string           name             = 2;
  TemporalClass    temporal_class   = 3;  // Periodic / Sporadic / Aperiodic
  CriticalityClass criticality      = 4;  // SafetyCritical / NonSafety → L1-L4 결정
  repeated TaskSpec task_specs       = 5;  // L1/L2: 필수, L3/L4: 무시
  ContainerSpec    container        = 6;
  FaultPolicy      fault_policy     = 7;
  string           pipeline_id      = 8;  // 분산 DAG Pipeline 소속 시 설정 (optional)
}

message TaskSpec {
  string task_id              = 1;   // 앱 내 식별자 (예: "sensor_read")
  uint32 period_us            = 2;   // L1만: TT 주기
  uint32 min_inter_arrival_us = 3;   // L2만: 최소 도착 간격 (MIT)
  uint32 wcet_us              = 4;   // Worst-Case Execution Time (μs)
  uint32 deadline_us          = 5;   // relative deadline (μs)
}

enum TemporalClass {
  PERIODIC  = 0;
  SPORADIC  = 1;
  APERIODIC = 2;
}

enum CriticalityClass {
  SAFETY_CRITICAL = 0;
  NON_SAFETY      = 1;
}

message ContainerSpec {
  string image       = 1;
  string target_node = 2;  // 필수 — Pullpiri가 배치 노드 결정 (노드 선택은 Pullpiri 책임)
}

message FaultPolicy {
  uint32      max_dmiss          = 1;
  FaultAction action_on_miss     = 2;
  uint32      watchdog_period_us = 3;  // 0 = disabled
}

enum FaultAction {
  NOTIFY  = 0;
  RESTART = 1;
  STOP    = 2;   // 우선 구현 (정상 종료)
}

message WorkloadId {
  string workload_id = 1;
}

message WorkloadResponse {
  int32  status  = 1;  // 0 = success
  string message = 2;
}
```

---

## 4. Interface 2: HierarchicalScheduleTable (TIMPANI-O → TIMPANI-N)

> **통신 서비스 변경 (DDR-006)**: 아래 정의는 DDR-006에서 `OrchestratorService.NodeStream` (양방향 스트리밍 gRPC)으로 대체되었다. `HierarchicalScheduleTable` 메시지 자체는 유지되며 `ControlCommand.full_table`로 전달.

L1~L4 매핑과 테이블 엔트리:
- **L1 (Periodic + Safety)**: TT 슬롯
- **L2 (Sporadic + Safety)**: CBS 예산 엔트리
- **L3/L4**: 테이블 대상 외. Non-Isolated CPU에서 CFS 위임.

```protobuf
syntax = "proto3";
package timpani.node.v1;

message HierarchicalScheduleTable {
  string   table_id         = 1;
  string   node_id          = 2;
  uint64   hyperperiod_us   = 3;  // 슬롯 테이블 반복 주기 (μs)
  uint64   epoch_ns         = 4;  // PTP 기준 절대 시작 시각 (CLOCK_REALTIME ns)
  repeated PartitionConfig partitions = 5;
}

message PartitionConfig {
  string           partition_id  = 1;
  CpuSetSpec       cpuset        = 2;
  repeated TtSlot    tt_slots    = 3;  // L1 Periodic
  repeated CbsConfig cbs_entries = 4;  // L2 Sporadic
}

message CpuSetSpec {
  repeated uint32 cpus      = 1;
  bool            isolated  = 2;  // cpuset.cpus.partition = "isolated"
}

message TtSlot {
  string workload_id  = 1;
  string task_id      = 2;
  uint32 offset_us    = 3;  // hyperperiod 시작으로부터 오프셋
  uint32 duration_us  = 4;  // 슬롯 크기 = WCET
  uint32 deadline_us  = 5;
  uint32 cpu          = 6;
}

message CbsConfig {
  string workload_id  = 1;
  string task_id      = 2;
  uint32 budget_us    = 3;  // Cs: 서버 예산 (μs)
  uint32 period_us    = 4;  // Ts: 예산 보충 주기 (μs)
  uint32 deadline_us  = 5;
}
```

---

## 5. Interface 3: FaultNotification (TIMPANI-N → TIMPANI-O → Pullpiri)

```protobuf
syntax = "proto3";
package timpani.fault.v1;

message FaultInfo {
  string    workload_id  = 1;
  string    node_id      = 2;
  string    task_name    = 3;
  FaultType type         = 4;
  uint64    timestamp_ns = 5;  // PTP 기준 발생 시각
  uint32    dmiss_count  = 6;  // 누적 deadline miss 횟수
}

enum FaultType {
  UNKNOWN       = 0;
  DMISS         = 1;  // deadline miss
  BUDGET_EXCEED = 2;  // CBS 예산 초과 (L2에서만 발생)
  WATCHDOG      = 3;  // watchdog timeout
}
```

---

## 6. PTP 시간 동기화

모든 TIMPANI-N 노드는 PTP 동기화된 `CLOCK_REALTIME`에서 파생된 공통 `epoch_ns`를 공유한다.

```
1. TIMPANI-O: epoch_ns = now() + propagation_margin (예: +500ms) 계산
2. HierarchicalScheduleTable에 epoch_ns 포함하여 각 TIMPANI-N에 전송
3. Timer Master: clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, epoch_ns)
4. epoch_ns 도달 → 모든 노드가 동시에 hyperperiod 시작
```

Timpani 25에서 사용하던 `SyncTimer` gRPC barrier 방식을 대체한다.

---

## 7. 영향받는 컴포넌트

| 컴포넌트 | 변경 내용 |
|:--|:--|
| `timpani-o/proto/` | WorkloadSpec 교체, HierarchicalScheduleTable 추가 |
| `timpani-o/src/` | WorkloadSpec 처리, 테이블 생성 |
| `timpani-n/src/` | PTP epoch 대기, 테이블 집행 |
| `pullpiri/proto/` | Proto 동기화 필요 |
