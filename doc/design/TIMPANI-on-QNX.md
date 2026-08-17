<!--
SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
SPDX-License-Identifier: MIT
-->

# TIMPANI on QNX: 기술 검토 문서

**날짜:** 2026-04-16
**상태:** Draft
**작성:** Copilot (GitHub Copilot)

---

## 1. 개요

### 1.1 목적

본 문서는 Linux sched_ext/eBPF 기반으로 구현된 TIMPANI를 QNX Neutrino RTOS로 포팅하기 위한 기술 검토를 제공한다.

### 1.2 배경

TIMPANI는 현재 다음 Linux 기술에 의존:
- **sched_ext**: 커널 스케줄러 확장 (BPF 기반)
- **eBPF**: 커널 내 실행 코드 (스케줄링 결정, fault 감지)
- **cgroup v2**: 프로세스 그룹화 및 자원 격리
- **gRPC**: 컴포넌트 간 통신
- **clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME)**: 정밀 타이밍

QNX는 이러한 기술을 직접 지원하지 않으므로, 동등한 기능을 제공하는 네이티브 메커니즘으로 대체해야 한다.

---

## 2. QNX Neutrino 개요

### 2.1 아키텍처 특성

| 특성 | Linux | QNX Neutrino |
|:--|:--|:--|
| **커널 구조** | Monolithic | Microkernel |
| **스케줄러** | CFS, SCHED_FIFO/RR, DEADLINE, sched_ext | 우선순위 기반 선점형 + APS |
| **IPC** | Unix socket, pipe, System V, D-Bus | Message passing (native), Pulse |
| **프로세스 격리** | cgroup, namespace | Adaptive Partitions |
| **실시간 보장** | PREEMPT_RT 패치 필요 | 네이티브 RT (POSIX 1003.1b) |
| **인증** | 없음 | ISO 26262, IEC 61508 인증 가능 |

### 2.2 QNX Scheduling Model

```
QNX 스케줄링 계층:

        ┌─────────────────────────────────────┐
        │     Adaptive Partitioning (APS)     │  ← 파티션 간 CPU 예산 분배
        └─────────────────────────────────────┘
                        │
        ┌───────────────┼───────────────┐
        ▼               ▼               ▼
   ┌─────────┐    ┌─────────┐    ┌─────────┐
   │ Part A  │    │ Part B  │    │ Part C  │
   │ 40% CPU │    │ 30% CPU │    │ 30% CPU │
   └─────────┘    └─────────┘    └─────────┘
        │               │               │
   ┌────┼────┐    ┌────┼────┐    ┌────┼────┐
   │ FIFO/RR │    │ FIFO/RR │    │ FIFO/RR │  ← 파티션 내 우선순위 스케줄링
   │ 0-255   │    │ 0-255   │    │ 0-255   │
   └─────────┘    └─────────┘    └─────────┘
```

### 2.3 QNX Adaptive Partitioning Scheduler (APS)

- **CPU 예산 할당**: 각 파티션에 % 단위로 CPU 시간 보장
- **계층 구조**: 파티션 > 쓰레드 우선순위
- **동적 재구성**: 런타임에 파티션 추가/제거/예산 변경 가능
- **Critical Time**: Safety-critical 쓰레드의 예산 보장 강화

```c
// QNX APS API 예시
#include <sys/sched_aps.h>

// 파티션 생성
sched_aps_create_parms parms = {
    .budget_percent = 40,
    .critical_budget_ms = 10,
    .name = "L1_Safety"
};
int partition_id = SchedCtl(SCHED_APS_CREATE_PARTITION, &parms, sizeof(parms));

// 쓰레드를 파티션에 할당
sched_aps_join_parms join = {
    .id = partition_id,
    .pid = target_pid,
    .tid = target_tid
};
SchedCtl(SCHED_APS_JOIN_PARTITION, &join, sizeof(join));
```

---

## 3. TIMPANI 컴포넌트별 QNX 매핑

### 3.1 sched_ext BPF Scheduler → QNX APS + SCHED_FIFO

