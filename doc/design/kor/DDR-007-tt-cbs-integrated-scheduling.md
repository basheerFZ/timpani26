<!--
SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
SPDX-License-Identifier: MIT
-->

# DDR-007: L1(TT) + L2(CBS) 통합 스케줄링 — Isolated CPU 구현방안

**날짜:** 2026-04-27
**상태:** Proposal
**작성:** Human (Lead Architect) + AI

---

## 0. 사전 정의 — 본 DDR의 범위

본 문서는 [DDR-001](DDR-001-workload-model.md) ~ [DDR-006](DDR-006-communication-architecture.md)
및 [HSF-concept](HSF-concept.md)에서 확정된 다음 사항을 **전제**로 한다.

| 전제 | 출처 |
|:--|:--|
| L1 = Periodic + SafetyCritical, **TT(Time-Triggered) 단일 메커니즘** | DDR-001 §4 |
| L2 = Sporadic + SafetyCritical, **CBS(Constant Bandwidth Server) 단일 메커니즘** | DDR-001 §4 |
| L3/L4 = NonSafety, Non-Isolated CPU에서 CFS 위임 (본 DDR 범위 외) | DDR-001 §4 |
| Partitioned Scheduling, task migration 금지 | DDR-004 §2 |
| sched_ext (`SCX_OPS_SWITCH_PARTIAL`) + Timer Master 패턴 | DDR-002, DDR-005 |
| HSF 3-Level (Root / Timpani-N / Workload) | HSF-concept §7 |

> **중요한 단순화 (vs 이전 초안):**
> 본 DDR은 **L1 dual-mode(TT-floor + CBS) 및 L2 Periodic(TT_CAPPED) 개념을 채택하지 않는다.**
> - L1은 **항상 Periodic 이므로 TT만**으로 스케줄링한다.
> - L2는 **항상 Sporadic 이므로 CBS만**으로 스케줄링한다.
> - "L1 Sporadic", "L2 Periodic" 조합은 워크로드 모델에서 정의되지 않는다 (DDR-001).
> - 따라서 TT 슬롯 타입은 단일하며, CBS DSQ도 분리할 필요가 없다.

---

## 1. 문제 정의

DDR-001~006은 L1(TT)과 L2(CBS)를 **개별 메커니즘**으로 정의했으나,
**동일한 Isolated CPU 위에서 두 메커니즘이 시간을 공유할 때의 통합 동작**은
다음 5개 질문에 대한 명확한 답을 갖지 못한다.

| 질문 | 답해야 할 주체 |
|:--|:--|
| Q1. L1 TT 슬롯과 L2 CBS 서버는 동일 CPU에서 어떻게 시간 분할하는가? | 통합 모델 §2 |
| Q2. Timpani-O는 TT 슬롯 배치 후 CBS 가용 대역폭을 어떻게 산출하는가? | Timpani-O §3 |
| Q3. Timpani-N BPF는 TT 발화와 CBS 실행을 런타임에 어떻게 중재하는가? | Timpani-N §4 |
| Q4. CBS 예산 보충이 TT 슬롯 도중 발생하면 어떻게 처리하는가? | §4.5 |
| Q5. 다중 Isolated CPU가 있을 때 L1/L2를 어떻게 배치하는가? | §6 |

---

## 2. 통합 스케줄링 모델

### 2.1 시간 분할 원칙 — "TT-first, CBS-in-gaps"

Isolated CPU의 시간축은 다음 두 단계로 결정된다.

```
Step A (오프라인, Timpani-O):
  L1 TT 슬롯을 hyperperiod 내 고정 위치에 먼저 배치한다.
  → 이 슬롯들은 절대 이동하지 않는 "예약된 시간"이다.

Step B (오프라인, Timpani-O):
  TT 슬롯 사이의 빈 구간(Gap)이 곧 L2 CBS 서버의 실행 가능 시간이다.
  → CBS feasibility는 "Gap 구간 내에서" 검증된다.

Step C (런타임, Timpani-N):
  매 슬롯 시각에 Timer Master가 BPF에 신호를 보낸다.
  TT 슬롯 활성 → L1 task 무조건 dispatch.
  TT 슬롯 비활성(= Gap) → 예산 잔액이 있는 L2 CBS task dispatch.
```

### 2.2 시간축 시각화

```
Isolated CPU, hyperperiod = 20ms (예시)

  0ms     2ms    5ms    7ms   10ms   12ms   15ms   17ms   20ms
  │       │      │      │      │      │      │      │      │
  ├──TT──┤       ├──TT─┤       ├──TT──┤       ├──TT─┤      │
  │ L1-A │       │L1-B │       │ L1-A │       │L1-B │      │
  │      │       │     │       │      │       │     │      │
  │      ├──CBS──┤     ├─CBS──┤       ├──CBS──┤     ├─CBS─┤
  │      │ L2-X  │     │ L2-Y │       │ L2-X  │     │idle │
  │      │       │     │      │       │       │     │     │

  TT 슬롯 (L1):  Timpani-O가 사전에 offset/duration 확정
  Gap (L2 CBS):  TT 슬롯 사이의 빈 구간 — CBS 서버 실행 영역
  idle:          CBS task가 없거나 예산을 모두 소진한 경우 → CPU 절전
```

### 2.3 디스패치 우선순위 체인

```
[최고 우선순위]
  현재 TT 슬롯 활성? ──Yes──→ L1 task 즉시 dispatch (선점 불가)
        │
        No (= Gap)
        ↓
  CBS_DSQ에 잔액 있는 task? ──Yes──→ L2 CBS task dispatch
        │
        No
        ↓
  Isolated CPU idle (L3/L4는 다른 CPU에서 실행)
[최저 우선순위]
```

### 2.4 HSF 3-Level 매핑

| HSF Level | 본 DDR에서의 역할 | 구현 위치 |
|:--|:--|:--|
| Level 0 (Root, 오프라인) | L1 TT 슬롯 + L2 CBS 예산 사전 산정 | Timpani-O §3 |
| Level 1 (Timpani-N) | TT 슬롯 발화 + CBS 예산 강제 | BPF + Timer Master §4 |
| Level 2 (워크로드) | 위임받은 슬롯/예산 내에서 실행 | DDR-005 User App Interface |

---

## 3. Timpani-O — 오프라인 통합 스케줄 생성

### 3.1 전체 흐름 (5-Step)

```
입력: WorkloadSpec[] (L1/L2만, task_specs 포함)
       + NodeTopology (Isolated CPU 목록)
        │
        ▼
Step 1: 워크로드 분류 (L1 → TT / L2 → CBS)
        │
        ▼
Step 2: Isolated CPU 배정 (Partitioned, DDR-004 §6 기반)
        │
        ▼
Step 3: TT 슬롯 배치 (L1 Periodic, DM 정렬, harmonic period 가정)
        │
        ▼
Step 4: Gap 분석 + CBS 예산 할당 (L2 Sporadic, feasibility 검증)
        │
        ▼
Step 5: HierarchicalScheduleTable 출력 (DDR-003 메시지)
```

### 3.2 Step 1 — 분류

