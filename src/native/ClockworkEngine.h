// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * ClockworkEngine.h — Top-level orchestrator
 */
#pragma once

#include "clockwork_product.h"
#include "clockwork_client.h"   // ClockworkStatus
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <stop_token>
#include <utility>
#include <vector>
#include "RingReader.h"
#include "IOscTransport.h"
#include "CallbackTransport.h"
#include "OscEgress.h"
#include "RingBufferWriter.h"
#include "OscIngress.h"
#include "clockwork_sys.h"
#include "IngressCallCtx.h"
#include "shm_peer_plane.h"
#include "EngineControl.h"
#ifdef CLOCKWORK_MIDI
#include "MidiControl.h"
#endif
#ifdef CLOCKWORK_GAMEPAD
#include "GamepadControl.h"
#endif
#ifdef CLOCKWORK_OSC
#include "OscControl.h"
#endif
#if CLOCKWORK_HAS_PLUGIN_TRACKS
#include "TrackControl.h"
#endif
#include "ClockworkProcessor.h"
#include "clock/MidiClockOut.h"
#include "clock/ClockworkClock.h"
#include "native/LinkAudioHost.h"
#include "DeviceInfo.h"
#include "DevicePolicy.h"
#include "DeviceLaneApartment.h"
#include "AudioRecovery.h"
#include "OscBuilder.h"
#include "engine_state.h"
#include "shm_segment.hpp"
#include <memory>

// Held, not seen. Their headers bring JUCE, and a host that includes this one
// must not need it: the engine uses JUCE inside, and none of it is a host's.
namespace juce { class AudioDeviceManager; }
namespace smoothie { class JuceDeviceCallback; }
class HeadlessDriver;
struct DeviceManagerFactory;   // DeviceManagerFactory.h

class ClockworkEngine {
    friend class EngineFixture;
public:
    // One engine per process half: its threads, device and segment are not things to copy or move.
    ClockworkEngine(const ClockworkEngine&) = delete;
    ClockworkEngine& operator=(const ClockworkEngine&) = delete;
    ClockworkEngine(ClockworkEngine&&) = delete;
    ClockworkEngine& operator=(ClockworkEngine&&) = delete;
    // Channel-count sentinel: negative means "open the device with all its
    // channels active (let JUCE/CoreAudio clamp to the hardware max)".
    // 0 still means "disabled" for inputs; positive N means "exactly N".
    static constexpr int kAutoChannelCount   = -1;
    // Upper bound on channels we'll ever request when the true capacity is
    // unknown. CoreAudio clamps an over-request to the device's real
    // count, but WASAPI rejects it outright — clamp against a probed
    // count wherever one is obtainable (see resolveInputWidth).
    static constexpr int kRequestMaxChannels =
        clockwork::device::kRequestMaxChannels;

    struct Config {
        int    sampleRate               = 48000;
        int    bufferSize               = 0;   // 0 = auto (smallest buffer >= 128)
        int    blockSize                = 0;   // the DSP's control block size;
                                               // 0 = match the opened device's buffer when
                                               // smaller than kDefaultBlockSize, else the
                                               // default (see chooseBlockSize). Clamped to
                                               // [32, kMaxBlockSize]. WASM ignores it (must
                                               // equal the 128-sample render quantum).
        int    udpPort                  = 57110;
        int    numOutputChannels        = kAutoChannelCount;
        int    numInputChannels         = kAutoChannelCount;
        // THE GUEST'S OWN CONFIGURATION, as opaque bytes. Clockwork copies
        // them into the block it reserves and hands the guest base and
        // length (DspConfig::guest_config); it reads none of them. What
        // they mean is between the host that fills this and the guest it
        // boots. The hosts in this repository spell them as `name=value`
        // lines (GuestConfigText.h), and a guest that does not know a name
        // refuses to boot with the line, rather than booting with a
        // silently different configuration. Empty: the guest's defaults.
        // Must fit the region with its terminating NUL (GUEST_CONFIG_SIZE),
        // or init refuses.
        std::string guestConfig;
        // The guest's bulk lanes (shm_segment.hpp): the inbox a client writes
        // samples and other assets into, the outbox the guest renders into.
        // Address space, not memory — pages are committed as they are
        // written — so a host sizes them for the largest set it expects,
        // not the smallest. A host exposes them as a launch option.
        size_t inboxBytes               = static_cast<size_t>(CLOCKWORK_INBOX_BYTES);
        size_t outboxBytes              = static_cast<size_t>(CLOCKWORK_OUTBOX_BYTES);
        bool   headless                 = false;   // skip audio device (for tests)
        bool   manualAudioPump          = false;   // skip the audio source entirely
                                                   // (no device, no headless driver):
                                                   // the caller drives process_audio()
                                                   // on its own thread. Implies headless.
                                                   // Prevents a second autonomous audio
                                                   // thread racing the manual caller.
        bool   freewheelClock           = false;   // deterministic sample-derived
                                                   // NTP (no wall-clock drift IIR);
                                                   // for offline/accuracy tests.
        double defaultBpm               = clockwork::kDefaultBpm;  // tempo the engine
                                                   // opens at; embedders (e.g. Sonic
                                                   // Pi) override. Seeded at init so
                                                   // it is consistent from the first
                                                   // clock read — not a post-boot set.
        bool   callbackWatchdog         = false;   // monitor audio-callback liveness:
                                                   // when processCount freezes for
                                                   // watchdogStallMs the source is
                                                   // restarted / device reopened.
                                                   // The audio callback is the sole
                                                   // drain for DSP commands, so a
                                                   // stalled device otherwise leaves
                                                   // the server deaf forever.
        int    watchdogStallMs          = 2500;    // frozen this long => recovery
        int    watchdogPollMs           = 250;     // liveness sampling interval
        int    watchdogRateWindowMs     = 5000;    // rate-skew: measurement window
                                                   // (0 disables the rate check).
                                                   // Catches a device whose
                                                   // callbacks tick at the wrong
                                                   // rate (post-sleep DirectSound
                                                   // timer free-run) — liveness
                                                   // reads Live but the clock IIR
                                                   // parks seconds off wall time.
        double watchdogRateTolerance    = 0.05;    // fractional deviation from the
                                                   // nominal rate counted as skew
        int    watchdogRateBadWindows   = 2;       // consecutive bad windows =>
                                                   // recovery (one window can be
                                                   // skewed by a transient stall)
        int    watchdogRateMaxRecoveries = 3;      // consecutive same-ratio skew
                                                   // recoveries before the fault
                                                   // is persistent: the last of
                                                   // them adopts the measured
                                                   // rate, and skew at THAT rate
                                                   // ends recovery for the
                                                   // session (RateSkewPolicy).
                                                   // A device clocked at 44.1k
                                                   // that reports 48k otherwise
                                                   // cold-swaps every window.
        int    watchdogRecoveryCooldownMs = 3000;  // between recoveries: covers
                                                   // the client's re-init after
                                                   // a promotion (seconds), so a
                                                   // second swap cannot land on
                                                   // a reinit still in progress
        // The watchdog's clock, for a test: milliseconds from any origin,
        // never going back. Set, the watchdog and the recovery cooldown read
        // time from it and no watchdog thread is started — the test calls
        // watchdogPoll() itself, so time passes only when the test says.
        // Empty: steady_clock, polled every watchdogPollMs on a thread.
        std::function<int64_t()> watchdogClockMs;
        // The host drains the egress rings itself, through the client API
        // (clockwork_client_poll on egressClient()), and routes what it takes
        // to its transport — as the JavaScript client does on the web. The
        // engine's gateway then leaves the rings alone. Off, the gateway
        // drains and routes through the transport given to setTransport.
        bool   hostDrainsEgress         = false;
        // The host runs the control plane itself: it calls controlPass() from
        // a thread of its own, and the engine starts no gateway thread. What
        // the pass carries — the control-ring drain that answers every
        // /clockwork/ verb the audio thread forwards, the peer command plane,
        // the MIDI clock producer, and the egress drain unless the host has
        // that too — then runs when, and only when, the host says. Off, the
        // engine's gateway thread runs the same pass every audio block.
        bool   hostDrivesControl        = false;
        bool   shmCommands              = false;   // drain the SHM segment's peer
                                                   // command plane (shm_peer_plane.h)
                                                   // in the control pass and publish
                                                   // the plane for ShmTransport.
                                                   // Needs udpPort > 0 (which is
                                                   // what creates the segment).
        std::string bindAddress       = "127.0.0.1"; // localhost only; use -B to override
        std::string hardwareDevice;                // -H flag: fuzzy match on "Driver : Device"
        std::string inputDevice;                   // -H input name (first of two, or the
                                                   // single shared name); empty = pair with
                                                   // the system default input
        std::string audioDriver;                   // --audio-driver: JUCE device type name
                                                   // to boot on (e.g. "Windows Audio",
                                                   // "DirectSound", "ASIO", "PipeWire").
                                                   // Resolved by resolveBootDriver; an
                                                   // unresolvable name (or ASIO with no -H
                                                   // device) keeps the platform default.
        std::string appName           = CLOCKWORK_PRODUCT_NAME; // --app-name: name published to OS
                                                   // registries (PipeWire nodes, ALSA seq
                                                   // MIDI clients, macOS aggregate devices,
                                                   // Link peers). Embedders pass their
                                                   // user-facing name.
        // The plugin bridge's executable, when the host knows where it put it
        // (on macOS the binary inside the .app). Empty: found beside this
        // process's executable, as before (TrackControl::resolveBridge). An
        // application with the engine linked in sets this: "beside the
        // executable" is then the application's own layout, which the engine
        // cannot know.
        std::string pluginBridgePath;

