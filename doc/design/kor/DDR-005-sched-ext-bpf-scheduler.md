<!--
SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
SPDX-License-Identifier: MIT
-->

# DDR-005: TIMPANI sched_ext BPF Scheduler 상세 설계

**날짜:** 2026-04-24 (최종 수정: 2026-06-16)
**상태:** Draft — 논의 중
**작성:** Human (Lead Architect) + AI
**관련 문서:** [DDR-011 (런타임 테이블 업데이트)](DDR-011-runtime-table-update.md) — double buffering/shadow map 아키텍처

---

## 목표

TIMPANI-N에서 동작하는 sched_ext BPF 커스텀 스케줄러의 내부 구조를 정의한다.
이 스케줄러는:
- `HierarchicalScheduleTable`의 HSF 계층을 kernel에서 강제 집행한다
- TT 슬롯의 정밀 발화 (수십 μs 정밀도)
- CBS 예산 강제 (L2 Sporadic 워크로드 전용)
- L1~L4 분류 기반 CPU 격리 (Isolated / Non-Isolated)
- Deadline miss 감지 → fault_ringbuf → daemon

---

## Phase 2 스케줄링 계층 (Scheduling Hierarchy) & DSQ 구조

TIMPANI Phase 2는 **Partial BPF Scheduler** (`SCX_OPS_SWITCH_PARTIAL` 모드)로 동작합니다. L1/L2 Safety-Critical 태스크들은 `scx_timpani`를 통해 명시적으로 스케줄링되는 반면, L3/L4 Non-Safety 태스크들은 완전히 표준 리눅스 CFS 커널로 위임하여 백그라운드 프로세스에 대한 BPF 오버헤드를 제거합니다.

**리눅스 시스템 전체 스케줄링 계층 (Top-Down 우선순위):**
```text
[최고 우선순위]
  SCHED_STOP
      ↓
  SCHED_DEADLINE
      ↓
  SCHED_FIFO / SCHED_RR  (Timer Master 위치)
      ↓
  SCHED_EXT (scx_timpani)
      ├─ TT_WAIT_QUEUE    (L1 Periodic)
      ├─ DSQ_CBS          (L2 Sporadic)
      ├─ [DSQ_THROTTLED]  (보류 전용 큐 - Dispatch 안 됨)
      ├─ DSQ_BE           (레거시 BE 래퍼)
      └─ SCX_DSQ_GLOBAL   (미등록 태스크 안전망)
      ↓
  SCHED_NORMAL (EEVDF)     (L3/L4 완전 위임)
      ↓
  SCHED_IDLE
[최저 우선순위]
```

### Dispatch Queue (DSQ) 정의

1. **TT_WAIT_QUEUE (Task별 Custom DSQ)**
   - **ID**: `meta->task_id_hash` (동적 생성)
   - **대상**: L1 Periodic. Timer Master가 BPF 스케줄러를 Kick할 때까지 대기하며, 슬롯 시작 시 독점적으로 디스패치됩니다.

2. **DSQ_CBS**
   - **ID**: `(1ULL << 61)` (글로벌 Custom DSQ)
   - **대상**: L2 Sporadic. 예산이 남은 경우(`budget > 0`) TT 슬롯 직후 소비됩니다.

3. **DSQ_THROTTLED**
   - **ID**: `(1ULL << 61) | 1` (글로벌 Custom DSQ)
   - **대상**: 예산을 소진한 L2 태스크 전용 보류 큐. **절대 디스패치되지 않으며** `bpf_timer`에 의한 보충을 대기합니다.

4. **DSQ_BE**
   - **대상**: L3/L4 레거시 대응 큐. Phase 2의 완전한 CFS 위임 정책에 따라 점차 사용되지 않습니다.

5. **SCX_DSQ_GLOBAL (System Fallback)**
   - **대상**: 미등록 `SCHED_EXT` 태스크. 유휴(Idle) CPU가 능동적으로 태스크를 당겨갈 수 있도록 하여 자동 글로벌 Load Balancing을 보장합니다.