DDR-001 매핑 규칙을 그대로 적용하되, 본 DDR은 L1/L2만 처리한다.

```rust
enum Mechanism {
    Tt,   // L1 Periodic 전용
    Cbs,  // L2 Sporadic 전용
}

struct ClassifiedTask {
    workload_id: String,
    task_id:     String,
    mechanism:   Mechanism,
    period_us:   u32,   // L1: period / L2: min_inter_arrival
    wcet_us:     u32,
    deadline_us: u32,
}

fn classify(spec: &WorkloadSpec) -> Result<Vec<ClassifiedTask>, ValidationError> {
    let mech = match (spec.temporal_class, spec.criticality) {
        (Periodic, SafetyCritical) => Mechanism::Tt,    // L1
        (Sporadic, SafetyCritical) => Mechanism::Cbs,   // L2
        _ => return Err(ValidationError::OutOfScope),    // L3/L4 또는 Aperiodic
    };
    Ok(spec.task_specs.iter().map(|t| ClassifiedTask {
        workload_id: spec.workload_id.clone(),
        task_id:     t.task_id.clone(),
        mechanism:   mech,
        period_us:   t.period_us,
        wcet_us:     t.wcet_us,
        deadline_us: t.deadline_us,
    }).collect())
}
```

### 3.3 Step 2 — Isolated CPU 배정

DDR-004 §7의 규칙을 따른다. 본 DDR에서 추가하는 통합 기준:

```
배정 우선순위 (Partitioned Scheduling):

① L1 워크로드 (TT)  → 워크로드별 전용 Isolated CPU
   (DDR-002 §5: cpuset isolated, 전용 코어 독점)

② L2 워크로드 (CBS) → Isolated CPU pool 공유
   (DDR-002 §5: cpuset isolated, sched_ext가 예산 강제)

L2 task의 CPU 선택 기준 (이전 Timpani-O 결정):
  ─ CBS Us = wcet/period
  ─ 후보 CPU c에 대해:
       residual_cap(c) = U_bound − U_tt(c) − U_cbs_assigned(c) − U_overhead
  ─ residual_cap(c) ≥ Us 인 CPU 중 잔여 용량이 가장 큰 것 선택
       (Worst-Fit Decreasing — gap fragmentation 최소화)
```

### 3.4 Step 3 — L1 TT 슬롯 배치

본 DDR은 **단일 Phase, Deadline Monotonic 정렬**로 배치한다.
모든 L1 task가 SafetyCritical이므로 Phase 분리(Safety vs RT) 불필요.

#### 전제 조건 — Harmonic Period

DDR-004 §8(5-A)에서 검토한 harmonic period 규칙을 본 DDR이 **요구**한다.
이 가정 하에서 hyperperiod = max(period_i)이며, TT 슬롯 충돌이 발생하지 않는다.

```
harmonic 조건:
  ∀ task_i, task_j:  period_i divides period_j  OR  period_j divides period_i

위반 시:
  Timpani-O는 InfeasibleError::NonHarmonicPeriod 보고
  → Pullpiri가 워크로드 거부
```

#### 배치 알고리즘

```rust
fn place_l1_tt_slots(
    cpu: CpuId,
    tt_tasks: &[ClassifiedTask],   // L1만
    hp_us: u64,                    // = max(period_i) under harmonic 가정
) -> Result<Vec<TtSlot>, InfeasibleError> {

    let mut timeline = Timeline::new(hp_us);  // [0, hp_us) 구간 점유 추적

    // Deadline Monotonic 정렬 — 짧은 deadline이 먼저 슬롯 선점
    let sorted: Vec<_> = tt_tasks.iter()
        .sorted_by_key(|t| t.deadline_us)
        .collect();

    for task in sorted {
        let repeats = hp_us / task.period_us as u64;
        for k in 0..repeats {
            // 기본 위치: 주기 시작점에 즉시 배치
            let preferred = (k as u64) * task.period_us as u64;

            // harmonic 가정 하에서는 정확히 preferred에 배치되어야 함
            // 충돌 시 가장 가까운 빈 자리 탐색
            let offset = timeline
                .place_at_or_nearest(preferred, task.wcet_us as u64)
                .ok_or_else(|| InfeasibleError::TtSlotConflict {
                    cpu,
                    task: task.task_id.clone(),
                })?;

            timeline.insert(TtSlot {
                workload_id: task.workload_id.clone(),
                task_id:     task.task_id.clone(),
                offset_us:   offset as u32,
                duration_us: task.wcet_us,
                deadline_us: task.deadline_us,
                cpu,
            });
        }
    }

    Ok(timeline.slots())
}
```

### 3.5 Step 4 — Gap 분석 + L2 CBS 예산 할당

#### 4-A: Gap 추출

TT 슬롯이 차지하지 않는 시간 구간을 Gap으로 정의한다.

```rust
struct GapInterval { start_us: u32, end_us: u32, length_us: u32 }

fn compute_gaps(slots: &[TtSlot], hp_us: u64) -> Vec<GapInterval> {
    let mut gaps = Vec::new();
    let mut cursor = 0u32;
    let sorted: Vec<_> = slots.iter().sorted_by_key(|s| s.offset_us).collect();

    for slot in sorted {
        if cursor < slot.offset_us {
            gaps.push(GapInterval {
                start_us:  cursor,
                end_us:    slot.offset_us,
                length_us: slot.offset_us - cursor,
            });
        }
        cursor = slot.offset_us + slot.duration_us;
    }
    if (cursor as u64) < hp_us {
        gaps.push(GapInterval {
            start_us:  cursor,
            end_us:    hp_us as u32,
            length_us: hp_us as u32 - cursor,
        });
    }
    gaps
}
```

#### 4-B: Feasibility 조건 (CPU 단위)

```
정의 (CPU k):
  U_tt(k)       = Σ (wcet_i / period_i)   ∀ L1 TT task i on CPU k
  U_cbs(k)      = Σ (Cs_j  / Ts_j)        ∀ L2 CBS server j on CPU k
  U_overhead    = 0.02                    (Timer Master + BPF dispatch)
  U_bound       = 0.80                    (L1 포함 시, DDR-004 §5의 보수 값)

Feasibility 조건:
  U_tt(k) + U_cbs(k) + U_overhead ≤ U_bound

추가 조건 (Gap 충분성):
  min_gap(k) ≥ CBS_MIN_EXEC_US   (예: 100μs)
    └ 너무 짧은 gap은 컨텍스트 스위치 오버헤드로 의미 없음
```

#### 4-C: CBS 파라미터 산정

DDR-004 §8(5-C)을 따른다.

```
모든 L2 CBS task j에 대해:
  Cs_j = task.wcet_us            (서버 예산 = 1회 도착에 처리할 WCET)
  Ts_j = task.min_inter_arrival_us  (보충 주기 = MIT)
  Us_j = Cs_j / Ts_j

deadline_us  = task.deadline_us  (BPF runtime이 dmiss 검출에 사용)
```

#### 4-D: 할당 알고리즘 (Safety CBS — 거부 불가)

