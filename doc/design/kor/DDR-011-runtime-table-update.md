<!--
SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
SPDX-License-Identifier: MIT
-->

# DDR-011: 무중단 Schedule Table 갱신

**작성일:** 2026-06-15
**상태:** Draft
**작성자:** Human (Lead Architect) + AI
**관련 문서:** DDR-005 (BPF 스케줄러), DDR-006 (통신 아키텍처)

---

## 1. 개요

본 문서는 TIMPANI의 **무중단 Schedule Table 갱신** 메커니즘을 기술한다. 목표는 어떠한 데몬도 재시작하지 않고, 다른 실행 중인 워크로드에 영향을 주지 않으면서 워크로드를 추가, 제거, 수정하는 것이다.

### 설계 목표

| 목표 | 설명 |
|:--|:--|
| **무중단** | 프로세스 재시작 없이 워크로드 변경 적용 |
| **결정론** | Hyperperiod 경계에서만 테이블 교체 |
| **격리** | 한 워크로드의 변경이 다른 워크로드에 영향 없음 (FFI) |
| **원자성** | 테이블 교체는 단일 원자적 연산 |

---

## 2. Double Buffering 아키텍처

### 2.1 개념

```
┌─────────────────────────────────────────────────────────────┐
│  BPF Map Double Buffer 구조                                 │
│                                                             │
│  ┌──────────────────┐    ┌──────────────────┐              │
│  │  tt_table_map[0] │    │  tt_table_map[1] │              │
│  │  (Shadow)        │    │  (Active)        │              │
│  └──────────────────┘    └──────────────────┘              │
│           ↑                       ↑                        │
│      다음 테이블 준비          현재 실행 중                  │
│                                                             │
│  active_map_idx = 1  (현재 Active는 [1])                   │
│                                                             │
│  Hyperperiod 경계에서:                                      │
│    active_map_idx = 0  (Atomic swap → [0]이 Active가 됨)    │
└─────────────────────────────────────────────────────────────┘
```

### 2.2 BPF Map 정의

```c
/* B-2.1: Shadow map 인덱스 관리 */
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, u32);
    __type(value, u32);  // 0 또는 1
} active_map_idx SEC(".maps");

/* Double buffer: 2개의 tt_table_map */
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, MAX_TT_SLOTS);
    __type(key, struct TtSlotKey);
    __type(value, struct TtSlotBpf);
} tt_table_map_0 SEC(".maps");  // Shadow 또는 Active

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, MAX_TT_SLOTS);
    __type(key, struct TtSlotKey);
    __type(value, struct TtSlotBpf);
} tt_table_map_1 SEC(".maps");  // Active 또는 Shadow

/* 지원 자료구조 */
struct TtSlotKey {
    u32 cpu;
    u32 slot_idx;
};

struct TtSlotBpf {
    u64 workload_id_hash;
    u64 task_id_hash;
    u32 offset_us;
    u32 duration_us;
    u32 deadline_us;
    u32 cpu;
};
```

### 2.3 ops.dispatch() 구현

```c
void BPF_STRUCT_OPS(timpani_dispatch, s32 cpu, struct task_struct *prev)
{
    /* active map index 읽기 (atomic read) */
    u32 key = 0;
    u32 *active = bpf_map_lookup_elem(&active_map_idx, &key);
    u32 idx = active ? *active : 0;

    /* active table에서 현재 slot 조회 */
    u32 *slot_idx = bpf_map_lookup_elem(&current_slot_map, &cpu);
    if (!slot_idx || *slot_idx == SLOT_NONE)
        goto fallback;

    struct TtSlotKey slot_key = { .cpu = cpu, .slot_idx = *slot_idx };
    struct TtSlotBpf *slot;

    /* 인덱스에 따라 active map 참조 */
    if (idx == 0)
        slot = bpf_map_lookup_elem(&tt_table_map_0, &slot_key);
    else
        slot = bpf_map_lookup_elem(&tt_table_map_1, &slot_key);

    if (slot) {
        /* L1 TT 태스크를 대기 큐에서 dispatch */
        scx_bpf_consume(slot->task_id_hash);
        return;
    }

fallback:
    /* L2 CBS → L3/L4 BE */
    if (scx_bpf_consume(DSQ_CBS))
        return;
    scx_bpf_consume(DSQ_BE);
}
```

---

## 3. 갱신 시나리오

### 3.1 워크로드 추가 (독립적)

**트리거:** Pullpiri가 `RegisterWorkload` 요청 전송

