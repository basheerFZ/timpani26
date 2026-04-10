// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the time-triggered scheduler context for the task thread.
 */
void ttsched_init(void);

/**
 * @brief Wait for the beginning of the next TT slot period.
 *        This synchronizes the application loop with the system scheduler.
 */
void ttsched_wait_next_period(void);

#ifdef __cplusplus
}
#endif