L2도 SafetyCritical이므로 예산 축소를 허용하지 않는다.
대역폭 부족 시 즉시 `InfeasibleError`로 보고하여 Pullpiri가 워크로드를 거부한다.

```rust
fn allocate_l2_cbs_budgets(
    cpu: CpuId,
    cbs_tasks: &[ClassifiedTask],   // L2만
    u_tt: f64,
    gaps: &[GapInterval],
) -> Result<Vec<CbsConfig>, InfeasibleError> {

    let u_overhead = 0.02;
    let u_bound    = 0.80;
    let u_avail    = u_bound - u_tt - u_overhead;

    if u_avail <= 0.0 {
        return Err(InfeasibleError::NoCbsBandwidth { cpu, u_tt });
    }

    let min_gap = gaps.iter().map(|g| g.length_us).min().unwrap_or(0);
    if min_gap < CBS_MIN_EXEC_US {
        warn!("CPU {}: 최소 gap {}μs < {}μs", cpu, min_gap, CBS_MIN_EXEC_US);
    }

    let mut configs = Vec::new();
    let mut u_alloc = 0.0f64;

    // utilization 큰 순서로 할당 (실패는 빨리)
    let sorted: Vec<_> = cbs_tasks.iter()
        .sorted_by(|a, b| {
            let ua = a.wcet_us as f64 / a.period_us as f64;
            let ub = b.wcet_us as f64 / b.period_us as f64;
            ub.partial_cmp(&ua).unwrap()
        })
        .collect();

    for task in sorted {
        let us = task.wcet_us as f64 / task.period_us as f64;
        if u_alloc + us > u_avail {
            // L2도 Safety이므로 거부 불가 → 즉시 실패 보고
            return Err(InfeasibleError::SafetyCbsExceeded {
                cpu,
                task: task.task_id.clone(),
                requested_us: us,
                available_us: u_avail - u_alloc,
            });
        }
        configs.push(CbsConfig {
            workload_id: task.workload_id.clone(),
            task_id:     task.task_id.clone(),
            budget_us:   task.wcet_us,
            period_us:   task.period_us,
            deadline_us: task.deadline_us,
        });
        u_alloc += us;
    }

    Ok(configs)
}
```

### 3.6 Step 5 — HierarchicalScheduleTable 출력

DDR-003의 메시지 정의를 그대로 사용한다 (필드 추가 불필요).
TT 슬롯 타입 분리(`TtSlotType`)나 CBS dual-mode 필드는 본 DDR에서 채택하지 않는다.

```protobuf
// DDR-003과 동일 — slot_type / dual_mode 필드 없음
message TtSlot {
  string workload_id = 1;
  string task_id     = 2;
  uint32 offset_us   = 3;
  uint32 duration_us = 4;
  uint32 deadline_us = 5;
  uint32 cpu         = 6;
}

message CbsConfig {
  string workload_id = 1;
  string task_id     = 2;
  uint32 budget_us   = 3;  // Cs
  uint32 period_us   = 4;  // Ts
  uint32 deadline_us = 5;
}
```

#### 출력 예시 (JSON)

```json
{
  "table_id": "sched-001",
  "node_id": "ecu-front",
  "hyperperiod_us": 20000,
  "epoch_ns": 1809500000000000000,
  "partitions": [{
    "partition_id": "isolated-0",
    "cpuset": { "cpus": [2], "isolated": true },
    "tt_slots": [
      { "workload_id": "brake",         "task_id": "brake_ctrl",
        "offset_us": 0,     "duration_us": 2000, "deadline_us": 5000, "cpu": 2 },
      { "workload_id": "brake",         "task_id": "brake_ctrl",
        "offset_us": 10000, "duration_us": 2000, "deadline_us": 5000, "cpu": 2 },
      { "workload_id": "steer",         "task_id": "steer_ctrl",
        "offset_us": 5000,  "duration_us": 2000, "deadline_us": 10000, "cpu": 2 }
    ],
    "cbs_entries": [
      { "workload_id": "lidar_proc",    "task_id": "lidar_main",
        "budget_us": 2000, "period_us": 5000, "deadline_us": 5000 },
      { "workload_id": "collision",     "task_id": "col_detect",
        "budget_us": 1500, "period_us": 10000, "deadline_us": 10000 }
    ]
  }]
}
```

---

## 4. Timpani-N — 런타임 통합 디스패치

### 4.1 BPF Map 정의 (DDR-005 대비 단순화)

```c
/* TtSlotBpf — slot_type 필드 없음 (단일 타입) */
struct TtSlotBpf {
    u64 workload_id_hash;
    u64 task_id_hash;
    u32 offset_us;
    u32 duration_us;
    u32 deadline_us;
    u32 cpu;
};

/* CbsState — dual_mode/floor_slot_idx 필드 없음.
   본 DDR은 bpf_timer 기반 보충을 채택하지 않으므로 timer 필드 또한 없음.
   보충은 Lazy(BPF ops 경로) + Backup(Replenisher Thread)로 수행된다 — §4.5 참조. */
struct CbsState {
    u32 budget_us;          /* Cs */
    u32 period_us;          /* Ts */
    u32 remaining_us;       /* 잔액 */
    u32 deadline_us;
    u64 replenish_at_ns;    /* 다음 보충 예정 시각 (lazy/backup 모두 참조) */
    u64 exec_start_ns;      /* ops.running()에서 갱신 */
};

/* TaskMeta — scheduling_type 2종 */
struct TaskMeta {
    u64 workload_id_hash;
    u64 task_id_hash;
    u8  scheduling_type;    /* 0 = STYPE_TT (L1) , 1 = STYPE_CBS (L2) */
    u8  layer;              /* 1 = L1 , 2 = L2 (참고용) */
    u64 activation_ns;
    u64 dsq_id;             /* TT task의 경우 task 전용 DSQ ID = task_id_hash */
};

/* current_slot_map: cpu → 활성 slot_idx (Timer Master가 갱신) */
/* SLOT_NONE (0xFFFFFFFF) → 현재 Gap 구간 */
```

### 4.2 DSQ 구조

```
DSQ                       우선순위    대상            ID
────────────────────────────────────────────────────────────────────
task-specific DSQ (L1)    최고       L1 TT task     meta->task_id_hash (동적)
CBS_DSQ                   중간       L2 CBS task    (1ULL << 61)
THROTTLED_DSQ             없음       예산 소진 L2   (1ULL << 61) | 1
SCX_DSQ_GLOBAL            최저       미등록 task    fallback (DDR-005)
```

> **CBS DSQ 단일화:** 이전 초안의 `CBS_L1_DSQ` / `CBS_L2_DSQ` 분리는 본 DDR에서 채택하지 않는다.
> L2만 CBS 대상이므로 단일 `CBS_DSQ`로 충분하며, BPF dispatch 분기가 1개로 단순화된다.

### 4.3 Timer Master 이벤트 타임라인

Timer Master는 본 DDR에서 두 종류의 이벤트만 발화한다.