        // Test boundary: what builds the device manager. When set, the engine
        // builds its manager from this instead of a plain one — both at boot
        // and in recovery's recreateDeviceManager — and skips every
        // platform-specific piece of device bring-up (PipeWire registration,
        // the Windows driver preference): the factory's manager owns its own
        // device types, typically fakes (test/FakeAudioDevice.h). Opaque here,
        // as the manager is JUCE's (DeviceManagerFactory.h). Production
        // leaves it null.
        std::shared_ptr<const DeviceManagerFactory> deviceManagerFactory;
    };

    ClockworkEngine();
    ~ClockworkEngine();

    void init(const Config& config);
    void shutdown();

    std::function<void(const uint8_t*, uint32_t)> onReply;

    // The same traffic, with the routing that decided where it went: `origin`
    // is the token it is addressed to and `route` an EgressRoute. onReply
    // cannot show this — an in-process reply and an in-process notification
    // both arrive through it — so a caller that needs to know whether an
    // answer reached the client that asked, or the notify audience, sets this.
    std::function<void(uint32_t origin, uint32_t route,
                       const uint8_t*, uint32_t)> onReplyRouted;
    std::function<void(const std::string&)>        onDebug;

    // Inject the egress transport (a host that owns sockets injects one from
    // the comms client library). Call before init(); if unset, the engine
    // uses the in-process CallbackTransport (replies via onReply).
    void setTransport(IOscTransport* transport) { mTransport = transport; }

    // The peer command plane slot (Config::shmCommands). Set to the segment's
    // plane by init(), nulled by shutdown(). ShmTransport binds to the SLOT
    // (not the plane) and loads it per send, so it always follows the engine's
    // current segment.
    std::atomic<ShmPeerPlaneHeader*>* peerPlaneSlot() { return &mPeerPlane; }

    // The public segment's OS handle, for the attach endpoint to duplicate
    // into readers (shm_attach::server) and for an in-process reader to
    // shm_dup_handle. Invalid when there is no segment (udpPort == 0, or
    // creation failed) and after shutdown(). Owned by the engine.
    detail_shm_segment::shm_native_handle shmNativeHandle() const {
        return mShmemCreator ? mShmemCreator->native_handle()
                             : detail_shm_segment::shm_invalid_handle;
    }
    // The segment's size — the lanes' sizes decide it — or 0 without one.
    size_t shmSegmentSize() const {
        return mShmemCreator ? mShmemCreator->segment_size() : 0;
    }

    void sendOSC(const uint8_t* data, uint32_t size);

    // Render exactly one audio block on the calling thread: the full per-block
    // sequence (install loader buffers, derive NTP/host-time, drain Link inputs,
    // process_audio, publish sinks, tick processCount) — the same body the
    // HeadlessDriver runs, via the shared renderAudioBlock(). For manual-pump
    // mode and tests that need to drive the engine deterministically (e.g. so a
    // bus snapshot doesn't race a real-time audio thread). The first call anchors
    // the audio-thread clock; safe to call after stopping the HeadlessDriver.
    void pumpAudioBlock();
    // Render one device callback of `frames` on the calling thread, as the
    // device path does: one clock step for the whole callback, then its blocks
    // back to back, `afterEachBlock` after each. For tests that need what a
    // device buffer of that size does to timing, not only to audio.
    void pumpAudioCallback(uint32_t frames, const std::function<void()>& afterEachBlock = {});
    // The next manual pump starts the audio-thread clock afresh, as a device
    // starting does.
    void restartManualPump() { mManualPumpStarted = false; }

    // The OSC ingress: classify a raw packet at the boundary and hand it to its
    // side — clockwork's own dispatch for "/clockwork/...", the DSP for
    // everything else. Every transport (UDP, NIF send_osc) funnels into this;
    // the engine owns the routing, the transports know only their medium.
    // Adding a clockwork subsystem is one ClockworkSysRoutes::add of a sub-namespace;
    // it cannot move the boundary, because the boundary has no table to register in.
    // `originToken` is an opaque transport handle (minted by the transport's
    // internOrigin at its entry point) the egress later resolves to route replies
    // back; 0 means an anonymous in-process / embedder caller. No default — every
    // entry point assigns an origin, so an unstamped 0 can never sneak in.
    // Returns what the client door said (clockwork_client_send): CLOCKWORK_OK,
    // CLOCKWORK_E_FULL (no room this moment), CLOCKWORK_E_TOO_BIG (never room),
    // CLOCKWORK_E_CLOSED (no engine to send to). Anything but OK is a drop.
    ClockworkStatus ingest(const uint8_t* data, uint32_t size, uint32_t originToken);
    bool isRunning() const { return mRunning.load(); }

    // The client handle over this engine's memory, for a host that drains
    // egress itself (Config::hostDrainsEgress): clockwork_client_poll takes
    // from both egress rings. ONE CONSUMER — the host's drain thread and no
    // other. Null before init.
    struct ClockworkClient* egressClient() { return mClientHandle; }

    // ONE pass of the control plane, on the calling thread, for a host that
    // runs it (Config::hostDrivesControl): the control-ring drain — every
    // /clockwork/ verb the audio thread forwarded, answered — the peer
    // command plane, the MIDI clock producer, and the egress drain unless the
    // host drains egress itself. ONE THREAD: the pass is single-consumer
    // through and through, so a host calls this from one thread and no other.
    // Call it often — the engine's own gateway runs it every audio block —
    // and stop calling it before shutdown(), which waits for a pass in
    // flight and then closes the door. Never blocks: an empty pass takes
    // nothing and returns. False when the door is closed — before init, after
    // shutdown, or whenever the engine runs its own gateway thread (the
    // config did not say the host drives) — or when another thread is inside
    // the pass. Each is a misuse, refused rather than let corrupt a ring.
    bool controlPass();
    // Whether the engine spawned a gateway thread of its own for the pass
    // (false when the host drives control, and after shutdown).
    bool hasControlThread() const { return mNrtGateway.running(); }

    // The guest's inbox as THIS engine placed it — the lane an in-process
    // client writes and then names by OFFSET in /b_allocPtr and its kin. A
    // client in another process reaches the same bytes through the segment;
    // one sharing this address space asks here. Null and 0 before init.
    const uint8_t* guestInbox()      const { return mGuestInbox; }
    uint32_t       guestInboxBytes() const { return mGuestInboxBytes; }
    // The guest's outbox likewise: the lane the guest writes and a client
    // reads in place, when the guest says where.
    const uint8_t* guestOutbox()      const { return mGuestOutbox; }
    uint32_t       guestOutboxBytes() const { return mGuestOutboxBytes; }

