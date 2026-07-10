// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#include "libttsched.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

static volatile struct timpani_ttsched_shm* g_shm = NULL;
static int g_task_idx = -1; /* index into g_shm->tasks[], -1 = not found yet */
static uint32_t g_shm_generation = 0;

static int find_task_slot(volatile struct timpani_ttsched_shm* shm,
                          const char* name)
{
    uint32_t n, i;

    if (shm->magic != TIMPANI_TTSCHED_MAGIC) return -1;

    n = shm->n_tasks;
    for (i = 0; i < n && i < TIMPANI_MAX_TASKS; i++) {
        if (strncmp((const char*)shm->tasks[i].name, name, 15) == 0)
            return (int)i;
    }
    return -1;
}

void ttsched_init(void)
{
    int fd = shm_open("/timpani_ttsched", O_RDONLY, 0666);
    if (fd < 0) return;

    g_shm = (volatile struct timpani_ttsched_shm*)mmap(
        NULL, sizeof(struct timpani_ttsched_shm), PROT_READ, MAP_SHARED, fd, 0);
    close(fd);

    if (g_shm == MAP_FAILED) {
        g_shm = NULL;
        return;
    }

    /* Slot lookup: schedule may not be ready yet — retry in wait_next_period */
    char my_name[16] = {};
    prctl(PR_GET_NAME, my_name, 0, 0, 0);
    g_task_idx = find_task_slot(g_shm, my_name);
    g_shm_generation = g_shm->generation;
}

int ttsched_wait_next_period(void)
{
    uint32_t current;

    if (!g_shm) {
        /* SHM not mapped yet — retry open/map on every call */
        ttsched_init();
        if (!g_shm) {
            usleep(1000);
            return -1;
        }
    }

    /* Detect timpani-n restart: magic is zeroed by TimerMaster before it
     * publishes a new schedule table (or remains 0 after shm_unlink +
     * re-create).  Re-init to pick up the new SHM object. */
    if (g_shm->magic != TIMPANI_TTSCHED_MAGIC) {
        /* Unmap stale SHM and re-open the (possibly new) object */
        munmap((void*)g_shm, sizeof(struct timpani_ttsched_shm));
        g_shm = NULL;
        g_task_idx = -1;
        ttsched_init();
        if (!g_shm) {
            usleep(1000);
            return -1;
        }
        /* If still not ready (magic still 0), wait for schedule publish */
        if (g_shm->magic != TIMPANI_TTSCHED_MAGIC) {
            usleep(1000);
            return -1;
        }
    }

    /* Detect schedule table update via generation counter */
    if (g_shm->generation != g_shm_generation) {
        g_shm_generation = g_shm->generation;
        g_task_idx = -1;
    }

    /* Retry slot lookup if schedule arrived after ttsched_init() */
    if (g_task_idx < 0) {
        char my_name[16] = {};
        prctl(PR_GET_NAME, my_name, 0, 0, 0);
        g_task_idx = find_task_slot(g_shm, my_name);
        if (g_task_idx < 0) {
            usleep(1000);
            return -1;
        }
    }

    current = g_shm->tasks[g_task_idx].counter;
    /*
     * FUTEX_WAIT on this task's own counter word.
     *
     * Return cases:
     *   0      — woken by TimerMaster FUTEX_WAKE: slot fired, proceed.
     *   EAGAIN — counter already changed before we slept (slot fired just
     *            before the syscall): proceed normally, no sleep needed.
     *   EINTR  — interrupted by a signal (spurious wakeup): re-check and
     *            go back to sleep.  Do NOT let the task run early.
     */
    while (syscall(SYS_futex, (uint32_t*)&g_shm->tasks[g_task_idx].counter,
                   FUTEX_WAIT, current, NULL, NULL, 0) == -1 &&
           errno ==
               EINTR); /* re-enter FUTEX_WAIT until genuinely woken or EAGAIN */

    /* After wakeup: check if TimerMaster zeroed magic (shutdown signal).
     * If so, invalidate our mapping so the next call triggers re-init. */
    if (g_shm->magic != TIMPANI_TTSCHED_MAGIC) {
        munmap((void*)g_shm, sizeof(struct timpani_ttsched_shm));
        g_shm = NULL;
        g_task_idx = -1;
        return -1;
    }

    return 0;
}
