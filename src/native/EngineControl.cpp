// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * EngineControl.cpp — the engine's own and the Link control endpoints, both
 * under clockwork's reserved prefix. Replies and
 * subscriber pushes go through the egress hub (mEgress); device/driver/link
 * state comes from the engine + ClockworkClock. (No record verbs: the engine
 * opens no files; a client records the master tap itself.)
 */
#include "EngineControl.h"

#include "clock/EngineClock.h"
#include "OscEgress.h"
#include "SubscribeAck.h"
#include "ClockworkEngine.h"
#include "clock/ClockworkClock.h"
#include "LinkAudioHost.h"
#include "clockwork_sys.h"
#include "osc/OscOutboundPacketStream.h"
#include "osc/OscReceivedElements.h"
#include "DevicePolicy.h"
#include "clockwork_config.h"  // CLOCKWORK_VERSION_MAJOR / _MINOR
#ifdef __APPLE__
#include "AggregateDeviceHelper.h"
#include "JuceAudioCallback.h"  // renderAudioBlock + the audio-width accessors
#endif
#include <juce_core/juce_core.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>



bool EngineControl::handleLinkCommand(const DrainCallCtx& meta, const uint8_t* data, uint32_t size) {
    const uint32_t token = meta.sourceId;
    if (!mClockworkClock || size < 16) return false;
    if (std::memcmp(data, CLOCKWORK_SYS("clock/"), CLOCKWORK_SYS_LEN("clock/")) != 0) return false;
    // Clock-core verbs (tempo/transport/sync/rpc/queries) go through the shared
    // handler — the same one the web worklet uses. Native-only Link-session
    // verbs (visibility, peers, Link Audio, notify) fall through to below.
    if (handleClockCoreOsc(*mClockworkClock, data, size,
            [this, token](const uint8_t* d, uint32_t n) { mEgress->reply(token, d, n); }))
        return true;
    try {
        osc::ReceivedPacket pkt(reinterpret_cast<const char*>(data),
                                static_cast<osc::osc_bundle_element_size_t>(size));
        osc::ReceivedMessage msg(pkt);
        const char* addr = msg.AddressPattern();

        // Correlation-token echo, same convention as EngineClock.cpp: a
        // request may carry an int32 as its last argument; the reply echoes
        // it as its last argument so clients can match replies exactly.
        int32_t echoToken = 0;
        bool    hasEchoToken = false;
        for (auto it = msg.ArgumentsBegin(); it != msg.ArgumentsEnd(); ++it) {
            auto next = it; ++next;
            if (next == msg.ArgumentsEnd() && it->IsInt32()) {
                hasEchoToken = true;
                echoToken = it->AsInt32Unchecked();
            }
        }

        if (std::strcmp(addr, CLOCKWORK_SYS("clock/visibility")) == 0) {
            // Write: /clockwork/clock/visibility <int 0|1|2> = Off | LoopbackOnly | NetworkWide.
            auto it = msg.ArgumentsBegin();
            if (it == msg.ArgumentsEnd() || !it->IsInt32()) return false;
            const int32_t mode = it->AsInt32Unchecked();
            using V = ClockworkClock::LinkVisibility;
            switch (mode) {
            case 0: mClockworkClock->setLinkVisibility(V::Off); break;
            case 1: mClockworkClock->setLinkVisibility(V::LoopbackOnly); break;
            case 2: mClockworkClock->setLinkVisibility(V::NetworkWide); break;
            default: return false;
            }
            return true;
        }

        if (std::strcmp(addr, CLOCKWORK_SYS("clock/visibility/get")) == 0) {
            // Read: reply /clockwork/clock/visibility.reply <int> to the sender.
            char buf[128];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage(CLOCKWORK_SYS("clock/visibility.reply"))
              << static_cast<int32_t>(mClockworkClock->getLinkVisibility());
            if (hasEchoToken) s << static_cast<osc::int32>(echoToken);
            s << osc::EndMessage;
            mEgress->reply(token, reinterpret_cast<const uint8_t*>(s.Data()),
                      static_cast<uint32_t>(s.Size()));
            return true;
        }

        if (std::strcmp(addr, CLOCKWORK_SYS("clock/audio/publish/set")) == 0) {
            // Write: /clockwork/clock/audio/publish/set <int 0|1>.
            auto it = msg.ArgumentsBegin();
            if (it == msg.ArgumentsEnd() || !it->IsInt32()) return false;
            mLinkAudio->setPublishEnabled(it->AsInt32Unchecked() != 0);
            return true;
        }

        if (std::strcmp(addr, CLOCKWORK_SYS("clock/audio/publish/get")) == 0) {
            char buf[64];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage(CLOCKWORK_SYS("clock/audio/publish.reply"))
              << static_cast<int32_t>(mLinkAudio->isPublishEnabled() ? 1 : 0);
            if (hasEchoToken) s << static_cast<osc::int32>(echoToken);
            s << osc::EndMessage;
            mEgress->reply(token, reinterpret_cast<const uint8_t*>(s.Data()),
                      static_cast<uint32_t>(s.Size()));
            return true;
        }

        if (std::strcmp(addr, CLOCKWORK_SYS("clock/peer_name/set")) == 0) {
            // Write: /clockwork/clock/peer_name/set <string>. Identifies us to other
            // Link peers (replaces the default engine name).
            auto it = msg.ArgumentsBegin();
            if (it == msg.ArgumentsEnd() || !it->IsString()) return false;
            mClockworkClock->setPeerName(it->AsStringUnchecked());
            return true;
        }

        if (std::strcmp(addr, CLOCKWORK_SYS("clock/peer_name/get")) == 0) {
            char buf[512];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage(CLOCKWORK_SYS("clock/peer_name.reply"))
              << mClockworkClock->peerName();
            if (hasEchoToken) s << static_cast<osc::int32>(echoToken);
            s << osc::EndMessage;
            mEgress->reply(token, reinterpret_cast<const uint8_t*>(s.Data()),
                      static_cast<uint32_t>(s.Size()));
            return true;
        }

        if (std::strcmp(addr, CLOCKWORK_SYS("clock/audio/channels/get")) == 0) {
            // Reply /clockwork/clock/audio/channels.reply <count> [channelId channelName
            //   peerId peerName] * count.
            auto chs = mLinkAudio->listChannels();
            std::vector<char> buf(8192);
            osc::OutboundPacketStream s(buf.data(), buf.size());
            s << osc::BeginMessage(CLOCKWORK_SYS("clock/audio/channels.reply"))
              << static_cast<int32_t>(chs.size());
            for (const auto& c : chs) {
                s << c.channelId.c_str() << c.channelName.c_str()
                  << c.peerId.c_str() << c.peerName.c_str();
            }
            s << osc::EndMessage;
            mEgress->reply(token, reinterpret_cast<const uint8_t*>(s.Data()),
                      static_cast<uint32_t>(s.Size()));
            return true;
        }

        if (std::strcmp(addr, CLOCKWORK_SYS("clock/audio/input/add")) == 0) {
            // /clockwork/clock/audio/input/add <peerName:string> <channelName:string>
            //   <busIdx:int32>
            // Reply /clockwork/clock/audio/input/add.reply <success:int 0|1>.
            // The arguments are still parsed so a client gets a
            // well-formed reply, but the verb is refused unconditionally —
            // see below for why.
            auto it = msg.ArgumentsBegin();
            const char* peerName = nullptr;
            const char* channelName = nullptr;
            int32_t busIdx = -1;
            if (it != msg.ArgumentsEnd() && it->IsString()) {
                peerName = it->AsStringUnchecked(); ++it;
            }
            if (it != msg.ArgumentsEnd() && it->IsString()) {
                channelName = it->AsStringUnchecked(); ++it;
            }
            if (it != msg.ArgumentsEnd() && it->IsInt32()) {
                busIdx = it->AsInt32Unchecked(); ++it;
            }
            /*
             * THE INDEX IS AN INPUT CHANNEL, and that is why this works again.
             *
             * It was refused unconditionally, and the reason was sound at the
             * time: a subscription claimed a pair of the DSP's PRIVATE signal
             * channels and had peer audio rendered into them each block. Both
             * halves needed the guest's internal channels — the validation
             * asked where the private region began, the render wrote straight
             * into it — and dsp_api.h exposes no such channels, because a DSP
             * need not have any addressable internal channels at all. There
             * was no index clockwork could validate and nothing to render
             * into, so accepting would have registered a subscription that
             * silently never sounded.
             *
             * Peer audio arrives through a source port bound to an INPUT
             * channel now (LinkAudioBridge), which is a thing clockwork owns
             * and can check. The bridge refuses an index that is not a free
             * channel pair, so a subscription that comes back 1 is one that
             * will actually sound.
             */
            const bool ok = (busIdx >= 0) && peerName && channelName
                          && mLinkAudio
                          && mLinkAudio->addInput(
                                 peerName, channelName,
                                 static_cast<uint32_t>(busIdx));
            char buf[64];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage(CLOCKWORK_SYS("clock/audio/input/add.reply"))
              << static_cast<int32_t>(ok ? 1 : 0)
              << osc::EndMessage;
            mEgress->reply(token, reinterpret_cast<const uint8_t*>(s.Data()),
                      static_cast<uint32_t>(s.Size()));
            return true;
        }

        if (std::strcmp(addr, CLOCKWORK_SYS("clock/audio/input/remove")) == 0) {
            // /clockwork/clock/audio/input/remove <peerName:string> <channelName:string>
            auto it = msg.ArgumentsBegin();
            const char* peerName = nullptr;
            const char* channelName = nullptr;
            if (it != msg.ArgumentsEnd() && it->IsString()) {
                peerName = it->AsStringUnchecked(); ++it;
            }
            if (it != msg.ArgumentsEnd() && it->IsString()) {
                channelName = it->AsStringUnchecked(); ++it;
            }
            if (peerName && channelName) {
                mLinkAudio->removeInput(peerName, channelName);
            }
            return true;
        }

        if (std::strcmp(addr, CLOCKWORK_SYS("clock/audio/input/clear")) == 0) {
            mLinkAudio->clearInputs();
            return true;
        }

        if (std::strcmp(addr, CLOCKWORK_SYS("clock/audio/input/latency/set")) == 0) {
            // /clockwork/clock/audio/input/latency/set <peer:str> <chan:str> <seconds:float>
            // Equivalent to Live's per-track latency slider (0..2 s).
            // Reply /clockwork/clock/audio/input/latency/set.reply <success:int 0|1>.
            auto it = msg.ArgumentsBegin();
            const char* peerName = nullptr;
            const char* channelName = nullptr;
            float seconds = -1.0f;
            if (it != msg.ArgumentsEnd() && it->IsString()) {
                peerName = it->AsStringUnchecked(); ++it;
            }
            if (it != msg.ArgumentsEnd() && it->IsString()) {
                channelName = it->AsStringUnchecked(); ++it;
            }
            if (it != msg.ArgumentsEnd() && it->IsFloat()) {
                seconds = it->AsFloatUnchecked(); ++it;
            }
            const bool ok = peerName && channelName && seconds >= 0.0f
                && mLinkAudio->setInputLatencySeconds(
                       peerName, channelName, seconds);
            char buf[64];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage(CLOCKWORK_SYS("clock/audio/input/latency/set.reply"))
              << static_cast<int32_t>(ok ? 1 : 0)
              << osc::EndMessage;
            mEgress->reply(token, reinterpret_cast<const uint8_t*>(s.Data()),
                      static_cast<uint32_t>(s.Size()));
            return true;
        }

        if (std::strcmp(addr, CLOCKWORK_SYS("clock/audio/sink/add")) == 0) {
            // /clockwork/clock/audio/sink/add <name:string> <busIdx:int> <numChannels:int>
            // → /clockwork/clock/audio/sink/add.reply <success:int 0|1>
            auto it = msg.ArgumentsBegin();
            const char* name = nullptr;
            int32_t busIdx = -1;
            int32_t numChans = -1;
            if (it != msg.ArgumentsEnd() && it->IsString()) {
                name = it->AsStringUnchecked(); ++it;
            }
            if (it != msg.ArgumentsEnd() && it->IsInt32()) {
                busIdx = it->AsInt32Unchecked(); ++it;
            }
            if (it != msg.ArgumentsEnd() && it->IsInt32()) {
                numChans = it->AsInt32Unchecked(); ++it;
            }
            const bool ok = name && busIdx >= 0 && numChans > 0
                && mLinkAudio->addSink(name,
                                                  static_cast<uint32_t>(busIdx),
                                                  static_cast<uint32_t>(numChans));
            char buf[64];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage(CLOCKWORK_SYS("clock/audio/sink/add.reply"))
              << static_cast<int32_t>(ok ? 1 : 0)
              << osc::EndMessage;
            mEgress->reply(token, reinterpret_cast<const uint8_t*>(s.Data()),
                      static_cast<uint32_t>(s.Size()));
            return true;
        }

        if (std::strcmp(addr, CLOCKWORK_SYS("clock/audio/sink/remove")) == 0) {
            auto it = msg.ArgumentsBegin();
            if (it != msg.ArgumentsEnd() && it->IsString()) {
                mLinkAudio->removeSink(it->AsStringUnchecked());
            }
            return true;
        }

        if (std::strcmp(addr, CLOCKWORK_SYS("clock/audio/sinks/get")) == 0) {
            // Reply /clockwork/clock/audio/sinks.reply <count>
            //   [name busIdx numChans hasSubscriber:int 0|1]*
            auto sinks = mLinkAudio->listSinks();
            std::vector<char> buf(4096);
            osc::OutboundPacketStream s(buf.data(), buf.size());
            s << osc::BeginMessage(CLOCKWORK_SYS("clock/audio/sinks.reply"))
              << static_cast<int32_t>(sinks.size());
            for (const auto& as : sinks) {
                s << as.name.c_str()
                  << static_cast<int32_t>(as.busIdx)
                  << static_cast<int32_t>(as.numChannels)
                  << static_cast<int32_t>(as.hasSubscriber ? 1 : 0);
            }
            s << osc::EndMessage;
            mEgress->reply(token, reinterpret_cast<const uint8_t*>(s.Data()),
                      static_cast<uint32_t>(s.Size()));
            return true;
        }

        if (std::strcmp(addr, CLOCKWORK_SYS("clock/audio/inputs/get")) == 0) {
            // Reply /clockwork/clock/audio/inputs.reply <count>
            //   [peerName:s channelName:s busIdx:i sampleRate:i
            //    sourceNumChannels:i (1 or 2, 0 until first buffer)
            //    bufferedMs:f connectionState:i (0..3)
            //    droppedSourceBuffers:i networkGapBuffers:i
            //    totalSourceBufferCalls:i duplicateCountCalls:i
            //    latencySeconds:f]*
            auto inputs = mLinkAudio->listInputs();
            // Pre-size for the actual reply; 512 B/entry is a generous
            // upper bound (two ≤64-char strings + 7 int32 + 2 floats +
            // tag/alignment). Avoids overflow when many subs or long
            // peer/channel names are present.
            constexpr size_t kBytesPerInputEntry = 512;
            const size_t bufSize = 1024 + inputs.size() * kBytesPerInputEntry;
            std::vector<char> buf(bufSize);
            osc::OutboundPacketStream s(buf.data(), buf.size());
            s << osc::BeginMessage(CLOCKWORK_SYS("clock/audio/inputs.reply"))
              << static_cast<int32_t>(inputs.size());
            for (const auto& st : inputs) {
                s << st.peerName.c_str()
                  << st.channelName.c_str()
                  << static_cast<int32_t>(st.busIdx)
                  << static_cast<int32_t>(st.sampleRate)
                  << static_cast<int32_t>(st.sourceNumChannels)
                  << (st.bufferedSeconds * 1000.0f)
                  << static_cast<int32_t>(st.state)
                  << static_cast<int32_t>(std::min<uint64_t>(
                         st.droppedSourceBuffers,
                         static_cast<uint64_t>(INT32_MAX)))
                  << static_cast<int32_t>(std::min<uint64_t>(
                         st.networkGapBuffers,
                         static_cast<uint64_t>(INT32_MAX)))
                  << static_cast<int32_t>(std::min<uint64_t>(
                         st.totalSourceBufferCalls,
                         static_cast<uint64_t>(INT32_MAX)))
                  << static_cast<int32_t>(std::min<uint64_t>(
                         st.duplicateCountCalls,
                         static_cast<uint64_t>(INT32_MAX)))
                  << static_cast<float>(st.latencySeconds);
            }
            s << osc::EndMessage;
            mEgress->reply(token, reinterpret_cast<const uint8_t*>(s.Data()),
                      static_cast<uint32_t>(s.Size()));
            return true;
        }


        if (std::strcmp(addr, CLOCKWORK_SYS("clock/reset")) == 0) {
            // Cycle through setLinkVisibility so the full Link Audio
            // teardown (inputSubs, auxSinks, main sink, threadPriority,
            // enableLinkAudio) runs — setLinkEnabled alone leaves all
            // of those wired to the old session.
            const auto prev = mClockworkClock->getLinkVisibility();
            mClockworkClock->setLinkVisibility(ClockworkClock::LinkVisibility::Off);
            if (prev != ClockworkClock::LinkVisibility::Off) {
                mClockworkClock->setLinkVisibility(prev);
            }
            return true;
        }


        if (std::strcmp(addr, CLOCKWORK_SYS("clock/notify/subscribe")) == 0) {
            // Subscribe the caller to Link events, then push a tempo + peers
            // snapshot immediately so it starts in sync. Link's change-callbacks
            // only fire on actual deltas — without this, joining a session at the
            // same tempo as our local default leaves the subscriber stuck on its
            // own default until something moves.
            // Subscribing is idempotent and clients confirm by resending
            // with a token until acked — so ack and snapshot regardless of
            // whether this call newly registered the caller. (Gating the ack
            // on "newly registered" made every confirm after the first
            // untokened subscribe time out.)
            mEgress->subscribeCallerToLinkNotify(token);
            ackSubscribeIfTokened(mEgress, token, data, size,
                                  CLOCKWORK_SYS("clock/notify/subscribe.reply"));
            const double initTempo = mClockworkClock->getBpm();
            const int32_t initPeers = static_cast<int32_t>(mClockworkClock->numPeers());
            {
                char buf[64];
                osc::OutboundPacketStream s(buf, sizeof(buf));
                s << osc::BeginMessage(CLOCKWORK_SYS("clock/notify/tempo")) << initTempo
                  << osc::EndMessage;
                mEgress->sendToCaller(token, reinterpret_cast<const uint8_t*>(s.Data()),
                                   static_cast<uint32_t>(s.Size()));
            }
            {
                char buf[64];
                osc::OutboundPacketStream s(buf, sizeof(buf));
                s << osc::BeginMessage(CLOCKWORK_SYS("clock/notify/peers")) << initPeers
                  << osc::EndMessage;
                mEgress->sendToCaller(token, reinterpret_cast<const uint8_t*>(s.Data()),
                                   static_cast<uint32_t>(s.Size()));
            }
            return true;
        }

        if (std::strcmp(addr, CLOCKWORK_SYS("clock/notify/unsubscribe")) == 0) {
            mEgress->unsubscribeCallerFromLinkNotify(token);
            return true;
        }

        if (std::strcmp(addr, CLOCKWORK_SYS("clock/peers/get")) == 0) {
            // Read: reply /clockwork/clock/peers.reply <count> [<nodeId> <gatewayIp>
            //   <isLoopback:int 0|1> <measurementIp> <measurementPort>
            //   <audioIp> <audioPort>] * count.
            // audioIp is "" when peer has no Link Audio capability.
            auto peers = mClockworkClock->listPeers();
            // IPv6 peers cost ~200 bytes per entry (four address strings
            // + 16-hex nodeId + two ports + isLoopback + alignment).
            // 32 KiB covers ~150 peers.
            std::vector<char> buf(32768);
            osc::OutboundPacketStream s(buf.data(), buf.size());
            s << osc::BeginMessage(CLOCKWORK_SYS("clock/peers.reply"))
              << static_cast<int32_t>(peers.size());
            for (const auto& p : peers) {
                s << p.nodeId.c_str()
                  << p.gatewayIp.c_str()
                  << static_cast<int32_t>(p.isLoopback ? 1 : 0)
                  << p.measurementIp.c_str()
                  << static_cast<int32_t>(p.measurementPort)
                  << p.audioIp.c_str()
                  << static_cast<int32_t>(p.audioPort);
            }
            s << osc::EndMessage;
            mEgress->reply(token, reinterpret_cast<const uint8_t*>(s.Data()),
                      static_cast<uint32_t>(s.Size()));
            return true;
        }
        // A /clock verb nothing owns (typo, or a verb from a newer client):
        // refuse explicitly instead of dropping silently, via the same
        // helper as the WASM route. Pair with /clockwork/clock/capabilities/get.
        replyClockUnsupported(data, size,
            [this, token](const uint8_t* d, uint32_t n) {
                mEgress->reply(token, d, n);
            });
        return true;
    } catch (...) {
        // The packet was /clockwork/clock/* but reply generation threw (typically
        // OutboundPacketStream overflow). Return true to consume it
        // rather than letting handlePacket forward it to the DSP.
        return true;
    }
    return false;
}

