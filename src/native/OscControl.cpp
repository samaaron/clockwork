// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * OscControl.cpp — see OscControl.h. All sockets, threading, OSC reframing and
 * hostname resolution live in the Rust subsystem (clockwork-osc-net); this file
 * only marshals between the engine and the clockwork_osc_* C ABI.
 */
#include "clockwork_prefix.h"
#include "OscControl.h"

#include "ClientVerbs.h"        // refuseClientVerb, when the build has none
#include "IngressCallCtx.h"
#include "OscEgress.h"
#include "SubscribeAck.h"
#include "clockwork_osc.h"
#include "clockwork_event_sink.h"
#include "clockwork_sys.h"        // clockwork_sys_refuse
#include "clockwork_config.h"   // clockwork_log
#include "osc/OscReceivedElements.h"

#include <cstdio>
#include <cstring>
#include <string>

// ── small arg readers (oscpack iterator) ─────────────────────────────────────
namespace {
int argInt(osc::ReceivedMessage::const_iterator& it,
           const osc::ReceivedMessage::const_iterator& end, int def) {
    if (it == end) return def;
    int v = def;
    if (it->IsInt32())      v = it->AsInt32Unchecked();
    else if (it->IsInt64()) v = static_cast<int>(it->AsInt64Unchecked());
    else if (it->IsFloat()) v = static_cast<int>(it->AsFloatUnchecked());
    ++it;
    return v;
}

bool argBool(osc::ReceivedMessage::const_iterator& it,
             const osc::ReceivedMessage::const_iterator& end, bool def) {
    if (it == end) return def;
    bool v = def;
    if (it->IsBool())        v = it->AsBoolUnchecked();
    else if (it->IsInt32())  v = it->AsInt32Unchecked() != 0;
    else if (it->IsFloat())  v = it->AsFloatUnchecked() != 0.0f;
    ++it;
    return v;
}
} // namespace

void OscControl::init(OscEgress* egress) {
    mEgress = egress;
    if (!mOsc) mOsc = clockwork_osc_create(this, &OscControl::emitCb);
}

void OscControl::shutdown() {
    if (mOsc) {
        clockwork_osc_destroy(mOsc);
        mOsc = nullptr;
    }
}

void OscControl::emitCb(void* ctx, int32_t kind, const uint8_t* osc, uint32_t len) {
    auto* self = static_cast<OscControl*>(ctx);
    if (!self->mEgress) return;
    if (kind == CLOCKWORK_OSC_EMIT_BROADCAST)
        self->mEgress->broadcastOscNotify(osc, len);
}

void OscControl::applyCueConfig() {
    if (mOsc) clockwork_osc_configure(mOsc, mPort, mLoopback ? 1 : 0, mCuesOn ? 1 : 0);
}

#if CLOCKWORK_CLIENT_VERBS
// The one sink onto host:port, opened on first use. Reuse is by target, so a
// client's second send to the same place shares the first's queue and stats.
// A hostname resolves at open (clockwork-sinks refuses a target that does not),
// which is also the only DNS lookup the send path ever does. An IPv6 literal
// is bracketed, as the sink's "[v6]:port" target form asks.
ClockworkSink OscControl::sinkFor(const std::string& host, int port) {
    const std::string target = (host.find(':') != std::string::npos ? "[" + host + "]" : host)
                             + ":" + std::to_string(port);
    ClockworkSink open[kMaxSinks];
    const uint32_t n = clockwork_sink_list(open, kMaxSinks);
    for (uint32_t i = 0; i < n && i < kMaxSinks; ++i) {
        if (clockwork_sink_kind(open[i]) != kClockworkSinkOsc) continue;
        const char* t = clockwork_sink_target(open[i]);
        if (t && target == t) return open[i];
    }
    return clockwork_sink_open(kClockworkSinkOsc, target.c_str(), kSinkCapacity);
}
#endif // CLOCKWORK_CLIENT_VERBS

