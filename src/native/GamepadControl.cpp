// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * GamepadControl.cpp — see GamepadControl.h. The Rust subsystem runs its
 * device IO on its own poll thread; its callback may fire off the audio
 * thread, so everything it touches (the egress ring) is already thread-safe.
 */
#include "clockwork_prefix.h"
#include "GamepadControl.h"

#include "IngressCallCtx.h"
#include "OscEgress.h"
#include "SubscribeAck.h"
#include "clockwork_gamepad.h"
#include "clockwork_config.h"   // clockwork_log
#include "lanes/lanes.h"        // clockwork_ingress_write: an event enters like any other
#include "osc/OscReceivedElements.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

void GamepadControl::init(OscEgress* egress) {
    mEgress = egress;
    if (!mGamepad) {
        mGamepad = clockwork_gamepad_create(this, &GamepadControl::emitCb);
    }
}

void GamepadControl::shutdown() {
    if (mGamepad) {
        clockwork_gamepad_destroy(mGamepad);
        mGamepad = nullptr;
    }
}

bool GamepadControl::handleGamepadCommand(const DrainCallCtx& meta, const uint8_t* data, uint32_t size) {
    const uint32_t token = meta.sourceId;
    if (size < CLOCKWORK_SYS_LEN("gamepad/") + 1 ||
        std::memcmp(data, CLOCKWORK_SYS("gamepad/"), CLOCKWORK_SYS_LEN("gamepad/")) != 0) return false;
    // Hold the origin for synchronous emitCb REPLYs during this call (see header).
    mReplyToken = token;

    // Subscription drives the egress audience (owned by the transport). The
    // address is the leading, NUL-terminated OSC string.
    const char* addr = reinterpret_cast<const char*>(data);
    if (std::strcmp(addr, CLOCKWORK_SYS("gamepad/notify/subscribe")) == 0) {
        if (mEgress && mEgress->subscribeCallerToGamepadNotify(token) && mGamepad)
            clockwork_gamepad_emit_devices(mGamepad);  // devices snapshot to the new subscriber
        ackSubscribeIfTokened(mEgress, token, data, size,
                              CLOCKWORK_SYS("gamepad/notify/subscribe.reply"));
        return true;
    }
    if (std::strcmp(addr, CLOCKWORK_SYS("gamepad/notify/unsubscribe")) == 0) {
        if (mEgress) mEgress->unsubscribeCallerFromGamepadNotify(token);
        return true;
    }

    if (mGamepad) clockwork_gamepad_handle_osc(mGamepad, data, size);
    return true;
}

// Hotplug logging. /clockwork/gamepad/devices broadcasts fire only on connect/
// disconnect/enable changes and an explicit /clockwork/gamepad/refresh, so this
// is flood-safe. Per-event traffic (/clockwork/gamepad/in/axis, /clockwork/gamepad/in/button) is
// deliberately not logged. Payload: <n:i> [name:s enabled:i]*
static void logGamepadDevicesChange(const uint8_t* data, uint32_t len) {
    if (std::strcmp(reinterpret_cast<const char*>(data), CLOCKWORK_SYS("gamepad/devices")) != 0) return;
    std::string names;
    try {
        osc::ReceivedMessage msg(osc::ReceivedPacket(
            reinterpret_cast<const char*>(data),
            static_cast<osc::osc_bundle_element_size_t>(len)));
        auto it = msg.ArgumentsBegin();
        if (it == msg.ArgumentsEnd() || !it->IsInt32()) return;
        const int n = it->AsInt32Unchecked(); ++it;
        for (int i = 0; i < n && it != msg.ArgumentsEnd(); ++i) {
            if (!it->IsString()) return;
            if (!names.empty()) names += ", ";
            names += it->AsStringUnchecked(); ++it;   // name
            if (it != msg.ArgumentsEnd()) ++it;       // enabled flag
        }
    } catch (...) { return; }
    fprintf(stderr, "[gamepad] devices: [%s]\n", names.c_str());
    fflush(stderr);
}

// A reply goes to its caller. An event goes into the IN ring, to be answered
// on the audio thread by clockwork_event_route — see MidiControl::emitCb.
void GamepadControl::emitCb(void* ctx, int32_t kind, const uint8_t* osc, uint32_t len) {
    auto* self = static_cast<GamepadControl*>(ctx);
    if (!self->mEgress) return;
    if (kind == CLOCKWORK_GAMEPAD_EMIT_REPLY) {
        self->mEgress->reply(self->mReplyToken, osc, len);
    } else {
        logGamepadDevicesChange(osc, len);
        if (!clockwork_ingress_write(osc, len, 0)) {
            static std::atomic<uint32_t> dropped{0};
            if (dropped.fetch_add(1, std::memory_order_relaxed) < 8)
                clockwork_log("WARNING: gamepad event %s dropped — IN ring full",
                              reinterpret_cast<const char*>(osc));
        }
    }
}
