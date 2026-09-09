// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * plugin_bridge_verbs.cpp — see plugin_bridge_verbs.h for the two halves.
 */
#include "plugin_bridge_verbs.h"

#include "plugin_discovery.h"
#include "plugin_host.h"
#include "clockwork_sys.h"
#include "osc/OscOutboundPacketStream.h"
#include "osc/OscReceivedElements.h"

#include <cstdio>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace {

// A track argument is an id (int) from a GUI or a name (string) from code.
// Resolved on the snapshot either way, so this is safe on the audio thread.
ClockworkTrackId trackArg(osc::ReceivedMessage::const_iterator& it,
                    const osc::ReceivedMessage::const_iterator& end) {
    if (it == end) return CLOCKWORK_TRACK_NONE;
    if (it->IsInt32())  { const int v = it->AsInt32Unchecked(); ++it; return v > 0 ? static_cast<ClockworkTrackId>(v) : CLOCKWORK_TRACK_NONE; }
    if (it->IsString()) { const char* s = it->AsStringUnchecked(); ++it; return clockwork_track_resolve(s); }
    return CLOCKWORK_TRACK_NONE;
}

int intArg(osc::ReceivedMessage::const_iterator& it,
           const osc::ReceivedMessage::const_iterator& end, int dflt) {
    if (it == end) return dflt;
    if (it->IsInt32()) { const int v = it->AsInt32Unchecked(); ++it; return v; }
    if (it->IsFloat()) { const float v = it->AsFloatUnchecked(); ++it; return static_cast<int>(v); }
    return dflt;
}

double numArg(osc::ReceivedMessage::const_iterator& it,
              const osc::ReceivedMessage::const_iterator& end, double dflt) {
    if (it == end) return dflt;
    if (it->IsFloat())  { const double v = it->AsFloatUnchecked();  ++it; return v; }
    if (it->IsDouble()) { const double v = it->AsDoubleUnchecked(); ++it; return v; }
    if (it->IsInt32())  { const double v = it->AsInt32Unchecked();  ++it; return v; }
    return dflt;
}

std::string strArg(osc::ReceivedMessage::const_iterator& it,
                   const osc::ReceivedMessage::const_iterator& end) {
    if (it == end || !it->IsString()) return {};
    const char* s = it->AsStringUnchecked(); ++it;
    return s ? s : "";
}

// A velocity or controller value: 0..1 as a float, or 0..127 as an int, the
// two forms code naturally has to hand. Both formats carry it as a float and
// converting through 0..127 would only lose steps, so the float is native.
float unitArg(osc::ReceivedMessage::const_iterator& it,
              const osc::ReceivedMessage::const_iterator& end, float dflt) {
    if (it == end) return dflt;
    if (it->IsFloat())  { const float v = it->AsFloatUnchecked(); ++it; return v; }
    if (it->IsDouble()) { const float v = static_cast<float>(it->AsDoubleUnchecked()); ++it; return v; }
    if (it->IsInt32())  { const int v = it->AsInt32Unchecked(); ++it; return static_cast<float>(v) / 127.0f; }
    return dflt;
}

}  // namespace

void TrackVerbs::init(Emit* emit, double sampleRate) {
    mEmit = emit;
    setSampleRate(sampleRate);
    clockwork_track_set_param_edit_listener(&TrackVerbs::onParamEdit, this);
}

void TrackVerbs::shutdown() {
    clockwork_track_set_param_edit_listener(nullptr, nullptr);
    clockwork_track_clear();
    mEmit = nullptr;
}

// ── Audio thread ─────────────────────────────────────────────────────────────

