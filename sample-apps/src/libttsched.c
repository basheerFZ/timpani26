// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#include "libttsched.h"
#include <stdint.h>
#include <unistd.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <stddef.h>

static volatile uint32_t* g_slot_counter = NULL;

void ttsched_init(void) {
    // Basic PoC implementation.
    // In a real implementation, this would shm_open the POSIX shared memory mapped to TimerMaster.
    int fd = shm_open("/timpani_ttsched", O_RDONLY, 0666);
    if (fd >= 0) {
        g_slot_counter = (volatile uint32_t*)mmap(NULL, sizeof(uint32_t), PROT_READ, MAP_SHARED, fd, 0);
        close(fd);
    }
}

void ttsched_wait_next_period(void) {
    if (g_slot_counter && g_slot_counter != MAP_FAILED) {
        uint32_t current = *g_slot_counter;
        // futex WAIT: sleep strictly linked to TimerMaster's slot update
        syscall(SYS_futex, (uint32_t*)g_slot_counter, FUTEX_WAIT, current, NULL, NULL, 0);
    } else {
        // Fallback for PoC if shm is not loaded
        usleep(1000);
    }
}