bool OscControl::handleOscCommand(const DrainCallCtx& meta, const uint8_t* data, uint32_t size) {
    const uint32_t token = meta.sourceId;
    if (size < CLOCKWORK_SYS_LEN("osc/") + 1 ||
        std::memcmp(data, CLOCKWORK_SYS("osc/"), CLOCKWORK_SYS_LEN("osc/")) != 0) return false;
    const char* addr = reinterpret_cast<const char*>(data);

    if (std::strcmp(addr, CLOCKWORK_SYS("osc/notify/subscribe")) == 0) {
        if (mEgress) mEgress->subscribeCallerToOscNotify(token);
        ackSubscribeIfTokened(mEgress, token, data, size,
                              CLOCKWORK_SYS("osc/notify/subscribe.reply"));
        return true;
    }
    if (std::strcmp(addr, CLOCKWORK_SYS("osc/notify/unsubscribe")) == 0) {
        if (mEgress) mEgress->unsubscribeCallerFromOscNotify(token);
        return true;
    }

#if !CLOCKWORK_CLIENT_VERBS
    // The client half is not in this build (docs/SURFACE.md): the send verb
    // is refused BY NAME, to the caller, rather than ignored.
    if (clockwork_sys_is_client_verb(addr)) {
        refuseClientVerb(mEgress, token, data, size);
        return true;
    }
#endif

    try {
        osc::ReceivedMessage msg(osc::ReceivedPacket(
            reinterpret_cast<const char*>(data),
            static_cast<osc::osc_bundle_element_size_t>(size)));
        auto it = msg.ArgumentsBegin();
        const auto end = msg.ArgumentsEnd();

#if CLOCKWORK_CLIENT_VERBS
        if (std::strcmp(addr, CLOCKWORK_SYS("osc/send")) == 0) {
            // Self-routing outbound user OSC: <host:s> <port:i> <inner:blob>.
            // Send `inner` to host:port. Reached immediately, or as the inner blob
            // of a /clockwork/schedule event — the deferred path re-dispatches it here.
            std::string host;
            int port = 0;
            const void* inner = nullptr;
            osc::osc_bundle_element_size_t innerLen = 0;
            if (it != end && it->IsString()) { host = it->AsStringUnchecked(); ++it; }
            if (it != end) port = argInt(it, end, 0);
            if (it != end && it->IsBlob()) it->AsBlobUnchecked(inner, innerLen);
            if (!(inner && innerLen > 0 && !host.empty() && port > 0)) {
                clockwork_log("WARNING: /clockwork/osc/send dropped — bad host/port/blob "
                       "(host='%s' port=%d innerLen=%d)",
                       host.c_str(), port, static_cast<int>(innerLen));
                return true;
            }
            // Through the sink for the endpoint, carrying the message's time:
            // for a send inside "/clockwork/schedule" that is the moment it was
            // scheduled for (nrtForwardSink threads it here), and the sink
            // holds it to that moment. Sending it now, as this once did, let
            // it go at the block it fired in. 0 and 1 both mean now.
            const ClockworkSink sink = sinkFor(host, port);
            const int64_t when = meta.when ? meta.when : 1;
            if (sink == CLOCKWORK_SINK_NONE ||
                !clockwork_sink_send(sink, static_cast<const uint8_t*>(inner),
                               static_cast<uint32_t>(innerLen), when)) {
                // Say why, to the log AND to the caller: a message that never
                // left is the client's problem to know about, and a debug line
                // nobody is watching is not telling them.
                char reason[128];
                const uint32_t widest = sink == CLOCKWORK_SINK_NONE
                                      ? 0 : clockwork_sink_max_message_bytes(sink);
                if (sink == CLOCKWORK_SINK_NONE)
                    std::snprintf(reason, sizeof reason, "host does not resolve");
                else if (static_cast<uint32_t>(innerLen) > widest)
                    std::snprintf(reason, sizeof reason,
                                  "message of %u bytes exceeds the sink's widest cell (%u bytes)",
                                  static_cast<unsigned>(innerLen), static_cast<unsigned>(widest));
                else
                    std::snprintf(reason, sizeof reason, "sink full");
                clockwork_log("WARNING: /clockwork/osc/send to %s:%d dropped — %s",
                       host.c_str(), port, reason);
                if (mEgress)
                    clockwork_sys_refuse(data, size, reason,
                        [this, &meta](const uint8_t* d, uint32_t n) {
                            mEgress->reply(meta.sourceId, d, n);
                        });
            }
        } else
#endif // CLOCKWORK_CLIENT_VERBS
        if (std::strcmp(addr, CLOCKWORK_SYS("osc/cue-server/config")) == 0) {
            mPort     = argInt(it, end, mPort);
            mLoopback = argBool(it, end, mLoopback);
            mCuesOn   = argBool(it, end, mCuesOn);
            applyCueConfig();
        } else if (std::strcmp(addr, CLOCKWORK_SYS("osc/cue-server/cues-on")) == 0) {
            mCuesOn = argBool(it, end, mCuesOn);
            applyCueConfig();
        } else if (std::strcmp(addr, CLOCKWORK_SYS("osc/cue-server/loopback")) == 0) {
            mLoopback = argBool(it, end, mLoopback);
            applyCueConfig();
        }
    } catch (...) {
        // Malformed /clockwork/osc/ control message — ignore.
    }
    return true;
}