| Linux (sched_ext) | QNX 대응 | 비고 |
|:--|:--|:--|
| `ops.select_cpu()` | APS 파티션 + CPU affinity | `ThreadCtl(_NTO_TCTL_RUNMASK, ...)` |
| `ops.enqueue()` | 쓰레드 우선순위 설정 | `sched_setparam()`, `setprio()` |
| `ops.dispatch()` | QNX 커널 자동 | APS가 파티션 예산 내에서 우선순위 기반 디스패치 |
| `ops.running/stopping()` | Resource Manager | 커스텀 RM으로 실행 시간 추적 |
| CBS 예산 강제 | APS `budget_percent` | 파티션 단위 예산 강제 |
| TT 슬롯 | Timer + Pulse | Timer 만료 시 Pulse로 쓰레드 깨움 |

**핵심 차이:**
- Linux sched_ext: BPF 코드가 커널 내에서 스케줄링 결정
- QNX: APS + SCHED_FIFO가 스케줄링, userspace에서 제어

### 3.2 eBPF → QNX Resource Manager + Pulse

eBPF는 QNX에 없으므로 대안 필요:

| eBPF 기능 | QNX 대응 방안 |
|:--|:--|
| **스케줄링 결정** | APS API (`SchedCtl`) |
| **실행 시간 측정** | `ClockCycles()`, `pthread_getcpuclockid()` |
| **Fault 감지** | Resource Manager로 모니터링 |
| **BPF Map** | 공유 메모리 (`shm_open`, `mmap`) |
| **Ring Buffer** | QNX Pulse 또는 `MsgSend/Receive` |

#### 3.2.1 Resource Manager 기반 모니터링

```c
// QNX Resource Manager 패턴
// timpani-n이 자체 /dev/timpani 리소스 제공

#include <sys/iofunc.h>
#include <sys/dispatch.h>

int main() {
    dispatch_t *dpp = dispatch_create();
    resmgr_attr_t rattr;
    
    // /dev/timpani 리소스 생성
    resmgr_attach(dpp, &rattr, "/dev/timpani", ...);
    
    // 워크로드에서 /dev/timpani에 devctl()로 상태 보고
    // timpani-n이 모니터링 및 fault 감지
}
```

### 3.3 cgroup v2 → QNX Adaptive Partitions

| Linux cgroup | QNX Adaptive Partition |
|:--|:--|
| `cpuset.cpus` | `ThreadCtl(_NTO_TCTL_RUNMASK, ...)` |
| `cpuset.cpus.partition=isolated` | APS 파티션 + CPU affinity |
| `cpu.max` (CFS bandwidth) | APS `budget_percent` |
| cgroup hierarchy | APS 파티션 계층 (부모-자식) |

**L1~L4 매핑:**

```
QNX APS 파티션 구성:

System Partition (10%)
│
├── L1_Safety (30%, critical_budget=10ms)
│   └── ASIL-D 워크로드
│
├── L2_Budget (25%)
│   └── RT 워크로드 (CBS 스타일)
│
├── L3_BestEffort (20%)
│   └── HMI, V2X
│
└── L4_Background (15%)
    └── 로깅, OTA
```

### 3.4 gRPC → QNX IPC 대안

| 옵션 | 장점 | 단점 | 권장 |
|:--|:--|:--|:--|
| **gRPC (포팅)** | Linux 코드 재사용 | 포팅 노력, 의존성 | 멀티노드 시 |
| **QNX MsgSend/Receive** | 네이티브, 고성능 | Linux 호환 없음 | 단일노드 |
| **POSIX MQ** | 이식성 | 성능 한계 | 간단한 경우 |
| **공유 메모리 + Pulse** | 고성능, 저지연 | 구현 복잡 | RT 경로 |

**권장 전략:**
- **timpani-o ↔ timpani-n (단일노드)**: QNX MsgSend/Receive + 공유 메모리
- **멀티노드 분산**: gRPC 포팅 또는 QNX Qnet

#### 3.4.1 QNX Message Passing 예시