    // Audio-thread control route (the fallback on mAudioRoutes: every claimed
    // address this thread does not answer itself):
    // forwards the message to the NRT command ring with its origin token. Runs on
    // the audio thread; the control pass drains the ring and runs the subsystems.
    static bool nrtForwardSink(void* ctx, const void* callCtx, const uint8_t* data, std::size_t len);

    // One route adapter for every backend route: forward the immutable call
    // metadata + (data, len) to a member handler of subsystem T. ctx is the owning
    // subsystem. Used for the control handlers on the NRT thread: handleXCommand
    // for "midi/", "gamepad/", "osc/", handleLinkCommand for "clock/", and
    // handleEngineCommand as the fallback (the engine's own top-level verbs).
    template <class T, auto Handler>
    static bool routeTo(void* ctx, const void* callCtx, const uint8_t* data, std::size_t len) {
        static const DrainCallCtx kEmpty{};
        const DrainCallCtx& meta = callCtx ? *static_cast<const DrainCallCtx*>(callCtx) : kEmpty;
        (static_cast<T*>(ctx)->*Handler)(meta, data, static_cast<uint32_t>(len));
        return true;
    }

    // NRT control-thread blocking, milliseconds. maxPass is the high-water mark
    // since boot (or the last reset); recentWorst is the worst pass in the
    // trailing ~60 s window (decays back to quiet); inFlight is non-zero only
    // while a pass is running long right now. Tests assert on these directly
    // rather than on wall-clock deadlines, which a loaded machine cannot
    // honour.
    uint32_t nrtMaxPassMs()      const { return mNrtGateway.maxPassUs()       / 1000; }
    uint32_t nrtRecentWorstMs()  const { return mNrtGateway.recentMaxPassUs() / 1000; }
    uint32_t nrtInFlightMs()     const { return mNrtGateway.inFlightUs()      / 1000; }
    // Clears the high-water mark, so a test can measure one stretch of work
    // rather than everything since boot — the "or the last reset" above.
    void     resetNrtMaxPass()         { mNrtGateway.resetMaxPassUs(); }

    // Run device work off the NRT gateway, in submission order. Enumerating or
    // reopening devices takes seconds (on Windows listDevices() activates every
    // device through COM), while the gateway is the sole consumer of control
    // commands and the sole deliverer of replies — doing it there stalls every
    // other client, which once cost a user a boot. Named device
    // switches already run on their own worker; this applies the same
    // discipline to the paths that didn't. Replies still work from here: the
    // egress accepts off-thread producers (sendSwitchDone always has).
    // False when the lane has stopped (shutdown) and the task was dropped.
    bool postDeviceTask(std::function<void()> task);

    // Device control surface (called by EngineControl + engine lifecycle).
    // Builds the device/input/info report and pushes it to notify subscribers.
    void sendDeviceReport();
    // Push the truthful outcome of a debounced switch to subscribers.
    void sendSwitchDone(const SwapResult& result,
                        const std::string& requestedOutput,
                        const std::string& requestedInput);
    // Debounced device switch: store the request, run only the last one after a
    // quiet period on a worker thread.
    void scheduleDeviceSwitch(const std::string& devName,
                              const std::string& inputDevName,
                              double sampleRate, int bufferSize);
    // Why a recovery is being asked for beyond "the device stopped". The
    // default is a plain reopen at the nominal rate. The watchdog's rate-skew
    // policy fills adoptRate when a skew has proved persistent: the cold swap
    // then REQUESTS that rate (snapped to what the device advertises) instead
    // of keeping the session rate that the device cannot deliver. The other
    // two fields are for the one message the user gets about it.
    struct RecoveryIntent {
        double adoptRate    = 0;   // > 0: the rate the swap asks for
        double nominalRate  = 0;   // what the device reports
        double measuredRate = 0;   // what it delivers
    };
    // Accept-or-reject an audio-recovery attempt (in-flight + cooldown gated).
    // On accept, posts recoverAudio() to the device task lane and returns true; on
    // reject fills `reason`. Called by the watchdog on a real-device stall or a
    // rate skew, and by the external /reopen OSC command.
    bool requestAudioRecovery(std::string& reason, const RecoveryIntent& intent);
    bool requestAudioRecovery(std::string& reason);   // a plain reopen

    // --- Engine lifecycle state ---
    EngineState engineState() const { return mEngineState.load(); }
    void        setEngineState(EngineState state, const std::string& reason = "");
    // Why the engine last went into Error ("" if it never has).
    std::string errorReason() const {
        std::lock_guard<std::mutex> lock(mErrorReasonMutex);
        return mErrorReason;
    }

    // --- Metrics ---
    // Shared PerformanceMetrics struct (defined in src/shared_memory.h).
    // Same struct that JS callers see via clockwork.getMetrics(). Both
    // runtimes write to it from symmetric paths (audio_processor, OSC
    // reader/writer, debug reader). Pointer is null before init().
    const PerformanceMetrics& getMetrics() const { return *mMetrics; }
    const PerformanceMetrics* metricsPtr() const { return mMetrics; }

    // --- MIDI clock out ---
    // The engine's clock-out coordinator, for its counters (sent / dropped):
    // what a test reads to see that pulses left, since the ports they left
    // through are the platform's. Owned here; the render path reaches it
    // through g_active_midi_clock_out, MidiControl through init().
    MidiClockOut& midiClockOut() { return mMidiClockOut; }

    // --- Link Audio ---
    // The engine's Link Audio host, for what its subscriptions report: what a
    // test reads mid-stream, where asking over OSC would pump a block out of
    // time.
    LinkAudioHost& linkAudio() { return mLinkAudio; }

    // --- Variadic OSC send (builds message + dispatches through sendOSC) ---
    template<typename... Args>
    void send(const char* address, Args&&... args) {
        auto pkt = OscBuilder::message(address, std::forward<Args>(args)...);
        sendOSC(pkt.ptr(), pkt.size());
    }

    // Bundle send — ntpTimeSec is NTP time in seconds (double -> uint64 timetag)
    void sendBundle(double ntpTimeSec, std::initializer_list<OscPacket> messages);

    // --- Device management ---
    //
    // Device-name sentinels recognised by switchDevice / setDeviceMode and
    // mirrored on the GUI side:
    //   "__system__"  — follow the macOS system default output (mDeviceMode
    //                   is kept empty internally while this is active).
    //   "__none__"    — (input only) disable audio inputs; clears the
    //                   preferred input sub-device.
    // Any other non-empty value is treated as a literal device name.
    //
    // rescan=true triggers a full CoreAudio re-enumeration (may disrupt a
    // just-opened device on macOS). Pass false to reuse JUCE's cached list.
    std::vector<DeviceInfo>  listDevices(bool rescan = true) const;
    CurrentDeviceInfo        currentDevice() const;
    // True when `name` is the device table's synthetic default-follow row
    // ("System Default" under a driver that has no real device of that
    // name) — callers translate the pick into a system-mode request, the
    // same path as the "__system__" sentinel.
    bool                     isSyntheticDefaultPick(const std::string& name) const;
    // origin: pass SwapOrigin::Internal for engine-originated swaps
    // (recovery reopen, hotplug re-attach) — they scope against the
    // driver actually open and leave the user's preferred-device memory
    // and pending switchDriver intent untouched.
    SwapResult               switchDevice(const std::string& deviceName,
                                          double sampleRate = 0,
                                          int bufferSize = 0,
                                          bool forceCold = false,
                                          const std::string& inputDeviceName = "",
                                          SwapOrigin origin = SwapOrigin::User);

    // Re-open the current device (tear down and recreate without changing
    // selection). Use when an external config change — e.g. a MOTU Pro
    // Audio Control "Computer" channel-count bump — needs to flow through
    // without a full engine restart. Preserves the input pairing and
    // system-default / manual mode semantics. sampleRate > 0 makes the reopen an
    // explicit rate request (snapped to the device's advertised rates); 0
    // keeps the session rate, as a reopen always did.
    SwapResult               reopenCurrentDevice(double sampleRate = 0);