---

## sched_ext 기본 개념

```
sched_ext (SCX): Linux 6.12+ 정식 merge
  → BPF 프로그램으로 커널 스케줄러 동작을 완전 교체
  → ops 구조체의 콜백들을 BPF로 구현

핵심 ops 콜백:
  ops.select_cpu()  : task를 어느 CPU에서 실행할지 선택
  ops.enqueue()     : task를 어느 dispatch queue에 넣을지 결정
  ops.dispatch()    : CPU가 idle 상태일 때 실행할 task 선택
  ops.running()     : task가 CPU를 획득한 시점
  ops.stopping()    : task가 CPU를 반납하는 시점
  ops.init_task()   : task 처음 생성 시 BPF 측 메타데이터 초기화
```

---

## HSF 3-Level 트리와 sched_ext ops 매핑

HSF의 3-Level 구조(DDR-002)가 sched_ext ops에 다음과 같이 매핑된다:

```
HSF Level 0 (Root / 오프라인 도구)
  → 정적 스케줄 테이블 + CBS 예산을 사전 계산
  → BPF maps로 주입 (tt_table_map, cbs_map, partition_map)
  → sched_ext가 실행 시 참조하는 "정답지"

HSF Level 1 (Timpani-N / 중간 노드)
  → ops.select_cpu():  L1~L4 분류에 따라 Isolated/Non-Isolated CPU 강제
  → ops.enqueue():     L1(TT) → TT 대기큐, L2(CBS) → CBS 큐, L3/L4 → BE 큐
  → ops.dispatch():    TT task 최우선 dispatch, CBS 잔액 확인 후 dispatch
  → ops.running/stopping(): 예산 차감, deadline miss 감지

HSF Level 2 (워크로드 / Leaf)
  → 워크로드는 자신에게 위임된 예산 내에서 실행
  → BPF가 예산 초과 시 선점 (FFI 보장)
```

---

## 전체 구조

```
┌─────────────────────────────────────────────────────────────┐
│  TIMPANI sched_ext BPF (kernel)                             │
│                                                             │
│  ┌─────────────────────────────────────────────────────┐   │
│  │  BPF Maps (TIMPANI-N daemon이 주입)                  │   │
│  │                                                     │   │
│  │  partition_map   : cgroup_id → L1~L4 분류 + CPU mask│   │
│  │  tt_table_map    : (cpu, slot_idx) → TtSlot         │   │
│  │  cbs_map         : workload_id → CbsState           │   │
│  │  current_slot_map: cpu → 현재 활성 TT slot_idx      │   │
│  │  task_meta_map   : pid → TaskMeta                   │   │
│  │  fault_ringbuf   : deadline miss 이벤트 → daemon    │   │
│  └─────────────────────────────────────────────────────┘   │
│                                                             │
│  ops.select_cpu()                                           │
│    → partition_map[cgroup_id] 조회                          │
│    → L1: 전용 Isolated CPU 강제                             │
│    → L2: Isolated CPU pool 내에서 선택                      │
│    → L3/L4: Non-Isolated CPU에서 선택                       │
│                                                             │
│  ops.enqueue()                                              │
│    → TT_SLOT (L1 Periodic): TT_WAIT_QUEUE (Custom)에 보류    │
│    → CBS (L2 Sporadic): cbs_map 잔액 확인                    │
│                  잔액 > 0 → DSQ_CBS                          │
│                  잔액 = 0 → DSQ_THROTTLED (bpf_timer 대기)   │
│    → BEST_EFFORT (L3/L4): DSQ_BE (낮은 우선순위)             │
│                                                             │
│  ops.dispatch()                                             │
│    → current_slot_map[cpu] 확인 (Timer Master가 갱신)        │
│    → 해당 슬롯의 L1 TT task 있으면 즉시 dispatch (최우선)   │
│    → 없으면 DSQ_CBS → DSQ_BE 순서                          │
│                                                             │
│  ops.running()                                              │
│    → L1 TT task: 시작 시각 기록 (deadline miss 체크용)      │
│    → L2 CBS task: exec_start_ns 기록, budget 차감 시작  │
│                                                             │
│  ops.stopping()                                             │
│    → L1 TT task: 완료 시각 - (epoch_ns + offset + deadline) │
│               초과 → fault_ringbuf 기록                     │
│    → L2 CBS task: 실행 시간 측정 → budget 차감          │
│               budget ≤ 0 → throttled_queue 이동            │
│               bpf_timer 설정 (Ts 후 예산 보충)              │
└─────────────────────────────────────────────────────────────┘
          ↑ scx_bpf_kick_cpu() 호출
┌─────────────────────────────────────────────────────────────┐
│  TIMPANI-N Timer Master Thread (C++, userspace)              │
│  SCHED_FIFO 최고 우선순위, CPU pinned                       │
│                                                             │
│  loop:                                                      │
│    next_slot = compute_next_tt_slot(current_time, table)    │
│    clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME,           │
│                    epoch_ns + next_slot.offset_us * 1000)   │
│    // 깨어남                                                 │
│    bpf_map_update(current_slot_map, cpu, next_slot.idx)    │
│    scx_bpf_kick_cpu(next_slot.cpu)                         │
└─────────────────────────────────────────────────────────────┘
```

