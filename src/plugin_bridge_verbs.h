// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * plugin_bridge_verbs.h — the "/clockwork/track/" verbs, where the tracks are.
 *
 * The track model (plugin_track.h) lives in the BRIDGE process, so the code
 * that turns a track verb into a call on the model lives there too. The
 * engine keeps the wire: it accepts "/clockwork/track/…" from clients, relays
 * each to the bridge (src/native/TrackControl.cpp), and relays what comes
 * back. Nothing here knows which process it is in: it takes OSC in and
 * hands OSC out through `Emit`, which in the bridge writes the OUT ring and
 * in a test can be a vector.
 *
 * TWO HALVES ON TWO THREADS, as before the move:
 *
 *   PLAYING a track — note, cc, bend, notes_off, param, plugin/param — is
 *   applyRealtime, called on the bridge's audio thread with the frame offset
 *   the engine's audio thread computed from the timetag, so a note on a
 *   track lands on its sample exactly as a synth trigger does. Nothing in
 *   this half locks or allocates beyond what plugin_host.h admits to.
 *
 *   EVERYTHING ELSE — create, remove, rename, chain edits, editors, rigs,
 *   folders — is handleControl, on the bridge's main thread (plugins open
 *   their windows there). These answer with a reply AND a broadcast: a reply
 *   because the caller asked, a broadcast because a track is shared state
 *   and every client with a panel has to learn what changed under it.
 *
 * WHY THE STATE MESSAGE IS THE WHOLE STUDIO. A client that accumulates deltas
 * eventually disagrees with the engine about what is loaded. One message that
 * says what IS cannot drift, so "/clockwork/track/list" carries every track and
 * every chain, and is sent after every edit. Fader moves are the exception:
 * a drag sends dozens a second and the chain has not changed, so gain and
 * mute go out as a two-field "/clockwork/track/state" instead.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

extern "C" {
#include "plugin_track.h"
}

class TrackVerbs {
public:
    // Where the answers go. Every method may be called from the main thread;
    // `broadcast` is also called from the audio thread (a plugin's own edit
    // arrives on whichever thread the plugin chose), so an implementation
    // must be safe for that — a lock-free ring is.
    struct Emit {
        virtual ~Emit() = default;
        virtual void reply(uint32_t token, const uint8_t* osc, uint32_t len) = 0;
        virtual void broadcast(const uint8_t* osc, uint32_t len) = 0;
        virtual void debug(const std::string& line) = 0;
    };

    void init(Emit* emit, double sampleRate);
    void shutdown();

    // The rate plugins are opened against. Changes on a device change; a
    // plugin already open is not resampled, which neither format offers.
    void setSampleRate(double sr) { mSampleRate = sr > 0.0 ? sr : mSampleRate; }

    // Called after every change to what is loaded — the bridge rewrites the
    // rig mirror the engine restores from. Main thread.
    void setChangeListener(std::function<void()> fn) { mOnChange = std::move(fn); }

    // Main thread: one control verb. `osc` is the whole message, address
    // first. Returns false if the address is not a track verb at all.
    bool handleControl(uint32_t token, const uint8_t* osc, uint32_t len);

    // Audio thread: one realtime verb, at `frameOffset` within the block.
    // Returns false if the address is not a realtime track verb.
    bool applyRealtime(const uint8_t* osc, uint32_t len, uint32_t frameOffset);

    // The whole studio, unasked. The bridge sends one on joining so a client
    // that was waiting on a dead bridge sees the restored tracks.
    void broadcastTracks();
    void broadcastError(const char* verb, const std::string& detail, int handle = 0);

private:
    static void onParamEdit(void* ctx, ClockworkTrackHandle h, uint32_t id, double normalized, int own);

    void broadcastState(ClockworkTrackId t);
    void broadcastFolders();
    void sendTracks(const char* address, uint32_t replyToken);
    void sendFolders(const char* address, uint32_t replyToken);
    void scanAndSendPlugins(uint32_t replyToken);
    void changed();

    Emit*  mEmit = nullptr;
    double mSampleRate = 48000.0;
    std::function<void()> mOnChange;
};