    // --- Input channel management ---
    // Enable/disable audio input. Triggers a cold swap (DSP rebuild).
    // numChannels=0 disables input, >0 enables that many input channels,
    // -1 re-enables with the configured input channel count.
    // On macOS, enabling input triggers the OS microphone permission dialog.
    SwapResult               enableInputChannels(int numChannels);

    // Set the configured input channel count (used when re-enabling via -1).
    // Does NOT trigger a swap — call enableInputChannels() afterward if needed.
    void setConfiguredInputChannels(int numChannels) { mBootInputChannels = numChannels; }
    int  configuredInputChannels() const { return mBootInputChannels; }

    // --- Audio driver management ---
    std::vector<std::string> listDrivers() const;
    // The driver type of whichever device JUCE actually has open (or
    // the active type if no device is open). Does NOT reflect the
    // user's pending choice — see intendedDriver().
    std::string              currentDriver() const;
    // The user's last switchDriver() pick when it wasn't followed by
    // an immediate device transition (no saved preference + ASIO, or
    // no devices visible for the type). Empty otherwise. The GUI uses
    // this to keep its driver dropdown sticky on the user's choice
    // while the info display continues to report the actual audio path.
    std::string              intendedDriver() const;
    SwapResult               switchDriver(const std::string& driverName);

    // True iff JUCE rejected the (outputName, inputName) pair on a
    // previous swap. The pair is remembered in mUngatableInputPairs
    // and filtered out of subsequent device-list pushes — "don't show
    // options we can't honour." Cleared by hotplug rescans.
    bool isInputKnownBadFor(const std::string& outputName,
                            const std::string& inputName) const;

    // Increments on every cold-swap rebuild of the DSP instance. Emitted on
    // /clockwork/setup so clients can detect that a swap they
    // were waiting on has completed and abort stale waits.
    uint32_t                 setupGeneration() const { return mSetupGeneration.load(); }

    // Send the current lifecycle state directly to one caller. Stream-
    // transport clients (TCP/UDS/pipe) can only connect after init, so they
    // miss the boot-time statechange broadcast — the /clockwork/notify
    // handler replays it per new registrant. State only, never the setup
    // event. Reads atomics only; safe from the NRT gateway thread.
    void snapshotStateTo(uint32_t token);

    // No recording API: the engine opens no files. Its master mix leaves
    // through the audio taps (shm_audio_buffer slot 0), and a client that
    // wants a session recording reads that slot and writes the file itself
    // (SuperSonic's front does).

    // Device swap event callback
    std::function<void(const std::string& event, const SwapResult& result)> onSwapEvent;

    // Injectable hook for testing: called on the swapping thread once a cold
    // swap has parked the control pass, before it tears the arena down.
    std::function<void()> testControlParked;

    // Injectable hook for testing: if set and returns non-empty string,
    // the device configuration step is treated as failed with that error.
    // The bool argument is true when an input device was requested in the
    // attempted setup, allowing the hook to simulate an input-specific
    // failure that should be retried output-only.
    std::function<std::string(bool inputRequested)> testSwapFailure;

    // Injectable hook for testing: if set and returns non-empty string,
    // rebuild_dsp() is skipped and the error triggers recovery to safe defaults.
    std::function<std::string()> testRebuildFailure;

    // Injectable hook for testing: if set, switchDriver() in headless mode
    // simulates a successful driver switch where the new driver's default
    // device reports this sample rate. Allows testing rate-mismatch cold swaps.
    std::function<double()> testDriverSwitchRate;

    // Injectable hook for testing: if set and returns a non-empty string,
    // init() throws std::runtime_error with that message just before
    // setting mRunning=true. Used to exercise the partial-init shutdown
    // cleanup path.
    std::function<std::string()> testInitFailure;

    // Times the callback watchdog has recovered a stalled audio source
    // (restarted the headless driver / requested a device reopen). See
    // Config::callbackWatchdog.
    uint32_t watchdogRecoveryCount() const { return mWatchdogRecoveries.load(); }

    // Subset of the above triggered by the rate-skew check (device ticking at
    // the wrong rate rather than stalled). See Config::watchdogRateWindowMs.
    uint32_t rateSkewRecoveryCount() const { return mRateSkewRecoveries.load(); }

    // One watchdog poll, now: what the watchdog's thread does every
    // watchdogPollMs. For a test that owns the watchdog's clock
    // (Config::watchdogClockMs); does nothing without Config::callbackWatchdog.
    void watchdogPoll();

    // The one authoritative answer to "is a device mutation in flight".
    // Set via PhaseGuard by switchDevice (Swapping) and recoverAudio
    // (Recovering). Replaces probing the swap gate with try_lock/unlock
    // (a TOCTOU, and a false positive whenever a mere reader held the
    // gate).
    enum class DevicePhase : uint8_t { Idle, Swapping, Recovering };
    DevicePhase devicePhase() const { return mDevicePhase.load(); }
    // A recovery accepted and not yet finished — including one queued on the
    // device lane that has not started, when devicePhase() still reads Idle.
    bool recoveryInFlight() const { return mReopenInProgress.load(); }

    // Bounded acquisition of the swap gate: up to `attempts` try_locks,
    // `sleepMs` apart, mirroring executePendingSwitch's retry discipline.
    // Used by setDeviceMode's system-default reinit so it cannot interleave
    // with an in-flight switchDevice — two threads driving JUCE's
    // AudioDeviceManager concurrently can wedge the device's callback
    // thread. Public so tests can exercise the gate without a device.
    bool tryAcquireSwapGate(std::unique_lock<std::recursive_mutex>& lk,
                            int attempts, int sleepMs);

    // Test-only: hold the swap gate AND present the Swapping phase —
    // together they are what "a swap is in flight" means to the rest of
    // the engine (watchdog deferral reads the phase; message-thread
    // handlers try the gate).
    class TestSwapHold {
    public:
        TestSwapHold(std::recursive_mutex& m, std::atomic<DevicePhase>& p)
            : mLock(m), mPhase(&p) { p.store(DevicePhase::Swapping); }
        TestSwapHold(TestSwapHold&& o) noexcept
            : mLock(std::move(o.mLock)), mPhase(o.mPhase) {
            o.mPhase = nullptr;
        }
        TestSwapHold(const TestSwapHold&) = delete;
        TestSwapHold& operator=(const TestSwapHold&) = delete;
        TestSwapHold& operator=(TestSwapHold&&) = delete;
        ~TestSwapHold() { unlock(); }
        void unlock() {
            if (mPhase) { mPhase->store(DevicePhase::Idle); mPhase = nullptr; }
            if (mLock.owns_lock()) mLock.unlock();
        }
    private:
        std::unique_lock<std::recursive_mutex> mLock;
        std::atomic<DevicePhase>* mPhase = nullptr;
    };
    TestSwapHold testHoldSwapGate();

    // --- Audio callback access (for preTick hook, pause/resume) ---
    ClockworkProcessor& processor() { return mProcessor; }

    // CFRunLoop suppression (macOS). The host's run-loop pump calls
    // isRunLoopSuppressed() each tick and sleeps instead of pumping
    // while true. Nothing in the engine sets it any more: the device
    // layer pairs devices without the run loop.
    bool isRunLoopSuppressed() const { return mSuppressRunLoop.load(); }
    void setRunLoopSuppressed(bool v) { mSuppressRunLoop.store(v); }

    // --- Device mode (system/auto vs manual device name) ---
    std::string setDeviceMode(const std::string& mode);
    void forceDeviceMode(const std::string& mode) { mDeviceMode = mode; }
    std::string deviceMode() const { return mDeviceMode; }

    // Preferred-device accessors. These track the user's long-lived
    // intent for auto-re-attach on hot-plug, independent of what device
    // is currently active (the device may have been removed and we
    // fell back to the system default — but we still want to come back
    // when it reappears). deviceMode() is "current selection"; these
    // are "want to use whenever available".
    std::string preferredOutputDevice() const { return mPreferredOutputDevice; }
    std::string preferredInputDevice() const  { return mPreferredInputDevice; }

    void printDeviceList();

