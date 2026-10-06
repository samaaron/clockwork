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
#include "clockwork_config.h"   // clockwork_log
#include "OscBuilder.h"
#include "osc/OscOutboundPacketStream.h"
#include "osc/OscReceivedElements.h"
#include "DevicePolicy.h"
#include "clockwork_config.h"  // CLOCKWORK_COMMIT
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
            const OscPacket packet = oscPacketOf(512, [&](osc::OutboundPacketStream& s) {
                s << osc::BeginMessage(CLOCKWORK_SYS("clock/peer_name.reply"))
                  << mClockworkClock->peerName();
                if (hasEchoToken) s << static_cast<osc::int32>(echoToken);
                s << osc::EndMessage;
            });
            mEgress->reply(token, packet.ptr(), packet.size());
            return true;
        }

        if (std::strcmp(addr, CLOCKWORK_SYS("clock/audio/channels/get")) == 0) {
            // Reply /clockwork/clock/audio/channels.reply <count> [channelId channelName
            //   peerId peerName] * count.
            auto chs = mLinkAudio->listChannels();
            const OscPacket packet = oscPacketOf(8192, [&](osc::OutboundPacketStream& s) {
                s << osc::BeginMessage(CLOCKWORK_SYS("clock/audio/channels.reply"))
                  << static_cast<int32_t>(chs.size());
                for (const auto& c : chs) {
                    s << c.channelId.c_str() << c.channelName.c_str()
                      << c.peerId.c_str() << c.peerName.c_str();
                }
                s << osc::EndMessage;
            });
            mEgress->reply(token, packet.ptr(), packet.size());
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
            const OscPacket packet = oscPacketOf(4096, [&](osc::OutboundPacketStream& s) {
                s << osc::BeginMessage(CLOCKWORK_SYS("clock/audio/sinks.reply"))
                  << static_cast<int32_t>(sinks.size());
                for (const auto& as : sinks) {
                    s << as.name.c_str()
                      << static_cast<int32_t>(as.busIdx)
                      << static_cast<int32_t>(as.numChannels)
                      << static_cast<int32_t>(as.hasSubscriber ? 1 : 0);
                }
                s << osc::EndMessage;
            });
            mEgress->reply(token, packet.ptr(), packet.size());
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
// The first 64 bytes of a name, on a UTF-8 boundary, with "…" when there was
// more: enough to recognise in a reply, without carrying all of it.
static std::string cutName(const std::string& name) {
    constexpr size_t kKeep = 64;
    if (name.size() <= kKeep) return name;
    size_t n = kKeep;
    while (n > 0 && (static_cast<unsigned char>(name[n]) & 0xC0) == 0x80) --n;
    return name.substr(0, n) + "\xE2\x80\xA6";
}

void EngineControl::refuseUnknown(uint32_t token, const uint8_t* data, uint32_t size) {
    if (!mEgress) return;
    clockwork_sys_refuse(data, size, "unknown clockwork verb",
                   [this, token](const uint8_t* d, uint32_t n) { mEgress->reply(token, d, n); });
}

void EngineControl::finishSwitch(SwapResult result, const std::string& requestedOutput,
                                 const std::string& requestedInput) {
    // Where the engine is now, whichever path got it there and whatever that
    // path filled in.
    const auto cur = mEngine->currentDevice();
    if (result.deviceName.empty())      result.deviceName      = cur.name;
    if (result.inputDeviceName.empty()) result.inputDeviceName = cur.inputDeviceName;
    mEngine->sendSwitchDone(result, requestedOutput, requestedInput);
    if (result.success) mEngine->sendDeviceReport();
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
              << CLOCKWORK_COMMIT   // which Clockwork (it has no version number: clockwork_config.h)
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
            // Device questions are answered on the device lane, never here: the
            // device readers take the swap gate, and a cold swap holding it
            // waits for this pass to park (test_gateway_swap_gate.cpp). The
            // answer then describes the device any swap in flight leaves.
            mEngine->postDeviceTask([this, token] {
                for (auto& dev : mEngine->listDevices()) {
                    if (dev.wireless) continue;
                    const OscPacket packet = oscPacketOf(4096, [&](osc::OutboundPacketStream& s) {
                        s << osc::BeginMessage(CLOCKWORK_SYS("devices/list.reply"))
                          << dev.name.c_str()
                          << dev.typeName.c_str()
                          << static_cast<osc::int32>(dev.maxOutputChannels)
                          << static_cast<osc::int32>(dev.maxInputChannels);
                        for (auto r : dev.availableSampleRates)
                            s << static_cast<float>(r);
                        s << osc::EndMessage;
                    });
                    mEgress->reply(token, packet.ptr(), packet.size());
                }
                // Done marker
                char buf[256];
                osc::OutboundPacketStream s(buf, sizeof(buf));
                s << osc::BeginMessage(CLOCKWORK_SYS("devices/list.done")) << osc::EndMessage;
                mEgress->reply(token, reinterpret_cast<const uint8_t*>(s.Data()),
                          static_cast<uint32_t>(s.Size()));
            });
            return true;

        } else if (std::strcmp(addr, CLOCKWORK_SYS("devices/current")) == 0) {
            mEngine->postDeviceTask([this, token] {   // on the lane: see devices/list
                auto dev = mEngine->currentDevice();
                const OscPacket packet = oscPacketOf(1024, [&](osc::OutboundPacketStream& s) {
                    s << osc::BeginMessage(CLOCKWORK_SYS("devices/current.reply"))
                      << dev.name.c_str()
                      << dev.typeName.c_str()
                      << static_cast<float>(dev.activeSampleRate)
                      << static_cast<osc::int32>(dev.activeBufferSize)
                      << static_cast<osc::int32>(dev.activeOutputChannels)
                      << static_cast<osc::int32>(dev.activeInputChannels)
                      << osc::EndMessage;
                });
                mEgress->reply(token, packet.ptr(), packet.size());
            });
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

            // Every switch is acknowledged here (switch.reply 1: heard), does
            // its work on the device lane — a device change can take seconds
            // and the control pass must not — and ends in exactly one
            // switch.done broadcast: what was asked for, where the engine is
            // now, and why not when it failed.
            {
                char buf[128];
                osc::OutboundPacketStream s(buf, sizeof(buf));
                s << osc::BeginMessage(CLOCKWORK_SYS("devices/switch.reply"))
                  << static_cast<osc::int32>(1) << osc::EndMessage;
                mEgress->reply(token, reinterpret_cast<const uint8_t*>(s.Data()),
                          static_cast<uint32_t>(s.Size()));
            }

            // No device has a name of a kilobyte. A longer one is refused
            // here, and echoed cut: every switch ends in a switch.done, and
            // one carrying a name that long three times would not fit an
            // egress frame (OscEgress::frame), so the client would never hear.
            constexpr size_t kLongestDeviceName = 1024;
            if (devName.size() > kLongestDeviceName || inputDevName.size() > kLongestDeviceName) {
                SwapResult result;
                result.error = "no audio device has a name that long";
                finishSwitch(result, cutName(devName), cutName(inputDevName));
                return true;
            }

            // Follow the system default: the "__system__" sentinel, or the
            // device table's default-follow row picked by name (clients send
            // table names verbatim).
            if (devName == "__system__" || mEngine->isSyntheticDefaultPick(devName)) {
                mEngine->postDeviceTask([this, devName, inputDevName] {
                    SwapResult result;
                    result.error   = mEngine->setDeviceMode("");
                    result.success = result.error.empty();
                    finishSwitch(result, devName, inputDevName);
                });
                return true;
            }

            // Inputs off. A named output in the same message is not switched:
            // a known gap — routed through switchDevice it could open an ASIO
            // device output-only, which enableInputChannels refuses because it
            // crashes real drivers.
            // Turning inputs off leaves the output mode alone: in system mode the
            // engine goes on following the default. (It used to lock the output
            // into manual mode here, against a list-change reinit that system
            // mode no longer does.)
            if (inputDevName == "__none__") {
                mEngine->postDeviceTask([this, devName, inputDevName] {
                    finishSwitch(mEngine->enableInputChannels(0), devName, inputDevName);
                });
                return true;
            }

            // A device, a rate or a buffer: debounced, so rapid picks collapse
            // to the last; executePendingSwitch sends its switch.done.
            mEngine->scheduleDeviceSwitch(devName, inputDevName, sr, bufSz);
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
            const OscPacket packet = oscPacketOf(512, [&](osc::OutboundPacketStream& s) {
                s << osc::BeginMessage(CLOCKWORK_SYS("devices/reopen.reply"))
                  << static_cast<osc::int32>(accepted ? 1 : 0)
                  << reason.c_str() << osc::EndMessage;
            });
            mEgress->reply(token, packet.ptr(), packet.size());
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
                const OscPacket packet = oscPacketOf(1024, [&](osc::OutboundPacketStream& s) {
                    s << osc::BeginMessage(CLOCKWORK_SYS("devices/mode.reply"))
                      << mEngine->deviceMode().c_str()
                      << static_cast<osc::int32>(error.empty() ? 1 : 0);
                    if (!error.empty())
                        s << error.c_str();
                    s << osc::EndMessage;
                });
                mEgress->reply(token, packet.ptr(), packet.size());
            });
            return true;

        } else if (std::strcmp(addr, CLOCKWORK_SYS("drivers/list")) == 0) {
            mEngine->postDeviceTask([this, token] {   // on the lane: see devices/list
                auto drivers = mEngine->listDrivers();
                auto current = mEngine->currentDriver();
                const OscPacket packet = oscPacketOf(4096, [&](osc::OutboundPacketStream& s) {
                    s << osc::BeginMessage(CLOCKWORK_SYS("drivers/list.reply"));
                    s << current.c_str();
                    for (auto& d : drivers)
                        s << d.c_str();
                    s << osc::EndMessage;
                });
                mEgress->reply(token, packet.ptr(), packet.size());
            });
            return true;

        } else if (std::strcmp(addr, CLOCKWORK_SYS("drivers/switch")) == 0) {
            auto it = msg.ArgumentsBegin();
            std::string driverName;
            if (it != msg.ArgumentsEnd() && it->IsString())
                driverName = it->AsStringUnchecked();

            // On the device lane: a cross-driver change takes seconds (the
            // device layer itself waits 1.5 s between drivers).
            mEngine->postDeviceTask([this, token, driverName] {
                auto result = mEngine->switchDriver(driverName);
                const OscPacket packet = oscPacketOf(1024, [&](osc::OutboundPacketStream& s) {
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
                });
                mEgress->reply(token, packet.ptr(), packet.size());
                if (result.success)
                    mEngine->sendDeviceReport();
            });
            return true;

        } else if (std::strcmp(addr, CLOCKWORK_SYS("inputs/enable")) == 0) {
            // Enable/disable audio input channels.
            // Args: numChannels(int32) — 0 to disable, >0 to enable that many,
            // -1 to re-enable at the boot width.
            auto it = msg.ArgumentsBegin();
            int numChannels = 0;
            if (it != msg.ArgumentsEnd() && it->IsInt32())
                numChannels = it->AsInt32Unchecked();

            // On the device lane — the swap can take seconds — and with the
            // output mode left alone: in system mode the engine goes on
            // following the default.
            mEngine->postDeviceTask([this, token, numChannels] {
                auto result = mEngine->enableInputChannels(numChannels);
                const OscPacket packet = oscPacketOf(1024, [&](osc::OutboundPacketStream& s) {
                    s << osc::BeginMessage(CLOCKWORK_SYS("inputs/enable.reply"));
                    if (result.success) {
                        s << static_cast<osc::int32>(1)
                          << static_cast<osc::int32>(numChannels);
                    } else {
                        s << static_cast<osc::int32>(0) << result.error.c_str();
                    }
                    s << osc::EndMessage;
                });
                mEgress->reply(token, packet.ptr(), packet.size());
                if (result.success)
                    mEngine->sendDeviceReport();
            });
            return true;

        }
    } catch (const std::exception& e) {
        // Malformed: refused below, as an unknown verb is, and said here with
        // the address, which is the packet's first string.
        clockwork_log("[control] malformed %.*s (%u bytes): %s",
                      static_cast<int>(strnlen(reinterpret_cast<const char*>(data), size)),
                      reinterpret_cast<const char*>(data), size, e.what());
    } catch (...) {
        clockwork_log("[control] malformed %.*s (%u bytes)",
                      static_cast<int>(strnlen(reinterpret_cast<const char*>(data), size)),
                      reinterpret_cast<const char*>(data), size);
    }

    refuseUnknown(token, data, size);
    return true;
}