bool TrackVerbs::applyRealtime(const uint8_t* data, uint32_t len, uint32_t frameOffset) {
    try {
        osc::ReceivedMessage msg(osc::ReceivedPacket(
            reinterpret_cast<const char*>(data),
            static_cast<osc::osc_bundle_element_size_t>(len)));
        const char* addr = msg.AddressPattern();
        if (std::strncmp(addr, CLOCKWORK_SYS("track/"), CLOCKWORK_SYS_LEN("track/")) != 0) return false;
        const char* verb = addr + CLOCKWORK_SYS_LEN("track/");
        auto it = msg.ArgumentsBegin();
        const auto end = msg.ArgumentsEnd();
        const uint32_t off = frameOffset;

        if (std::strcmp(verb, "note") == 0) {
            // /clockwork/track/note <track> <on:int> <pitch:int> <velocity> [channel:int]
            const ClockworkTrackId t = trackArg(it, end);
            const int on = intArg(it, end, 1);
            const int pitch = intArg(it, end, 60);
            const float vel = unitArg(it, end, 0.8f);
            const int ch = intArg(it, end, 0);
            clockwork_track_note(t, on, static_cast<int16_t>(ch & 15), static_cast<int16_t>(pitch), vel, off);
            return true;
        }
        if (std::strcmp(verb, "cc") == 0) {
            // /clockwork/track/cc <track> <number:int> <value> [channel:int]
            const ClockworkTrackId t = trackArg(it, end);
            const int num = intArg(it, end, 1);
            const float v = unitArg(it, end, 0.0f);
            const int ch = intArg(it, end, 0);
            clockwork_track_cc(t, static_cast<int16_t>(ch & 15), static_cast<uint8_t>(num & 127), v, off);
            return true;
        }
        if (std::strcmp(verb, "bend") == 0) {
            // /clockwork/track/bend <track> <bend:float -1..1> [channel:int]
            const ClockworkTrackId t = trackArg(it, end);
            const float b = static_cast<float>(numArg(it, end, 0.0));
            const int ch = intArg(it, end, 0);
            clockwork_track_pitch_bend(t, static_cast<int16_t>(ch & 15), b, off);
            return true;
        }
        if (std::strcmp(verb, "notes_off") == 0) {
            // /clockwork/track/notes_off [track]
            // No track, or "*": every track — a stop.
            const bool all = it == end || (it->IsString() && std::strcmp(it->AsStringUnchecked(), "*") == 0);
            clockwork_track_all_notes_off(all ? CLOCKWORK_TRACK_NONE : trackArg(it, end), off);
            return true;
        }
        if (std::strcmp(verb, "param") == 0) {
            // /clockwork/track/param <track> <name:string> <value> [handle:int]
            // By NAME: what code says. The first node in the chain with a
            // parameter so called takes it, unless a handle narrows it.
            const ClockworkTrackId t = trackArg(it, end);
            const std::string name = strArg(it, end);
            const double v = numArg(it, end, 0.0);
            const int h = intArg(it, end, 0);
            clockwork_track_param_by_name(t, static_cast<ClockworkTrackHandle>(h > 0 ? h : 0), name.c_str(), v, off);
            return true;
        }
        if (std::strcmp(verb, "plugin/param") == 0) {
            // /clockwork/track/plugin/param <handle:int> <id:int> <value>
            // By ID: what a panel says, having listed the parameters.
            const int h = intArg(it, end, 0);
            const int id = intArg(it, end, 0);
            const double v = numArg(it, end, 0.0);
            clockwork_track_node_param(static_cast<ClockworkTrackHandle>(h), static_cast<uint32_t>(id), v, off);
            return true;
        }
    } catch (...) {
        return true;   // malformed: consumed, never the DSP's
    }
    return false;
}

// ── Edits coming back from a plugin ──────────────────────────────────────────

void TrackVerbs::onParamEdit(void* ctx, ClockworkTrackHandle h, uint32_t id, double normalized, int own) {
    auto* self = static_cast<TrackVerbs*>(ctx);
    if (!self || !self->mEmit) return;
    // /clockwork/track/plugin/param/edit  <handle:int> <id:int> <value:float>
    // /clockwork/track/plugin/param/value <handle:int> <id:int> <value:float>
    // The mirror of track/plugin/param, argument for argument. `value` is
    // NORMALISED 0..1, the plugin's native form for an edit, so a client can
    // echo one straight back. Broadcast: neither answers a request. An
    // `edit` is the plugin's own doing (its editor window) — a panel with
    // Configure armed takes one as "put this control on the face"; a
    // `value` is the host's, set by name from code (track/param), and says
    // only what the parameter is now.
    char buf[128];
    osc::OutboundPacketStream s(buf, sizeof buf);
    s << osc::BeginMessage(own ? CLOCKWORK_SYS("track/plugin/param/edit")
                               : CLOCKWORK_SYS("track/plugin/param/value"))
      << static_cast<osc::int32>(h)
      << static_cast<osc::int32>(id)
      << static_cast<float>(normalized)
      << osc::EndMessage;
    self->mEmit->broadcast(reinterpret_cast<const uint8_t*>(s.Data()),
                           static_cast<uint32_t>(s.Size()));
}

// ── State pushes ─────────────────────────────────────────────────────────────