    // --- Purge stale messages ---
    void purge();

private:
    bool interceptBufferFreed(const uint8_t* data, uint32_t size);

    std::string reinitialiseWithDefaultsPreservingConfig();
    // A driver that names a default: the boot's when it does, otherwise the
    // first; empty when none does.
    std::string driverWithADefault();
    // openSystemDefault's way when the default is not opened by name: the
    // driver opens its own default, and the engine plays on it afterwards,
    // or on what it played on before when that fails.
    std::string openDriverDefault();

    // The two halves of init(): open the audio device (skipped headless),
    // then bring the engine up around whatever it settled on. Split so
    // each half is readable alone and the boot/swap unification has its
    // boundary — see the definitions for the half-by-half contracts.
    void initAudioDevice(const Config& cfg);
    void initEngine(const Config& cfg);

    // The one place a juce::AudioDeviceManager is constructed: the
    // Config::deviceManagerFactory boundary when set (tests), else a plain
    // manager. Both boot (init) and recovery (recreateDeviceManager) go
    // through this so a recovered engine keeps its injected fakes, and
    // every manager hands its device changes to devicesChanged().
    std::unique_ptr<juce::AudioDeviceManager> makeDeviceManager();

    // What changed about the devices, as a pass of reconcileDevices is told.
    enum DeviceChanges : unsigned {
        kListChanged       = 1u << 0u,   // devices came or went: rescan the lists
        kDefaultChanged    = 1u << 1u,   // the OS default output moved
        kOpenDeviceChanged = 1u << 2u,   // the device played on changed where it stands
        kPassQueued        = 1u << 31u,
    };
    // Something about the devices changed: the device layer's report (a
    // device came or went, or the open device changed), or macOS's that the
    // default output moved. Called on whatever thread the OS reports it, the
    // moment it does; queues one pass of reconcileDevices on the device
    // lane, which owns the device manager. A burst of reports is one pass.
    void devicesChanged(unsigned changes);
    // On the device lane: compares what the devices are with what the
    // engine plays on, and acts only where they differ — so a report of a
    // change the engine made itself finds nothing to do, and no report needs
    // to be dropped.
    void reconcileDevices();
    std::atomic<unsigned> mDeviceChanges{0};   // DeviceChanges not yet looked at
    // A pass that came while the engine was booting: init runs one once
    // the engine is running.
    std::atomic<bool> mReconcileAfterBoot{false};
    // The device played on has gone: recover onto what should play now,
    // unless a recovery is already on its way.
    void recoverLostDevice(const std::string& name, const char* how);
    // System mode: when the default output differs from the device played
    // on, open the default (openSystemDefault). True if it tried.
    bool followDefaultOutput();
    // A default output that failed to open, not tried again until the
    // default moves: on macOS a failed switch to a pair builds and pulls
    // down an aggregate device, which changes the list again. Device lane,
    // under the gate.
    std::string mUnopenableDefault;
    // The default output as the last pass saw it, so a decision not to
    // follow it is said once, not on every pass. Device lane.
    std::string mDefaultSeen;
    // The device played on, as its driver reports it now: gone where it
    // stands, unable to carry on as opened, or with other channels than it
    // was opened with, it is opened again. True if it acted.
    bool reconcileOpenDevice();

    // Destroy and recreate mDeviceManager, then re-open the default device and
    // re-attach the audio callback. Unlike a reopen (which reuses the existing
    // CoreAudio/HAL client), this forces a brand-new connection — the recovery
    // for a hibernate-killed device where the IO thread is no longer driven by
    // coreaudiod. Called only from recoverAudio() on the device task lane under the swap
    // gate — leaves the fresh manager initialised to the default device for the
    // cold swap that follows. Empty return on success.
    std::string recreateDeviceManager();

    // Detach listeners/callback, close the device, and release mDeviceManager.
    // Shared by shutdown() and recreateDeviceManager().
    void teardownDeviceManager();

    // On a fresh device manager, the driver boot settled on: recovery must
    // not change the driver the user chose (a fresh manager starts on JUCE's
    // first type). Not under the factory boundary — an injected manager owns
    // its types.
    void restoreBootDriver();

    // ── Audio source state machine ──────────────────────────────────────────
    //
    // process_audio() runs from exactly one of two drivers:
    //   RealCallback: JUCE's AudioDeviceManager fires Smoothie's device callback, which runs our ClockworkProcessor
    //                 when a device is open.
    //   Headless:     a high-priority timer thread (HeadlessDriver) fakes
    //                 the same contract when no device is available. Used
    //                 for explicit cfg.headless==true, for boot-time
    //                 device-init failures (issue #3526: ALSA/PipeWire
    //                 "no channels"), and for cold-swap rollback when no
    //                 usable device is left.
    //
    // mActiveSource is the source of truth. Every device-swap path goes
    // through stopAudioSource() then startAudioSource() so the "exactly
    // one source active while running" invariant holds. Without it, a
    // partially-failed init can leave mDeviceManager non-null with no
    // current device, neither driver firing, and process_audio silently
    // never called.
public:
    enum class AudioSource { None, RealCallback, Headless };
    AudioSource audioSource() const { return mActiveSource.load(std::memory_order_acquire); }

    // True when the running engine has no audio source because no real device
    // could be opened — the recoverable "waiting for audio device" state. Not a
    // deliberate headless / manual-pump build, and not a stopped engine. The
    // watchdog drives recovery out of it so the engine self-heals when a device
    // appears.
    bool waitingForAudioDevice() const {
        return mRunning.load()
            && mActiveSource.load() == AudioSource::None
            && !mHeadless
            && !mCurrentConfig.manualAudioPump;
    }

private:
    // Atomic: written on whichever thread runs start/stopAudioSource (boot,
    // the watchdog, the device task lane during recoverAudio, and the device-switch
    // workers) and read lock-free by audioSource() and status paths on other
    // threads.
    std::atomic<AudioSource> mActiveSource{AudioSource::None};

    AudioSource desiredAudioSource() const;

    // Precondition: mActiveSource == None. Picks RealCallback or Headless
    // based on desiredAudioSource(), then blocks until process_audio has
    // ticked at least once (or 5s with a warning). This blocking wait is
    // the boot/swap barrier so callers can sendOSC() immediately after.
    // False when the source started and never ticked — a device that opened
    // and then delivered nothing; true otherwise, including when no source
    // was started (no device, manual pump) and so none was waited for.
    bool startAudioSource();

    // Idempotent. Does NOT remove the change listener (shutdown-only) so
    // hot-plug events survive swaps.
    void stopAudioSource();

    // True once process_audio has ticked past `before`; false after 5 s.
    bool waitForFirstAudioTick(uint32_t before);

    // switchDevice sub-stages. Each has a single responsibility and is
    // safe to call independently (they read / write engine state, so
    // they're member methods rather than pure functions). Extracted to
    // keep switchDevice itself readable.
    //
    // Refuses "add an input to an output that plays alone" (its driver says
    // it doesn't pair) upfront — the pairing check later would drop the
    // input anyway, but only after a cold swap. Returns non-empty error
    // string on refusal; empty = OK.
    std::string refuseUnpairableInput(const std::string& deviceName,
                                      const std::string& inputDeviceName);

    // Refuses a swap whose output or input name doesn't resolve to any
    // visible device (and isn't a known sentinel like "__system__" /
    // "__none__"). Without this, switchDevice mutates state (mCurrentConfig,
    // the channel ceilings, destroy_dsp) before the doomed setAudioDeviceSetup —
    // leaving the engine half-broken when JUCE returns "No such device".
    // Returns non-empty error string on refusal; empty = OK.
    std::string refuseUnknownDeviceName(const std::string& deviceName,
                                        const std::string& inputDeviceName);

    // Probes a named device (input or output) via JUCE and returns the
    // number of channels it advertises, or -1 if the name doesn't
    // match / device can't be opened. Handles the "__none__" sentinel
    // by returning -1 without probing.
    // typeName narrows the lookup to one driver type, which lets the answer
    // come from the cached device list instead of opening the device.
    int probeDeviceChannelCount(const std::string& name, bool isInput,
                                const std::string& typeName = "");