---

## BPF Map 상세 정의

```c
// partition_map: cgroup_id → L1~L4 분류 + CPU mask
// Key: u64 (cgroup_id)
// Value:
struct PartitionInfo {
    u8  layer;             // 1=L1, 2=L2, 3=L3, 4=L4
    u64 cpu_mask;          // 허용 CPU bitmask
};

// tt_table_map: (cpu, slot_idx) → TtSlot
// Key: struct { u32 cpu; u32 slot_idx; }
// Value:
struct TtSlotBpf {
    u64 workload_id_hash;  // workload_id의 hash
    u64 task_id_hash;      // task_id의 hash (TaskSpec.task_id)
    u32 offset_us;
    u32 duration_us;
    u32 deadline_us;
    u32 cpu;
};

// cbs_map: task_key (workload+task hash) → CbsState (L2 Sporadic task 전용)
// Key: u64 (workload_id_hash ^ task_id_hash)
// Value:
struct CbsState {
    u32 budget_us;         // Cs: 최대 예산
    u32 period_us;         // Ts: 보충 주기
    u32 remaining_us;      // 현재 잔액 (runtime에 차감)
    u64 replenish_at_ns;   // 다음 보충 시각 (bpf_timer 기준)
    u32 deadline_us;
};

// current_slot_map: cpu → 현재 활성 슬롯 인덱스
// Key: u32 (cpu)
// Value: u32 (slot_idx, 0xFFFFFFFF = 없음)

// task_meta_map: pid → TaskMeta
// Key: u32 (pid)
// Value:
struct TaskMeta {
    u64 workload_id_hash;
    u64 task_id_hash;      // TaskSpec.task_id의 hash
    u8  scheduling_type;   // 0=TT_SLOT, 1=CBS, 2=BEST_EFFORT
    u8  layer;             // 1=L1, 2=L2, 3=L3, 4=L4
    u64 activation_ns;     // ops.running()에서 기록
};

// fault_ringbuf: deadline miss 이벤트 → userspace
struct FaultEvent {
    u64 workload_id_hash;
    u32 cpu;
    u64 expected_deadline_ns;
    u64 actual_completion_ns;
    u8  fault_type;        // 0=DMISS, 1=BUDGET_EXCEED
};
```

---

## 핵심 ops 구현 의사코드

### ops.select_cpu()