```
이벤트         의미                                     동작
─────────────────────────────────────────────────────────────────────
TT_START      TT 슬롯 시작                            current_slot_map[cpu] = idx
                                                     scx_bpf_kick_cpu(cpu)
TT_END        TT 슬롯 종료 (= Gap 시작)               current_slot_map[cpu] = SLOT_NONE
                                                     scx_bpf_kick_cpu(cpu)  /* CBS 깨우기 */
```

> **GAP_START 별도 이벤트 불필요:** TT_END가 곧 Gap 시작을 의미하므로 별도 신호가 없어도 된다.
> kick으로 ops.dispatch()가 호출되면 Phase 2(CBS)로 자연스럽게 진행한다.

#### Timer Master 의사코드

```cpp
struct TimelineEvent {
    uint64_t offset_ns;   // epoch_ns 기준
    enum { TT_START, TT_END } type;
    uint32_t cpu;
    uint32_t slot_idx;
};

void TimerMaster::run(const ScheduleTable& table) {
    auto events = build_events(table);   // 모든 cpu의 TT_START/TT_END 정렬
    uint64_t hp_ns = table.hyperperiod_us() * 1000ULL;
    uint64_t cycle_start = table.epoch_ns();

    while (running_) {
        for (const auto& ev : events) {
            uint64_t target = cycle_start + ev.offset_ns;
            struct timespec ts = ns_to_timespec(target);
            clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &ts, nullptr);

            uint32_t value = (ev.type == TT_START) ? ev.slot_idx : SLOT_NONE;
            bpf_map_update_elem(current_slot_fd_, &ev.cpu, &value, BPF_ANY);
            scx_bpf_kick_cpu(ev.cpu);
        }
        cycle_start += hp_ns;
    }
}
```

### 4.4 BPF ops 구현 — 통합 디스패치

#### ops.select_cpu()

DDR-005와 동일. L1 → 전용 Isolated CPU, L2 → Isolated CPU pool.

#### ops.enqueue() — 2-way 분기

본 DDR은 CBS 보충을 **Lazy + Backup** 방식으로 수행한다 (§4.5).
`enqueue` 경로는 lazy 보충의 주 진입점 중 하나다 — 이벤트 도착 시점에
`replenish_at_ns` 경과 여부를 검사하여 throttle 상태를 우회한다.

```c
void BPF_STRUCT_OPS(timpani_enqueue, struct task_struct *p, u64 enq_flags)
{
    struct TaskMeta *meta = bpf_map_lookup_elem(&task_meta_map, &p->pid);
    if (!meta) {
        scx_bpf_dispatch(p, SCX_DSQ_GLOBAL, SCX_SLICE_DFL, enq_flags);
        return;
    }

    if (meta->scheduling_type == STYPE_TT) {
        /* L1: task 전용 DSQ에 무한 slice로 대기. Timer Master kick 시 consume. */
        scx_bpf_dispatch(p, meta->dsq_id, SCX_SLICE_INF, enq_flags);
        return;
    }

    /* L2 CBS */
    u64 cbs_key = meta->workload_id_hash ^ meta->task_id_hash;
    struct CbsState *cbs = bpf_map_lookup_elem(&cbs_map, &cbs_key);
    if (!cbs) {
        scx_bpf_dispatch(p, SCX_DSQ_GLOBAL, SCX_SLICE_DFL, enq_flags);
        return;
    }

    /* ★ Lazy replenish — 이벤트 도착 시점이 보충 시각 이후이라면
         이 한 줄로 throttle을 우회 (가장 흔한 경로, §4.5.2) */
    cbs_lazy_replenish(cbs, bpf_ktime_get_ns());

    if (cbs->remaining_us > 0)
        scx_bpf_dispatch(p, CBS_DSQ, SCX_SLICE_DFL, enq_flags);
    else
        scx_bpf_dispatch(p, THROTTLED_DSQ, SCX_SLICE_DFL, enq_flags);
}
```

#### ops.dispatch() — 2-Phase 통합 중재

```c
void BPF_STRUCT_OPS(timpani_dispatch, s32 cpu, struct task_struct *prev)
{
    /* ─── Phase 1: 활성 TT 슬롯 → L1 무조건 dispatch ─── */
    u32 *slot_idx = bpf_map_lookup_elem(&current_slot_map, &cpu);
    if (slot_idx && *slot_idx != SLOT_NONE) {
        struct TtSlotKey key = { .cpu = cpu, .slot_idx = *slot_idx };
        struct TtSlotBpf *slot = bpf_map_lookup_elem(&tt_table_map, &key);
        if (slot && scx_bpf_dsq_nr_queued(slot->task_id_hash) > 0) {
            scx_bpf_consume(slot->task_id_hash);
            return;
        }
        /* 슬롯은 활성이지만 task가 enqueue되지 않은 상태:
           → 컨테이너 미준비 또는 재시작 중. dispatch skip (idle).
           → fault_monitor가 별도로 missing 이벤트 보고 (DDR-005 §재시작) */
        return;
    }

    /* ─── Phase 2: Gap → L2 CBS dispatch ─── */
    if (scx_bpf_consume(CBS_DSQ))
        return;

    /* idle: Isolated CPU에서 L3/L4 미실행 */
}
```

#### ops.running() — CBS slice 제한

CBS task의 time slice를 **다음 TT 슬롯 시작까지의 잔여 시간**으로 제한한다.
이로써 TT 슬롯 시작 시점에 자연스럽게 양보된다 (kick이 와도 즉시 stopping).

```c
void BPF_STRUCT_OPS(timpani_running, struct task_struct *p)
{
    struct TaskMeta *meta = bpf_map_lookup_elem(&task_meta_map, &p->pid);
    if (!meta) return;

    u64 now = bpf_ktime_get_ns();
    meta->activation_ns = now;

    if (meta->scheduling_type != STYPE_CBS)
        return;

    u64 cbs_key = meta->workload_id_hash ^ meta->task_id_hash;
    struct CbsState *cbs = bpf_map_lookup_elem(&cbs_map, &cbs_key);
    if (!cbs) return;

    cbs->exec_start_ns = now;

    /* slice = min(예산 잔액, 다음 TT까지의 시간) */
    u32 us_to_next_tt = compute_us_to_next_tt(bpf_get_smp_processor_id(), now);
    u32 effective_us  = min_u32(cbs->remaining_us, us_to_next_tt);
    p->scx.slice = (u64)effective_us * 1000ULL;
}
```

#### ops.stopping() — TT dmiss / CBS 예산 차감