    // The driver type device probes should ask about: the target type on
    // a cross-driver swap, otherwise the type actually open. Callers
    // outside a swap pass no scope and get the current type.
    std::string probeDriverTypeName(
        const clockwork::device::SwapScope& scope = {}) const;

    // Returns the sample rates advertised by a named device, or an
    // empty vector if the name doesn't match or the device can't be
    // opened. Core primitive: the rate-matching helpers below compose
    // it so there's one place that owns "walk device types → scan →
    // createDevice → getAvailableSampleRates".
    std::vector<double> probeDeviceSampleRates(const std::string& name,
                                               bool isInput);


    // Records the user's preferred output/input device for future
    // hot-plug re-attach, and caches the successfully-used sample rate
    // for this device name so a later switch back can restore it.
    // Called from switchDevice's success tail. deviceName empty means
    // rate/buffer-only change — no preference update. Internal-origin
    // swaps record only the rate memory (a device fact): preference and
    // pending-intent state belong to the user and survive recovery
    // fallbacks untouched.
    void recordSwapPreferences(const std::string& deviceName,
                               const std::string& inputDeviceName,
                               double sampleRate,
                               SwapOrigin origin);

    // What the OS says its default output is, and whether it is wireless or
    // virtual — the follow policy and setDeviceMode's system path branch on
    // both. Empty name when it cannot be read. macOS asks the HAL; elsewhere
    // the current device type's default device is the default (WASAPI lists
    // the system default first). Under the factory boundary the factory's
    // device type answers on every platform, as it does for the rest of the
    // device edge.
    struct SystemDefaultOutput {
        std::string name;
        bool        wireless  = false;
        bool        isVirtual = false;
    };
    SystemDefaultOutput systemDefaultOutput();
    // Open the system default output, keeping the input: system mode's way
    // onto a device (setDeviceMode, followDefaultOutput). Empty on success.
    std::string openSystemDefault();

    ClockworkProcessor mProcessor;
    // What the device manager calls: the processor behind Smoothie's worklet.
    std::unique_ptr<smoothie::JuceDeviceCallback> mDeviceCallback;
    // The default egress transport: in-process, replies via onReply. An embedder
    // injects a real transport via setTransport (a host that owns sockets
    // injects one from the comms client library). The engine owns no socket.
    // Bound to onReply by pointer so a runtime callback swap is seen.
    CallbackTransport mDefaultTransport{&onReply, &onReplyRouted};
    IOscTransport*    mTransport = nullptr;
    OscEgress         mEgress;
    // The boundary (audio thread): ONE predicate, clockwork vs DSP. No route table.
    OscSplit           mSplit;
    // Clockwork's own dispatch, both sides of it. Neither can name an address
    // outside the reserved prefix — add() takes only what follows it.
    ClockworkSysRoutes      mAudioRoutes;  // audio thread: answer here, or forward to NRT
    ClockworkSysRoutes      mControlRoutes;  // NRT thread: subsystem commands by sub-namespace
    // OSC address currently being handled on the NRT gateway, for the slow-pass
    // report. Written and read on that thread only; empty between commands.
    std::array<char, 64> mInFlightCommand{};
    void              noteInFlightCommand(const uint8_t* data, uint32_t size);
    EngineControl     mEngineControl;
#ifdef CLOCKWORK_MIDI
    MidiControl       mMidiControl;
#endif
#ifdef CLOCKWORK_GAMEPAD
    GamepadControl    mGamepadControl;
#endif
#ifdef CLOCKWORK_OSC
    OscControl        mOscControl;
#endif
#if CLOCKWORK_HAS_PLUGIN_TRACKS
    TrackControl      mTrackControl;
#endif
    ClockworkClock        mClockworkClock;
    // The audio half of Link, beside the clock: declared after it because it
    // borrows the clock's session and registers as its visibility listener.
    LinkAudioHost   mLinkAudio{mClockworkClock};
    // MIDI clock out: the ring the render thread drains and the NRT gateway
    // fills. Unconditional, like its source file: a build without MIDI still
    // owns one, and its ticks are counted as dropped rather than the feature
    // quietly not existing.
    MidiClockOut    mMidiClockOut;

    // Manual-pump state for pumpAudioBlock(): sample position advanced by the
    // caller's thread, and a one-shot flag to anchor the audio-thread clock on
    // the first pump.
    double mManualSamplePos    = 0.0;
    bool   mManualPumpStarted  = false;

    // Audio-plane ingress: the default OscIngress route writes the IN ring
    // directly. The ring pointers moved off the transport so it stays a dumb
    // pipe; the engine owns the audio plane.
    uint8_t*              mInBufferStart = nullptr;
    uint32_t              mInBufferSize  = 0;
    std::atomic<int32_t>* mInHead        = nullptr;
    std::atomic<int32_t>* mInTail        = nullptr;
    std::atomic<int32_t>* mInSequence    = nullptr;
    std::atomic<int32_t>* mInWriteLock   = nullptr;

    // NRT control plane: the audio-thread ingress forwards clockwork verbs it
    // does not answer itself onto this process-local, Message-framed ring. The
    // control pass (mNrtGateway's registry) drains it and runs the subsystems
    // off the audio thread — on the engine's gateway thread, woken every audio
    // block via processCount, or on the host's own (Config::hostDrivesControl,
    // controlPass()). Native-only (wasm has no NRT thread).
    // Sized so a whole UDP datagram inside /clockwork/osc/send — the widest
    // control message a client can put on the wire — crosses this hop with
    // room for a few more behind it. A frame that will not fit is dropped and
    // counted, and the log says so.
    static constexpr uint32_t kNrtRingSize = 262144;
    std::array<uint8_t, kNrtRingSize> mNrtBuffer{};
    // What the audio thread prefixes each control frame with, so the call's
    // time survives the hop (the ring header carries only the origin token).
    struct NrtEnvelope {
        int64_t when      = 0;   // the message's OSC timetag; 0/1 = immediate
        int64_t blockTime = 0;   // the block it was fired in
    };
    // The widest command the hop carries: a datagram (65507) and the verb
    // that wraps it, rounded up. The frame the envelope is prefixed in is
    // this object's, not the audio thread's stack: the audio thread is the
    // ring's only producer (nrtForwardSink is an audio route), so one buffer
    // it owns is enough, and 64 KB is not something to put on a stack.
    static constexpr uint32_t kNrtMaxCommand = 66560;
    alignas(8) std::array<uint8_t, sizeof(NrtEnvelope) + kNrtMaxCommand> mNrtFrame{};
    // How many over-wide control messages this engine has said it dropped;
    // it says so eight times and then keeps count in the metrics alone.
    std::atomic<uint32_t>     mNrtOversizeLogged{0};
    std::atomic<int32_t>      mNrtHead{0};
    std::atomic<int32_t>      mNrtTail{0};
    std::atomic<int32_t>      mNrtSeq{0};
    std::atomic<int32_t>      mNrtLock{0};

    // The control pass: its registry of drains and tasks, and — unless the
    // host drives control — the gateway thread that runs it every audio block.
    // The NRT_OUT_BUFFER SHM region carries ALL NRT-thread outgoing OSC —
    // command replies, Link/device notifications, off-thread debug — written
    // multi-producer through the lanes producer (clockwork_egress_nrt_write, which
    // owns the lock). The pass drains it (unless the host drains egress) and
    // is the sole transport caller, so no engine thread ever touches a
    // socket. (Web has no NRT thread.)
    RingReader                mNrtGateway{"clockwork-NrtGateway"};
    // The door controlPass() comes through: open from the point init() would
    // start the gateway thread until shutdown() closes it, which then waits
    // for a pass in flight — the host-driven equivalent of the thread join.
    std::atomic<bool>         mControlOpen{false};
    std::atomic<uint32_t>     mControlPassesInFlight{0};
    // Held by the one thread inside the pass; a second is refused.
    std::atomic<bool>         mControlPassBusy{false};
    // Set while a cold swap rebuilds the arena the pass drains: a host's
    // controlPass() runs nothing meanwhile. The thread inside a pass, so a
    // swap run from within one does not wait for itself.
    std::atomic<bool>            mControlParked{false};
    std::atomic<std::thread::id> mControlPassThread{};
    // Park the control pass for a cold swap's arena rebuild, and let it go,
    // whoever runs it: the engine's own gateway thread or a host. False if
    // the pass did not park in time and the swap goes on beside it.
    bool parkControlPass();
    void resumeControlPass();
    // Every frame waiting in the egress rings, to the transport, on the
    // calling thread: the control pass's work, done by a cold swap too once
    // the pass is parked (the rings' only reader then).
    void drainEgressNow();

