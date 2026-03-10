/*
 * SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
 * SPDX-License-Identifier: MIT
 */

#define _GNU_SOURCE
#include "internal.h"

static tt_error_t initialize(struct context *ctx);
static tt_error_t run(struct context *ctx);

int main(int argc, char *argv[])
{
    struct context ctx;
    tt_error_t ret;

    // 구조체 명시적 초기화
    memset(&ctx, 0, sizeof(ctx));
    LIST_INIT(&ctx.runtime.workloads);

    // 설정 파싱
    ret = parse_config(argc, argv, &ctx);
    if (ret != TT_SUCCESS) {
        TT_LOG_ERROR("Configuration error: %s", tt_error_string(ret));
        return EXIT_FAILURE;
    }

    // 초기화
    ret = initialize(&ctx);
    if (ret != TT_SUCCESS) {
        TT_LOG_ERROR("Initialization failed: %s", tt_error_string(ret));
        goto cleanup;
    }

    // 실행
    ret = run(&ctx);
    if (ret != TT_SUCCESS) {
        TT_LOG_ERROR("Runtime error: %s", tt_error_string(ret));
    }

cleanup:
    cleanup_context(&ctx);
    return (ret == TT_SUCCESS) ? EXIT_SUCCESS : EXIT_FAILURE;
}

static tt_error_t initialize(struct context *ctx)
{
    pid_t pid = getpid();

    // 시그널 핸들러 설정
    if (setup_signal_handlers(ctx) != TT_SUCCESS) {
        return TT_ERROR_SIGNAL;
    }

    // 프로세스 우선순위 설정
    if (ctx->config.cpu != -1) {
        ttsched_error_t affinity_result = set_affinity(pid, ctx->config.cpu);
        if (affinity_result != TTSCHED_SUCCESS) {
            TT_LOG_WARNING("Failed to set CPU affinity to %d: %s",
                ctx->config.cpu, ttsched_error_string(affinity_result));
        }
    }
    if (ctx->config.prio > 0 && ctx->config.prio <= 99) {
        ttsched_error_t sched_result = set_schedattr(pid, ctx->config.prio, SCHED_FIFO);
        if (sched_result != TTSCHED_SUCCESS) {
            TT_LOG_WARNING("Failed to set scheduling attributes (prio=%d): %s",
                ctx->config.prio, ttsched_error_string(sched_result));
        }
    }

    // BPF 초기화
    if (calibrate_bpf_time_offset() != TT_SUCCESS) {
        TT_LOG_ERROR("Failed to calibrate BPF time offset");
        return TT_ERROR_BPF;
    }

    // TRPC 초기화 및 스케줄 정보 획득
    if (init_trpc(ctx) != TT_SUCCESS) {
        TT_LOG_ERROR("Failed to initialize TRPC and get schedule info");
        return TT_ERROR_NETWORK;
    }

    if (!ctx->config.enable_apex) {
        // BPF 활성화 (PID-based, workload-agnostic)
        bpf_on(handle_sigwait_bpf_event, handle_schedstat_bpf_event, (void *)ctx);

        // 각 워크로드의 태스크 리스트 초기화
        struct workload *wl;
        bool has_apex_workload = false;
        int total_tasks = 0;

        LIST_FOREACH(wl, &ctx->runtime.workloads, entry) {
            if (strcmp(wl->sched_info.workload_id, "Apex.OS") == 0) {
                has_apex_workload = true;
                if (init_apex_list(ctx) != TT_SUCCESS) {
                    TT_LOG_ERROR("Failed to initialize Apex.OS task list");
                    return TT_ERROR_CONFIG;
                }
            } else {
                if (init_task_list(wl) != TT_SUCCESS) {
                    TT_LOG_ERROR("Failed to initialize task list for workload %s",
                        wl->sched_info.workload_id);
                    return TT_ERROR_CONFIG;
                }
                total_tasks += wl->nr_active_tasks;
            }
        }

        TT_LOG_INFO("Initialized %u workload(s) with %d total active tasks",
            ctx->runtime.nr_workloads, total_tasks);
    }

    // Initialize Apex.OS Monitor
    if (apex_monitor_init(ctx) != TT_SUCCESS) {
        TT_LOG_ERROR("Failed to initialize Apex.OS Monitor");
        return TT_ERROR_NETWORK;
    }

    return TT_SUCCESS;
}

static tt_error_t run(struct context *ctx)
{
    // 타이머 동기화
    if (sync_timer_with_server(ctx) != TT_SUCCESS) {
        TT_LOG_ERROR("Failed to synchronize timers");
        return TT_ERROR_NETWORK;
    }

    // 태스크 타이머 시작
    if (start_timers(ctx) != TT_SUCCESS) {
        TT_LOG_ERROR("Failed to start timers");
        return TT_ERROR_TIMER;
    }

    // 각 워크로드의 하이퍼피리어드 타이머 시작
    struct workload *wl;
    LIST_FOREACH(wl, &ctx->runtime.workloads, entry) {
        if (start_hyperperiod_timer(wl) != TT_SUCCESS) {
            TT_LOG_ERROR("Failed to start hyperperiod timer for workload %s",
                wl->sched_info.workload_id);
            return TT_ERROR_TIMER;
        }
    }

    // 메인 이벤트 루프
    tt_error_t result = epoll_loop(ctx);

    TT_LOG_INFO("Shutdown requested, cleaning up resources...");

    return result;
}