void TrackVerbs::sendTracks(const char* address, uint32_t replyToken) {
    if (!mEmit) return;
    /*
     * /clockwork/track/list <lane_base:int> <count:int> then per track:
     *     <id:int> <slot:int> <name:string> <send_channel:int>
     *     <return_channel:int> <gain:float> <mute:int> <node_count:int>
     *   then per node:
     *     <handle:int> <is_instrument:int> <bypass:int> <channel:int>
     *     <name:string> <vendor:string> <format:string> <path:string>
     *     <index:int> <latency:int>
     *   then, after the last track, per track again in the same order:
     *     <timeline:string>
     *
     * Tracks in display order, nodes in chain order — the order IS the signal
     * path. Channels are 0-based clockwork channels; a client that speaks
     * 1-based buses adds one. lane_base is 0 until the engine has booted
     * with room for the lanes, and a client that sees 0 should say so
     * rather than compute channels from it.
     *
     * The timelines trail the message rather than sit with the track's
     * other fields because the readers of this payload are positional and
     * outside this repository: a field inserted before the nodes would
     * shift every one of them. A reader is NOT untouched by what follows,
     * though, if it insists on "no more arguments" once it has counted
     * the nodes — Sonic Pi's did, and rejected every list that had a
     * track in it (2026-09-07). Trailing fields still need their readers.
     */
    const uint32_t n = clockwork_track_list(nullptr, 0);
    std::vector<ClockworkTrackInfo> tracks(n ? n : 1);
    clockwork_track_list(tracks.data(), n);

    std::vector<std::vector<ClockworkTrackNode>> chains(n);
    size_t bytes = 256;
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t k = clockwork_track_nodes(tracks[i].id, nullptr, 0);
        chains[i].resize(k ? k : 1);
        clockwork_track_nodes(tracks[i].id, chains[i].data(), k);
        chains[i].resize(k);
        bytes += sizeof(ClockworkTrackInfo) + 64;
        for (const ClockworkTrackNode& nd : chains[i])
            bytes += std::strlen(nd.name) + std::strlen(nd.vendor) + std::strlen(nd.path)
                   + std::strlen(nd.id) + 96;
    }

    // Sized for the real message: a full studio of long bundle paths does not
    // fit a fixed kilobyte, and a truncated OSC packet is worse than none.
    std::vector<char> buf(bytes);
    osc::OutboundPacketStream s(buf.data(), buf.size());
    s << osc::BeginMessage(address)
      << static_cast<osc::int32>(clockwork_track_lane_base())
      << static_cast<osc::int32>(n);
    for (uint32_t i = 0; i < n; ++i) {
        const ClockworkTrackInfo& t = tracks[i];
        s << static_cast<osc::int32>(t.id)
          << static_cast<osc::int32>(t.slot)
          << t.name
          << static_cast<osc::int32>(t.send_channel)
          << static_cast<osc::int32>(t.return_channel)
          << t.gain
          << static_cast<osc::int32>(t.mute)
          << static_cast<osc::int32>(chains[i].size());
        for (const ClockworkTrackNode& nd : chains[i]) {
            s << static_cast<osc::int32>(nd.handle)
              << static_cast<osc::int32>(nd.is_instrument)
              << static_cast<osc::int32>(nd.bypass)
              << static_cast<osc::int32>(nd.channel)
              << nd.name
              << nd.vendor
              << nd.format
              << nd.path
              << static_cast<osc::int32>(nd.index)
              << static_cast<osc::int32>(nd.latency);
        }
    }
    for (uint32_t i = 0; i < n; ++i) s << tracks[i].timeline;
    s << osc::EndMessage;
    if (replyToken) mEmit->reply(replyToken, reinterpret_cast<const uint8_t*>(s.Data()),
                                 static_cast<uint32_t>(s.Size()));
    else mEmit->broadcast(reinterpret_cast<const uint8_t*>(s.Data()),
                          static_cast<uint32_t>(s.Size()));
}

void TrackVerbs::broadcastTracks() { sendTracks(CLOCKWORK_SYS("track/list"), 0); changed(); }

void TrackVerbs::broadcastState(ClockworkTrackId t) {
    if (!mEmit) return;
    ClockworkTrackInfo info {};
    if (!clockwork_track_info(t, &info)) return;
    changed();
    // /clockwork/track/state <id:int> <gain:float> <mute:int> <timeline:string>
    // The timeline is appended, after the two fields the readers of this
    // message already count, for the reason sendTracks gives.
    char buf[256];
    osc::OutboundPacketStream s(buf, sizeof buf);
    s << osc::BeginMessage(CLOCKWORK_SYS("track/state"))
      << static_cast<osc::int32>(info.id) << info.gain << static_cast<osc::int32>(info.mute)
      << info.timeline
      << osc::EndMessage;
    mEmit->broadcast(reinterpret_cast<const uint8_t*>(s.Data()),
                     static_cast<uint32_t>(s.Size()));
}

void TrackVerbs::sendFolders(const char* address, uint32_t replyToken) {
    if (!mEmit) return;
    // /clockwork/track/folders <extra_count:int> <extra...:string>
    //                        <platform_count:int> <platform...:string>
    // The user's folders, then the platform's, so a settings panel can show
    // both and offer to remove only the first.
    const uint32_t ne = plugin_extra_search_paths(nullptr, 0);
    std::vector<const char*> extra(ne ? ne : 1);
    plugin_extra_search_paths(extra.data(), ne);
    const uint32_t np = plugin_search_paths(nullptr, 0);
    std::vector<const char*> plat(np ? np : 1);
    plugin_search_paths(plat.data(), np);

    size_t bytes = 128;
    for (uint32_t i = 0; i < ne; ++i) bytes += std::strlen(extra[i]) + 8;
    for (uint32_t i = 0; i < np; ++i) bytes += std::strlen(plat[i]) + 8;
    std::vector<char> buf(bytes);
    osc::OutboundPacketStream s(buf.data(), buf.size());
    s << osc::BeginMessage(address) << static_cast<osc::int32>(ne);
    for (uint32_t i = 0; i < ne; ++i) s << extra[i];
    s << static_cast<osc::int32>(np);
    for (uint32_t i = 0; i < np; ++i) s << plat[i];
    s << osc::EndMessage;
    if (replyToken) mEmit->reply(replyToken, reinterpret_cast<const uint8_t*>(s.Data()),
                                 static_cast<uint32_t>(s.Size()));
    else mEmit->broadcast(reinterpret_cast<const uint8_t*>(s.Data()),
                          static_cast<uint32_t>(s.Size()));
}