```c
s32 BPF_STRUCT_OPS(timpani_select_cpu, struct task_struct *p,
                   s32 prev_cpu, u64 wake_flags)
{
    u64 cgroup_id = /* task_meta_map에서 조회 (아래 Q1 참조) */;
    struct PartitionInfo *part = bpf_map_lookup_elem(&partition_map, &cgroup_id);
    if (!part)
        return prev_cpu;  // 알 수 없는 task → 기존 CPU 유지

    // L1: 전용 Isolated CPU 강제
    if (part->layer == 1) {
        s32 cpu = bpf_cpumask_any_and(p->cpus_ptr, part->cpu_mask);
        return cpu >= 0 ? cpu : -ENOENT;
    }

    // L2: Isolated CPU pool 내에서 idle CPU 우선 선택
    if (part->layer == 2) {
        s32 cpu = scx_bpf_pick_idle_cpu(part->cpu_mask, 0);
        return cpu >= 0 ? cpu : bpf_cpumask_any(part->cpu_mask);
    }

    // L3/L4: Non-Isolated CPU에서 선택
    s32 cpu = scx_bpf_pick_idle_cpu(part->cpu_mask, 0);
    return cpu >= 0 ? cpu : bpf_cpumask_any(part->cpu_mask);
}
```

### ops.enqueue()

```c
void BPF_STRUCT_OPS(timpani_enqueue, struct task_struct *p, u64 enq_flags)
{
    struct TaskMeta *meta = bpf_map_lookup_elem(&task_meta_map, &p->pid);
    if (!meta) {
        // 등록되지 않은 task → BE queue (L4 취급)
        scx_bpf_dispatch(p, SCX_DSQ_LOCAL, SCX_SLICE_DFL, enq_flags);
        return;
    }

    switch (meta->scheduling_type) {
    case SCHED_TYPE_TT:
        // L1 TT task: TT_WAIT_QUEUE에 보류 (Custom DSQ ID: task_id_hash)
        scx_bpf_dispatch(p, meta->task_id_hash, SCX_SLICE_DFL, enq_flags);
        break;

    case SCHED_TYPE_CBS: {
        // L2 Sporadic: CBS 예산 잔액 확인
        struct CbsState *cbs = bpf_map_lookup_elem(&cbs_map,
                                                    &meta->workload_id_hash);
        if (cbs && cbs->remaining_us > 0)
            scx_bpf_dispatch(p, DSQ_CBS, SCX_SLICE_DFL, enq_flags);
        else
            scx_bpf_dispatch(p, DSQ_THROTTLED, SCX_SLICE_DFL, enq_flags);
        break;
    }

    default:
        // L3/L4: BE queue
        scx_bpf_dispatch(p, DSQ_BE, SCX_SLICE_DFL, enq_flags);
    }
}
```

### ops.dispatch()

```c
void BPF_STRUCT_OPS(timpani_dispatch, s32 cpu, struct task_struct *prev)
{
    // 1순위: 현재 활성 TT 슬롯 확인 (L1 Periodic)
    u32 *slot_idx = bpf_map_lookup_elem(&current_slot_map, &cpu);
    if (slot_idx && *slot_idx != SLOT_NONE) {
        struct TtSlotKey key = { .cpu = cpu, .slot_idx = *slot_idx };
        struct TtSlotBpf *slot = bpf_map_lookup_elem(&tt_table_map, &key);
        if (slot) {
            // 해당 task를 자신의 TT_WAIT_QUEUE에서 꺼내 즉시 dispatch
            scx_bpf_consume(slot->task_id_hash);  // 이 DSQ에서 가장 앞 task 실행
            return;
        }
    }

    // 2순위: L2 CBS 예산 있는 sporadic task
    if (scx_bpf_consume(DSQ_CBS))
        return;

    // 3순위: L3/L4 BE queue
    scx_bpf_consume(DSQ_BE);
}
```

### ops.running() / ops.stopping()