```
시간축 →
                    Hyperperiod = 100ms
├────────────────────────────────────────────────────────────┤
│  Slot 0      │  Slot 1      │  Slot 2      │  Slot 3      │
│  Task A      │  Task B      │  (empty)     │  Task A      │
│  0~25ms      │  25~50ms     │  50~75ms     │  75~100ms    │
├────────────────────────────────────────────────────────────┤
                     ↑
              현재 실행 중 (t = 30ms)

Pullpiri → RegisterWorkload(Task C, L2 Sporadic)

┌─────────────────────────────────────────────────────────────┐
│  timpani-n 내부 처리                                        │
│                                                             │
│  Step 1: timpani-o 요청 수신, 테이블 재계산                 │
│  Step 2: timpani-o가 timpani-n에 ScheduleTableUpdate 전송   │
│  Step 3: timpani-n이 새 테이블을 Shadow map에 로드          │
│                                                             │
│  [Active Map: tt_table_map_1] ← ops.dispatch()가 참조       │
│    Slot 0: Task A                                           │
│    Slot 1: Task B                                           │
│    Slot 2: (empty)                                          │
│    Slot 3: Task A                                           │
│                                                             │
│  [Shadow Map: tt_table_map_0] ← 백그라운드 업데이트         │
│    Slot 0: Task A                                           │
│    Slot 1: Task B                                           │
│    Slot 2: Task C (NEW!)  ← CBS slot 추가                   │
│    Slot 3: Task A                                           │
│                                                             │
│  Step 4: shadow_ready = true 설정                           │
└─────────────────────────────────────────────────────────────┘

                    Hyperperiod 경계 (t = 100ms)
                              ↓
┌─────────────────────────────────────────────────────────────┐
│  Atomic Swap                                                │
│                                                             │
│  active_map_idx = 0  ← 단일 u32 write (atomic)              │
│                                                             │
│  → ops.dispatch()는 이제 tt_table_map_0을 참조              │
│  → Task C가 Slot 2에서 실행 가능                            │
│  → Task A, B는 중단 없이 계속 실행                          │
└─────────────────────────────────────────────────────────────┘
```

### 3.2 워크로드 제거 (STOP)

**트리거:** Pullpiri가 `RemoveWorkload` 요청 전송 (FaultAction = STOP)

```
현재 상태:
├────────────────────────────────────────────────────────────┤
│  Slot 0      │  Slot 1      │  Slot 2      │  Slot 3      │
│  Task A      │  Task B      │  Task C      │  Task A      │
├────────────────────────────────────────────────────────────┤

Pullpiri → RemoveWorkload(Task B)

┌─────────────────────────────────────────────────────────────┐
│  Shadow Map 준비                                            │
│                                                             │
│  [Active: tt_table_map_0]                                   │
│    Slot 0: Task A                                           │
│    Slot 1: Task B  ← 아직 실행 중                           │
│    Slot 2: Task C                                           │
│    Slot 3: Task A                                           │
│                                                             │
│  [Shadow: tt_table_map_1] ← Task B 제거된 테이블 준비       │
│    Slot 0: Task A                                           │
│    Slot 1: (empty)  ← Task B 제거                          │
│    Slot 2: Task C                                           │
│    Slot 3: Task A                                           │
└─────────────────────────────────────────────────────────────┘

                    Hyperperiod 경계
                         ↓
┌─────────────────────────────────────────────────────────────┐
│  Atomic Swap + BPF map cleanup                              │
│                                                             │
│  1. active_map_idx = 1                                      │
│  2. task_meta_map에서 Task B의 pid 항목 삭제                │
│  3. cbs_budget_map에서 Task B의 budget 항목 삭제 (L2인 경우)│
│                                                             │
│  → Task B는 더 이상 dispatch되지 않음                       │
│  → Task A, C는 중단 없이 계속 실행                          │
└─────────────────────────────────────────────────────────────┘
```

### 3.3 전체 테이블 교체 (모드 전환)

**트리거:** 주행 모드 전환 (예: Parking → Highway)