void TrackVerbs::broadcastFolders() { sendFolders(CLOCKWORK_SYS("track/folders"), 0); }

void TrackVerbs::scanAndSendPlugins(uint32_t replyToken) {
    if (!mEmit) return;
    // /clockwork/track/plugins <total:int> <offset:int> <count:int>
    //   per plugin: <name:string> <vendor:string> <format:string>
    //               <path:string> <index:int> <is_instrument:int>
    // Sorted by vendor then name and de-duplicated, as plugin_scan_all
    // leaves them: a browser can show this as it is. Scanned afresh every
    // time — a client asks when a folder changed or a plugin was installed,
    // and a cached answer would be the one it already has.
    //
    // PAGED, like plugin/params, and for the same reason: a page is one
    // frame on the NRT egress lane, and that lane refuses a frame over 8 KB
    // (kNrtEgressMax). A studio machine can have five hundred plugins and a
    // VST3 path under a vendor folder runs to a hundred characters, so a
    // page is bounded by bytes as well as entries — sixty-four entries, or
    // fewer if the strings would take the page past kPageBytes. The one
    // entry that is itself too long still goes alone, so a page always
    // makes progress.
    const uint32_t n = plugin_scan_all(nullptr, 0);
    std::vector<PluginEntry> found(n ? n : 1);
    if (n) plugin_scan_all(found.data(), n);
    found.resize(n);

    constexpr uint32_t PAGE = 64;
    constexpr size_t kPageBytes = 6144;
    uint32_t offset = 0;
    do {
        uint32_t count = 0;
        size_t bytes = 128;
        while (count < PAGE && offset + count < n) {
            const PluginEntry& e = found[offset + count];
            const size_t cost =
                std::strlen(e.name) + std::strlen(e.vendor) + std::strlen(e.path) + 40;
            if (count > 0 && bytes + cost > kPageBytes) break;
            bytes += cost;
            ++count;
        }
        std::vector<char> buf(bytes);
        auto build = [&](const char* address, osc::OutboundPacketStream& s) {
            s << osc::BeginMessage(address) << static_cast<osc::int32>(n)
              << static_cast<osc::int32>(offset) << static_cast<osc::int32>(count);
            for (uint32_t i = 0; i < count; ++i) {
                const PluginEntry& e = found[offset + i];
                s << e.name << e.vendor << (e.format == kPluginFormatClap ? "clap" : "vst3")
                  << e.path << static_cast<osc::int32>(e.index)
                  << static_cast<osc::int32>(e.is_instrument ? 1 : 0);
            }
            s << osc::EndMessage;
        };
        // Reply and broadcast, for the same reason as `list`: the GUI hears
        // broadcasts, a script hears its reply.
        if (replyToken) {
            osc::OutboundPacketStream s(buf.data(), buf.size());
            build(CLOCKWORK_SYS("track/plugins.reply"), s);
            mEmit->reply(replyToken, reinterpret_cast<const uint8_t*>(s.Data()),
                         static_cast<uint32_t>(s.Size()));
        }
        osc::OutboundPacketStream s(buf.data(), buf.size());
        build(CLOCKWORK_SYS("track/plugins"), s);
        mEmit->broadcast(reinterpret_cast<const uint8_t*>(s.Data()), static_cast<uint32_t>(s.Size()));
        offset += count;
    } while (offset < n);
}

void TrackVerbs::broadcastError(const char* verb, const std::string& detail, int handle) {
    if (!mEmit) return;
    // /clockwork/track/error <verb:string> <detail:string> <handle:int>
    // A failure the user needs to SEE. The verb's own .reply carries the same
    // detail, but a reply goes down the requesting socket, and the GUI's
    // requests leave on a socket nothing listens to; this is how "that name is
    // taken" or "plugin failed to load" reaches the panel.
    std::vector<char> buf(256 + detail.size());
    osc::OutboundPacketStream s(buf.data(), buf.size());
    // The handle names the plugin the failure is about, so a panel can put the
    // message on that device rather than in a corner; 0 when there is none.
    s << osc::BeginMessage(CLOCKWORK_SYS("track/error")) << verb << detail.c_str()
      << static_cast<osc::int32>(handle) << osc::EndMessage;
    mEmit->broadcast(reinterpret_cast<const uint8_t*>(s.Data()),
                     static_cast<uint32_t>(s.Size()));
}

// ── NRT thread ───────────────────────────────────────────────────────────────