```c
void BPF_STRUCT_OPS(timpani_running, struct task_struct *p)
{
    struct TaskMeta *meta = bpf_map_lookup_elem(&task_meta_map, &p->pid);
    if (!meta) return;

    u64 now = bpf_ktime_get_ns();
    meta->activation_ns = now;

    // L2 CBS: 예산 차감 시작 (실행 시작 시각 기록)
    if (meta->scheduling_type == SCHED_TYPE_CBS) {
        struct CbsState *cbs = bpf_map_lookup_elem(&cbs_map,
                                                    &meta->workload_id_hash);
        if (cbs) cbs->exec_start_ns = now;
    }
}

void BPF_STRUCT_OPS(timpani_stopping, struct task_struct *p, bool runnable)
{
    struct TaskMeta *meta = bpf_map_lookup_elem(&task_meta_map, &p->pid);
    if (!meta) return;

    u64 now = bpf_ktime_get_ns();

    if (meta->scheduling_type == SCHED_TYPE_TT) {
        // L1 TT: Deadline miss 체크
        u64 deadline_abs = meta->activation_ns +
                           (u64)get_tt_deadline(meta) * 1000;
        if (now > deadline_abs) {
            struct FaultEvent evt = {
                .workload_id_hash     = meta->workload_id_hash,
                .cpu                  = bpf_get_smp_processor_id(),
                .expected_deadline_ns = deadline_abs,
                .actual_completion_ns = now,
                .fault_type           = FAULT_DMISS,
            };
            bpf_ringbuf_output(&fault_ringbuf, &evt, sizeof(evt), 0);
        }
    }

    if (meta->scheduling_type == SCHED_TYPE_CBS) {
        // L2 CBS: 예산 차감
        struct CbsState *cbs = bpf_map_lookup_elem(&cbs_map,
                                                    &meta->workload_id_hash);
        if (cbs) {
            u64 exec_ns = now - cbs->exec_start_ns;
            u32 exec_us = exec_ns / 1000;
            if (cbs->remaining_us > exec_us)
                cbs->remaining_us -= exec_us;
            else {
                cbs->remaining_us = 0;
                // 예산 소진 → bpf_timer로 Ts 후 보충 예약
                bpf_timer_start(&cbs->replenish_timer, cbs->period_us * 1000,
                                BPF_F_TIMER_ABS);
                // throttled_queue로 이동 (다음 enqueue에서 처리)
            }
        }
    }
}
```

---



## 열린 설계 질문

### Q1. cgroup_id를 BPF에서 어떻게 얻는가? ✅ 해결

```
→ **결정: Option B**
  TIMPANI-N daemon이 컨테이너 시작 시점에
  pid를 task_meta_map에 등록 (cgroup_id, scheduling_type, L1~L4 분류 포함)

  구현 시점: DDR-006 C++ rework의 BPF Loader 컴포넌트가 담당.
  DDR-006 NodeReady에서 보고된 topology 기반으로 partition_map도 동시 초기화.

참고 — 검토된 대안:
  Option A: bpf_task_cgroup_id() — 존재하지 않음 (현재 BPF API 없음)
  Option C: BPF iterator로 cgroup 계층 순회 (복잡, 불채택)
```

### Q2. TT_WAIT_DSQ에서 올바른 task 선택

```
문제: TT_WAIT_DSQ에 여러 TT task thread가 대기 중일 때
      ops.dispatch()에서 scx_bpf_consume(TT_WAIT_DSQ)는
      DSQ 맨 앞 task를 꺼냄 — 슬롯에 맞는 task thread인지 보장 못함

해결 방안:
  Option A: task thread별 전용 DSQ 사용
            DSQ ID = task_id_hash → 해당 DSQ에서 consume
  Option B: task_meta_map에서 pid 직접 조회 후
            scx_bpf_dispatch_vtime()으로 특정 task 우선 발화

→ Option A가 BPF 구현상 더 단순:
  각 task thread별로 전용 DSQ 생성 (최대 L1/L2 task 수만큼)
  → 슬롯 활성화 시 해당 task의 DSQ만 consume
```

### Q3. PREEMPT_RT + sched_ext 호환성

```
PREEMPT_RT 패치와 sched_ext의 조합:
  Linux 6.12: sched_ext 정식 포함
  PREEMPT_RT: 6.12 기준 mainline merge 진행 중 (일부 arch)

  aarch64 (차량 ECU 주력): PREEMPT_RT 6.12 패치 존재
  → Yocto meta-realtime 레이어에서 적용 가능

  검증 필요: sched_ext + PREEMPT_RT 조합의 실제 jitter 측정
  → Antigravity PoC Task #1로 위임
```