```c
// timpani-o (서버)
#include <sys/neutrino.h>
#include <sys/dispatch.h>

name_attach_t *attach = name_attach(NULL, "timpani-o", 0);
int rcvid = MsgReceive(attach->chid, &msg, sizeof(msg), NULL);
// 테이블 생성 후 응답
MsgReply(rcvid, EOK, &table, sizeof(table));

// timpani-n (클라이언트)
int coid = name_open("timpani-o", 0);
MsgSend(coid, &request, sizeof(request), &table, sizeof(table));
```

### 3.5 Timer Master → QNX Timer API

| Linux | QNX |
|:--|:--|
| `clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME)` | `TimerTimeout()` + `CLOCK_REALTIME` |
| SCHED_FIFO + dedicated CPU | SCHED_FIFO + `ThreadCtl(_NTO_TCTL_RUNMASK)` |
| `scx_bpf_kick_cpu()` | Pulse 전송 (`MsgSendPulse`) |

#### 3.5.1 QNX Timer Master 구현

```c
#include <sys/neutrino.h>
#include <time.h>

void timer_master_loop() {
    struct sigevent event;
    timer_t timer_id;
    
    // Pulse 기반 타이머 이벤트
    int chid = ChannelCreate(0);
    SIGEV_PULSE_INIT(&event, ConnectAttach(0, 0, chid, 0, 0), 
                     TIMER_PULSE_CODE, 0);
    
    timer_create(CLOCK_REALTIME, &event, &timer_id);
    
    while (!shutdown) {
        struct itimerspec itime = {
            .it_value = next_slot_time,
            .it_interval = {0, 0}
        };
        timer_settime(timer_id, TIMER_ABSTIME, &itime, NULL);
        
        // Pulse 대기
        struct _pulse pulse;
        MsgReceivePulse(chid, &pulse, sizeof(pulse), NULL);
        
        // TT 슬롯 발화: 대상 쓰레드에 Pulse 전송
        MsgSendPulse(worker_coid, -1, WAKEUP_PULSE_CODE, slot_id);
    }
}
```

---

## 4. QNX TIMPANI 아키텍처 설계

### 4.1 전체 구조

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                           QNX TIMPANI Architecture                          │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│   ┌─────────────────────────────────────────────────────────────────────┐   │
│   │                         timpani-o (QNX Process)                     │   │
│   │                                                                     │   │
│   │  ┌───────────────┐  ┌───────────────┐  ┌───────────────────────┐   │   │
│   │  │ SchedInfo     │  │ Table Builder │  │ QNX MsgSend Server    │   │   │
│   │  │ Service       │  │               │  │ (name: "timpani-o")   │   │   │
│   │  └───────────────┘  └───────────────┘  └───────────────────────┘   │   │
│   └─────────────────────────────────────────────────────────────────────┘   │
│                               │ MsgSend/Receive                             │
│                               ▼                                             │
│   ┌─────────────────────────────────────────────────────────────────────┐   │
│   │                         timpani-n (QNX Process)                     │   │
│   │                                                                     │   │
│   │  ┌───────────────┐  ┌───────────────┐  ┌───────────────────────┐   │   │
│   │  │ Timer Master  │  │ APS Manager   │  │ Fault Monitor         │   │   │
│   │  │ (SCHED_FIFO)  │  │ (SchedCtl)    │  │ (Resource Manager)    │   │   │
│   │  └───────────────┘  └───────────────┘  └───────────────────────┘   │   │
│   │         │                   │                     │                │   │
│   │         │ Pulse             │ SchedCtl            │ /dev/timpani   │   │
│   │         ▼                   ▼                     ▼                │   │
│   │  ┌─────────────────────────────────────────────────────────────┐   │   │
│   │  │           QNX Kernel (Microkernel + APS)                    │   │   │
│   │  │                                                             │   │   │
│   │  │  ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌─────────────┐    │   │   │
│   │  │  │ Part_L1 │  │ Part_L2 │  │ Part_L3 │  │ Part_System │    │   │   │
│   │  │  │  30%    │  │  25%    │  │  20%    │  │    25%      │    │   │   │
│   │  │  └─────────┘  └─────────┘  └─────────┘  └─────────────┘    │   │   │
│   │  └─────────────────────────────────────────────────────────────┘   │   │
│   └─────────────────────────────────────────────────────────────────────┘   │
│                                                                             │
│   ┌─────────────────────────────────────────────────────────────────────┐   │
│   │                     Workload Processes                              │   │
│   │                                                                     │   │
│   │  ┌─────────────────┐  ┌─────────────────┐  ┌─────────────────┐     │   │
│   │  │ L1: Brake Ctrl  │  │ L2: Perception  │  │ L3: HMI         │     │   │
│   │  │ Part_L1, pri=63 │  │ Part_L2, pri=50 │  │ Part_L3, pri=30 │     │   │
│   │  │ Pulse wakeup    │  │ Pulse wakeup    │  │ Regular         │     │   │
│   │  └─────────────────┘  └─────────────────┘  └─────────────────┘     │   │
│   └─────────────────────────────────────────────────────────────────────┘   │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 4.2 컴포넌트 상세