bool TrackVerbs::handleControl(uint32_t token, const uint8_t* data, uint32_t size) {
    if (!mEmit) return false;
    if (size < CLOCKWORK_SYS_LEN("track/") + 4u) return false;
    if (std::memcmp(data, CLOCKWORK_SYS("track/"), CLOCKWORK_SYS_LEN("track/")) != 0) return false;

    try {
        osc::ReceivedPacket pkt(reinterpret_cast<const char*>(data),
                                static_cast<osc::osc_bundle_element_size_t>(size));
        osc::ReceivedMessage msg(pkt);
        const char* verb = msg.AddressPattern() + CLOCKWORK_SYS_LEN("track/");
        auto it = msg.ArgumentsBegin();
        const auto end = msg.ArgumentsEnd();

        auto reply = [&](osc::OutboundPacketStream& s) {
            mEmit->reply(token, reinterpret_cast<const uint8_t*>(s.Data()),
                         static_cast<uint32_t>(s.Size()));
        };
        auto log = [&](const std::string& line) { mEmit->debug(line); };

        if (std::strcmp(verb, "list") == 0) {
            // /clockwork/track/list → /clockwork/track/list.reply (same payload as the broadcast)
            //
            // AND the broadcast. A reply reaches the socket the request came
            // in on, and a GUI behind a daemon sends on a socket nothing reads: what
            // it hears is what the daemon forwards. Asking for the list is how
            // a client that has just arrived catches up, so the answer goes to
            // everyone — the others were already in step and lose nothing.
            sendTracks(CLOCKWORK_SYS("track/list.reply"), token);
            broadcastTracks();
            return true;
        }
        if (std::strcmp(verb, "create") == 0) {
            // /clockwork/track/create <name:string>
            //   → /clockwork/track/create.reply <id:int> <ok:int> <name-or-error:string>
            const std::string name = strArg(it, end);
            const char* err = nullptr;
            const ClockworkTrackId t = clockwork_track_create(name.c_str(), &err);
            log("[track] create " + name + (t ? " -> id " + std::to_string(t) : std::string(" FAILED: ") + (err ? err : "")));
            char buf[256];
            osc::OutboundPacketStream s(buf, sizeof buf);
            s << osc::BeginMessage(CLOCKWORK_SYS("track/create.reply"))
              << static_cast<osc::int32>(t) << (t ? 1 : 0)
              << (t ? name.c_str() : (err ? err : "create failed"))
              << osc::EndMessage;
            reply(s);
            if (t) broadcastTracks();
            else   broadcastError("create", std::string(err ? err : "create failed") + ": " + name);
            return true;
        }
        if (std::strcmp(verb, "remove") == 0) {
            // /clockwork/track/remove <track> → /clockwork/track/remove.reply <id:int> <removed:int>
            const ClockworkTrackId t = trackArg(it, end);
            const int removed = clockwork_track_remove(t);
            char buf[128];
            osc::OutboundPacketStream s(buf, sizeof buf);
            s << osc::BeginMessage(CLOCKWORK_SYS("track/remove.reply"))
              << static_cast<osc::int32>(t) << static_cast<osc::int32>(removed)
              << osc::EndMessage;
            reply(s);
            broadcastTracks();
            return true;
        }
        if (std::strcmp(verb, "rename") == 0) {
            // /clockwork/track/rename <track> <name:string>
            //   → /clockwork/track/rename.reply <id:int> <ok:int> <name-or-error:string>
            const ClockworkTrackId t = trackArg(it, end);
            const std::string name = strArg(it, end);
            const char* err = nullptr;
            const int ok = clockwork_track_rename(t, name.c_str(), &err);
            char buf[256];
            osc::OutboundPacketStream s(buf, sizeof buf);
            s << osc::BeginMessage(CLOCKWORK_SYS("track/rename.reply"))
              << static_cast<osc::int32>(t) << static_cast<osc::int32>(ok)
              << (ok ? name.c_str() : (err ? err : "rename failed"))
              << osc::EndMessage;
            reply(s);
            if (ok) broadcastTracks();
            else    broadcastError("rename", std::string(err ? err : "rename failed") + ": " + name);
            return true;
        }
        if (std::strcmp(verb, "move") == 0) {
            // /clockwork/track/move <track> <to_index:int>
            const ClockworkTrackId t = trackArg(it, end);
            const int to = intArg(it, end, 0);
            clockwork_track_move(t, static_cast<uint32_t>(to < 0 ? 0 : to));
            broadcastTracks();
            return true;
        }
        if (std::strcmp(verb, "clear") == 0) {
            clockwork_track_clear();
            broadcastTracks();
            return true;
        }
        if (std::strcmp(verb, "gain") == 0) {
            // /clockwork/track/gain <track> <gain:float>   (linear; 1 = unity)
            const ClockworkTrackId t = trackArg(it, end);
            const float g = static_cast<float>(numArg(it, end, 1.0));
            if (clockwork_track_set_gain(t, g)) broadcastState(t);
            return true;
        }
        if (std::strcmp(verb, "mute") == 0) {
            // /clockwork/track/mute <track> <mute:int>
            const ClockworkTrackId t = trackArg(it, end);
            const int m = intArg(it, end, 0);
            if (clockwork_track_set_mute(t, m)) broadcastState(t);
            return true;
        }
        if (std::strcmp(verb, "timeline") == 0) {
            /*
             * /clockwork/track/timeline <track> [name:string [id:int]]
             *   → /clockwork/track/timeline.reply <id:int> <ok:int> <timeline-or-error:string>
             *
             * Which timeline the track's plugins are told the time of:
             * "link" (the session clock, the default) or a midi follower
             * timeline by its /clockwork/clock name ("midi:<port>", claimed if
             * the port has not clocked yet, as the clock verbs claim). With
             * no name it is a query. The name is resolved in the ENGINE,
             * which owns the registry — TrackControl appends the id it
             * resolved before relaying (resolveTimelineVerb) — so a midi
             * name arrives here with its id, and one without is a name the
             * engine could not resolve, or a caller with no engine.
             */
            const ClockworkTrackId t = trackArg(it, end);
            const std::string name = strArg(it, end);
            ClockworkTrackInfo info {};
            int ok = clockwork_track_info(t, &info);
            const char* detail = ok ? nullptr : "no such track";
            if (ok && !name.empty()) {
                const int id = name == "link" ? 0 : intArg(it, end, -1);
                if (id < 0) { ok = 0; detail = "no such timeline"; }
                else if (!clockwork_track_set_timeline(t, id, name.c_str())) { ok = 0; detail = "timeline not set"; }
                else clockwork_track_info(t, &info);
            }
            char buf[256];
            osc::OutboundPacketStream s(buf, sizeof buf);
            s << osc::BeginMessage(CLOCKWORK_SYS("track/timeline.reply"))
              << static_cast<osc::int32>(t) << static_cast<osc::int32>(ok)
              << (ok ? info.timeline : detail)
              << osc::EndMessage;
            reply(s);
            if (!name.empty()) {
                if (ok) broadcastTracks();
                else    broadcastError("timeline", std::string(detail) + ": " + name);
            }
            return true;
        }
        if (std::strcmp(verb, "plugin/add") == 0) {
            /*
             * /clockwork/track/plugin/add <track> <path:string> [index:int] [at:int]
             *   → /clockwork/track/plugin/add.reply <handle:int> <ok:int>
             *                                      <name-or-error:string> <path:string>
             *
             * Loading opens a shared library and allocates, so it happens here
             * and never on the audio thread; the audio thread only ever sees
             * the finished node appear. The reply carries the HANDLE, which is
             * what every other node verb takes — not a position, because a
             * position changes under the caller the moment anything moves.
             * The path is echoed so a client with more than one add in flight
             * can tell the replies apart.
             */
            const ClockworkTrackId t = trackArg(it, end);
            const std::string path = strArg(it, end);
            const int index = intArg(it, end, 0);
            const int at = intArg(it, end, -1);
            const char* err = nullptr;
            const ClockworkTrackHandle h = clockwork_track_add_plugin(
                t, path.c_str(), static_cast<uint32_t>(index < 0 ? 0 : index),
                at < 0 ? UINT32_MAX : static_cast<uint32_t>(at), mSampleRate, 0, &err);
            ClockworkTrackNode node {};
            if (h) clockwork_track_node_info(h, &node);
            // Logged either way. A load that fails is the single most likely
            // thing to go wrong here and without a line in the log the only
            // symptom is a control that does nothing.
            log("[track] add " + path + (h ? " -> handle " + std::to_string(h) + " (" + node.name + ")"
                                           : std::string(" FAILED: ") + (err ? err : "load failed")));
            std::vector<char> buf(1536 + path.size());
            osc::OutboundPacketStream s(buf.data(), buf.size());
            s << osc::BeginMessage(CLOCKWORK_SYS("track/plugin/add.reply"))
              << static_cast<osc::int32>(h) << (h ? 1 : 0)
              << (h ? node.name : (err ? err : "load failed"))
              << path.c_str()
              << osc::EndMessage;
            reply(s);
            if (h) broadcastTracks();
            else   broadcastError("plugin/add", std::string(err ? err : "load failed") + ": " + path);
            return true;
        }
        if (std::strcmp(verb, "plugin/remove") == 0) {
            // /clockwork/track/plugin/remove <handle:int>
            //   → /clockwork/track/plugin/remove.reply <handle:int> <removed:int>
            const int h = intArg(it, end, 0);
            const int removed = clockwork_track_remove_plugin(static_cast<ClockworkTrackHandle>(h));
            char buf[128];
            osc::OutboundPacketStream s(buf, sizeof buf);
            s << osc::BeginMessage(CLOCKWORK_SYS("track/plugin/remove.reply"))
              << static_cast<osc::int32>(h) << static_cast<osc::int32>(removed)
              << osc::EndMessage;
            reply(s);
            broadcastTracks();
            return true;
        }
        if (std::strcmp(verb, "plugin/move") == 0) {
            // /clockwork/track/plugin/move <handle:int> <to_index:int>
            const int h = intArg(it, end, 0);
            const int to = intArg(it, end, 0);
            clockwork_track_move_plugin(static_cast<ClockworkTrackHandle>(h), static_cast<uint32_t>(to < 0 ? 0 : to));
            broadcastTracks();
            return true;
        }
        if (std::strcmp(verb, "plugin/bypass") == 0) {
            // /clockwork/track/plugin/bypass <handle:int> <bypass:int>
            const int h = intArg(it, end, 0);
            const int b = intArg(it, end, 1);
            clockwork_track_node_bypass(static_cast<ClockworkTrackHandle>(h), b);
            broadcastTracks();
            return true;
        }
        if (std::strcmp(verb, "plugin/channel") == 0) {
            // /clockwork/track/plugin/channel <handle:int> <channel:int 0..16>
            // The MIDI channel this instrument listens on; 0 is every channel.
            // How one of several instruments on a track is addressed: give it
            // a channel here and play it with `channel:` there.
            const int h = intArg(it, end, 0);
            const int ch = intArg(it, end, 0);
            if (!clockwork_track_node_set_channel(static_cast<ClockworkTrackHandle>(h), static_cast<int16_t>(ch)))
                broadcastError("plugin/channel", "a listen channel is 1 to 16, or 0 for all", h);
            broadcastTracks();
            return true;
        }
        if (std::strcmp(verb, "plugin/editor") == 0) {
            // /clockwork/track/plugin/editor <handle:int> <show:int>
            // The editor of the instance that is ACTUALLY MAKING SOUND, in a
            // window this process owns — a native view can only be parented in
            // the process that created it. Marshals to the main thread inside.
            const int h = intArg(it, end, 0);
            const int show = intArg(it, end, 1);
            if (show) {
                if (!clockwork_track_editor_show(static_cast<ClockworkTrackHandle>(h))) {
                    ClockworkTrackNode nd {};
                    const char* name = clockwork_track_node_info(static_cast<ClockworkTrackHandle>(h), &nd)
                                       ? nd.name : "this plugin";
                    broadcastError("plugin/editor", std::string(name) + " has no window of its own to show", h);
                }
            } else {
                clockwork_track_editor_hide(static_cast<ClockworkTrackHandle>(h));
            }
            return true;
        }
        if (std::strcmp(verb, "plugin/params") == 0) {
            /*
             * /clockwork/track/plugin/params <handle:int> [offset:int]
             *   → /clockwork/track/plugin/params <handle:int> <total:int>
             *         <offset:int> <count:int>
             *       then per parameter: <id:int> <name:string> <min:float>
             *       <max:float> <value:float> <group:int> <group_name:string>
             *       <automatable:int>
             *
             * What a panel that draws its own controls needs, from the
             * instance that is making sound. `value` is in the plugin's own
             * range (min..max), as track/plugin/param takes it.
             *
             * BROADCAST, ONE PAGE PER REQUEST. Broadcast because the panel's
             * requests leave on a socket nothing listens to (see
             * broadcastError). One page because a synth publishes thousands
             * of parameters — Surge XT, 2855 — and the whole list is bigger
             * than the egress ring: it is drained only after this handler
             * returns, so a handler that framed sixty pages at once lost
             * everything past the first twenty. Each page carries the total
             * and its offset; the client asks for `offset + count` next
             * until it has the lot. A page holds up to 48 parameters and
             * stays under 6 KB, so it always fits one frame however long the
             * names are.
             */
            const int h = intArg(it, end, 0);
            const uint32_t n = clockwork_track_node_param_count(static_cast<ClockworkTrackHandle>(h));
            uint32_t offset = static_cast<uint32_t>(intArg(it, end, 0));
            if (offset > n) offset = n;
            constexpr uint32_t kPageMax = 48;
            constexpr size_t kPageBytes = 6144;
            // The strings are borrowed and valid only until the next call,
            // so each is copied out before the next parameter is asked for.
            std::vector<PluginParam> params;
            std::vector<std::string> names, groups;
            size_t bytes = 160;
            for (uint32_t i = offset; i < n && params.size() < kPageMax; ++i) {
                PluginParam p {};
                if (!clockwork_track_node_param_info(static_cast<ClockworkTrackHandle>(h), i, &p)) break;
                std::string name(p.name ? p.name : ""), group(p.group_name ? p.group_name : "");
                const size_t cost = name.size() + group.size() + 48;
                if (!params.empty() && bytes + cost > kPageBytes) break;
                bytes += cost;
                names.push_back(std::move(name));
                groups.push_back(std::move(group));
                params.push_back(p);
            }
            std::vector<char> buf(bytes + 64);
            osc::OutboundPacketStream s(buf.data(), buf.size());
            s << osc::BeginMessage(CLOCKWORK_SYS("track/plugin/params"))
              << static_cast<osc::int32>(h) << static_cast<osc::int32>(n)
              << static_cast<osc::int32>(offset) << static_cast<osc::int32>(params.size());
            for (size_t i = 0; i < params.size(); ++i) {
                const PluginParam& p = params[i];
                s << static_cast<osc::int32>(p.id) << names[i].c_str()
                  << static_cast<float>(p.min) << static_cast<float>(p.max)
                  << static_cast<float>(p.value)
                  << static_cast<osc::int32>(p.group) << groups[i].c_str()
                  << static_cast<osc::int32>(p.automatable);
            }
            s << osc::EndMessage;
            mEmit->broadcast(reinterpret_cast<const uint8_t*>(s.Data()),
                             static_cast<uint32_t>(s.Size()));
            return true;
        }
        if (std::strcmp(verb, "rig/save") == 0) {
            // /clockwork/track/rig/save <path:string>
            //   → /clockwork/track/rig/save.reply <ok:int> <path-or-error:string>
            const std::string path = strArg(it, end);
            const char* err = nullptr;
            const int ok = clockwork_track_rig_save(path.c_str(), &err);
            log("[track] rig save " + path + (ok ? " ok" : std::string(" FAILED: ") + (err ? err : "")));
            std::vector<char> buf(256 + path.size());
            osc::OutboundPacketStream s(buf.data(), buf.size());
            s << osc::BeginMessage(CLOCKWORK_SYS("track/rig/save.reply"))
              << static_cast<osc::int32>(ok) << (ok ? path.c_str() : (err ? err : "save failed"))
              << osc::EndMessage;
            reply(s);
            if (!ok) broadcastError("rig/save", std::string(err ? err : "save failed") + ": " + path);
            return true;
        }
        if (std::strcmp(verb, "rig/load") == 0) {
            // /clockwork/track/rig/load <path:string>
            //   → /clockwork/track/rig/load.reply <ok:int> <missing:int> <path-or-error:string>
            // REPLACES the current tracks. Slow: opens every plugin in the rig.
            // A plugin whose path has moved is looked up by id across every
            // folder — the one time the engine scans, and only because the
            // user asked for exactly these plugins by loading the rig.
            const std::string path = strArg(it, end);
            const char* err = nullptr;
            uint32_t missing = 0;
            const int ok = clockwork_track_rig_load(path.c_str(), mSampleRate, 0, &missing, &err);
            log("[track] rig load " + path + (ok ? " ok, missing " + std::to_string(missing)
                                                  : std::string(" FAILED: ") + (err ? err : "")));
            std::vector<char> buf(256 + path.size());
            osc::OutboundPacketStream s(buf.data(), buf.size());
            s << osc::BeginMessage(CLOCKWORK_SYS("track/rig/load.reply"))
              << static_cast<osc::int32>(ok) << static_cast<osc::int32>(missing)
              << (ok ? path.c_str() : (err ? err : "load failed"))
              << osc::EndMessage;
            reply(s);
            broadcastTracks();
            broadcastFolders();
            if (!ok) broadcastError("rig/load", std::string(err ? err : "load failed") + ": " + path);
            else if (missing) broadcastError("rig/load", std::to_string(missing) +
                                             (missing == 1 ? " plugin in this rig could not be found"
                                                           : " plugins in this rig could not be found"));
            return true;
        }
        if (std::strcmp(verb, "scan") == 0) {
            // /clockwork/track/scan → /clockwork/track/plugins (broadcast; see sendPlugins)
            //
            // Every plugin in every folder. This opens each one and runs its
            // initialisation, which is seconds of work and the most likely
            // place for a plugin to crash — which is why it is here, in the
            // process built to be crashed, and not in a GUI. It runs on this
            // thread, so the other verbs and the editor windows wait for it;
            // the tracks keep playing, the audio thread is elsewhere.
            log("[track] scan");
            scanAndSendPlugins(token);
            return true;
        }
        if (std::strcmp(verb, "folders") == 0) {
            // Reply and broadcast, for the same reason as `list`.
            sendFolders(CLOCKWORK_SYS("track/folders.reply"), token);
            broadcastFolders();
            return true;
        }
        if (std::strcmp(verb, "folders/add") == 0) {
            // /clockwork/track/folders/add <dir:string>
            const std::string dir = strArg(it, end);
            plugin_add_search_path(dir.c_str());
            broadcastFolders();
            return true;
        }
        if (std::strcmp(verb, "folders/remove") == 0) {
            // /clockwork/track/folders/remove <dir:string>
            const std::string dir = strArg(it, end);
            plugin_remove_search_path(dir.c_str());
            broadcastFolders();
            return true;
        }
    } catch (...) {
        // fall through to the refusal
    }
    clockwork_sys_refuse(data, size, "unknown track verb",
                   [this, token](const uint8_t* d, uint32_t n) { mEmit->reply(token, d, n); });
    return true;
}

void TrackVerbs::changed() {
    if (mOnChange) mOnChange();
}