---

## Antigravity PoC Task #1 — 위임 준비

이 DDR에서 설계 논의가 마무리되면 다음 PoC를 Antigravity에 위임:

```
ANTIGRAVITY TASK: sched_ext + PREEMPT_RT Jitter 측정 PoC

Context:
  TIMPANI-N은 sched_ext 기반 BPF 스케줄러 + PREEMPT_RT 커널을 사용
  TT 슬롯 정밀도 요건: 수십 μs

Goal:
  scx_minimal 기반 최소 BPF 스케줄러 구현
  Timer Master 패턴 (SCHED_FIFO + clock_nanosleep ABSTIME) 구현
  두 가지 타이밍 측정:
    1. Timer Master wakeup jitter (clock_nanosleep 정밀도)
    2. BPF ops.dispatch() 호출 latency (kick_cpu → 실제 실행)
  PREEMPT_RT 유무에 따른 비교 측정

Reference:
  timpani-n/src/core.c (기존 C 구현 — 런타임 루프 참조)
  timpani-n/src/sched.c (스케줄링 API 래퍼)
  scx/scx_minimal.bpf.c (Linux kernel tools/sched_ext)

Output:
  최소 PoC 코드 (C userspace + BPF C)
  jitter 측정 결과 리포트 (표 + 분석)
  PREEMPT_RT 적용/미적용 비교
```

---

## User App Interface

### Task Thread 식별

TIMPANI-N은 **cgroup + thread name** 조합으로 task thread를 식별한다.

| 식별 단계 | 방법 | 자동/수동 |
|:--|:--|:--|
| 워크로드 식별 | cgroup_id (컨테이너 시작 시 자동 생성) | 자동 |
| task thread 식별 | `task_struct->comm` (thread name) | 앱이 설정 |

**앱 제약**: 각 task thread는 `pthread_setname_np()` 또는 `prctl(PR_SET_NAME)`으로 thread name을 설정해야 한다. 이 이름은 `WorkloadSpec.task_specs[].task_id`와 일치해야 한다.

```c
// 앱 코드 예시 — 유일한 필수 제약
pthread_setname_np(pthread_self(), "sensor_read");  // task_id와 일치
```

이는 TIMPANI 고유의 제약이 아니라 RT 앱 개발의 일반적인 best practice(디버깅, Perfetto tracing, 로깅)를 필수화한 것이다.

**식별 동작**:

```
① Pullpiri: container start → cgroup 생성 (자동)
② 앱: 각 task thread에서 pthread_setname_np(task_id) (1줄)
③ Timpani-N: cgroup 내 새 thread 출현 감지
   → /proc/<pid>/task/<tid>/comm 읽기
   → WorkloadSpec.task_specs[].task_id와 매칭
   → task_meta_map[tid] = { workload_id, task_id_hash, layer, ... } 등록
④ BPF: 등록된 tid에 대해 스케줄링 정책 적용
```

### 재시작 시 재식별

Container stop → start 시 pid/tid가 변경된다. 이름 기반 식별이므로 재시작에 안전하다:

```
① Container Stop:
   Timpani-N: cgroup 삭제 이벤트 감지
   → 해당 워크로드의 모든 task_meta_map 엔트리 무효화
   → 해당 TT 슬롯은 "task 없음" 상태 → dispatch skip + missing 이벤트 보고

② Container Start (새 pid):
   Timpani-N: 새 cgroup + 새 thread 출현 감지
   → comm 읽기 → 동일한 task_id로 재매칭
   → task_meta_map에 새 tid 등록
   → 다음 슬롯부터 새 task dispatch 재개
```

### 주기적 실행 동기화: ttsched_wait_next_period()

**L1/L2 모두 필수 사용.**