```
[Parking Mode 테이블]
├──────────────────────────────────────────────────────────────┤
│  Slot 0       │  Slot 1       │  Slot 2       │  Slot 3     │
│  Parking-Cam  │  Ultrasonic   │  Parking-AI   │  Display    │
│  Period: 50ms │  Period: 25ms │  Period: 50ms │  Period: 50ms│
├──────────────────────────────────────────────────────────────┤
Hyperperiod = 50ms

모드 전환 요청 → 전체 테이블 교체 필요

[Highway Mode 테이블] ← Shadow map에 준비
├──────────────────────────────────────────────────────────────┤
│  Slot 0       │  Slot 1       │  Slot 2       │  Slot 3     │
│  Front-Cam    │  Radar-Fusion │  ADAS-AI      │  Dashboard  │
│  Period: 20ms │  Period: 10ms │  Period: 20ms │  Period: 33ms│
├──────────────────────────────────────────────────────────────┤
Hyperperiod = 660ms (LCM 재계산)

┌─────────────────────────────────────────────────────────────┐
│  전환 과정                                                   │
│                                                             │
│  1. timpani-o: 새 테이블 생성 + feasibility 검증            │
│  2. timpani-n: Shadow map에 Highway 테이블 로드             │
│  3. Timer Master: 현재 hyperperiod 완료 대기 (최대 50ms)    │
│  4. Hyperperiod 경계에서:                                    │
│     - active_map_idx swap (atomic)                          │
│     - 새 hyperperiod(660ms) 기준으로 Timer Master 재설정    │
│  5. 새 워크로드들 즉시 실행 시작                             │
│                                                             │
│  ★ 전환 중 jitter: 0 (hyperperiod 경계에서만 swap)          │
│  ★ 기존 워크로드 중단 시간: 0ms                             │
└─────────────────────────────────────────────────────────────┘
```

---

## 4. Timer Master 구현

### 4.1 유저스페이스 스레드 (C++)

```cpp
class TimerMaster {
public:
    void run() {
        while (running_) {
            // 다음 hyperperiod 경계까지 대기
            uint64_t next_boundary = epoch_ns_ +
                ((current_hp_ + 1) * hyperperiod_ns_);

            struct timespec ts = ns_to_timespec(next_boundary);
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);

            // Shadow map 준비 완료 확인
            if (shadow_ready_.load()) {
                performAtomicSwap();
            }

            // 다음 hyperperiod로 진행
            current_hp_++;

            // 이 hyperperiod의 TT slot들 실행
            dispatchTtSlots();
        }
    }

private:
    void performAtomicSwap() {
        // Atomic swap: 단일 u32 write
        uint32_t key = 0;
        uint32_t new_idx = 1 - current_active_idx_;

        bpf_map_update_elem(active_map_idx_fd_, &key, &new_idx, BPF_ANY);

        current_active_idx_ = new_idx;
        shadow_ready_.store(false);

        // 새 hyperperiod 적용 (변경된 경우)
        if (new_hyperperiod_ns_ != hyperperiod_ns_) {
            hyperperiod_ns_ = new_hyperperiod_ns_;
            LOG_INFO("Hyperperiod가 {} ns로 업데이트됨", hyperperiod_ns_);
        }

        LOG_INFO("Schedule table이 hyperperiod {}에서 swap됨", current_hp_);
    }

    void dispatchTtSlots() {
        // 현재 hyperperiod의 TT slot들을 순회
        for (const auto& slot : tt_slots_) {
            uint64_t fire_time = epoch_ns_ +
                (current_hp_ * hyperperiod_ns_) +
                (slot.offset_us * 1000);

            struct timespec ts = ns_to_timespec(fire_time);
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);

            // current_slot_map 업데이트 및 CPU kick
            updateCurrentSlot(slot.cpu, slot.slot_idx);
            kickCpu(slot.cpu);
        }
    }

    std::atomic<bool> shadow_ready_{false};
    uint32_t current_active_idx_ = 0;
    uint64_t hyperperiod_ns_;
    uint64_t new_hyperperiod_ns_;
    uint64_t current_hp_ = 0;
    int active_map_idx_fd_;
};
```

### 4.2 Shadow Map 업데이트 (gRPC Handler)

```cpp
Status GrpcHandler::HandleScheduleTableUpdate(
    const ScheduleTableUpdate& update) {

    // Shadow(비활성) map 결정
    uint32_t shadow_idx = 1 - timer_master_->currentActiveIdx();
    int shadow_fd = (shadow_idx == 0) ?
        tt_table_map_0_fd_ : tt_table_map_1_fd_;

    // Shadow map 초기화
    clearBpfMap(shadow_fd);

    // 새 테이블을 shadow map에 로드
    for (const auto& slot : update.tt_slots()) {
        struct TtSlotKey key = {
            .cpu = slot.cpu(),
            .slot_idx = slot.slot_idx()
        };
        struct TtSlotBpf value = {
            .workload_id_hash = hashWorkloadId(slot.workload_id()),
            .task_id_hash = hashTaskId(slot.task_id()),
            .offset_us = slot.offset_us(),
            .duration_us = slot.duration_us(),
            .deadline_us = slot.deadline_us(),
            .cpu = slot.cpu()
        };
        bpf_map_update_elem(shadow_fd, &key, &value, BPF_ANY);
    }

    // Hyperperiod가 변경된 경우 업데이트
    if (update.hyperperiod_us() != timer_master_->hyperperiodUs()) {
        timer_master_->setNewHyperperiod(update.hyperperiod_us() * 1000);
    }

    // Shadow map 준비 완료 시그널
    timer_master_->setShadowReady(true);

    return Status::OK;
}
```