```c
void BPF_STRUCT_OPS(timpani_stopping, struct task_struct *p, bool runnable)
{
    struct TaskMeta *meta = bpf_map_lookup_elem(&task_meta_map, &p->pid);
    if (!meta) return;

    u64 now = bpf_ktime_get_ns();

    if (meta->scheduling_type == STYPE_TT) {
        /* L1 TT: deadline miss 검출 */
        u64 deadline_abs = meta->activation_ns +
                           (u64)get_tt_deadline(meta) * 1000ULL;
        if (now > deadline_abs) {
            struct FaultEvent ev = {
                .workload_id_hash     = meta->workload_id_hash,
                .cpu                  = bpf_get_smp_processor_id(),
                .expected_deadline_ns = deadline_abs,
                .actual_completion_ns = now,
                .fault_type           = FAULT_DMISS,
            };
            bpf_ringbuf_output(&fault_ringbuf, &ev, sizeof(ev), 0);
        }
        return;
    }

    /* L2 CBS: 예산 차감 */
    u64 cbs_key = meta->workload_id_hash ^ meta->task_id_hash;
    struct CbsState *cbs = bpf_map_lookup_elem(&cbs_map, &cbs_key);
    if (!cbs) return;

    u64 exec_ns = now - cbs->exec_start_ns;
    u32 exec_us = (u32)(exec_ns / 1000ULL);

    if (cbs->remaining_us > exec_us) {
        cbs->remaining_us -= exec_us;
    } else {
        cbs->remaining_us = 0;
        /* 다음 보충 예정 시각만 기록. 실제 보충은 §4.5의
           Lazy(BPF ops 경로) 또는 Backup(Replenisher Thread)에서 수행.
           bpf_timer_start() 호출 없음 — PREEMPT_RT 제약 회피. */
        cbs->replenish_at_ns += (u64)cbs->period_us * 1000ULL;
        /* 다음 enqueue 시점에 THROTTLED_DSQ로 분기됨 */
    }

    /* CBS deadline miss 검사 (옵션) */
    u64 deadline_abs = meta->activation_ns + (u64)cbs->deadline_us * 1000ULL;
    if (now > deadline_abs) {
        struct FaultEvent ev = {
            .workload_id_hash     = meta->workload_id_hash,
            .cpu                  = bpf_get_smp_processor_id(),
            .expected_deadline_ns = deadline_abs,
            .actual_completion_ns = now,
            .fault_type           = FAULT_DMISS,
        };
        bpf_ringbuf_output(&fault_ringbuf, &ev, sizeof(ev), 0);
    }
}
```

### 4.5 CBS 예산 보충 — Lazy + Backup 단일화

#### 4.5.1 설계 결정 — `bpf_timer` 미채택

`bpf_timer` 콜백은 mainline 커널에서 **softirq context** (hrtimer 콜백 경로)에서
실행되지만, PREEMPT_RT에서는 다음 제약이 있다.

```
① Softirq 처리 변경
   PREEMPT_RT에서 softirq는 ksoftirqd kthread context로 강제 이전됨
   → hrtimer/bpf_timer 콜백이 더 이상 hardirq 직후 즉시 실행되지 않음
   → ksoftirqd의 스케줄링 지연만큼 jitter 추가 (수십~수백 μs)

② BPF context 제약
   특정 PREEMPT_RT 빌드에서 bpf_timer는 비활성화 또는 sleepable 모드로만 허용
   (커널 버전별 차이 — 현재 6.12 mainline은 부분 허용, RT 패치셋에 따라 상이)
```

본 DDR이 요구하는 정밀도(수십 μs, DDR-002 §8)와 PREEMPT_RT 강제 배포 요건
(DDR-005 §Q3)을 동시에 만족하려면 `bpf_timer`는 부적합하다.
따라서 본 DDR은 **`bpf_timer`를 채택하지 않으며**, CBS 예산 보충은
다음 두 메커니즘의 결합으로 단일화한다.

```
[Primary]  Lazy Replenishment in BPF
  → 모든 BPF ops 진입점(enqueue/dispatch)에서 시각 비교 후 즉시 보충
  → 타이머 자체가 불필요. 자기-클럭(self-clocking) 방식.

[Backup]   Replenisher Thread (userspace)
  → Timer Master와 동일한 idiom (SCHED_FIFO + clock_nanosleep ABSTIME)
     으로 보충 시각에 깨어나 cbs_map을 갱신하고 scx_bpf_kick_cpu() 호출
  → throttled 상태로 잠든 task의 wakeup 보장 (lazy 누락 보완)
```

**역할 분담:**

| 상황 | 동작 주체 |
|:--|:--|
| CBS task가 BPF ops 경로를 지나갈 때 (enqueue/dispatch) | Lazy in-place 보충 (BPF) |
| CBS task가 throttled 상태로 잠들어 있을 때 (이벤트 미도착) | Replenisher Thread가 kick |
| TT 슬롯 종료 직후 CBS DSQ에 대기자가 있는지 점검 | dispatch에서 lazy 보충 후 consume |

#### 4.5.2 Primary — Lazy Replenishment

CBS 보충은 "정확한 시각에 콜백이 호출"되어야 하는 것이 아니라,
"보충 시각이 지난 후 다음 dispatch 시점에 잔액이 회복"되어 있으면 충분하다.
주요 BPF ops 진입점에서 `now ≥ replenish_at_ns` 검사를 한 번 수행하면 된다.

##### 보충 헬퍼

```c
/* 경과한 Ts 주기 수만큼 보충하고 replenish_at_ns를 갱신.
   다중 주기 누락도 한 번에 정정 (CBS task가 오래 잠들었던 경우).
   호출 비용: O(1) — 산술 연산만 수행. */
static __always_inline void cbs_lazy_replenish(struct CbsState *cbs, u64 now)
{
    if (now < cbs->replenish_at_ns)
        return;  /* 아직 보충 시각 미도래 */

    u64 ts_ns     = (u64)cbs->period_us * 1000ULL;
    u64 elapsed   = now - cbs->replenish_at_ns;
    u64 n_periods = elapsed / ts_ns + 1;       /* 1회 + 누락분 */

    /* CBS 의미상 잔액은 budget_us를 초과하지 않음 */
    cbs->remaining_us    = cbs->budget_us;
    cbs->replenish_at_ns += n_periods * ts_ns;
}
```

##### 진입점 1 — `ops.enqueue()`

§4.4 enqueue 코드에서 이미 `cbs_lazy_replenish(cbs, now)` 호출이 포함되어 있다.
이벤트가 보충 시각 이후 도착하면 throttle을 즉시 우회한다 (가장 흔한 경로).

##### 진입점 2 — `ops.dispatch()` (Phase 2 진입 직전)

`THROTTLED_DSQ`에 누적된 task 중 보충 시각이 지난 것을 `CBS_DSQ`로 승급시킨다.

```c
void BPF_STRUCT_OPS(timpani_dispatch, s32 cpu, struct task_struct *prev)
{
    /* Phase 1: TT (§4.4와 동일) */
    u32 *slot_idx = bpf_map_lookup_elem(&current_slot_map, &cpu);
    if (slot_idx && *slot_idx != SLOT_NONE) {
        struct TtSlotKey key = { .cpu = cpu, .slot_idx = *slot_idx };
        struct TtSlotBpf *slot = bpf_map_lookup_elem(&tt_table_map, &key);
        if (slot && scx_bpf_dsq_nr_queued(slot->task_id_hash) > 0) {
            scx_bpf_consume(slot->task_id_hash);
            return;
        }
        return;  /* 슬롯 활성이지만 task 미준비 → idle skip */
    }

    /* Phase 2 직전: throttled 큐에서 보충된 task를 CBS_DSQ로 승급 */
    promote_throttled_if_replenished(cpu, bpf_ktime_get_ns());

    if (scx_bpf_consume(CBS_DSQ))
        return;
    /* idle */
}

/* THROTTLED_DSQ를 순회하며 보충된 task를 CBS_DSQ로 이동.
   BPF verifier 제약상 iteration 수에 상한 필요 (예: PROMOTE_BUDGET = 16). */
static __always_inline
void promote_throttled_if_replenished(s32 cpu, u64 now)
{
    int budget = PROMOTE_BUDGET;
    bpf_for_each(scx_dsq, p, THROTTLED_DSQ, 0) {
        if (--budget == 0) break;
        struct TaskMeta *m = bpf_map_lookup_elem(&task_meta_map, &p->pid);
        if (!m) continue;
        u64 k = m->workload_id_hash ^ m->task_id_hash;
        struct CbsState *cbs = bpf_map_lookup_elem(&cbs_map, &k);
        if (!cbs) continue;
        cbs_lazy_replenish(cbs, now);
        if (cbs->remaining_us > 0)
            scx_bpf_dispatch_from_dsq(BPF_FOR_EACH_ITER, p, CBS_DSQ, 0);
    }
}
```

