// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#pragma once

#include <stdint.h>

/* ---- /timpani_ttsched POSIX shared memory layout ---- */

#define TIMPANI_TTSCHED_MAGIC  0x54494D50U  /* "TIMP" — set when schedule is ready */
#define TIMPANI_MAX_TASKS      32

/**
 * Per-task wakeup slot.
 * TimerMaster increments @counter and FUTEX_WAKEs when this task's slot fires.
 * The task calls FUTEX_WAIT on @counter to sleep until its next period.
 */
struct timpani_task_slot {
    char     name[16];   /* task comm name (PR_SET_NAME, <=15 chars + NUL) */
    uint32_t counter;    /* monotonically incremented by TimerMaster per slot fire */
    uint32_t _pad;
};

/**
 * Full layout of /timpani_ttsched SHM object.
 * Written by timpani-n TimerMaster (O_RDWR).
 * Read by task processes (O_RDONLY) — FUTEX_WAIT only needs read access.
 */
struct timpani_ttsched_shm {
    uint32_t magic;    /* TIMPANI_TTSCHED_MAGIC when schedule is ready, 0 otherwise */
    uint32_t n_tasks;  /* number of active task slots (atomic RELEASE write) */
    struct timpani_task_slot tasks[TIMPANI_MAX_TASKS];
};

/* ---- API ---- */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * ttsched_init() - Open and map /timpani_ttsched SHM.
 * Must be called after PR_SET_NAME has been applied to the calling thread.
 * Slot lookup is retried lazily in ttsched_wait_next_period() if the schedule
 * is not yet published at init time.
 */
void ttsched_init(void);

/**
 * ttsched_wait_next_period() - Block until TimerMaster fires this task's slot.
 * Falls back to usleep(1000) if SHM is not ready or slot not found yet.
 */
void ttsched_wait_next_period(void);

#ifdef __cplusplus
}
#endif