#### timpani-o (QNX)
- **역할**: 스케줄 테이블 생성, 노드 관리
- **IPC**: QNX name_attach() 기반 MsgSend 서버
- **변경 최소화**: table_builder 로직 재사용

#### timpani-n (QNX)
- **Timer Master**: QNX timer_create + Pulse
- **APS Manager**: SchedCtl() API로 파티션/쓰레드 관리
- **Fault Monitor**: Resource Manager 또는 공유 메모리 + Pulse

#### ttsched 라이브러리 (QNX)
- **ttsched_wait_next_period()**: Pulse 수신 대기
- **POSIX 호환**: 인터페이스 유지, 내부 구현만 변경

---

## 5. 구현 전략

### 5.1 단계별 접근

| 단계 | 목표 | 산출물 |
|:--|:--|:--|
| **Phase 1** | QNX 빌드 환경 + POSIX 호환 부분 포팅 | timpani-o 기본 빌드 |
| **Phase 2** | APS 기반 L1~L4 파티션 구현 | timpani-n APS Manager |
| **Phase 3** | Timer Master + Pulse 기반 TT 슬롯 | 정밀 타이밍 검증 |
| **Phase 4** | IPC 통합 (MsgSend 또는 gRPC) | 전체 파이프라인 |
| **Phase 5** | Fault Monitor + 통합 테스트 | 프로덕션 준비 |

### 5.2 코드 재사용 분석

| 컴포넌트 | 재사용 가능 | QNX 전용 구현 필요 |
|:--|:--|:--|
| **table_builder.cpp** | 90% | epoch_ns 계산 방식 |
| **schedinfo_service.cpp** | 80% | gRPC → QNX IPC |
| **timer_master.cpp** | 30% | clock_nanosleep → QNX timer |
| **bpf_loader.cpp** | 0% | 완전 재작성 (APS Manager) |
| **task_registry.cpp** | 50% | cgroup → APS 파티션 |
| **fault_monitor.cpp** | 40% | ringbuf → Resource Manager |
| **node_client.cpp** | 20% | gRPC → MsgSend |

### 5.3 HAL (Hardware Abstraction Layer) 도입

Linux와 QNX 공통 코드 최대화를 위한 HAL 설계:

```cpp
// hal/timer.h
namespace timpani::hal {

class Timer {
public:
    virtual ~Timer() = default;
    virtual void sleep_until(uint64_t abs_ns) = 0;
    virtual uint64_t now_ns() = 0;
};

#ifdef __QNX__
class QnxTimer : public Timer { ... };
#else
class LinuxTimer : public Timer { ... };
#endif

}

// hal/partition.h
namespace timpani::hal {

class Partition {
public:
    virtual ~Partition() = default;
    virtual int create(const std::string& name, uint32_t budget_percent) = 0;
    virtual int join(pid_t pid, int tid) = 0;
    virtual int set_cpu_affinity(uint64_t mask) = 0;
};

#ifdef __QNX__
class QnxApsPartition : public Partition { ... };
#else
class LinuxCgroupPartition : public Partition { ... };
#endif

}
```