    // Peer command plane (SHM segment; shm_peer_plane.h). init() publishes the
    // segment's plane here when Config::shmCommands is set; shutdown() nulls it
    // before the segment unmaps. The gateway task drains its command ring into
    // the ingest path; ShmTransport (bound to this slot via peerPlaneSlot())
    // produces its reply ring. Null ⇒ the plane is inert.
    std::atomic<ShmPeerPlaneHeader*> mPeerPlane{nullptr};
    ClockworkDrainState                     mPeerDrainState;

    // Debounced device switch — rapid clicks settle into one final switch.
    struct PendingSwitch {
        std::string devName;
        std::string inputDevName;
        double sampleRate = 0;
        int bufferSize = 0;
        std::chrono::steady_clock::time_point timestamp;
        bool active = false;
    };
    std::mutex                 mPendingSwitchMutex;
    PendingSwitch              mPendingSwitch;                 // guarded by mPendingSwitchMutex
    bool                       mDebounceSwitchRunning = false; // guarded by mPendingSwitchMutex
    std::atomic<bool>          mDebounceSwitchStop{false};
    void executePendingSwitch();   // runs on the device task lane

    // THE device lane. The devices a device boot opens are opened, started,
    // switched, recovered and closed here, and nowhere else: boot's open and
    // start, every control verb's switch, every pass of reconcileDevices,
    // recovery, and shutdown's close — serialised, on one thread, in the
    // apartment the device layer needs (DeviceLaneApartment.h). Started on
    // first use (a device boot's open) and joined in shutdown().
    std::jthread                       mDeviceTaskThread;
    std::mutex                         mDeviceTaskMutex;
    std::deque<std::function<void()>>  mDeviceTasks;
    std::atomic<bool>                  mDeviceTaskStop{false};
    clockwork::device::DeviceLaneApartment mDeviceLane;   // its apartment and its wake
    clockwork::device::ProcessComApartment mProcessCom;   // any other thread's COM
    void deviceTaskLoop(const std::stop_token& stop);
    // `work` on the device lane, waited for; its exception, if any, rethrown
    // here. On the calling thread when the lane has stopped (shutdown).
    void runOnDeviceLane(const std::function<void()>& work);

    // Device reopen — rejected while one is in flight or within a short cooldown
    // after completion; accepted requests run on the device task lane.
    std::atomic<bool>          mReopenInProgress{false};
    // watchdogNowMs() at the last recovery's completion. Atomic: written by
    // the recovery worker, read by the watchdog / control threads.
    static constexpr int64_t   kNeverMs = INT64_MIN;
    std::atomic<int64_t>       mLastReopenFinishedAtMs{kNeverMs};
    // Recreate the device manager, rebuild the real device, and promote back to
    // it — or, if none opens, settle into the idle "waiting for audio device"
    // state. Runs on the device task lane (posted by requestAudioRecovery) so
    // it works on every platform: the Linux standalone loop never pumps the
    // JUCE message queue, and this keeps the multi-second swap off the
    // message thread everywhere. The intent says whether the reopen requests
    // a rate (see RecoveryIntent).
    void recoverAudio(RecoveryIntent intent);

    // ── Explicit device-mutation phase (enum declared public, above) ────
    std::atomic<DevicePhase>   mDevicePhase { DevicePhase::Idle };
    struct PhaseGuard {
        std::atomic<DevicePhase>& phase;
        DevicePhase prev;
        PhaseGuard(std::atomic<DevicePhase>& p, DevicePhase v)
            : phase(p), prev(p.load()) { p.store(v); }
        PhaseGuard(const PhaseGuard&) = delete;
        PhaseGuard& operator=(const PhaseGuard&) = delete;
        ~PhaseGuard() { phase.store(prev); }
    };

    // Broadcast /clockwork/devices/reopen.done to the client/GUI. Every accepted
    // recovery reports exactly one of these so a /reopen caller never hangs.
    void broadcastReopenDone(bool success, const std::string& deviceName,
                             double sampleRate, int bufferSize,
                             const std::string& error);

    // Callback-starvation watchdog (Config::callbackWatchdog) — samples
    // processCount and recovers a source whose callbacks stopped. Its
    // monitors live between polls here; a poll runs on the watchdog's thread,
    // or on a test's (watchdogPoll) when the test owns the clock.
    struct WatchdogState {
        explicit WatchdogState(const Config& cfg);
        int64_t stallMs;
        int     pollMs;
        bool    rateCheck;
        // Liveness by SUSTAINED ticks: a lone tick (one callback per failed
        // attempt) reads as Confirming, not Live, so it can't masquerade as
        // recovered. Ticks must sustain for a stall window to count as live.
        clockwork::audio::LivenessMonitor liveness;
        // Rate skew by rendered frames against a monotonic clock: a device
        // can keep ticking while its timer free-runs fast or slow
        // (post-sleep DirectSound), splitting the audio timebase seconds from
        // the wall clock. maxGap of a few polls makes any sampling pause
        // discard the window rather than read as skew.
        clockwork::audio::RateSkewMonitor rateSkew;
        // Detection is logged once per skew episode, separately from the
        // recovery action: requestAudioRecovery can be gated (cooldown /
        // already in flight) for many polls, and a silent gated detection
        // would leave user logs with no trace of WHY a later recovery fired.
        bool rateSkewLogged = false;
        // What to do about a verdict. A reopen fixes a transient skew and is
        // the wrong tool for a persistent one — a device clocked at 44.1k
        // that reports 48k skews identically after every reopen, and
        // reopening it every window is a storm that stops the client's jobs
        // each time. The policy counts same-ratio verdicts, switches the
        // remedy to "adopt the measured rate", and after that gives up for
        // the session. Its streak is only advanced by recoveries that
        // actually launched (acted), and cleared by a window that came back
        // healthy (healthy) — see RateSkewPolicy. "The same fault" is judged
        // with twice the skew tolerance: a verdict is one noisy measurement
        // against 1.0, but two consecutive verdicts are two noisy
        // measurements against each other, and a 0.919x device read as 0.89x
        // then 0.94x on a loaded machine (macOS CI, 2026-09-13) is still one
        // device — with the tolerance alone the streak restarted at every
        // wobble and the adopt came after five plain reopens instead of two.
        clockwork::audio::RateSkewPolicy skewPolicy;
        uint64_t skewWindowsSeen  = 0;
        int      skewGoodWindows  = 0;   // consecutive, since the last bad one or recovery
        int      skewGoodRequired;
        bool     skewGiveUpLogged = false;
    };
    std::unique_ptr<WatchdogState> mWatchdog;
    std::jthread               mWatchdogThread;
    std::atomic<uint32_t>      mWatchdogRecoveries{0};
    std::atomic<uint32_t>      mRateSkewRecoveries{0};
    void watchdogLoop(const std::stop_token& stop);
    int64_t watchdogNowMs() const;   // Config::watchdogClockMs, else steady_clock

    std::unique_ptr<HeadlessDriver> mHeadlessDriver;
    std::unique_ptr<juce::AudioDeviceManager> mDeviceManager;
    PerformanceMetrics*          mMetrics = nullptr;  // points into the shared arena; null before init()
    std::atomic<bool>        mRunning{false};
    std::atomic<EngineState> mEngineState{EngineState::Stopped};
    // Why the engine is in error, replayed to a client that registers after
    // the transition (snapshotStateTo). Set on every move into Error.
    mutable std::mutex       mErrorReasonMutex;
    std::string              mErrorReason;
    // Read by Link network-thread callbacks before they touch the egress.
    // Cleared early in shutdown() so in-flight callbacks skip
    // broadcastLinkNotify. Narrows the window; shutdown's
    // setLinkVisibility(Off) call that follows joins the Link thread
    // and closes the race fully.
    std::atomic<bool>        mLinkCallbacksAlive{true};
    bool                     mHeadless{false};
    Config                   mCurrentConfig;
    int                      mBootInputChannels = 2;  // original -i value, for re-enabling inputs
    // Rate held while on a non-wireless device. Remembered so a detour
    // through wireless (AirPlay/Bluetooth) — which forces 44.1 or its own
    // negotiated rate — doesn't leave the engine sticky at that rate once
    // the user switches back to a hardware device that can do the original.
    int                      mPreWirelessRate = 0;