##### `ops.stopping()` 변경

§4.4 stopping에서 `bpf_timer_start()` 호출이 이미 제거되었다.
예산 소진 시 `replenish_at_ns`만 기록하고, 실제 보충은 lazy 또는 backup에서 수행한다.

#### 4.5.3 Backup — Replenisher Thread (userspace)

**필요성:** Lazy 방식은 BPF ops 경로가 호출되어야 동작한다.
다음 경우는 lazy만으로 부족하다.

```
시나리오: collision_det이 t=3ms에 예산 소진 → THROTTLED_DSQ
          이벤트 미도착으로 enqueue 호출 없음
          TT 슬롯이 다른 task의 것 → 해당 CPU의 dispatch 빈도 낮음
          예산 보충 시각 t=10ms가 지났지만 잔액 회복 안 됨
          t=11ms에 이벤트 도착 → enqueue → 그제야 lazy 보충

문제: 보충 후 dispatch까지 수 μs ~ 수백 μs 지연.
      Soft하게 동작하지만 "결정론" 관점에서 약점.
```

**Replenisher Thread**는 Timer Master와 동일한 idiom으로 이 약점을 메운다.

##### 데몬 측 구현

```cpp
/* timpani-n/src/replenisher.cpp */

class Replenisher {
public:
    void run(const ScheduleTable& table) {
        /* (1) 모든 CBS 서버의 (cbs_key, cpu, period_ns, next_at_ns) min-heap 구성 */
        auto pq = build_priority_queue(table);

        while (running_) {
            auto top = pq.top();
            struct timespec ts = ns_to_timespec(top.next_at_ns);
            clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &ts, nullptr);

            /* (2) cbs_map atomic 업데이트 — lazy 헬퍼와 idempotent하게 동작 */
            CbsState s;
            bpf_map_lookup_elem(cbs_fd_, &top.key, &s);
            cbs_apply_replenish_userspace(&s, top.next_at_ns);
            bpf_map_update_elem(cbs_fd_, &top.key, &s, BPF_EXIST);

            /* (3) 해당 task가 배정된 CPU에 kick → dispatch() 재진입 유도 */
            scx_bpf_kick_cpu_from_user(top.cpu);

            /* (4) 다음 보충 시각으로 갱신 */
            top.next_at_ns += top.period_ns;
            pq.pop(); pq.push(top);
        }
    }
};
```

##### 스레드 속성

| 속성 | 값 | 근거 |
|:--|:--|:--|
| 스케줄링 정책 | `SCHED_FIFO`, prio = Timer Master − 1 | TT 정밀도가 우선 |
| CPU 핀 | Non-Isolated CPU 또는 Timer Master와 같은 CPU | RT critical path 분리 (DDR-006 §4) |
| 깨어남 정밀도 | `clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME)` | Timer Master와 동일 |
| RT-safe | gRPC/할당 금지, atomic map 업데이트만 | DDR-006 §4 |

#### 4.5.4 Lazy / Backup의 정합성

두 메커니즘은 동일한 헬퍼(`cbs_lazy_replenish` / `cbs_apply_replenish_userspace`)
의미를 따르므로 **idempotent**하다. race가 발생해도 안전하다.

```
race 시나리오:
  t=10000μs (보충 시각)
    BPF 측 enqueue 호출 → cbs_lazy_replenish() 실행 →
        remaining_us = budget_us, replenish_at_ns = 20000μs
    (거의 동시에) Replenisher Thread도 깨어남 →
        BPF map read → remaining_us = budget_us 이미 적용된 상태 →
        cbs_apply_replenish_userspace() 동일 결과 → no-op
        kick은 추가로 발생하지만 dispatch 재진입은 무해

결과: 어느 쪽이 먼저 실행되어도 상태는 동일.
      kick의 중복은 sched_ext가 처리 가능 (idle CPU에 대한 kick은 무비용).
```

#### 4.5.5 TT 슬롯과의 충돌 처리

##### 충돌 시나리오 — 보충이 TT 슬롯 직전에 발생

```
t=2ms                t=4ms              t=5ms
├──── CBS 실행 ──────┤                   ┤ TT_START
                     │ Replenisher kick │ kick (Timer Master)
                     │ remaining_us=Cs  │
                     │                  │
→ THROTTLED 였던 task가 다음 enqueue 시 CBS_DSQ로 이동
→ Phase 1(TT)이 우선이므로 CBS는 t=5ms에 실행 불가
→ TT 슬롯 종료(TT_END) 후 자동으로 CBS dispatch (Phase 2)

결과: 충돌 없음. 보충 시점과 dispatch 시점은 독립.
```

##### 선점 시나리오 — CBS 실행 중 TT 슬롯 도래

```
t=2ms     t=4.5ms (≈)    t=5ms
│         │              │
├── CBS ──┤              │ TT_START
          │ slice 만료    │ kick (이미 idle)
          │ stopping()    │
          │ 예산 차감     │
          │              │
          └─ idle ───────┤ → ops.dispatch() Phase 1 (TT) 발화

→ §4.4 ops.running()의 slice 제한이 작동하여 TT 슬롯 시작 전 자동 양보
→ kick이 와도 단순히 dispatch 재진입만 발생
```

#### 4.5.6 정밀도 / 오버헤드 특성

| 항목 | 본 DDR (Lazy + Backup) |
|:--|:--|
| 보충 발화 정밀도 | `clock_nanosleep` 정밀도 (~수 μs) — Timer Master와 동일 |
| BPF 코드 복잡도 | ops 진입점에 1줄 helper 호출, timer 등록/콜백 없음 |
| Userspace 의존 | Replenisher Thread 1개 |
| Throttled wakeup | Replenisher의 kick (확정) 또는 enqueue 시 lazy (조기 회복) |
| 다중 주기 누락 처리 | `n_periods` 계산으로 한 번에 정정 |
| PREEMPT_RT 적합성 | ○ (Timer Master와 동일 idiom — `bpf_timer` 비의존) |

---

## 5. 통합 시퀀스 — End-to-End

### 5.1 시나리오 워크로드