---

## 6. 기술적 과제 및 리스크

### 6.1 과제

| 과제 | 난이도 | 대응 방안 |
|:--|:--|:--|
| **sched_ext 기능 없음** | 높음 | APS 활용, 일부 기능 userspace 구현 |
| **eBPF 없음** | 높음 | Resource Manager + 공유 메모리 |
| **CBS 네이티브 지원 없음** | 중간 | APS budget_percent로 근사 |
| **gRPC 포팅** | 중간 | QNX IPC 또는 gRPC-lite 검토 |
| **TT 슬롯 정밀도** | 낮음 | QNX 네이티브 RT 활용 (Linux보다 우수) |

### 6.2 제한사항

1. **CBS 의미론 차이**: 
   - Linux CBS: task 단위 예산 (Cs, Ts)
   - QNX APS: 파티션 단위 예산 (budget_percent)
   - **영향**: Sporadic L2 워크로드 관리 방식 변경 필요

2. **동적 워크로드 추가**:
   - QNX APS 파티션 생성은 root 권한 필요
   - Runtime partition 생성 시 시스템 부하

3. **멀티노드 통신**:
   - QNX Qnet (네이티브) vs gRPC 포팅 선택 필요

### 6.3 장점

| 항목 | Linux + PREEMPT_RT | QNX Neutrino |
|:--|:--|:--|
| **RT 지연** | 수십 μs (최적화 시) | 수 μs (네이티브) |
| **안전 인증** | 추가 작업 필요 | ISO 26262 인증 가능 |
| **결정론** | 노력 필요 | 설계부터 내장 |
| **APS** | cgroup + sched_ext | 네이티브 지원 |

---

## 7. 테스트 전략

### 7.1 검증 항목

| 항목 | 검증 방법 |
|:--|:--|
| **TT 슬롯 정밀도** | Pulse latency 측정 (< 10μs 목표) |
| **APS 예산 강제** | CPU 사용률 모니터링, 파티션 간 격리 확인 |
| **Fault 감지** | 의도적 deadline miss → Fault 전파 경로 확인 |
| **IPC 지연** | MsgSend round-trip time 측정 |
| **모드 전환** | 주차 ↔ 주행 시나리오 테스트 |

### 7.2 벤치마크 비교

Linux TIMPANI와 QNX TIMPANI의 성능 비교:
- Jitter histogram
- Scheduling latency
- Fault detection latency
- IPC throughput

---

## 8. 로드맵

| 마일스톤 | 목표 | 예상 기간 |
|:--|:--|:--|
| **M1: 환경 구축** | QNX SDP 설치, 크로스 컴파일 환경 | 2주 |
| **M2: 기본 포팅** | timpani-o POSIX 부분 빌드 | 2주 |
| **M3: APS Manager** | L1~L4 파티션 구현 | 4주 |
| **M4: Timer Master** | QNX Timer + Pulse 구현 | 2주 |
| **M5: IPC 통합** | MsgSend 기반 통신 | 2주 |
| **M6: Fault Monitor** | Resource Manager 구현 | 3주 |
| **M7: 통합 테스트** | 전체 파이프라인 검증 | 2주 |
| **M8: 최적화** | 성능 튜닝, 벤치마크 | 2주 |
| **예상 총 기간** | | **약 19주 (4~5개월)** |

---

## 9. 참고 자료

### 9.1 QNX 문서
- QNX Neutrino RTOS Programmer's Guide
- QNX Adaptive Partitioning User's Guide
- QNX Resource Managers

### 9.2 관련 DDR
- [DDR-002: Scheduling Architecture](DDR-002-scheduling-architecture.md)
- [DDR-005: sched_ext BPF Scheduler](DDR-005-sched-ext-bpf-scheduler.md)
- [DDR-006: Communication Architecture](DDR-006-communication-architecture.md)

---

## 변경 이력

| 날짜 | 버전 | 변경 내용 |
|:--|:--|:--|
| 2026-04-16 | 0.1 | 초기 작성 |