// "/clockwork/error ,ss <address> <reason>" — clockwork refusing one of its own
// addresses. Byte-identical to the audio thread's refusal (clockwork_sys.h owns the
// shape); only the thread it is emitted from differs.
void EngineControl::refuseUnknown(uint32_t token, const uint8_t* data, uint32_t size) {
    if (!mEgress) return;
    clockwork_sys_refuse(data, size, "unknown clockwork verb",
                   [this, token](const uint8_t* d, uint32_t n) { mEgress->reply(token, d, n); });
}

// The FALLBACK for the reserved prefix on the NRT thread: the engine's own
// top-level verbs, and — because this is the last link in clockwork's chain —
// the refusal for a claimed address nobody recognised. The prefix is claimed
// whole, so an unknown verb under it is answered with an error and never falls
// through to the DSP.
bool EngineControl::handleEngineCommand(const DrainCallCtx& meta, const uint8_t* data, uint32_t size) {
    const uint32_t token = meta.sourceId;
    if (!clockwork_sys_claims(data, size)) return false;
    if (!mEngine || size < CLOCKWORK_SYS_PREFIX_LEN + 4u) { refuseUnknown(token, data, size); return true; }

    try {
        osc::ReceivedPacket pkt(reinterpret_cast<const char*>(data),
                                static_cast<osc::osc_bundle_element_size_t>(size));
        osc::ReceivedMessage msg(pkt);
        const char* addr = msg.AddressPattern();

        if (std::strcmp(addr, CLOCKWORK_SYS("notify")) == 0) {
            // Register the caller as a notify target for lifecycle events.
            // Registration is device-free by design: enumerating devices here
            // put a Windows COM probe of every device (pathological with
            // virtual drivers like Voicemeeter installed) directly in the path
            // of the client's boot handshake. Clients that want the
            // device list ask for it with /clockwork/devices/report.
            mEgress->subscribeCaller(token);
            char buf[128];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage(CLOCKWORK_SYS("notify.reply"))
              << static_cast<osc::int32>(1)
              << CLOCKWORK_VERSION_STRING
              << osc::EndMessage;
            mEgress->reply(token, reinterpret_cast<const uint8_t*>(s.Data()),
                      static_cast<uint32_t>(s.Size()));
            // Replay current lifecycle state to the new registrant: stream
            // clients connect only after init and missed the boot broadcasts
            // (a scheduling client ignores boot-time replays; the GUI needs them to leave
            // its "waiting" state).
            mEngine->snapshotStateTo(token);
            return true;

        } else if (std::strcmp(addr, CLOCKWORK_SYS("notify/unregister")) == 0) {
            // Remove the caller from notify targets (polite shutdown).
            mEgress->unsubscribeCaller(token);
            return true;

        } else if (std::strcmp(addr, CLOCKWORK_SYS("notify/clear")) == 0) {
            // Remove all notify targets (used before a client restart).
            mEgress->clearSubscribers();
            return true;

        } else if (std::strcmp(addr, CLOCKWORK_SYS("devices/list")) == 0) {
            auto devices = mEngine->listDevices();
            for (auto& dev : devices) {
                if (dev.isWirelessTransport()) continue;
                char buf[4096];
                osc::OutboundPacketStream s(buf, sizeof(buf));
                s << osc::BeginMessage(CLOCKWORK_SYS("devices/list.reply"))
                  << dev.name.c_str()
                  << dev.typeName.c_str()
                  << static_cast<osc::int32>(dev.maxOutputChannels)
                  << static_cast<osc::int32>(dev.maxInputChannels);
                for (auto r : dev.availableSampleRates)
                    s << static_cast<float>(r);
                s << osc::EndMessage;
                mEgress->reply(token, reinterpret_cast<const uint8_t*>(s.Data()),
                          static_cast<uint32_t>(s.Size()));
            }
            // Done marker
            char buf[256];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage(CLOCKWORK_SYS("devices/list.done")) << osc::EndMessage;
            mEgress->reply(token, reinterpret_cast<const uint8_t*>(s.Data()),
                      static_cast<uint32_t>(s.Size()));
            return true;

        } else if (std::strcmp(addr, CLOCKWORK_SYS("devices/current")) == 0) {
            auto dev = mEngine->currentDevice();
            char buf[1024];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage(CLOCKWORK_SYS("devices/current.reply"))
              << dev.name.c_str()
              << dev.typeName.c_str()
              << static_cast<float>(dev.activeSampleRate)
              << static_cast<osc::int32>(dev.activeBufferSize)
              << static_cast<osc::int32>(dev.activeOutputChannels)
              << static_cast<osc::int32>(dev.activeInputChannels)
              << osc::EndMessage;
            mEgress->reply(token, reinterpret_cast<const uint8_t*>(s.Data()),
                      static_cast<uint32_t>(s.Size()));
            return true;

        } else if (std::strcmp(addr, CLOCKWORK_SYS("devices/switch")) == 0) {
            // Args: outputDevice(str), sampleRate(float), bufferSize(int32), [inputDevice(str)]
            auto it = msg.ArgumentsBegin();
            std::string devName, inputDevName;
            double sr = 0;
            int bufSz = 0;
            if (it != msg.ArgumentsEnd() && it->IsString()) {
                devName = it->AsStringUnchecked(); ++it;
            }
            if (it != msg.ArgumentsEnd() && it->IsFloat()) {
                sr = it->AsFloatUnchecked(); ++it;
            }
            if (it != msg.ArgumentsEnd() && it->IsInt32()) {
                bufSz = it->AsInt32Unchecked(); ++it;
            }
            if (it != msg.ArgumentsEnd() && it->IsString()) {
                inputDevName = it->AsStringUnchecked();
            }

            // "__system__" sentinel means "follow system default output" —
            // as does picking the device table's synthetic default-follow
            // row by name (the GUI sends table names verbatim).
            // Off the gateway: the reinit plus its report can take seconds.
            if (devName == "__system__" || mEngine->isSyntheticDefaultPick(devName)) {
                mEngine->postDeviceTask([this, token] {
                    auto error = mEngine->setDeviceMode("");
                    char buf[1024];
                    osc::OutboundPacketStream s(buf, sizeof(buf));
                    s << osc::BeginMessage(CLOCKWORK_SYS("devices/switch.reply"))
                      << static_cast<osc::int32>(error.empty() ? 1 : 0);
                    if (!error.empty()) s << error.c_str();
                    s << osc::EndMessage;
                    mEgress->reply(token, reinterpret_cast<const uint8_t*>(s.Data()),
                              static_cast<uint32_t>(s.Size()));
                    if (error.empty()) mEngine->sendDeviceReport();
                });
                return true;
            }

            // "__none__" sentinel from GUI means "disable audio inputs"
            if (inputDevName == "__none__") {
                // Lock device mode so changeListenerCallback doesn't interfere
                auto curDev = mEngine->currentDevice();
                if (!curDev.name.empty())
                    mEngine->forceDeviceMode(curDev.name);
                auto result = mEngine->enableInputChannels(0);
                char buf[1024];
                osc::OutboundPacketStream s(buf, sizeof(buf));
                s << osc::BeginMessage(CLOCKWORK_SYS("devices/switch.reply"))
                  << static_cast<osc::int32>(result.success ? 1 : 0);
                if (!result.success) s << result.error.c_str();
                s << osc::EndMessage;
                mEgress->reply(token, reinterpret_cast<const uint8_t*>(s.Data()),
                          static_cast<uint32_t>(s.Size()));
                if (result.success) mEngine->sendDeviceReport();
                return true;
            }

            // Debounce: the transport stores the request and runs only the
            // last one after a quiet period.
            mEngine->scheduleDeviceSwitch(devName, inputDevName, sr, bufSz);

            // Ack immediately so the GUI knows we received it
            {
                char buf[128];
                osc::OutboundPacketStream s(buf, sizeof(buf));
                s << osc::BeginMessage(CLOCKWORK_SYS("devices/switch.reply"))
                  << static_cast<osc::int32>(1) << osc::EndMessage;
                mEgress->reply(token, reinterpret_cast<const uint8_t*>(s.Data()),
                          static_cast<uint32_t>(s.Size()));
            }
            return true;

        } else if (std::strcmp(addr, CLOCKWORK_SYS("devices/reopen")) == 0) {
            // Reopen the current device to re-read its properties (e.g. the
            // user just bumped channel count in MOTU Pro Audio Control). The
            // transport gates this — rejected while a reopen is in flight or
            // within the cooldown after one completes. Replies: .reply
            // (accepted=1|0 + reason) immediately; .done (success, device,
            // rate, buffer, error) when the swap finishes.
            std::string reason;
            const bool accepted = mEngine->requestAudioRecovery(reason);
            char buf[512];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage(CLOCKWORK_SYS("devices/reopen.reply"))
              << static_cast<osc::int32>(accepted ? 1 : 0)
              << reason.c_str() << osc::EndMessage;
            mEgress->reply(token, reinterpret_cast<const uint8_t*>(s.Data()),
                      static_cast<uint32_t>(s.Size()));
            return true;

        } else if (std::strcmp(addr, CLOCKWORK_SYS("devices/report")) == 0) {
            // Register the caller for /clockwork/devices pushes. Two forms:
            //   - port arg > 0: UDP legacy — send to that port
            //   - no arg / 0:   connection-oriented transports (TCP/UDS/pipe)
            //                   — subscribe the caller's own connection, since
            //                   ports don't address a stream peer
            auto it = msg.ArgumentsBegin();
            int replyPort = 0;
            if (it != msg.ArgumentsEnd() && it->IsInt32()) {
                replyPort = it->AsInt32Unchecked();
            }
            if (replyPort > 0) {
                mEgress->subscribeNotifyPort(replyPort);
            } else {
                // Connection-oriented transports (TCP/UDS/pipe) have no
                // addressable reply port — register the caller's own
                // connection, same audience as /clockwork/notify.
                mEgress->subscribeCaller(token);
            }
            // listDevices() is the ~10 s Windows COM probe — never inline here.
            mEngine->postDeviceTask([this] { mEngine->sendDeviceReport(); });
            return true;

        } else if (std::strcmp(addr, CLOCKWORK_SYS("devices/mode")) == 0) {
            auto it = msg.ArgumentsBegin();
            std::string mode;
            if (it != msg.ArgumentsEnd() && it->IsString()) {
                mode = it->AsStringUnchecked();
            }

            // Off the gateway: setDeviceMode reinitialises the device.
            mEngine->postDeviceTask([this, token, mode] {
                auto error = mEngine->setDeviceMode(mode);
                char buf[1024];
                osc::OutboundPacketStream s(buf, sizeof(buf));
                s << osc::BeginMessage(CLOCKWORK_SYS("devices/mode.reply"))
                  << mEngine->deviceMode().c_str()
                  << static_cast<osc::int32>(error.empty() ? 1 : 0);
                if (!error.empty())
                    s << error.c_str();
                s << osc::EndMessage;
                mEgress->reply(token, reinterpret_cast<const uint8_t*>(s.Data()),
                          static_cast<uint32_t>(s.Size()));
            });
            return true;

        } else if (std::strcmp(addr, CLOCKWORK_SYS("drivers/list")) == 0) {
            auto drivers = mEngine->listDrivers();
            auto current = mEngine->currentDriver();
            char buf[4096];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage(CLOCKWORK_SYS("drivers/list.reply"));
            s << current.c_str();
            for (auto& d : drivers)
                s << d.c_str();
            s << osc::EndMessage;
            mEgress->reply(token, reinterpret_cast<const uint8_t*>(s.Data()),
                      static_cast<uint32_t>(s.Size()));
            return true;

        } else if (std::strcmp(addr, CLOCKWORK_SYS("drivers/switch")) == 0) {
            auto it = msg.ArgumentsBegin();
            std::string driverName;
            if (it != msg.ArgumentsEnd() && it->IsString())
                driverName = it->AsStringUnchecked();

            auto result = mEngine->switchDriver(driverName);
            char buf[1024];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage(CLOCKWORK_SYS("drivers/switch.reply"));
            if (result.success) {
                s << static_cast<osc::int32>(1)
                  << mEngine->currentDriver().c_str()
                  << static_cast<float>(result.sampleRate)
                  << static_cast<osc::int32>(result.bufferSize);
            } else {
                s << static_cast<osc::int32>(0) << result.error.c_str();
            }
            s << osc::EndMessage;
            mEgress->reply(token, reinterpret_cast<const uint8_t*>(s.Data()),
                      static_cast<uint32_t>(s.Size()));
            if (result.success)
                mEngine->sendDeviceReport();
            return true;

        } else if (std::strcmp(addr, CLOCKWORK_SYS("inputs/enable")) == 0) {
            // Enable/disable audio input channels.
            // Args: numChannels(int32) — 0 to disable, >0 to enable that many channels
            auto it = msg.ArgumentsBegin();
            int numChannels = 0;
            if (it != msg.ArgumentsEnd() && it->IsInt32())
                numChannels = it->AsInt32Unchecked();

            // Lock device mode so changeListenerCallback doesn't interfere
            auto curDev = mEngine->currentDevice();
            if (!curDev.name.empty())
                mEngine->forceDeviceMode(curDev.name);

            auto result = mEngine->enableInputChannels(numChannels);
            char buf[1024];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage(CLOCKWORK_SYS("inputs/enable.reply"));
            if (result.success) {
                s << static_cast<osc::int32>(1)
                  << static_cast<osc::int32>(numChannels);
            } else {
                s << static_cast<osc::int32>(0) << result.error.c_str();
            }
            s << osc::EndMessage;
            mEgress->reply(token, reinterpret_cast<const uint8_t*>(s.Data()),
                      static_cast<uint32_t>(s.Size()));
            if (result.success)
                mEngine->sendDeviceReport();
            return true;

        }
    } catch (...) {
        // Don't let parsing errors crash the server
    }

    refuseUnknown(token, data, size);
    return true;
}
