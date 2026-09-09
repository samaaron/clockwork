// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * engine_state.h — Native engine lifecycle state
 *
 * Tracks the native engine's lifecycle as an atomic state machine.
 * Transitions drive the same lifecycle events the JS/WASM side already
 * emits: 'setup' (DSP ready), 'reload:start/complete' (rebuild), etc.
 *
 * On native, these are delivered as OSC: /clockwork/statechange and
 * /clockwork/setup.  The JS side uses its own event emitter and boolean
 * flags (#initialized, #initializing) rather than this enum.
 */
#pragma once

enum class EngineState {
    Booting,      // First init in progress (DSP being created)
    Running,      // DSP instance exists, audio callback active
    Restarting,   // Cold swap in progress (DSP destroyed, being rebuilt)
    Stopped,      // Audio callback stopped (device removed, shutdown)
    Error         // Device error
};

inline const char* engineStateToString(EngineState s) {
    switch (s) {
        case EngineState::Booting:    return "booting";
        case EngineState::Running:    return "running";
        case EngineState::Restarting: return "restarting";
        case EngineState::Stopped:    return "stopped";
        case EngineState::Error:      return "error";
    }
    return "unknown";
}