```
brake_ctrl    (L1, Periodic, period=10ms,    wcet=2ms,   deadline=5ms)
steer_ctrl    (L1, Periodic, period=20ms,    wcet=2ms,   deadline=10ms)
collision_det (L2, Sporadic, MIT=10ms,       wcet=1.5ms, deadline=10ms)
lidar_proc    (L2, Sporadic, MIT=5ms,        wcet=2ms,   deadline=5ms)
```

### 5.2 Timpani-O 처리 (오프라인)

```
Step 1 분류:
  brake_ctrl    → Mechanism::Tt   (L1)
  steer_ctrl    → Mechanism::Tt   (L1)
  collision_det → Mechanism::Cbs  (L2)
  lidar_proc    → Mechanism::Cbs  (L2)

Step 2 CPU 배정 (Isolated cpu=2):
  hyperperiod = LCM(10, 20) = 20ms
  U_tt  = 2/10 + 2/20         = 0.30
  U_cbs = 1.5/10 + 2/5        = 0.55
  U_total = 0.30 + 0.55 + 0.02 = 0.87  > 0.80 → InfeasibleError

조정: lidar_proc 를 cpu=3으로 분리
  cpu=2: U_tt=0.30, U_cbs=0.15 (collision_det), U=0.47 ≤ 0.80 ✓
  cpu=3: U_tt=0,    U_cbs=0.40 (lidar_proc),    U=0.42 ≤ 0.80 ✓

Step 3 TT 슬롯 배치 (cpu=2, DM 정렬):
  brake_ctrl(d=5ms)  → offset=0,  10000  duration=2000
  steer_ctrl(d=10ms) → offset=5000        duration=2000

Step 4 Gap 분석 (cpu=2):
  Gap 구간: [2000, 5000], [7000, 10000], [12000, 20000]
  → 총 14ms / 20ms

Step 5 CBS 예산:
  collision_det → Cs=1500, Ts=10000, deadline=10000
  lidar_proc    → Cs=2000, Ts=5000,  deadline=5000  (cpu=3에 배치)
```

### 5.3 Timpani-N 런타임 동작 (cpu=2, 1 hyperperiod)

```
t=0ms      TT_START(cpu=2, brake_ctrl)
           → current_slot_map[2] = idx_brake_0
           → kick → dispatch Phase 1 → consume(brake.task_id_hash)
           → brake_ctrl 실행 [0, 2ms]

t=2ms      TT_END(cpu=2)
           → current_slot_map[2] = SLOT_NONE
           → kick → dispatch Phase 2 → consume(CBS_DSQ)
           → collision_det 도착 대기 중이면 실행, 아니면 idle

t=5ms      TT_START(cpu=2, steer_ctrl)
           → 진행 중 CBS task가 있다면 slice 만료(또는 kick)으로 stopping
           → CBS 예산 차감 → dispatch Phase 1 → steer_ctrl 실행 [5, 7ms]

t=7ms      TT_END(cpu=2) → Gap [7, 10ms]
           → CBS dispatch 가능

t=10ms     TT_START(cpu=2, brake_ctrl)  /* 두 번째 인스턴스 */
           → ...

t=20ms     hyperperiod 종료, 다음 주기 반복
```

이벤트 도착 흐름 (collision_det):

```
앱: ttsched_signal_arrival("col_detect");   /* DDR-005 User App API */
  → futex_wake → BPF가 task wake-up 감지
  → ops.enqueue(): cbs->remaining_us > 0 → CBS_DSQ
  → 다음 Gap에서 dispatch
```

---

## 6. 다중 Isolated CPU 배치 전략

### 6.1 설계 원칙 — TT-heavy / CBS-heavy 분리

L1 task가 많은 CPU에서 L2 CBS를 함께 실행하면 다음 비용이 누적된다:
- Gap fragmentation → CBS 컨텍스트 스위치 빈발
- min_gap < CBS_MIN_EXEC_US 발생 가능
- Feasibility 분석 복잡도 증가

따라서 가능한 경우 **L1 전용 CPU와 L2 전용 CPU를 분리**하는 것이 권장된다.

```
가용 Isolated CPU: [cpu2, cpu3, cpu4, cpu5]

  cpu2, cpu3 (TT-primary):  L1 워크로드
    U_tt ≈ 0.50~0.70, U_cbs ≈ 0 ~ 0.10 (소량 spillover만)

  cpu4, cpu5 (CBS-primary): L2 워크로드
    U_tt ≈ 0,        U_cbs ≈ 0.50~0.78
```

### 6.2 배정 알고리즘

```rust
fn assign_isolated_cpus(
    node: &Node,
    classified: &[ClassifiedTask],
) -> Result<CpuAssignment, InfeasibleError> {

    let isolated = node.isolated_cpus();
    let mut assignment = CpuAssignment::new();

    /* ① L1 TT: 워크로드별 전용 CPU 우선, 부족 시 같은 cpuset에 추가 */
    for task in classified.iter().filter(|t| t.mechanism == Tt) {
        let cpu = pick_tt_cpu(&isolated, &assignment, task)?;
        assignment.add(cpu, task.clone(), Mechanism::Tt);
    }

    /* ② L2 CBS: TT가 가벼운 CPU의 Gap에 들어가는지 확인 */
    for task in classified.iter().filter(|t| t.mechanism == Cbs) {
        let us = task.wcet_us as f64 / task.period_us as f64;

        /* 후보 1: TT CPU에 잔여 용량이 있고 min_gap 충분 */
        if let Some(cpu) = pick_tt_cpu_with_gap(&assignment, task, us) {
            assignment.add(cpu, task.clone(), Mechanism::Cbs);
            continue;
        }

        /* 후보 2: 별도 CBS-primary CPU 할당 */
        let cpu = pick_or_alloc_cbs_cpu(&isolated, &assignment, us)
            .ok_or(InfeasibleError::NoCbsCpu(task.task_id.clone()))?;
        assignment.add(cpu, task.clone(), Mechanism::Cbs);
    }

    Ok(assignment)
}
```

### 6.3 배정 결과 검증

각 CPU에 대해 다음을 검증한 후 테이블을 출력한다:

```
∀ cpu ∈ assigned_cpus:
  U_tt(cpu) + U_cbs(cpu) + U_overhead ≤ U_bound (= 0.80)
  min_gap(cpu) ≥ CBS_MIN_EXEC_US (CBS task가 있는 경우)
  TT 슬롯 배치 충돌 없음 (harmonic 조건 검증)
```

---

## 7. 통합 규칙 요약표

| 메커니즘 | 워크로드 분류 | Timpani-O 역할 | Timpani-N 역할 | DSQ | 선점 규칙 |
|:--|:--|:--|:--|:--|:--|
| **TT** | L1 (Periodic + Safety) | DM 단일 Phase 배치 (harmonic 가정) | Timer Master kick → 즉시 dispatch | task 전용 DSQ | 선점 불가 (최고 우선순위) |
| **CBS** | L2 (Sporadic + Safety) | Gap 내 예산 할당, 거부 불가 | Gap 구간에서 예산 기반 dispatch, Ts 주기 보충 | `CBS_DSQ` / `THROTTLED_DSQ` | TT 슬롯에 양보 (slice 제한) |

