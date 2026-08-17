// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#pragma once

#ifndef __VMLINUX_H__
#include <linux/types.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

struct schedstat_event {
    __s32 pid;
    __s32 cpu;
    __u64 ts_wakeup;
    __u64 ts_start;
    __u64 ts_stop;
};

#ifdef __cplusplus
}
#endif