    // User's preferred output device name across hot-plug cycles. Set from
    // -H / sound_card_name at boot even when that device isn't present at
    // boot time (so we fall back to system default now but auto-re-attach
    // when the device reappears), and also from explicit user switches.
    // Empty = no preference (follow macOS default, don't auto-switch on
    // hot-plug). mDeviceMode tracks "current selection regardless of
    // whether device is present"; mPreferredOutputDevice tracks "want to
    // use this device whenever it's available".
    std::string              mPreferredOutputDevice;
    // The preferred output is still -H's words, not yet a device's name: no
    // device they match has been seen. Matched the way -H matches ("motu"
    // finds "motu-xaero") when the device list changes, and replaced by the
    // name of the device they find. An exact name is never matched loosely:
    // "USB Audio" absent must not become "USB Audio Pro" present.
    bool                     mPreferredOutputIsWords = false;
    // Same for the input.
    std::string              mPreferredInputDevice;
    std::string              mLastInputDeviceName;    // saved on disable, restored on re-enable
    // Recursive so the device readers (currentDevice/listDevices/currentDriver/
    // listDrivers/sendDeviceReport) can take it to serialise against device
    // mutations without deadlocking the mutation paths that call those readers
    // while already holding it, and so a recovery can hold it across
    // reopenCurrentDevice (which re-takes it). mutable for the const readers.
    mutable std::recursive_mutex mSwapMutex;

    // listDrivers() cache — avoids re-scanning every AudioIODeviceType
    // on every /clockwork/info push. Re-scanning is expensive (each
    // scan touches JACK / ASIO / etc. whether or not they're usable)
    // and, on Linux without a JACK server, produces stderr spam from
    // libjack's connect() failures. Cache TTL is short so a user who
    // starts jackd mid-session sees JACK reappear within a few seconds.
    mutable std::mutex                           mListDriversMutex;
    mutable std::vector<std::string>             mCachedDrivers;
    mutable std::chrono::steady_clock::time_point mCachedDriversAt{};
    // Cache for listDevices(false). Building this list calls JUCE's
    // type->createDevice() + initialise() per device — on Windows that's a
    // full WASAPI IAudioClient activation each time, ~50–100 ms per device.
    // With ~150 device/type combinations on a typical machine the call takes
    // ~10 s, which during boot starves the OSC thread and causes the client's
    // /clockwork/notify handshake to time out. Refreshed by listDevices(true),
    // which a pass runs when the list changed (reconcileDevices). Empty until
    // the first scan.
    mutable std::mutex                           mListDevicesMutex;
    mutable std::vector<DeviceInfo>              mCachedDevices;
    // Each listed device's probe (its rates, buffer sizes and channels, read
    // from a device created to be asked), by driver, name and direction, so
    // a rescan probes only devices it has not seen: on Windows each probe
    // activates the device. A driver that describes a device without opening
    // it (traits with its rates) is never probed. A name gone from its driver's list is forgotten, so a
    // device plugged back in is probed afresh. Under the gate (listDevices).
    mutable std::map<std::string, DeviceInfo>    mDeviceProbes;
    // The audio device list as reconcileDevices last reported it, so
    // a change that leaves the list as it was (a device's own property, an
    // aggregate the device layer made) doesn't re-send the device report. Device lane only.
    std::string                                  mLastAudioDeviceFingerprint;
    // Read by the host's CFRunLoop pump at every tick (isRunLoopSuppressed).
    std::atomic<bool>        mSuppressRunLoop{false};
    std::string              mDeviceMode;   // empty = system/auto, non-empty = manual device name
    bool                     mDspRebuilt{false};
    // Set while switchDevice is taking the engine back to the device it was
    // playing on, after the new one delivered no audio. Under the swap gate.
    // A rollback whose device is silent too stops there: it does not roll back.
    bool                     mNoAudioRollbackInFlight{false};
    std::map<std::string, int> mDeviceRateMemory; // per-device remembered sample rate

    // Cold-swap generation. Cross-thread: write from setEngineState on
    // the audio-management thread, read from the OSC server thread.
    std::atomic<uint32_t>    mSetupGeneration{1};

    // Per-driver-type remembered device name. Written by
    // recordSwapPreferences after a successful open; read by
    // switchDriver to delegate "change driver" to "open the device
    // the user last opened on that driver." Closes the
    // alphabetical-first-auto-pick hazard.
    std::map<std::string, std::string> mPreferredDeviceByDriver;

    // User's pending driver pick when no per-driver preference exists
    // yet (ASIO with no remembered device, or any driver with no
    // visible devices). currentDriver() stays truthful — this field
    // is only exposed via intendedDriver(). Cleared in
    // recordSwapPreferences on the next successful open.
    std::string              mIntendedDriver;

    // Driver (JUCE device type) selected at boot: the --audio-driver
    // request when it resolved, else the platform default ("DirectSound"
    // on Windows, empty elsewhere). Recovery paths that rebuild the
    // device manager restore this instead of re-deriving the platform
    // default, so a recovered engine doesn't silently change driver.
    std::string              mBootDriver;

    // (output, input) pairs that JUCE rejected. Populated from
    // switchDevice's input-fallback branch when setAudioDeviceSetup
    // returns "Couldn't open the input device!" — typically WASAPI
    // Shared / DirectSound combiner refusing a full-duplex hardware
    // combination. Queried by sendDeviceReport to hide known-bad
    // inputs from the dropdown while the matching output is active.
    mutable std::mutex                                mUngatableInputPairsMutex;
    std::set<std::pair<std::string, std::string>>    mUngatableInputPairs;

    // Shared memory — owned by the engine, survives across cold swaps.
    std::unique_ptr<shm_segment_creator> mShmemCreator;

    // The engine's own handle onto the client boundary. Every message that
    // reaches ingest() — a UDP datagram, a stream peer, an in-process call —
    // goes out through this, so a remote client's traffic and an embedder's
    // meet the same code. Opened over the arena at init, closed at shutdown.
    struct ClockworkClient* mClientHandle = nullptr;

    // Opened on first use by whichever side gets there first — ingest or the
    // egress gateway. Not at construction: `shared_memory` is not chosen until
    // init_memory() runs.
    // Opened once in bringUp, after the arena is published and before any thread can ask for it.
    void openClient();

    // The guest's arena (DspConfig::arena). NOT in the shared segment: no
    // client maps it, and while it lived there it existed only when a client
    // did — an engine embedded with no segment handed its guest no working
    // memory at all. Allocated once and reused across cold swaps;
    // audio_processor zeroes it at every dsp_new.
    std::unique_ptr<uint8_t[]> mGuestArena;   // NOLINT(modernize-avoid-c-arrays): a raw block got with nothrow new and refused on failure (bringUp), not a container

    // The inbox and outbox when there is no segment to carve them from — one
    // block, inbox first. A plugin has no public segment and still has a
    // client: the host process itself, reading in place.
    std::unique_ptr<uint8_t[]> mGuestLanes;   // NOLINT(modernize-avoid-c-arrays): as mGuestArena
    size_t mGuestLanesInboxBytes  = 0;   // how mGuestLanes is split
    size_t mGuestLanesOutboxBytes = 0;
    // Where the inbox ended up, whichever of the two above supplied it.
    uint8_t* mGuestInbox      = nullptr;
    uint32_t mGuestInboxBytes = 0;
    uint8_t* mGuestOutbox      = nullptr;
    uint32_t mGuestOutboxBytes = 0;

};