### 디스패치 우선순위 체인

```
[1] L1 TT (active slot)
       ↓ (Gap)
[2] L2 CBS (예산 잔액 > 0)
       ↓ (CBS task 없음 또는 모두 throttled)
[3] Idle  (Isolated CPU에서 L3/L4 미실행)
```

### Fault 보고

| Fault Type | 발생 조건 | 발생 위치 |
|:--|:--|:--|
| `FAULT_DMISS` (L1) | TT task가 deadline_abs까지 종료하지 못함 | ops.stopping() |
| `FAULT_DMISS` (L2) | CBS task가 deadline_us 내 종료하지 못함 | ops.stopping() |
| `BUDGET_EXCEED` (L2) | CBS task가 budget_us 소진 (자연스러운 throttle) | ops.stopping() — 알림용, fault 아님 |
| `WATCHDOG` | Timpani-N daemon이 Sporadic task 미도착 감지 | userspace fault_monitor (FaultPolicy.watchdog_period_us) |

---

## 8. Feasibility 분석 — 통합 조건 정리

### 8.1 CPU 단위 (Sufficient Condition)

```
∀ Isolated CPU k ∈ assigned_cpus:

  U_tt(k)    = Σ_i (wcet_i / period_i)         ∀ L1 task i on CPU k
  U_cbs(k)   = Σ_j (Cs_j  / Ts_j)              ∀ L2 CBS server j on CPU k
  U_overhead = 0.02
  U_bound    = 0.80   (L1 포함 시, DDR-004 §5)

  Feasibility:
    U_tt(k) + U_cbs(k) + U_overhead ≤ U_bound        ─ (필요조건 1)
    min_gap(k) ≥ CBS_MIN_EXEC_US                     ─ (필요조건 2)
    L1 task의 모든 period가 harmonic 관계             ─ (필요조건 3)
```

### 8.2 정성적 보장

| 보장 항목 | 근거 |
|:--|:--|
| L1 TT의 결정론 | 정적 슬롯 + Phase 1 dispatch는 최고 우선순위 |
| L2 CBS의 시간적 격리 | 예산(Cs)이 Ts 내 정확히 1회 보충, BPF가 강제 |
| L1 ↔ L2 간섭 차단 (FFI) | TT 슬롯 활성 중 CBS dispatch 불가 (Phase 1 우선) |
| L2 ↔ L2 간섭 차단 | 각 CBS 서버의 budget_us는 독립 차감, 폭주 격리 |
| L3/L4와의 격리 | cpuset isolated partition (DDR-002 §5) |
| Compositional 분석 | L1 분석 ⊥ L2 분석 (Gap 분석으로 분리됨) |

---

## 9. ISO 26262 / FFI 관점

본 모델의 FFI는 3계층으로 보장된다.

```
Layer 1: 물리적 격리 (cpuset isolated)
  Isolated CPU ↔ Non-Isolated CPU
  → L1/L2 ↔ L3/L4 완전 격리

Layer 2: 시간적 격리 (TT 슬롯)
  L1 task는 고정 offset/duration의 슬롯에서만 실행
  → L1 슬롯의 위치는 L2 워크로드 추가/제거에 영향받지 않음
  → Compositional 분석: L1만 따로 분석 가능

Layer 3: 예산 격리 (CBS)
  각 L2 CBS 서버의 예산은 BPF가 독립적으로 강제
  → 한 L2 task의 폭주가 다른 L2 task에 영향 불가
  → L1은 항상 Phase 1 dispatch이므로 L2 폭주에 무관

인증 범위 분리:
  Timpani-N (BPF) — 단순 집행 로직만, Criticality 분기 없음 → 검증 용이
  Timpani-O      — Safety 자원 할당 검증, 오프라인 도구로 별도 검증 가능
```

---

## 10. 영향받는 컴포넌트 / DDR

| DDR | 영향 | 변경 내용 |
|:--|:--|:--|
| DDR-001 | 변경 없음 | 본 DDR이 DDR-001의 L1/L2 정의를 그대로 채택 |
| DDR-002 | 보완 | §7 Sporadic+Safety 듀얼모드 → 본 DDR 채택하지 않음 (L1=Periodic 전용으로 단순화). DDR-002 §7은 향후 정합성 업데이트 필요 |
| DDR-003 | 변경 없음 | TtSlot/CbsConfig는 기존 정의 그대로 사용. `slot_type`/`dual_mode` 추가 불필요 |
| DDR-004 | Phase 5 알고리즘 | 본 DDR의 §3.4~3.5(TT 배치 + Gap + CBS 예산)로 구체화 |
| DDR-005 | BPF Map | `TtSlotBpf.slot_type`, `CbsState.dual_mode` 등 미채택. §4.1의 단순화된 구조 적용 |
| DDR-005 | ops.dispatch() | §4.4의 2-Phase 통합 로직으로 교체 |
| DDR-005 | ops.enqueue() | 2-way 분기 (STYPE_TT / STYPE_CBS) |
| DDR-005 | ops.running() | §4.4의 CBS slice 제한 추가 |
| DDR-005 | Timer Master | TT_START/TT_END 두 이벤트만 사용 (GAP_START 별도 이벤트 불필요) |
| DDR-005 | CBS 보충 메커니즘 | `bpf_timer` 머지트리 추가 안 함. Lazy(BPF ops) + Replenisher Thread(userspace)로 단일화 (§4.5) |
| DDR-006 | Replenisher Thread | RT critical path 스레드 표(§4)에 추가 필요 (SCHED_FIFO, Timer Master − 1 priority) |
| DDR-006 | 런타임 갱신 | 변경 없음 — DDR-006의 hot update 메커니즘 그대로 |

---

## 11. 미결 항목

- [ ] `CBS_MIN_EXEC_US` 값 결정 (초기 후보: 100μs — Antigravity PoC 측정 필요)
- [ ] `U_overhead = 0.02` 값 검증 (PREEMPT_RT + sched_ext 실측, DDR-005 §Q3)
- [ ] L1 harmonic period 강제 정책 (DDR-004 미결 항목과 연계)
- [ ] L2 CBS deadline miss 처리 정책 (FaultPolicy 적용 범위 — DDR-001 fault 정책과 연계)
- [ ] CPU 배정 시 worst-fit vs best-fit 선택 (§3.3, §6.2)
- [ ] `compute_us_to_next_tt()`의 BPF 구현 (per-CPU 다음 TT_START 시각을 어떻게 조회하는가) — `next_tt_start_map` 신규 도입 필요 검토
- [ ] §4.5 Lazy 보충의 BPF iteration 한계값 (`THROTTLED_DSQ` 순회 상한, `PROMOTE_BUDGET`) — verifier 통과 가능한 최대치 측정
- [ ] Replenisher Thread를 Timer Master와 동일 thread로 통합할지 별도 thread로 둘지 결정 (이벤트 큐 병합 vs 분리)
- [ ] Replenisher Thread의 priority 책정 (Timer Master − 1이 적절한지 PoC 검증, §4.5.3)
