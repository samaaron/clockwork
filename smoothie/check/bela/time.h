// SPDX-License-Identifier: ISC
// Copyright (c) 2026 Sam Aaron
//
// Stands in for Xenomai Cobalt's time.h, which a Bela toolchain puts ahead of
// the system header: the system header, plus the wrapped clock_gettime that
// juce_SystemStats_linux.cpp calls under JUCE_BELA. Compile only.
#pragma once
#include_next <time.h>

#ifdef __cplusplus
extern "C" {
#endif
int __wrap_clock_gettime (clockid_t clock_id, struct timespec* tp);
#ifdef __cplusplus
}
#endif
