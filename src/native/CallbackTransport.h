// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * CallbackTransport.h — in-process IOscTransport for embedders and tests.
 *
 * Everything the engine sends out surfaces through the engine's onReply
 * callback (debug through onDebug): the client is the host that set it. This
 * is the engine's default transport, so a bare `ClockworkEngine` works
 * standalone. Only the NRT gateway calls deliver, so no locking is needed.
 */
#pragma once

#include "IOscTransport.h"

#include <cstdint>
#include <functional>

class CallbackTransport : public IOscTransport {
public:
    // Bound to the engine's onReply member by pointer so a callback swapped at
    // runtime is always seen.
    explicit CallbackTransport(const std::function<void(const uint8_t*, uint32_t)>* onReply)
        : mOnReply(onReply) {}

    // A host that set no callback hears nothing.
    bool deliver(const uint8_t* data, uint32_t size) override {
        if (!mOnReply || !*mOnReply) return false;
        (*mOnReply)(data, size);
        return true;
    }

private:
    const std::function<void(const uint8_t*, uint32_t)>* mOnReply;
};