---

## 5. STOP 시 BPF Map Cleanup

워크로드가 정지될 때 다음 BPF map들을 정리해야 한다:

| Map | Cleanup 동작 |
|:--|:--|
| `tt_table_map` | 해당 워크로드의 slot 제거 (shadow swap 통해) |
| `task_meta_map` | pid → TaskMeta 항목 삭제 |
| `cbs_budget_map` | workload → CbsState 항목 삭제 (L2만) |
| `partition_map` | cgroup_id → PartitionInfo 항목 삭제 |

```cpp
void BpfLoader::cleanupWorkload(const std::string& workload_id) {
    uint64_t wid_hash = hashWorkloadId(workload_id);

    // 1. task_meta 항목 찾아서 삭제
    for (auto& [pid, meta] : iterateMap(task_meta_map_fd_)) {
        if (meta.workload_id_hash == wid_hash) {
            bpf_map_delete_elem(task_meta_map_fd_, &pid);
        }
    }

    // 2. CBS budget 항목 삭제 (L2)
    bpf_map_delete_elem(cbs_budget_map_fd_, &wid_hash);

    // 3. partition 항목 삭제
    uint64_t cgroup_id = workload_cgroup_map_[workload_id];
    bpf_map_delete_elem(partition_map_fd_, &cgroup_id);

    LOG_INFO("워크로드 {}의 BPF map 정리 완료", workload_id);
}
```

---

## 6. 타이밍 보장

### 6.1 최악 지연 시간

| 연산 | 지연 시간 | 비고 |
|:--|:--|:--|
| Shadow map 업데이트 | O(n) slots | 백그라운드, 비차단 |
| Atomic swap | < 1 μs | 단일 u32 write |
| 테이블 활성화 지연 | 0 ~ 1 hyperperiod | 경계 대기 |

### 6.2 최대 전환 지연

```
최대 지연 = 현재 Hyperperiod 길이

예시:
- Hyperperiod = 100ms
- 요청 도착 시점 t = 10ms
- Swap 발생 시점 t = 100ms (다음 경계)
- 최대 지연 = 90ms
```

### 6.3 왜 Hyperperiod 경계인가?

임의 시점에서 swap하면 다음 문제 발생:
1. **부분 slot 실행**: 태스크가 slot 중간에 선점될 수 있음
2. **데드라인 miss**: 새 데드라인이 이미 지났을 수 있음
3. **Budget 불일치**: CBS budget이 period와 정렬되지 않음

Hyperperiod 경계에서 swap하면:
- 모든 현재 slot이 자연스럽게 완료
- 새 테이블이 정렬된 budget으로 시작
- 결정론적 동작 유지

---

## 7. 비교: 재시작 vs 무중단

| 시나리오 | 재시작 방식 | 무중단 방식 |
|:--|:--|:--|
| 워크로드 추가 | 전체 데몬 재시작 (~수초) | Hyperperiod 경계 swap (~ms) |
| 모드 전환 | 시스템 재부팅 (~수십초) | Atomic swap (< 1 μs) |
| 장애 워크로드 제거 | 다른 워크로드도 영향 | 대상 워크로드만 격리 제거 |
| SDV 안전 영향 | 재시작 중 차량 제어 불능 | 연속 운영 |

---

## 8. 미결 항목

- [ ] **B-2.1**: `active_map_idx` BPF map 구현 (Phase 2)
- [ ] **B-3.1**: BPF atomic swap 가능성 검증
- [ ] 타겟 하드웨어에서 실제 swap 지연 시간 측정
- [ ] 허용 가능한 전환 지연을 위한 최대 hyperperiod 정의

---

## 9. 참조

- DDR-005: sched_ext BPF 스케줄러 설계
- DDR-006: 통신 아키텍처
- DDR-007: TT + CBS 통합 스케줄링
- Linux sched_ext 문서: `tools/sched_ext/`