sched_ext가 커널 수준에서 TT 슬롯 타이밍에 맞춰 task를 dispatch하지만, 앱이 자체 timer/sleep으로 주기를 관리하면 앱의 wakeup 시점과 TT 슬롯 시작 시점 사이에 위상차가 발생하여 jitter가 증가한다.

```
문제: 앱이 자체 sleep 사용 시

  TT 슬롯:     |──────|         |──────|
  앱의 sleep:        |──sleep──|──work──|──sleep──|
                                ↑
                  앱이 자기 타이밍에 깨어남
                  → 슬롯 시작과 불일치 → jitter 수십~수백 μs 추가
```

`ttsched_wait_next_period()`는 TIMPANI가 제공하는 경량 동기화 API로, 앱의 main loop를 TT 슬롯 시작 시점에 정확히 동기화한다:

```c
// libttsched.h — TIMPANI 제공 경량 라이브러리
#include <libttsched.h>

void* task_thread(void* arg) {
    pthread_setname_np(pthread_self(), "sensor_read");  // 필수: task 식별
    ttsched_init();                                      // 초기화

    while (1) {
        ttsched_wait_next_period();  // TT 슬롯 시작까지 대기 (필수)
        do_periodic_work();           // 주기 작업 수행
    }
}
```

**구현 메커니즘** (후보):

| 방법 | 설명 | 지연 |
|:--|:--|:--|
| futex (권장) | Timpani-N Timer Master가 슬롯 시작 시 futex_wake() | ~1μs |
| eventfd | Timer Master가 eventfd_write() → 앱이 read() 대기 | ~2μs |
| shared memory flag + busy poll | 앱이 공유 메모리 플래그를 polling | <1μs (CPU 소모) |

> 구현 방식은 DDR-005 범위 외. 별도 상세 설계에서 결정.

### L1~L4별 앱 제약 요약

| L1~L4 | pthread_setname_np | ttsched_wait_next_period() | sigwait | 비고 |
|:--|:--|:--|:--|:--|
| **L1** | **필수** | **필수** | 불필요 | μs 정밀 동기화 |
| **L2** | **필수** | **필수** | 불필요 | 미사용 시 jitter 증가, 예산 낭비 |
| **L3/L4** | 불필요 | 불필요 | 불필요 | TIMPANI 관할 외 |

> **기존 timpani 25 대비 변경**: `sigwait()` 기반 main loop 강제가 **완전 제거**됨. 앱의 main loop 구조는 자유이며, `ttsched_wait_next_period()` 한 줄로 대체.

### 기존 timpani 25 → 26 앱 마이그레이션

```c
// 기존 timpani 25
prctl(PR_SET_NAME, "sensor_read");
sigset_t set;
sigemptyset(&set);
sigaddset(&set, SIGRTMIN);
while (1) {
    sigwait(&set, &sig);       // ← 제거
    do_periodic_work();
}

// 새 timpani 26
pthread_setname_np(pthread_self(), "sensor_read");  // 유지
ttsched_init();
while (1) {
    ttsched_wait_next_period();  // ← 대체 (sigwait → ttsched)
    do_periodic_work();
}
```

마이그레이션 비용: **2줄 변경** (`sigwait` → `ttsched_wait_next_period`, signal 설정 코드 제거).

---

## 영향받는 컴포넌트

| 파일 | 변경 내용 |
|------|---------|
| `timpani-n/src/core.c` | Timer Master 루프 구현 (기존 루프를 sched_ext 기반으로 재설계) |
| `timpani-n/src/bpf/` | 신규 — TIMPANI sched_ext BPF 스케줄러 디렉토리 |
| `timpani-n/src/bpf/timpani.bpf.c` | 신규 — sched_ext ops 구현 |
| `timpani-n/src/bpf/maps.h` | 신규 — BPF map 정의 |
| `timpani-n/src/task_registry.c` | 신규 — cgroup 감시 + thread name 매칭 → task_meta_map 등록 |
| `timpani-n/src/trace_bpf.c` | fault_ringbuf polling 통합 |
| `sample-apps/src/libttsched.h` | 신규 — ttsched_wait_next_period() 경량 라이브러리 |
