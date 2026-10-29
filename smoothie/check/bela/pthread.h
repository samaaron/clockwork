// SPDX-License-Identifier: ISC
// Copyright (c) 2026 Sam Aaron
//
// Stands in for Xenomai Cobalt's pthread.h, which a Bela toolchain puts ahead
// of the system header: the system header, plus the thread-mode query that
// juce_SystemStats_linux.cpp calls under JUCE_BELA. Compile only.
#pragma once
#include_next <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif
int cobalt_thread_mode (void);
#ifdef __cplusplus
}
#endif
