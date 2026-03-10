/*
 * SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
 * SPDX-License-Identifier: MIT
 */

#include "internal.h"

static void cleanup_workloads(struct context *ctx);
static void cleanup_communication(struct context *ctx);
static void cleanup_bpf_trace(void);

void cleanup_context(struct context *ctx)
{
    if (!ctx) return;

    TT_LOG_INFO("Cleaning up resources...");

    cleanup_workloads(ctx);
    cleanup_communication(ctx);
    cleanup_bpf_trace();

    TT_LOG_INFO("Time Trigger shutdown completed.");
}

static void cleanup_workloads(struct context *ctx)
{
    if (!ctx) {
        return;
    }

    struct workload *wl;

    while (!LIST_EMPTY(&ctx->runtime.workloads)) {
        wl = LIST_FIRST(&ctx->runtime.workloads);

        if (!wl) {
            break;  // 안전장치
        }

        TT_LOG_INFO("Cleaning up workload: %s", wl->sched_info.workload_id);

        // Clean up tasks in this workload
        struct time_trigger *tt_p;
        while (!LIST_EMPTY(&wl->tt_list)) {
            tt_p = LIST_FIRST(&wl->tt_list);

            if (!tt_p) {
                break;
            }

            // BPF에서 PID 제거
            bpf_del_pid(tt_p->task.pid);

            // pidfd 닫기
            if (tt_p->task.pidfd >= 0) {
                close(tt_p->task.pidfd);
            }

            // 타이머 삭제
            timer_delete(tt_p->timer);

            // 리스트에서 제거 및 메모리 해제
            LIST_REMOVE(tt_p, entry);
            TT_FREE(tt_p);
        }

        // 스케줄 정보의 태스크 리스트 정리
        destroy_task_info_list(wl->sched_info.tasks);
        wl->sched_info.tasks = NULL;

        // 하이퍼피리어드 타이머 정리
        if (wl->hp_manager.hyperperiod_us > 0) {
            timer_delete(wl->hp_manager.hyperperiod_timer);
            log_hyperperiod_statistics(&wl->hp_manager);
        }

        // 워크로드 리스트에서 제거 및 메모리 해제
        LIST_REMOVE(wl, entry);
        TT_FREE(wl);
    }

    ctx->runtime.nr_workloads = 0;
}

static void cleanup_communication(struct context *ctx)
{
    if (ctx->comm.dbus) {
        sd_bus_unref(ctx->comm.dbus);
        ctx->comm.dbus = NULL;
    }

    if (ctx->comm.event) {
        sd_event_unref(ctx->comm.event);
        ctx->comm.event = NULL;
    }
}

static void cleanup_bpf_trace(void)
{
    bpf_off();
}
