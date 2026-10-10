// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
#include "ClockworkEngine.h"
#include "DeviceManagerFactory.h"
#include "HeadlessDriver.h"
#include "smoothie_juce_device_callback.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include "clockwork_product.h"
#include "clockwork_sys.h"
#include "clockwork_client.h"
#include "../clockwork_heap.h"
#include "clockwork_config.h"   // clockwork_log
// MIDI clock out is ClockworkClock's, not the scheduler's: each tick carries its
// own time to its sink, so it is present whatever this build decided about
// the timed queue (matching audio_processor.cpp).
#include "clock/MidiClockOut.h"
#include "DevicePolicy.h"
#include "AsioDriverCheck.h"
#include "PipeWireAudio.h"
#include "audio_processor.h"
#include "lanes/lanes.h"
#include "audio_config.h"
#include "shared_memory.h"
#include "osc_debug.h"
#include "clock/clock_math.h"
#include "native/AudioBlockClock.h"
#include "osc/OscReceivedElements.h"
#include "OscBuilder.h"
#include "osc/OscOutboundPacketStream.h"
#include "RingBufferWriter.h"
#include "IngressCallCtx.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <future>
#include <thread>
#ifdef __linux__
#include <dlfcn.h>
#endif
#ifdef __APPLE__
#include "MicPermission.h"
#endif

extern "C" {
    // Publishes NRT control-thread blocking into the native-stats region.
    // Defined in engine_support.cpp; called from the watchdog poll below.
    void clockwork_publish_nrt_blocking(uint32_t maxPassUs, uint32_t recentWorstUs,
                                  uint32_t inFlightUs);

    // Unified-arena base. When non-null, init_memory() points the engine's
    // whole shared_memory.h blob (rings, control, metrics, node-tree, audio
    // taps, scope) at this address — the public POSIX segment — instead of the
    // process-local ring_buffer_storage. Everything is observable for free.
    extern uint8_t* g_external_segment;

    // Real-pointer arena base (== init_memory's `shared_memory`). Use for any
    // engine region so native glue stays consistent with the unified arena.
    void* get_shared_memory_base();

    void destroy_dsp();
    void rebuild_dsp(double sample_rate);
}

namespace {
// Name published to OS registries (PipeWire nodes, ALSA seq MIDI clients,
// macOS aggregate devices, Link peers). Written once in init() from
// cfg.appName before any device manager or MIDI subsystem exists, read-only
// after; consumers declare `extern "C" const char* clockwork_app_name()`.
std::string& publishedAppName() {
    static std::string name = CLOCKWORK_PRODUCT_NAME;
    return name;
}
}

extern "C" const char* clockwork_app_name() { return publishedAppName().c_str(); }

namespace {
using clockwork::device::sameDeviceName;

#ifdef __linux__
// Silence libjack's stderr chatter where no jackd / pipewire-jack server is
// running: with JUCE_JACK=1, jack_client_open() during scanForDevices makes
// libjack write a two-line connect-failed pair for every attempt. JUCE still
// sees the open-failed return and reports zero JACK devices.
void silentJackLog(const char*) {}

void silenceJackLogsIfPossible() {
    // Only relevant once libjack is loaded — JUCE dlopens it lazily inside
    // JackAudioIODeviceType's ctor, so calling this after AudioDeviceManager
    // construction catches that first scan. No libjack installed: dlopen
    // fails and we skip.
    void* handle = dlopen("libjack.so.0", RTLD_LAZY | RTLD_NOLOAD);
    if (!handle) handle = dlopen("libjack.so.0", RTLD_LAZY);
    if (!handle) return;
    using set_fn = void (*)(void (*)(const char*));
    if (auto set_err  = (set_fn)dlsym(handle, "jack_set_error_function"))
        set_err(silentJackLog);
    if (auto set_info = (set_fn)dlsym(handle, "jack_set_info_function"))
        set_info(silentJackLog);
    // Don't dlclose — JUCE needs libjack resident for its own dlsym path.
}
#endif
}

ClockworkEngine::ClockworkEngine()
    : mDeviceCallback(std::make_unique<smoothie::JuceDeviceCallback>(mProcessor)),
      mHeadlessDriver(std::make_unique<HeadlessDriver>()) {}

void ClockworkEngine::recordSwapPreferences(const std::string& deviceName,
                                             const std::string& inputDeviceName,
                                             double sampleRate,
                                             SwapOrigin origin) {
    if (!deviceName.empty() && sampleRate > 0) {
        // Cap against unbounded growth over long sessions of hot-plug churn. 32 is
        // well above any realistic device set; forgotten entries get re-probed
        // next time the device is chosen.
        static constexpr size_t kMaxDeviceRateMemoryEntries = 32;
        if (mDeviceRateMemory.size() >= kMaxDeviceRateMemoryEntries
            && mDeviceRateMemory.find(deviceName) == mDeviceRateMemory.end())
            mDeviceRateMemory.clear();
        mDeviceRateMemory[deviceName] = static_cast<int>(sampleRate);
    }

    // Everything below is user-intent state. Internal swaps (recovery
    // reopen, hotplug re-attach) record only the rate memory above — a
    // recovery landing on the system default must not become the
    // "preferred" device, and must not consume a pending driver intent.
    if (origin != SwapOrigin::User)
        return;

    // Track the user's long-lived preferred device for hot-plug re-attach; an
    // explicit deviceName means the caller picked it, so remember it even
    // across cycles where the device disappears. inputDeviceName ==
    // "__none__" means disable inputs, so clear the preferred input.
    if (!deviceName.empty()) {
        mPreferredOutputDevice  = deviceName;
        mPreferredOutputIsWords = false;
    }
    if (inputDeviceName == "__none__")
        mPreferredInputDevice.clear();
    else if (!inputDeviceName.empty())
        mPreferredInputDevice = inputDeviceName;

    // Per-driver memory: record the just-opened device under the
    // driver JUCE actually opened on. switchDriver reads this to
    // delegate driver-only picks to an explicit-name switchDevice,
    // closing the alphabetical-first-auto-pick hazard. Use JUCE's
    // type directly — currentDriver() hides the intent fallback.
    if (!deviceName.empty() && mDeviceManager) {
        if (auto* dev = mDeviceManager->getCurrentAudioDevice()) {
            std::string drv = dev->getTypeName().toStdString();
            if (!drv.empty())
                mPreferredDeviceByDriver[drv] = deviceName;
            mIntendedDriver.clear();
        }
    }
}

std::string ClockworkEngine::refuseUnknownDeviceName(
        const std::string& deviceName,
        const std::string& inputDeviceName) {
    if (!mDeviceManager) return {};
    if (deviceName.empty() && inputDeviceName.empty()) return {};
    std::vector<std::string> visibleNames;
    for (auto& d : listDevices(false)) visibleNames.push_back(d.name);
    return clockwork::device::validateSwapDeviceNames(
        deviceName, inputDeviceName, visibleNames);
}

std::string ClockworkEngine::refuseUnpairableInput(
        const std::string& deviceName,
        const std::string& inputDeviceName) {
    // Only applies to "add an input while keeping the current output" swaps.
    if (!deviceName.empty()) return {};
    if (inputDeviceName.empty() || inputDeviceName == "__none__") return {};
    if (!mDeviceManager) return {};
    auto* cur = mDeviceManager->getCurrentAudioDevice();
    auto* type = mDeviceManager->getCurrentDeviceTypeObject();
    if (!cur || !type) return {};
    const juce::String curOut = cur->getName();
    if (sameDeviceName(inputDeviceName, curOut.toStdString())) return {};
    const auto traits = type->getDeviceTraits(curOut);
    if (traits.pairs) return {};
    std::string err = "can't add input '" + inputDeviceName
                    + "' — current output '" + curOut.toStdString() + "' is "
                    + (traits.wireless ? "wireless, and " : "")
                    + "can't be paired with another device's input";
    clockwork_log("[switchDevice] %s", err.c_str());
    return err;
}

namespace {
// The device layer's diagnostics (smoothie writes them through JUCE's
// Logger) on the engine's log. One for the process, installed once, and
// never over a logger the embedder set.
struct DeviceLayerLog final : juce::Logger {
    void logMessage(const juce::String& message) override {
        clockwork_log("%s", message.toRawUTF8());
    }
};

void routeDeviceLayerLog() {
    static DeviceLayerLog log;
    static std::once_flag once;
    std::call_once(once, [] {
        if (juce::Logger::getCurrentLogger() == nullptr)
            juce::Logger::setCurrentLogger(&log);
    });
}
} // namespace

std::unique_ptr<juce::AudioDeviceManager>
ClockworkEngine::makeDeviceManager() {
    routeDeviceLayerLog();
    auto manager = mCurrentConfig.deviceManagerFactory
        ? mCurrentConfig.deviceManagerFactory->make()
        : std::make_unique<juce::AudioDeviceManager>();
    // Device changes come to the engine, not to the manager: left to itself
    // the manager closes and reopens devices on whatever thread the OS
    // reports a change, under the device lane's feet.
    manager->setDeviceChangeSink([this](juce::AudioIODeviceType::DeviceChange change) {
        using Change = juce::AudioIODeviceType::DeviceChange;
        devicesChanged(change == Change::list          ? kListChanged
                     : change == Change::systemDefault ? kDefaultChanged
                                                       : kOpenDeviceChanged);
    });
    // What the device layer names in the OS after the app: a CoreAudio
    // aggregate device pairing an output with an input, a PipeWire node.
    manager->setClientName(juce::String(publishedAppName()));
    return manager;
}

std::string ClockworkEngine::probeDriverTypeName(
        const clockwork::device::SwapScope& scope) const {
    if (scope.crossDriver) return scope.targetDriver;
    if (!mDeviceManager) return {};
    if (auto* dev = mDeviceManager->getCurrentAudioDevice())
        return dev->getTypeName().toStdString();
    return mDeviceManager->getCurrentAudioDeviceType().toStdString();
}

int ClockworkEngine::probeDeviceChannelCount(const std::string& name,
                                              bool isInput,
                                              const std::string& typeName) {
    if (name.empty() || name == "__none__") return -1;
    if (!mDeviceManager) return -1;

    // The currently open device answers from its live handle: authoritative
    // and free. Its cache entry is never probed (listDevices must not
    // createDevice on a device that is already open), and creating a second
    // instance below can fail or disturb the live one on some drivers.
    if (auto* cur = mDeviceManager->getCurrentAudioDevice()) {
        if (sameDeviceName(cur->getName().toStdString(), name)
            && (typeName.empty()
                || cur->getTypeName().toStdString() == typeName)) {
            const int n = isInput ? cur->getInputChannelNames().size()
                                  : cur->getOutputChannelNames().size();
            if (n > 0) return n;
        }
    }

    // Use the cached device list: createDevice() below costs a full driver
    // init (~0.7 s on ASIO, worse for registered-but-unplugged hardware).
    //
    // ONLY when typeName is known — one device name has different counts under
    // different drivers (a MOTU is 2-in on ASIO, 24-in on WASAPI) — and ONLY
    // when the count was genuinely probed, since unprobed entries carry
    // placeholders good enough for a list but not for a swap decision.
    if (!typeName.empty()) {
        for (const auto& dev : listDevices(false)) {
            if (dev.typeName != typeName) continue;
            if (!sameDeviceName(dev.name, name)) continue;
            if (!(isInput ? dev.inChannelsProbed : dev.outChannelsProbed)) continue;
            const int n = isInput ? dev.maxInputChannels : dev.maxOutputChannels;
            if (n > 0) return n;
        }
    }

    auto& types = mDeviceManager->getAvailableDeviceTypes();
    for (auto* type : types) {
        for (auto& n : type->getDeviceNames(isInput)) {
            if (!sameDeviceName(n.toStdString(), name)) continue;
            auto outArg = isInput ? juce::String() : n;
            auto inArg  = isInput ? n : juce::String();
            std::unique_ptr<juce::AudioIODevice> probe(
                type->createDevice(outArg, inArg));
            if (probe)
                return isInput ? probe->getInputChannelNames().size()
                               : probe->getOutputChannelNames().size();
        }
    }
    return -1;
}

std::vector<double> ClockworkEngine::probeDeviceSampleRates(
        const std::string& name, bool isInput) {
    std::vector<double> result;
    // Serialise mDeviceManager access against swaps/recovery — the
    // scanForDevices() below mutates each type's cached device lists, which
    // listDevices() readers iterate. Same gate discipline as listDevices().
    std::lock_guard<std::recursive_mutex> gate(mSwapMutex);
    if (name.empty() || !mDeviceManager) return result;
    auto& types = mDeviceManager->getAvailableDeviceTypes();
    for (auto* type : types) {
        type->scanForDevices();
        for (auto& n : type->getDeviceNames(isInput)) {
            if (!sameDeviceName(n.toStdString(), name)) continue;
            auto outArg = isInput ? juce::String() : n;
            auto inArg  = isInput ? n : juce::String();
            std::unique_ptr<juce::AudioIODevice> probe(
                type->createDevice(outArg, inArg));
            if (!probe) return result;
            for (auto r : probe->getAvailableSampleRates())
                result.push_back(r);
            return result;
        }
    }
    return result;
}


#if defined(__linux__) && defined(CLOCKWORK_PIPEWIRE)
// Touch the type list before adding ours: JUCE only creates its built-in
// types (ALSA/JACK) when the list is empty, so appending first would leave
// PipeWire as the sole driver.
static void registerPipeWireDriver(juce::AudioDeviceManager& dm) {
    dm.getAvailableDeviceTypes();
    if (auto pwType = createPipeWireAudioIODeviceType())
        dm.addAudioDeviceType(std::move(pwType));
}

// Prefer the native PipeWire driver whenever the daemon exposes devices:
// friendly names, follows the system default, and needs neither
// pipewire-alsa nor pipewire-jack. The scan matters on its own — JUCE
// settles default-device init on the first type that reports devices, and
// a never-scanned type reports none.
static void preferPipeWireDriverIfAvailable(juce::AudioDeviceManager& dm) {
    for (auto* t : dm.getAvailableDeviceTypes()) {
        if (t->getTypeName() != "PipeWire")
            continue;
        t->scanForDevices();
        if (t->getDeviceNames(false).size() > 0)
            dm.setCurrentAudioDeviceType("PipeWire", true);
        break;
    }
}
#endif

void ClockworkEngine::setEngineState(EngineState state, const std::string& reason) {
    if (state == EngineState::Error) {
        // Kept for a client that registers later: an error's reason is the
        // one thing it needs, and the transition has gone by.
        std::lock_guard<std::mutex> lock(mErrorReasonMutex);
        mErrorReason = reason;
    }
    EngineState prev = mEngineState.exchange(state);
    if (prev == state) return;  // no transition

    const char* stateStr = engineStateToString(state);
    clockwork_log("[engine] state: %s -> %s (%s)",
                   engineStateToString(prev), stateStr,
                   reason.empty() ? "-" : reason.c_str());

    mEgress.sendStateChange(stateStr, reason.c_str());

    // /clockwork/setup fires ONLY when the DSP instance was actually rebuilt
    // (cold swap). It tells a client that everything the DSP was holding is
    // gone and whatever it tracked must be re-established. Sending it on a
    // swap-failed rollback or a hot swap — where the instance survives —
    // would make clients throw away state that is still live.
    if (state == EngineState::Running && mDspRebuilt) {
        mDspRebuilt = false;
        // Bump before emit so the wire value is the post-rebuild
        // generation (mSetupGeneration starts at 1; first cold swap = 2).
        uint32_t gen = mSetupGeneration.fetch_add(1) + 1;
        auto* dev = mDeviceManager ? mDeviceManager->getCurrentAudioDevice() : nullptr;
        int sr  = dev ? static_cast<int>(dev->getCurrentSampleRate()) : mCurrentConfig.sampleRate;
        int buf = dev ? dev->getCurrentBufferSizeSamples() : mCurrentConfig.bufferSize;
        mEgress.sendSetup(sr, buf, gen);
    }
}

void ClockworkEngine::snapshotStateTo(uint32_t token) {
    // Statechange only: lifecycle state is idempotent display state, safe to
    // replay to a late registrant. /clockwork/setup is deliberately NOT
    // replayed — it is an EVENT ("the DSP was rebuilt; reset what you were
    // tracking") and replaying it forced clients into spurious reinits. Late
    // joiners query config instead (devices/report, shm metrics).
    //
    // An error replays with its reason rather than "snapshot": a client that
    // arrives after the transition has no other way to learn why.
    const EngineState state = mEngineState.load();
    if (state == EngineState::Error) {
        std::lock_guard<std::mutex> lock(mErrorReasonMutex);
        mEgress.sendStateChangeTo(token, engineStateToString(state), mErrorReason.c_str());
        return;
    }
    mEgress.sendStateChangeTo(token, engineStateToString(state), "snapshot");
}

ClockworkEngine::~ClockworkEngine() {
    // A destructor that throws ends the process, and there is no one left to
    // answer: what shutdown could not do is logged and let go.
    try {
        shutdown();
    } catch (const std::exception& e) {
        clockwork_log("[engine] shutdown threw in the destructor: %s", e.what());
    } catch (...) {
        clockwork_log("[engine] shutdown threw in the destructor");
    }
}


void ClockworkEngine::init(const Config& cfg) {
    if (mRunning.load()) return;
    // What the devices depend on, started from the thread that boots the
    // engine: the one every host started it on, before the engine did it for
    // them (smoothie_worklet.h).
    smoothie::startPlatform();
    // The guest's config goes to it in a region of GUEST_CONFIG_SIZE bytes, its
    // NUL included. One that does not fit is refused here, before anything is
    // built, as the web side refuses it (writeGuestConfigToMemory): cut short
    // instead, it would boot a guest on a line sliced mid-value — the silent
    // misconfiguration the text block exists to rule out.
    if (cfg.guestConfig.size() + 1 > GUEST_CONFIG_SIZE) {
        throw std::runtime_error(
            "guest config is " + std::to_string(cfg.guestConfig.size() + 1)
            + " bytes with its NUL, but its region holds " + std::to_string(GUEST_CONFIG_SIZE)
            + ": raise GUEST_CONFIG_SIZE (shared_memory.h) or send less");
    }

    // Nothing logged during boot is lost: lines land on the debug ring the
    // moment init_memory has built it (see clockwork_log). The guard covers
    // every way out of here, including a throw before the ring exists.
    clockwork_log_hold();
    struct ReleaseLog {
        ReleaseLog() = default;
        ~ReleaseLog() { clockwork_log_release(); }
        ReleaseLog(const ReleaseLog&) = delete;
        ReleaseLog& operator=(const ReleaseLog&) = delete;
    } releaseLog;
#if CLOCKWORK_RUST_LOG
    // The Rust subsystems log onto the same ring from here on (a no-op after
    // the first engine in the process; the sink forwards to whichever ring
    // is up).
    clockwork_rust_log_install([](const char* line, uint32_t len) {
        clockwork_log("%.*s", static_cast<int>(len), line);
    });
#endif

    setEngineState(EngineState::Booting, "init");

    if (!cfg.appName.empty())
        publishedAppName() = cfg.appName;

    mHeadless = cfg.headless;
    mClockworkClock.setFreewheelClock(cfg.freewheelClock);
    mCurrentConfig = cfg;
    // mBootInputChannels may be kAutoChannelCount (-1) here — resolved to a
    // concrete count when enableInputChannels() is eventually called.
    mBootInputChannels = cfg.numInputChannels;

    // Seed the pre-wireless rate from the boot config, so a user who boots
    // straight into a wireless default still has a rate to restore later.
    // Without it the first non-wireless switch has mPreWirelessRate=0 and the
    // AirPlay-negotiated 44.1 kHz sticks onto the new device.
    mPreWirelessRate = cfg.sampleRate;

    // Seed the preferred output from -H / sound_card_name so a boot with the
    // device absent still remembers the user's intent — the device-list
    // listener auto-switches to it when it reappears. "__system__" means
    // follow the macOS default; leave the preferred empty.
    if (!cfg.hardwareDevice.empty() && cfg.hardwareDevice != "__system__") {
        mPreferredOutputDevice  = cfg.hardwareDevice;
        mPreferredOutputIsWords = true;   // until boot or a device list finds it
    }
    // -H's input name, remembered so boot pairing and hotplug both honour it.
    // ONLY when inputs are enabled: with -i 0, or the macOS mic-permission
    // guard's zeroing, a seeded preference lets decideHotplugAction reopen the
    // very stream the disable exists to avoid.
    if (cfg.numInputChannels != 0
        && !cfg.inputDevice.empty() && cfg.inputDevice != "__none__")
        mPreferredInputDevice = cfg.inputDevice;

    // The device lane opens boot's device, as it opens every other.
    if (!cfg.headless) runOnDeviceLane([this, &cfg] { initAudioDevice(cfg); });
    initEngine(cfg);
}

// Boot half 1: open the audio device. Device-side only — driver selection, the
// -H named open, the default open with its wireless-avoiding fallback, input
// pairing, aggregate promotion, rate/buffer negotiation. No DSP, no rings, so
// initEngine can assume whatever device (or none) this settled on.
void ClockworkEngine::initAudioDevice(const Config& cfg) {
#ifdef __APPLE__
    // Mic-permission diagnostics, once per boot, on the log like the rest of
    // device setup: essential for triaging "live_audio is silent" reports.
    MicPermission::logDiagnostics();
#endif

    // Map -1 (auto/max) to a large request count. JUCE/CoreAudio clamps the
    // bitmask to the device's real channel count, and the callback reads the
    // real count back later.
    auto resolveReq = [](int n) -> int {
        return n < 0 ? kRequestMaxChannels : n;
    };
    int reqIn  = resolveReq(cfg.numInputChannels);
    int reqOut = resolveReq(cfg.numOutputChannels);

    mDeviceManager = makeDeviceManager();
#if defined(__linux__) || defined(_WIN32)
    // Every platform-specific piece of bring-up below is skipped under the
    // deviceManagerFactory boundary — an injected (fake-device) manager
    // must not touch real PipeWire or DirectSound state. macOS has no such
    // step: CoreAudio needs nothing of the engine before a device opens.
    const bool platformSetup = !cfg.deviceManagerFactory;
#endif

#ifdef __linux__
    if (platformSetup) {
        silenceJackLogsIfPossible();
#ifdef CLOCKWORK_PIPEWIRE
        registerPipeWireDriver(*mDeviceManager);
#endif
    }
#endif

    {
        auto& types = mDeviceManager->getAvailableDeviceTypes();
        std::string drivers;
        for (auto* t : types)
            drivers += " [" + t->getTypeName().toStdString() + "]";
        clockwork_log("[device-setup] available drivers:%s", drivers.c_str());
    }

#if defined(__linux__) && defined(CLOCKWORK_PIPEWIRE)
    if (platformSetup) {
        preferPipeWireDriverIfAvailable(*mDeviceManager);
        if (mDeviceManager->getCurrentAudioDeviceType() == "PipeWire")
            mBootDriver = "PipeWire";
    }
#endif

#ifdef _WIN32
    // Default to WASAPI shared mode ("Windows Audio") on Windows when no
    // driver preference is supplied, DirectSound only if WASAPI is absent.
    // DirectSound was the historical default, kept against a WASAPI
    // shared-mode crackle nobody had confirmed. Measured on Windows 11
    // ARM64 (2026-09-09, an embedded engine at 0.8% DSP load, zero
    // callback overruns): DirectSound at its 2560-frame duplex buffer put
    // 85 discontinuities into 15 s of output that the offline render of
    // the same piece did not have — the clock-rate skew of two DirectSound
    // streams, audible as constant pops — while WASAPI on the same device
    // put in 2, the same as a WAV played by the OS. A --audio-driver
    // request overrides this below.
    if (platformSetup) {
        auto& types = mDeviceManager->getAvailableDeviceTypes();
        for (const char* preferred : { "Windows Audio", "DirectSound" }) {
            bool found = false;
            for (auto* t : types) {
                if (t->getTypeName() == preferred) {
                    mDeviceManager->setCurrentAudioDeviceType(preferred, true);
                    mBootDriver = preferred;
                    found = true;
                    break;
                }
            }
            if (found) break;
        }
    }
#endif

    // Honour --audio-driver, so the engine opens on the saved preference rather
    // than booting the platform default and being cold-swapped after the
    // handshake. Resolution is resolveBootDriver: exact type name, then unique
    // case-insensitive. ASIO IS REFUSED WITHOUT A -H DEVICE — it has no default
    // device and probing one can hang in IASIO::init. An unresolvable name keeps
    // the platform default with a warning.
    //
    // The platform's own choice is kept so a requested driver that opens nothing
    // can fall back to it rather than leaving the engine deaf.
    const std::string platformDefaultDriver = mBootDriver;
    bool bootDriverRequested = false;
    if (!cfg.audioDriver.empty()) {
        std::vector<std::string> typeNames;
        for (auto* t : mDeviceManager->getAvailableDeviceTypes())
            typeNames.push_back(t->getTypeName().toStdString());
        const bool hasDeviceRequest = !cfg.hardwareDevice.empty()
                                   && cfg.hardwareDevice != "__system__";
        auto choice = clockwork::device::resolveBootDriver(
            cfg.audioDriver, typeNames, hasDeviceRequest);
        if (!choice.warning.empty()) {
            clockwork_log("[device-setup] %s", choice.warning.c_str());
        }
        if (!choice.driver.empty()) {
            mDeviceManager->setCurrentAudioDeviceType(
                juce::String(choice.driver), true);
            mBootDriver = choice.driver;
            bootDriverRequested = true;
            clockwork_log("[device-setup] boot driver: '%s'",
                           choice.driver.c_str());
        }
    }

    // -H "__system__" is the GUI sentinel for "follow macOS default".
    // Skip fuzzy-match and go straight to initialiseWithDefaultDevices.
    juce::String initError;
    bool openedByHardwareFlag = false;

    if (!cfg.hardwareDevice.empty() && cfg.hardwareDevice != "__system__") {
        struct DevEntry { std::string combined, typeName, devName; };
        std::vector<DevEntry> entries;

        auto& types = mDeviceManager->getAvailableDeviceTypes();
        for (auto* type : types) {
            type->scanForDevices();
            for (auto& name : type->getDeviceNames(false)) {
                DevEntry e;
                e.typeName = type->getTypeName().toStdString();
                e.devName  = name.toStdString();
                e.combined = e.typeName + " : " + e.devName;
                entries.push_back(e);
            }
        }

#ifdef __APPLE__
        // Filter wireless (AirPlay/Bluetooth) from fuzzy-match candidates.
        // These can't be opened via HAL — the route is only warmed up when
        // the device becomes the macOS system default via System Settings.
        {
            auto allDevs = listDevices();
            std::set<std::string> wirelessNames;
            for (auto& d : allDevs)
                if (d.wireless) wirelessNames.insert(d.name);

            entries.erase(std::remove_if(entries.begin(), entries.end(),
                [&wirelessNames](const DevEntry& e) {
                    for (auto& w : wirelessNames)
                        if (sameDeviceName(e.devName, w)) return true;
                    return false;
                }), entries.end());
        }
#endif

        // Resolve the requested device SCOPED to the requested driver. An unscoped
        // match resolves by shortest combined name, landing a bare device name on
        // whichever driver has the shortest NAME rather than the one the user
        // chose (resolveBootHardwareMatch).
        std::vector<std::pair<std::string, std::string>> deviceTable;
        for (auto& e : entries)
            deviceTable.emplace_back(e.typeName, e.devName);
        std::string matched = clockwork::device::resolveBootHardwareMatch(
            cfg.hardwareDevice,
            bootDriverRequested ? mBootDriver : std::string(),
            deviceTable);
        if (matched.empty()) {
            clockwork_log(
                    "[device-setup] WARNING: requested output device '%s' not found. "
                    "Falling back to system default. Available outputs:",
                    cfg.hardwareDevice.c_str());
            for (auto& e : entries)
                clockwork_log("    %s", e.combined.c_str());
        } else {
            for (auto& e : entries) {
                if (e.combined != matched) continue;
                // What -H's words found is the device, by its name.
                mPreferredOutputDevice  = e.devName;
                mPreferredOutputIsWords = false;

                mDeviceManager->setCurrentAudioDeviceType(
                    juce::String(e.typeName), true);

                juce::AudioDeviceManager::AudioDeviceSetup setup;
                setup.outputDeviceName = juce::String(e.devName);
                setup.inputDeviceName  = juce::String();
                setup.useDefaultOutputChannels = true;
                setup.useDefaultInputChannels  = false;
                // Requested input is this same device (full-duplex, e.g. a virtual
                // loopback): open both directions at once — no aggregate, and no
                // transient default-input state for the GUI to correct with a cold swap.
                // Resolution is exact-or-"(N)": with "USB Audio Device" and "USB Audio
                // Device (2)" both attached, opening box 2 never swallows box 1's input.
                if (cfg.numInputChannels != 0
                    && !mPreferredInputDevice.empty()) {
                    std::vector<std::string> devNames;
                    for (auto& cand : entries)
                        devNames.push_back(cand.devName);
                    if (clockwork::device::resolveJuceDeviceName(
                            mPreferredInputDevice, devNames) == e.devName) {
                        setup.inputDeviceName = juce::String(e.devName);
                        setup.useDefaultInputChannels = true;
                    }
                }
                if (cfg.sampleRate > 0) setup.sampleRate = cfg.sampleRate;
                if (cfg.bufferSize > 0) setup.bufferSize = cfg.bufferSize;

                // Only when the setup NAMES an input — a blank name with
                // reqIn > 0 made macOS pair the default mic via JUCE's
                // Combiner (#3554), and elsewhere activated zero channels.
                // The input-pairing blocks below attach preferred inputs.
                const int hwReqIn =
                    setup.inputDeviceName.isNotEmpty() ? reqIn : 0;
                initError = mDeviceManager->initialise(
                    hwReqIn, reqOut,
                    nullptr, false, juce::String(), &setup);

                // A failed full-duplex attempt may be the input half alone (mic-privacy
                // denial, exclusive-mode contention, input-side rate limits). Retry
                // output-only so a bad input never costs the user their chosen output;
                // boot's input pairing below pairs an input normally.
                if (initError.isNotEmpty()
                    && setup.inputDeviceName.isNotEmpty()) {
                    clockwork_log("[device-setup] -H full-duplex open of "
                            "'%s' failed (%s) — retrying output-only",
                            e.devName.c_str(), initError.toRawUTF8());
                    setup.inputDeviceName = juce::String();
                    setup.useDefaultInputChannels = false;
                    initError = mDeviceManager->initialise(
                        0, reqOut,
                        nullptr, false, juce::String(), &setup);
                }

                if (initError.isNotEmpty()) {
                    clockwork_log("[device-setup] -H '%s' matched '%s' but failed: %s",
                            cfg.hardwareDevice.c_str(), e.combined.c_str(),
                            initError.toRawUTF8());
                } else {
                    clockwork_log("  -H '%s' -> %s",
                            cfg.hardwareDevice.c_str(), e.combined.c_str());
                    mDeviceMode = e.devName;
                    mBootDriver = e.typeName;
                    openedByHardwareFlag = true;
                }
                break;
            }
        }
    }

    if (!openedByHardwareFlag) {
        // The default output, with the default input when inputs are wanted:
        // the driver pairs the two (CoreAudio on an aggregate device of them).
        //
        // NEVER OPEN A WIRELESS DEFAULT (AirPlay, Bluetooth) at boot on
        // macOS: opening wireless and then moving to a wired device for the
        // aggregate a microphone needs halts CoreAudio's IOProc for ~15 s.
        // Pick a wired output up front.
        std::string bootFallback;
#ifdef __APPLE__
        if (const SystemDefaultOutput def = systemDefaultOutput();
            def.wireless && !def.name.empty()) {
            std::vector<std::string> names;
            std::vector<bool> wirelessFlags;
            for (auto& d : listDevices()) {
                names.push_back(d.name);
                wirelessFlags.push_back(d.wireless);
            }
            bootFallback = clockwork::device::selectBootOutputDevice(
                def.name, def.wireless, names, wirelessFlags);
            if (!bootFallback.empty()) {
                clockwork_log("[device-setup] boot: default '%s' "
                        "is wireless; using non-wireless fallback '%s'",
                        def.name.c_str(), bootFallback.c_str());
            } else {
                clockwork_log("[device-setup] boot: default '%s' "
                        "is wireless and no non-wireless fallback "
                        "available — opening wireless default may "
                        "silence audio for ~15 s during boot handshake",
                        def.name.c_str());
            }
        }

#endif
        auto openDefault = [&](int ins) -> juce::String {
            if (bootFallback.empty())
                return mDeviceManager->initialiseWithDefaultDevices(ins, reqOut);
            juce::AudioDeviceManager::AudioDeviceSetup setup;
            setup.outputDeviceName = juce::String(bootFallback);
            setup.useDefaultOutputChannels = true;
            if (ins != 0)
                if (auto* type = mDeviceManager->getCurrentDeviceTypeObject()) {
                    setup.inputDeviceName = type->getSystemDefaultDeviceName(true);
                    setup.useDefaultInputChannels = true;
                }
            return mDeviceManager->initialise(ins, reqOut, nullptr, false,
                                              juce::String(), &setup);
        };
        initError = openDefault(reqIn);
        // An input the driver won't pair with the output (CoreAudio refuses a
        // wireless one, whose codec would drop the pair to 16 kHz — #3555)
        // costs the input, never the output.
        if (initError.isNotEmpty() && reqIn != 0) {
            clockwork_log("[device-setup] init with %d in / %d out failed: %s "
                    "— opening the output alone", reqIn, reqOut, initError.toRawUTF8());
            initError = openDefault(0);
        }
        if (initError.isEmpty() && !bootFallback.empty()) mDeviceMode = bootFallback;
        if (initError.isNotEmpty()) {
            clockwork_log("[device-setup] init with 0 in / %d out failed: %s",
                    reqOut, initError.toRawUTF8());
            initError = mDeviceManager->initialiseWithDefaultDevices(0, 2);
        }
        if (initError.isNotEmpty()) {
            clockwork_log("[device-setup] init with 0 in / 2 out failed: %s",
                    initError.toRawUTF8());
            initError = mDeviceManager->initialiseWithDefaultDevices(0, 0);
        }
        if (initError.isNotEmpty()) {
            clockwork_log("[device-setup] all init attempts failed: %s",
                    initError.toRawUTF8());
        }
    }

    // A requested driver can open nothing — exclusive-mode WASAPI refuses a
    // device another process holds, an ASIO box may be unplugged — and the
    // engine would come up headless with nothing to recover it. Fall back to the
    // platform's driver and run the ladder again. mBootDriver points at what was
    // actually opened; the GUI still shows the saved choice.
    if (!mDeviceManager->getCurrentAudioDevice()
        && bootDriverRequested
        && !platformDefaultDriver.empty()
        && platformDefaultDriver != mBootDriver) {
        clockwork_log("[device-setup] requested driver '%s' opened no device "
                "— falling back to '%s'",
                mBootDriver.c_str(), platformDefaultDriver.c_str());
        mDeviceManager->setCurrentAudioDeviceType(
            juce::String(platformDefaultDriver), true);
        juce::String fbErr =
            mDeviceManager->initialiseWithDefaultDevices(reqIn, reqOut);
        if (fbErr.isNotEmpty())
            fbErr = mDeviceManager->initialiseWithDefaultDevices(0, 2);
        if (fbErr.isEmpty() && mDeviceManager->getCurrentAudioDevice()) {
            mBootDriver = platformDefaultDriver;
            initError = juce::String();
            clockwork_log("[device-setup] fallback opened '%s' on '%s'",
                    mDeviceManager->getCurrentAudioDevice()
                        ->getName().toRawUTF8(),
                    platformDefaultDriver.c_str());
        } else {
            clockwork_log("[device-setup] fallback to '%s' also failed: %s",
                    platformDefaultDriver.c_str(), fbErr.toRawUTF8());
        }
    }

    // Pair the requested input with the output (the driver opens the two as
    // one: CoreAudio on an aggregate device, others side by side). Without it
    // the GUI reconciler sees intent != actual on every boot and corrects with
    // the redundant cold swap this exists to remove.
    if (initError.isEmpty() && cfg.numInputChannels != 0
        && !mPreferredInputDevice.empty()
        && mDeviceManager->getCurrentAudioDevice()) {
        juce::AudioDeviceManager::AudioDeviceSetup setup;
        mDeviceManager->getAudioDeviceSetup(setup);
        const std::string currentIn = setup.inputDeviceName.toStdString();
        // Candidates must be SCOPED to the driver actually open. Windows lists the same
        // hardware under each driver with a different name, so an unscoped list makes a
        // stale pref from another driver look pairable — it then fails at the open,
        // with the output already up. See scopeInputsToDriver. Nor one its driver won't
        // pair with the output (a wireless one, on CoreAudio): tried, it is refused with
        // the output already closed, and the output opened again.
        const auto devices = listDevices(false);
        const std::string currentOut =
            mDeviceManager->getCurrentAudioDevice()->getName().toStdString();
        bool outputPairs = true;
        for (auto& d : devices)
            if (sameDeviceName(d.name, currentOut)) outputPairs = d.pairs;
        std::vector<std::pair<std::string, std::string>> inputTable;
        for (auto& d : devices)
            if (d.maxInputChannels > 0
                && (sameDeviceName(d.name, currentOut) || (outputPairs && d.pairs)))
                inputTable.emplace_back(d.typeName, d.name);
        const std::vector<std::string> inputNames =
            clockwork::device::scopeInputsToDriver(
                inputTable,
                mDeviceManager->getCurrentAudioDeviceType().toStdString());
        const std::string chosen = clockwork::device::chooseBootInputDevice(
            mPreferredInputDevice, currentIn, inputNames);
        if (!chosen.empty() && chosen != currentIn) {
            clockwork_log("[device-setup] boot: pairing requested input "
                    "'%s' (default was '%s')",
                    chosen.c_str(), currentIn.c_str());
            setup.inputDeviceName = juce::String(chosen);
            setup.useDefaultInputChannels = false;
            // Clamp the bitmask to the device's real capacity — WASAPI
            // rejects a setup asking for more inputs than exist, and
            // the auto-max sentinel requests kRequestMaxChannels.
            int wantIn = reqIn;
            const int probedIn = probeDeviceChannelCount(
                chosen, true, probeDriverTypeName());
            if (probedIn > 0 && probedIn < wantIn) wantIn = probedIn;
            juce::BigInteger inputBits;
            inputBits.setRange(0, wantIn, true);
            setup.inputChannels = inputBits;
            // Snapshot before the attempt: a rejected setAudioDeviceSetup does not leave
            // the previous device running — it closes it and opens nothing, so without an
            // explicit restore a bad input name costs the whole boot, output included.
            // Pairing is an optimisation and must never take the output down with it.
            juce::AudioDeviceManager::AudioDeviceSetup previous;
            mDeviceManager->getAudioDeviceSetup(previous);
            const juce::String pairErr =
                mDeviceManager->setAudioDeviceSetup(setup, true);
            if (pairErr.isNotEmpty()) {
                clockwork_log("[device-setup] boot input pairing failed: "
                        "%s — keeping '%s'",
                        pairErr.toRawUTF8(), currentIn.c_str());
                if (!mDeviceManager->getCurrentAudioDevice()) {
                    const juce::String restoreErr =
                        mDeviceManager->setAudioDeviceSetup(previous, true);
                    clockwork_log("[device-setup] boot: restored output-only "
                            "device after failed pairing%s%s",
                            restoreErr.isEmpty() ? "" : " — FAILED: ",
                            restoreErr.isEmpty() ? "" : restoreErr.toRawUTF8());
                }
            } else {
                mLastInputDeviceName = chosen;
            }
        }
    }

    // Negotiate rate and buffer. A hardware buffer that is a whole multiple of
    // the DSP's fixed block avoids prefetch overhead and NTP discontinuities at
    // callback boundaries.
    if (auto* dev = mDeviceManager->getCurrentAudioDevice()) {
        juce::AudioDeviceManager::AudioDeviceSetup setup;
        mDeviceManager->getAudioDeviceSetup(setup);
        bool changed = false;

        // The configured rate, when the device offers it (a paired device
        // offers the rates both its devices share).
        if (static_cast<int>(setup.sampleRate) != cfg.sampleRate) {
            auto rates = dev->getAvailableSampleRates();
            bool supported = false;
            for (auto r : rates) {
                if (static_cast<int>(r) == cfg.sampleRate) {
                    supported = true;
                    break;
                }
            }
            if (supported) {
                setup.sampleRate = cfg.sampleRate;
                changed = true;
            } else {
                clockwork_log("[device-setup] requested sr %d not supported, "
                        "keeping %.0f", cfg.sampleRate, setup.sampleRate);
            }
        }

        if (cfg.bufferSize > 0) {
            // The user's -z / -Z / TOML buffer size (a drift-compensated
            // pair raises one too small for it itself).
            setup.bufferSize = cfg.bufferSize;
            changed = true;
        } else if (dev->getTypeName() != "DirectSound") {
            // Auto: pick the smallest available buffer that is at least
            // 128 samples. The block size follows the HW buffer on native
            // (see chooseBlockSize below), so any size works. Buffers below
            // 128 mostly add callback overhead without a latency win.
            constexpr int kMinBuf = 128;
            auto sizes = dev->getAvailableBufferSizes();
            int best = 0;
            for (auto s : sizes) {
                if (s >= kMinBuf && s <= clockwork::kMaxBlockSize) {
                    best = s;
                    break;  // sizes are sorted ascending
                }
            }
            if (best > 0 && best != dev->getCurrentBufferSizeSamples()) {
                setup.bufferSize = best;
                changed = true;
            }
        }

        if (changed) {
            juce::String setupErr = mDeviceManager->setAudioDeviceSetup(setup, true);
            if (setupErr.isNotEmpty()) {
                clockwork_log("[device-setup] setAudioDeviceSetup error: %s",
                        setupErr.toRawUTF8());
                clockwork_log("[device-setup] recovering with device defaults");
                mDeviceManager->initialiseWithDefaultDevices(
                    reqIn, reqOut);
            }
        }
    }

    // Read what the device actually settled on and override config to match —
    // INCLUDING bufferSize. Leaving it unset made every pre-first-swap consumer
    // (headless driver configure, SwapResult.bufferSize, state events) read the
    // requested value instead of the real one.
    if (auto* dev = mDeviceManager->getCurrentAudioDevice()) {
        double sr   = dev->getCurrentSampleRate();
        mCurrentConfig.sampleRate        = static_cast<int>(sr);
        mCurrentConfig.bufferSize        = dev->getCurrentBufferSizeSamples();
        mCurrentConfig.numOutputChannels = dev->getOutputChannelNames().size();
        mCurrentConfig.numInputChannels  = dev->getInputChannelNames().size();
    } else {
        clockwork_log("[engine] warning: no audio device available");
    }
}

// Boot half 2: bring up the engine around whatever initAudioDevice
// settled on (or headless). Shared memory, the DSP instance, ClockworkClock
// + SHM binding, reader drains (registered here, where the ring pointers
// exist, before the reader threads start), transports, subsystem routes,
// workers, the audio source, and finally the watchdog.
void ClockworkEngine::initEngine(const Config& cfg) {
    // Resolve any remaining auto-max sentinels to concrete counts before the
    // DSP instance is built. This covers headless mode and any path where the
    // device failed to open (readback block above didn't run).
    if (mCurrentConfig.numOutputChannels < 0) mCurrentConfig.numOutputChannels = 2;
    if (mCurrentConfig.numInputChannels  < 0) mCurrentConfig.numInputChannels  = 0;

    // -- Create shared memory (owned by engine, survives cold swaps) --------
    // Anonymous: nothing is named, nothing is cleaned up. Readers get it from
    // the host's attach endpoint (shm_attach.hpp) via shmNativeHandle().
    if (cfg.udpPort > 0) {
        try {
            mShmemCreator = std::make_unique<shm_segment_creator>(cfg.inboxBytes, cfg.outboxBytes);
            // Point the engine's whole shared_memory.h arena at the public
            // segment: rings, control, metrics, node-tree, audio taps and
            // scope all live there, observable cross-process for free.
            g_external_segment = mShmemCreator->get_base();
        } catch (const std::exception& e) {
            clockwork_log("[engine] shared memory creation failed: %s", e.what());
            mShmemCreator.reset();
            g_external_segment = nullptr;
        }
    } else {
        g_external_segment = nullptr;
    }

    // -- Build the DSP instance --------------------------------------------
    // cfg.blockSize wins; otherwise match the device's callback buffer only when
    // it is SMALLER than the default (chooseBlockSize). NEVER RAISE ABOVE
    // kDefaultBlockSize — matching upward coarsens the control rate, so larger
    // buffers keep the default block and the decoupling machinery spans the
    // difference. Clamped to [32, kMaxBlockSize]. WASM has no knob: its block is
    // the 128-sample render quantum.
    int chosenBufLen = cfg.blockSize;
    if (chosenBufLen <= 0) {
        int hwBuf = 0;
        if (mDeviceManager) {
            if (auto* dev = mDeviceManager->getCurrentAudioDevice())
                hwBuf = dev->getCurrentBufferSizeSamples();
        } else {
            hwBuf = mCurrentConfig.bufferSize;  // headless: the manual pump size
        }
        chosenBufLen = clockwork::device::chooseBlockSize(
            hwBuf, clockwork::kDefaultBlockSize, 32,
            clockwork::kDefaultBlockSize);
    }
    clockwork_log("[engine] DSP block size = %d samples", chosenBufLen);

    // The arena is the public segment when one exists, so the whole
    // shared_memory.h blob lives cross-process; else the process-local
    // ring_buffer_storage. init_memory() resolves the same base from
    // g_external_segment, so both agree.
    uint8_t* arena = g_external_segment ? g_external_segment : ring_buffer_storage;

    // The guest's config block: whatever the host put in Config::guestConfig,
    // carried as opaque bytes with its terminating NUL so a guest that reads
    // text finds the end. Nothing here interprets it.
    //
    // Clockwork's own geometry is NOT in it — rate, block size and channel
    // counts go to initialiseDsp as arguments, because they are what the
    // device opened rather than what the guest asked for.
    const std::string& guestText  = cfg.guestConfig;
    const uint32_t     guestBytes = static_cast<uint32_t>(guestText.size() + 1);

    // The guest's arena, allocated once and reused across cold swaps. It does
    // not depend on the segment: a guest needs working memory whether or not
    // anything is watching it, and dsp_process may not allocate either way.
    if (!mGuestArena)
        mGuestArena.reset(new (std::nothrow) uint8_t[static_cast<size_t>(CLOCKWORK_ARENA_BYTES)]);   // NOLINT(cppcoreguidelines-owning-memory): nothrow new into the unique_ptr; make_unique cannot be nothrow
    uint8_t* guestArena      = mGuestArena.get();
    uint32_t guestArenaBytes = mGuestArena ? CLOCKWORK_ARENA_BYTES : 0u;

    // The bulk lanes. A client writes a sample into the inbox and sends a short
    // message saying where; a guest writes a rendered blob into the outbox and
    // does the same.
    //
    // THE SEGMENT PUBLISHES THESE, IT DOES NOT CREATE THEM, which is how the
    // rest of the layout works: shared_memory is the public segment when there
    // is one and process-local ring_buffer_storage when there is not, so
    // metrics, the scope and the node-tree window are there either way. An
    // engine embedded with no UDP port still has a client — the host
    // application, in this process, reading in place — and a plugin's GUI
    // reads what the guest publishes through the outbox.
    // Sized by the host's config, like the segment's lanes; untouched here,
    // so the pages cost nothing until a client writes them.
    const auto laneGeom = detail_shm_segment::shm_lane_geometry::of(mCurrentConfig.inboxBytes, mCurrentConfig.outboxBytes);
    if (!mShmemCreator
        && (!mGuestLanes || mGuestLanesInboxBytes != laneGeom.inbox_size
                         || mGuestLanesOutboxBytes != laneGeom.outbox_size)) {
        mGuestLanes.reset(new (std::nothrow) uint8_t[laneGeom.inbox_size + laneGeom.outbox_size]);   // NOLINT(cppcoreguidelines-owning-memory): nothrow new into the unique_ptr; make_unique cannot be nothrow
        mGuestLanesInboxBytes  = mGuestLanes ? laneGeom.inbox_size  : 0;
        mGuestLanesOutboxBytes = mGuestLanes ? laneGeom.outbox_size : 0;
    }
    uint8_t* guestInbox      = mShmemCreator ? mShmemCreator->get_inbox()
                                             : mGuestLanes.get();
    uint32_t guestInboxBytes = mShmemCreator
        ? static_cast<uint32_t>(mShmemCreator->get_inbox_size())
        : static_cast<uint32_t>(mGuestLanesInboxBytes);
    mGuestInbox      = guestInbox;
    mGuestInboxBytes = guestInboxBytes;
    uint8_t* guestOutbox      = mShmemCreator ? mShmemCreator->get_outbox()
                                              : (mGuestLanes ? mGuestLanes.get() + mGuestLanesInboxBytes
                                                             : nullptr);
    uint32_t guestOutboxBytes = mShmemCreator
        ? static_cast<uint32_t>(mShmemCreator->get_outbox_size())
        : static_cast<uint32_t>(mGuestLanesOutboxBytes);
    mGuestOutbox      = guestOutbox;
    mGuestOutboxBytes = guestOutboxBytes;

    // The heap the guest takes its larger allocations from, sized for what
    // this configuration asks of it (dsp_heap_bytes): taken once per build and
    // never grown on the audio thread. Every build, including 0 for the
    // profile's, so an earlier engine's figure in this process is never the
    // one used. One the system cannot provide stops the boot with the reason.
    {
        const uint64_t wanted = dsp_heap_bytes(guestText.c_str(), guestBytes);
        const uint64_t capped = std::min<uint64_t>(wanted, std::numeric_limits<size_t>::max());
        clockwork_set_heap_bytes(capped > static_cast<uint64_t>(CLOCKWORK_HEAP_SIZE)
                                 ? static_cast<size_t>(capped) : 0);
    }

    mProcessor.initialiseDsp(
        arena,
        mCurrentConfig.sampleRate,
        mCurrentConfig.numOutputChannels,
        mCurrentConfig.numInputChannels,
        guestText.c_str(),
        guestBytes,
        guestArena,
        guestArenaBytes,
        chosenBufLen,
        guestInbox,
        guestInboxBytes,
        guestOutbox,
        guestOutboxBytes
    );
    // Move the clock state into the shared arena's CLOCK_STATE region so
    // the native SHM has the same shape as web. Done before publish() below,
    // so a cross-process reader sees it populated.
    mClockworkClock.bindStateToShm(
        reinterpret_cast<ClockworkClockState*>(arena + CLOCK_STATE_START));
    mClockworkClock.bindSampleClockToShm(arena + SAMPLE_CLOCK_START);

    // Seed the session tempo before any consumer can read the clock and before the
    // tempo-changed callback is installed, so the engine opens at the embedder's
    // tempo with no notify and no transient.
    //
    // A later /clockwork/clock/tempo/set would mirror bpm immediately but
    // re-anchor beat_origin on Link's thread async, leaving a window where the two
    // disagree.
    if (cfg.defaultBpm != clockwork::kDefaultBpm)
        mClockworkClock.setBpm(cfg.defaultBpm);

    // The arena is populated by init_memory() (run synchronously inside
    // initialiseDsp). Publish the segment — store MAGIC last — so a
    // cross-process reader that observes MAGIC sees fully-initialised regions
    // (e.g. node-tree 0xFF empty markers), never the half-zeroed boot state.
    if (mShmemCreator)
        mShmemCreator->publish();

    // The engine's own client handle over the arena, opened once, here: the
    // gateway thread below and any caller's ingest both use it, and opening it
    // on first use from whichever came first was a race between the two.
    openClient();

    // Publish the peer command plane only when enabled: the gateway task below
    // drains its command ring and ShmTransport sends through this slot.
    // Disabled (or no segment) ⇒ the slot stays null and both sides are inert.
    if (cfg.shmCommands) {
        if (mShmemCreator) {
            mPeerPlane.store(mShmemCreator->get_peer_plane(), std::memory_order_release);
        } else {
            clockwork_log("[engine] WARNING: shmCommands requested but there is "
                            "no SHM segment (udpPort == 0) — command plane disabled");
        }
    }

    // Derive worker pointers from the arena. With the unified layout the rings,
    // control words and metrics all live in the arena (the public segment when
    // present), so external observers read the same structs with no redirect.
    uint8_t* base = arena;
    ControlPointers*    ctrl = reinterpret_cast<ControlPointers*>(base + CONTROL_START);
    mMetrics                 = reinterpret_cast<PerformanceMetrics*>(base + METRICS_START);

    // -- The control pass. Everything registered on mNrtGateway from here
    //    down is ONE pass, run in order by one thread: the engine's gateway
    //    thread, woken every audio block via processCount — or the host's
    //    own, through controlPass(), when Config::hostDrivesControl says the
    //    host runs the control plane. The registry is the same either way;
    //    only whose thread walks it differs.
    //
    //    Drain #1 = the RT egress lane (OUT ring), through the CLIENT
    //    BOUNDARY. The pass is the ring's single consumer, so it holds the
    //    engine's handle and polls it; a remote peer's reply and an
    //    in-process client's therefore come off the ring by the same code, as
    //    they already go on by it (see ingest). (Drain #2 = the control ring,
    //    added with the NRT plane below.)
    mNrtGateway.setWake(&mProcessor.processCount);
    // The egress rings' consumer is EITHER this pass or the host
    // (Config::hostDrainsEgress). Draining here goes through the client API
    // like any client's would — one poll, both rings — and routes through
    // the same table a host uses (EgressRouter.h). A host that drains still
    // needs the off-audio-thread heap maintenance the drain used to carry,
    // so that runs here regardless.
    if (cfg.hostDrainsEgress) {
        mNrtGateway.addTask([this]() { interceptBufferFreed(nullptr, 0); });
    } else {
        mNrtGateway.addTask([this]() { drainEgressNow(); });
    }

    // -- Control pass: peer command plane (SHM segment; shm_peer_plane.h) ----
    // One trusted external peer writes OSC frames into the plane's SPSC command
    // ring; this task feeds them into the ordinary ingest path, stamped with the
    // reserved SHM-peer origin token — IDENTITY IS ASSIGNED HERE, NEVER TRUSTED
    // FROM SHARED MEMORY. Registered before the control-ring drain so a forwarded
    // command is handled the same pass. Bounded per pass so a flooding peer cannot
    // starve the other drains; a full IN ring retains the frame, giving lossless
    // backpressure through to the peer's own ring-full signal.
    mNrtGateway.addTask([this]() {
        auto* plane = mPeerPlane.load(std::memory_order_acquire);
        if (!plane) return;
        constexpr uint32_t kPeerDrainMaxFrames = 256;
        clockwork_drain_ring(
            shm_peer_cmd_ring(plane), SHM_PEER_CMD_RING_SIZE,
            &plane->cmd_head, &plane->cmd_tail, mPeerDrainState,
            ClockworkDrainMetrics{ nullptr, nullptr,
                            mMetrics ? &mMetrics->osc_in_corrupted : nullptr,
                            nullptr },
            kPeerDrainMaxFrames,
            [this](uint32_t /*frameSrc*/, const uint8_t* d, uint32_t n, uint32_t) {
                if (!clockwork_ingress_write(d, n, SHM_PEER_ORIGIN_TOKEN))
                    return ClockworkDrainVerdict::Retain;   // IN ring full — retry next wake
                if (mMetrics) {
                    mMetrics->osc_out_messages_sent.fetch_add(1, std::memory_order_relaxed);
                    mMetrics->osc_out_bytes_sent.fetch_add(n, std::memory_order_relaxed);
                }
                return ClockworkDrainVerdict::Consume;
            });
    });

    // -- Audio-plane ingress (engine-owned) ---------------------------------
    // The engine owns the IN ring; the default OscIngress route writes it. The
    // transport is a dumb pipe.
    mInBufferStart = base + IN_BUFFER_START;
    mInBufferSize  = IN_BUFFER_SIZE;
    mInHead        = &ctrl->in_head;
    mInTail        = &ctrl->in_tail;
    mInSequence    = &ctrl->in_sequence;
    mInWriteLock   = &ctrl->in_write_lock;

    // The engine owns no socket. By default egress is in-process (CallbackTransport
    // → onReply); an embedder injects a real transport via setTransport before
    // init() — a host that owns sockets, one from the comms client library.
    if (!mTransport)
        mTransport = &mDefaultTransport;

    // Egress is deferred: producers frame OSC into the NRT-out ring (via the
    // lanes producer, clockwork_egress_nrt_write); the gateway drains it and is the
    // sole transport caller. /clockwork/debug log lines surface via onDebug.
    mEgress.init(mTransport, &onDebug);

    // Pre-dispatch hook for both egress rings. It inspects nothing — it is
    // just a reliable off-audio-thread moment to drain the deferred-free
    // queue. See interceptBufferFreed for why it no longer matches a verb.
    mEgress.setInterceptor([this](const uint8_t* d, uint32_t n) { return interceptBufferFreed(d, n); });

    // -- The boundary, and clockwork's own dispatch behind it -----------------
    //
    // ONE predicate decides clockwork-vs-DSP and it is OscSplit's. The boundary has
    // no route table, so no subsystem can widen clockwork's share of the namespace;
    // and the two tables that DO exist take only the part of an address FOLLOWING
    // the prefix, so no verb can be spelled outside it either.
    mSplit.setDsp(&clockwork_dsp_default_route, nullptr);
    mSplit.setSys(&ClockworkSysRoutes::route, &mAudioRoutes);

    // Audio-thread half: the two verbs answerable without leaving this thread
    // (the liveness surface — they must not depend on the NRT gateway being
    // alive, since proving it is alive is half of what they are for), plus the
    // schedule flush, which must run beside enqueue/tick. EVERYTHING else the
    // boundary claimed goes to the NRT command ring.
    mAudioRoutes.add("ping", &clockwork_clockwork_sys_route, nullptr);
    mAudioRoutes.add("echo", &clockwork_clockwork_sys_route, nullptr);
    // The barrier — on this thread, in the same drain as the guest's
    // messages, or it would answer before what it answers for.
    mAudioRoutes.add("sync", &clockwork_clockwork_sys_route, nullptr);
    mAudioRoutes.add("sched/flush", &clockwork_sched_flush_route, nullptr);
    // The clock snapshot, from the published mirror, as on every host
    // (clockwork_sys.h): a caller that wants the clock this block renders
    // against cannot wait for the control thread.
    mAudioRoutes.add("clock/state/get", &clockwork_clockwork_sys_route, nullptr);
    // The asset hand-off (dsp_api.h, "Assets"): audio-thread work everywhere.
    mAudioRoutes.add("asset/", &clockwork_asset_route, nullptr);
    // Inbound events (clockwork_sys.h, clockwork_event_route): a subsystem's
    // callback writes them into the IN ring, and from here they go to the
    // subsystem's audience over the egress and to the guest if it asked —
    // the same route the worklet runs, so neither can tell the host apart.
    // Longest match wins, so "midi/ports/list" and "midi/ports/get" — verbs,
    // not events — are not caught by "midi/ports": they are registered as
    // their own routes below, beside the control table's namespaces.
    mAudioRoutes.add("midi/in/",        &clockwork_event_route, nullptr);
    mAudioRoutes.add("midi/in/enable",  &ClockworkEngine::nrtForwardSink, this);
    mAudioRoutes.add("midi/ports",      &clockwork_event_route, nullptr);
    mAudioRoutes.add("midi/ports/list", &ClockworkEngine::nrtForwardSink, this);
    mAudioRoutes.add("midi/ports/get",  &ClockworkEngine::nrtForwardSink, this);
    mAudioRoutes.add("gamepad/in/",     &clockwork_event_route, nullptr);
    mAudioRoutes.add("gamepad/devices", &clockwork_event_route, nullptr);
    mAudioRoutes.add("gamepad/devices/list", &ClockworkEngine::nrtForwardSink, this);
    mAudioRoutes.add("gamepad/devices/get",  &ClockworkEngine::nrtForwardSink, this);
    // Registered in BOTH configurations. With no store the sink refuses and
    // says why; leaving the verb unregistered would let it fall through to the
    // NRT fallback and be answered as an unknown address, which tells a client
    // the verb was never real rather than that this build dropped it.
    mAudioRoutes.setFallback(&ClockworkEngine::nrtForwardSink, this);

    mEngineControl.init(this, &mEgress, &mClockworkClock, &mLinkAudio);

    // NRT half: by sub-namespace, with the engine's own top-level verbs
    // (devices, drivers, record, notify, …) as the fallback. An address under
    // the prefix that nobody recognises is refused there — the prefix is
    // claimed whole, so it never falls through to the DSP.
    mControlRoutes.add("clock/", &ClockworkEngine::routeTo<EngineControl, &EngineControl::handleLinkCommand>, &mEngineControl);
    mControlRoutes.setFallback(&ClockworkEngine::routeTo<EngineControl, &EngineControl::handleEngineCommand>, &mEngineControl);
#ifdef CLOCKWORK_MIDI
    mMidiControl.init(&mEgress, &mClockworkClock, &mMidiClockOut);
    mControlRoutes.add("midi/", &ClockworkEngine::routeTo<MidiControl, &MidiControl::handleMidiCommand>, &mMidiControl);
#endif
#ifdef CLOCKWORK_GAMEPAD
    mGamepadControl.init(&mEgress);
    mControlRoutes.add("gamepad/", &ClockworkEngine::routeTo<GamepadControl, &GamepadControl::handleGamepadCommand>, &mGamepadControl);
#endif
#ifdef CLOCKWORK_OSC
    // The OSC cue server + outbound user OSC. All "/clockwork/osc/" verbs are
    // control (forwarded to the NRT thread); outbound user OSC is
    // "/clockwork/osc/send" (immediate, or wrapped in "/clockwork/schedule" for
    // timed sends).
    mOscControl.init(&mEgress);
    mControlRoutes.add("osc/", &ClockworkEngine::routeTo<OscControl, &OscControl::handleOscCommand>, &mOscControl);
#endif
#if CLOCKWORK_HAS_PLUGIN_TRACKS
    // Tracks of hosted plugins, which live in the bridge process
    // (TrackControl.h). PLAYING one is an audio-thread route, so a note scheduled
    // through "/clockwork/schedule" fires inside its block and lands on its
    // sample. Everything that EDITS a track goes to the NRT gateway and from there
    // to the bridge's control ring. Longest match wins, so "plugin/params" is the
    // gateway's and not "plugin/param"'s. The gateway task relays the bridge's
    // answers and watches its life, once per block wake.
    mTrackControl.init(this, &mEgress, &mClockworkClock);
    mNrtGateway.addTask([this]() { mTrackControl.gatewayPass(); });
    mAudioRoutes.add("track/note",         &TrackControl::audioSink, &mTrackControl);
    mAudioRoutes.add("track/cc",           &TrackControl::audioSink, &mTrackControl);
    mAudioRoutes.add("track/bend",         &TrackControl::audioSink, &mTrackControl);
    mAudioRoutes.add("track/notes_off",    &TrackControl::audioSink, &mTrackControl);
    mAudioRoutes.add("track/param",        &TrackControl::audioSink, &mTrackControl);
    mAudioRoutes.add("track/plugin/param", &TrackControl::audioSink, &mTrackControl);
    mControlRoutes.add("track/", &ClockworkEngine::routeTo<TrackControl, &TrackControl::handleTrackCommand>, &mTrackControl);
#endif
    // "/clockwork/schedule" is handled in the shared drain (audio_processor.cpp),
    // beside timestamped-bundle scheduling, so it works identically on every
    // target — it never reaches these dispatchers.

    // Single-engine-per-process: g_active_clockwork_clock has no per-engine
    // routing, so a second publisher would steer the /clockwork/clock queries
    // to whichever was last initialised. CAS to detect; warn and overwrite.
    {
        ClockworkClock* expected = nullptr;
        if (!g_active_clockwork_clock.compare_exchange_strong(
                expected, &mClockworkClock, std::memory_order_release)) {
            clockwork_log(
                "[engine] WARNING: another ClockworkEngine has already "
                "published a ClockworkClock; multi-engine native is not "
                "supported. /clockwork/clock will reflect this engine.");
            g_active_clockwork_clock.store(&mClockworkClock, std::memory_order_release);
        }
    }

    // Publish the boundary for the audio-thread drain to classify through (mirrors
    // g_active_clockwork_clock; single publisher per process).
    {
        OscSplit* expected = nullptr;
        if (!g_active_split.compare_exchange_strong(
                expected, &mSplit, std::memory_order_release)) {
            g_active_split.store(&mSplit, std::memory_order_release);
        }
    }

    // Publish the MIDI clock out for the render path to drain (the same
    // discipline). The ring starts empty: a fresh engine owns no clock-out
    // ports and follows nothing.
    {
        MidiClockOut* expected = nullptr;
        if (!g_active_midi_clock_out.compare_exchange_strong(
                expected, &mMidiClockOut, std::memory_order_release)) {
            g_active_midi_clock_out.store(&mMidiClockOut, std::memory_order_release);
        }
    }

    // Control pass drain #2: the control ring. Thread the origin token to the
    // handler as call metadata (the egress resolves it via the transport at reply
    // time — no address touches the engine) and route the command by address to
    // its subsystem handler, off the audio thread. The control dispatcher takes
    // "clock/" to the Link surface and everything unclaimed to the engine's own.
    mNrtGateway.addDrain(
        mNrtBuffer.data(), kNrtRingSize, &mNrtHead, &mNrtTail,
        [this](uint32_t token, const uint8_t* d, uint32_t n, uint32_t) {
            // Thread the origin AND the time to the handler as metadata: the
            // audio thread prefixed the frame with NrtEnvelope (nrtForwardSink).
            if (n < sizeof(NrtEnvelope)) return;
            NrtEnvelope env{};
            std::memcpy(&env, d, sizeof env);
            d += sizeof env;
            n -= static_cast<uint32_t>(sizeof env);
            DrainCallCtx cc{ token, env.when, env.blockTime };
            // Remember the address across the call so a pass that blocks can be
            // reported by name, not just duration.
            noteInFlightCommand(d, n);
            mControlRoutes.ingest(d, n, &cc);
            mInFlightCommand[0] = '\0';
        },
        {});

    // MIDI clock out's periodic producer: every follower's pulses due within the
    // horizon, recorded once per pass. HERE rather than on ClockworkClock's session
    // worker, which exists only under CLOCKWORK_LINK — and because the pass's
    // thread is already MidiClockOut's one producer, so a second thread pushing
    // into the same SPSC ring is what the ring forbids. After the control drain,
    // so a follow handled this pass records its first pulses the same pass.
    mNrtGateway.addTask([this]() {
        mMidiClockOut.tick(mClockworkClock, mClockworkClock.now());
    });

    // Report a control pass that blocked its thread. Everything queued behind
    // it — later commands and the egress drains below — waited this long, so a
    // client's unanswered request is explained here rather than inferred from a
    // silence in the log.
    mNrtGateway.onSlowPass([this](uint32_t us) {
        clockwork_log("[nrt] control drain blocked %.1fs%s%s", us / 1'000'000.0,
               mInFlightCommand[0] ? " handling " : "",
               mInFlightCommand[0] ? mInFlightCommand.data() : "");
    });

    // The NRT egress lane is taken by the same poll as the OUT ring (the
    // client API reads both), in the drain above or in the host's — there is
    // no separate drain of it here any more.

    // -- Audio callback wiring ---------------------------------------------
    mProcessor.setClockworkClock(&mClockworkClock);
    mProcessor.setLinkAudio(&mLinkAudio);
    mProcessor.setMetrics(mMetrics);
    // The Link input endpoint renders a peer's stream onto our timeline and needs to
    // know what a frame is worth.
    //
    // HERE, not in initialiseDsp or audioDeviceAboutToStart. initialiseDsp runs
    // BEFORE setClockworkClock above, so the callback has no clock yet; and
    // audioDeviceAboutToStart never runs at all on a headless engine driven by a
    // manual pump. This is the first point where both the clock and the rate exist.
    mLinkAudio.setAudioFormat(
        static_cast<uint32_t>(mCurrentConfig.sampleRate),
        clockworkLinkInputWidth(mCurrentConfig.numInputChannels));
    mProcessor.onWake = [this]() { purge(); };

    // Wire ClockworkClock's Link-event callbacks → OSC notify push.
    // Callbacks fire on Link's network thread; the egress serialises socket
    // writes via the transport's send().
    {
        OscEgress* egr = &mEgress;
        std::atomic<bool>* alive = &mLinkCallbacksAlive;
        mClockworkClock.setTempoChangedCallback([egr, alive](double bpm) {
            if (!alive->load(std::memory_order_acquire)) return;
            // Tempo slides fire this per-update — cap logging at one
            // line per second so a slide can't flood the log.
            static std::atomic<int64_t> lastLogSec{0};
            const int64_t nowSec = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            int64_t prev = lastLogSec.load(std::memory_order_relaxed);
            if (nowSec != prev && lastLogSec.compare_exchange_strong(prev, nowSec)) {
                clockwork_log("[link] tempo -> %.2f bpm", bpm);
            }
            std::array<char, 64> buf{};
            osc::OutboundPacketStream s(buf.data(), buf.size());
            s << osc::BeginMessage(CLOCKWORK_SYS("clock/notify/tempo")) << bpm << osc::EndMessage;
            egr->broadcastLinkNotify(
                reinterpret_cast<const uint8_t*>(s.Data()),
                static_cast<uint32_t>(s.Size()));
        });
        mClockworkClock.setNumPeersChangedCallback([egr, alive](std::size_t n) {
            if (!alive->load(std::memory_order_acquire)) return;
            clockwork_log("[link] peers -> %zu", n);
            std::array<char, 64> buf{};
            osc::OutboundPacketStream s(buf.data(), buf.size());
            s << osc::BeginMessage(CLOCKWORK_SYS("clock/notify/peers"))
              << static_cast<int32_t>(n) << osc::EndMessage;
            egr->broadcastLinkNotify(
                reinterpret_cast<const uint8_t*>(s.Data()),
                static_cast<uint32_t>(s.Size()));
        });
        mClockworkClock.setStartStopChangedCallback([egr, alive](bool playing, double atNtp) {
            if (!alive->load(std::memory_order_acquire)) return;
            clockwork_log("[link] transport -> %s", playing ? "playing" : "stopped");
            std::array<char, 64> buf{};
            osc::OutboundPacketStream s(buf.data(), buf.size());
            // Timestamp in NTP micros, like every other /clock wire time.
            s << osc::BeginMessage(CLOCKWORK_SYS("clock/notify/transport"))
              << static_cast<int32_t>(playing ? 1 : 0)
              << static_cast<osc::int64>(std::llround(atNtp * 1e6))
              << osc::EndMessage;
            egr->broadcastLinkNotify(
                reinterpret_cast<const uint8_t*>(s.Data()),
                static_cast<uint32_t>(s.Size()));
        });
    }

    // -- Start the control pass ----------------------------------------------
    // The pass must be running before the audio source starts, otherwise
    // OUT/NRT-out ring buffers can back up during the audio thread's first
    // few hundred ticks (~ms) before any reader is draining them. A host that
    // drives control is expected to be calling controlPass() from here on:
    // the door opens, and no thread of the engine's is started for it. The
    // door opens ONLY then — with the engine's own thread on the pass, a
    // host's controlPass() is refused, so the two can never overlap.
    if (cfg.hostDrivesControl) mControlOpen.store(true, std::memory_order_release);
    else                       mNrtGateway.start();
    // The pass now drains NRT-out — off-audio-thread debug (clockwork_log) routes there
    // instead of the single-writer RT-out ring. Kept true through shutdown's thread
    // teardown so off-thread logging never falls back to racing RT-out; cleared
    // once every egress producer and the audio device are stopped (see shutdown).
    g_nrt_egress_drained.store(true, std::memory_order_relaxed);

    // -- Start the audio source (real callback or headless fallback) -------
    // Blocks until process_audio has ticked once, or 5 s with a warning. After this
    // returns the engine is fully responsive: OSC sent via sendOSC() or UDP is
    // drained on the next audio block. A device is started on the device lane,
    // which opened it.
    if (mDeviceManager) runOnDeviceLane([this] { startAudioSource(); });
    else                startAudioSource();


    if (testInitFailure) {
        auto msg = testInitFailure();
        if (!msg.empty()) throw std::runtime_error(msg);
    }

    mRunning.store(true);
    // A change to the devices that came while booting, looked at now (see
    // reconcileDevices).
    if (mReconcileAfterBoot.exchange(false)) devicesChanged(0);
    // An engine whose build left no guest is not running a guest, and says so:
    // in error, with the reason the build gave, which every client that
    // registers is told (snapshotStateTo). It stays up — its transport and its
    // own verbs still answer — so a client can learn why and choose what next.
    if (const char* why = clockwork_boot_error())
        setEngineState(EngineState::Error, why);
    else
        setEngineState(EngineState::Running, "boot");

    // Callback-starvation watchdog (see watchdogPoll). Pointless in manual-
    // pump mode, where the test owns process_audio and long gaps are normal.
    // A test that owns the watchdog's clock polls it itself.
    if (mCurrentConfig.callbackWatchdog && !mCurrentConfig.manualAudioPump) {
        mWatchdog = std::make_unique<WatchdogState>(mCurrentConfig);
        if (!mCurrentConfig.watchdogClockMs)
            mWatchdogThread = std::jthread([this](const std::stop_token& stop) { watchdogLoop(stop); });
    }
}


void ClockworkEngine::shutdown() {
    // Don't early-out on !mRunning here: a partial init() (which
    // throws before mRunning becomes true) still needs the cleanup below
    // to run, particularly the macOS CoreAudio property listener removal,
    // which would otherwise fire against a destroyed `this`. Each cleanup
    // step below is individually guarded against missing resources.
    bool wasRunning = mRunning.exchange(false);
    if (wasRunning)
        setEngineState(EngineState::Stopped, "shutdown");

    // Join the ClockworkClock session worker FIRST. It drives MIDI staleness every
    // ~250 ms and reaches both the SHM arena the clock binds into and the Link Audio
    // bus, freed below. It is otherwise only joined when mClockworkClock is
    // destroyed, which happens after this returns — leaving a window where it runs
    // against freed state.
    mClockworkClock.stopBackgroundWork();


    // Stop the control pass BEFORE the subsystems its drains call into: the control
    // drain routes midi/gamepad/osc commands straight to the Rust FFI boundaries, so a
    // clockwork_midi_handle_osc still running in a pass while
    // clockwork_midi_destroy frees what it reads is a use-after-free. The pass
    // also runs EngineControl, whose scheduleDeviceSwitch posts to the device task
    // lane joined below. Late notifications after this sit undrained in NRT-out —
    // acceptable, the consumer is going away.
    //
    // Two ways the pass may be running. The engine's own gateway thread is
    // joined. A host's thread cannot be: the door controlPass() comes through
    // is closed instead, and a pass already inside is waited for — the same
    // guarantee as the join, that no pass runs past this line. A host that
    // keeps calling after this gets false and nothing else.
    mControlOpen.store(false, std::memory_order_release);
    while (mControlPassesInFlight.load(std::memory_order_acquire) != 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    mNrtGateway.stop();

    // Tear down the MIDI subsystem before mEgress/mClockworkClock: clockwork_midi_destroy
    // closes its midir connections (stopping midir's input thread), so no MIDI
    // callback can fire into mEgress/mClockworkClock after this returns.
#ifdef CLOCKWORK_MIDI
    mMidiControl.shutdown();
#endif

    // Tear down the gamepad subsystem before mEgress for the same reason:
    // clockwork_gamepad_destroy deregisters the host callback (synchronising on the
    // emission lock), so no gamepad callback can fire into mEgress after this
    // returns. The subsystem's poll thread itself is process-global and parks.
#ifdef CLOCKWORK_GAMEPAD
    mGamepadControl.shutdown();
#endif

    // Order: signal alive=false, then disable Link (this joins the
    // network thread synchronously inside link.enable(false), so
    // in-flight callbacks complete), then clear the callback wrappers.
    // Clearing earlier leaves the wrapper closure installed during the
    // brief window before Link quiesces.
    mLinkCallbacksAlive.store(false, std::memory_order_release);
    mClockworkClock.setLinkVisibility(ClockworkClock::LinkVisibility::Off);
    mClockworkClock.setTempoChangedCallback({});
    mClockworkClock.setNumPeersChangedCallback({});
    mClockworkClock.setStartStopChangedCallback({});

    // Tear down the OSC cue server (joins its recv thread, closes the socket). The
    // gateway reader is already joined: it drains inbound commands into
    // OscControl::handleOscCommand, which reads mOsc, so destroying mOsc under a
    // running reader would race. Still before mEgress, so no inbound cue can fire
    // into it after this.
#ifdef CLOCKWORK_OSC
    mOscControl.shutdown();
#endif

    // Stop the plugin bridge and take its ports out of the bus. Safe with
    // the audio thread still running — the bindings go first — and before
    // mEgress, so nothing relayed from the bridge can reach it after this.
#if CLOCKWORK_HAS_PLUGIN_TRACKS
    mTrackControl.shutdown();
#endif

    // Stop the watchdog before tearing down — it can launch a recovery right up
    // until it observes its stop, and the device task lane that runs recovery
    // is freed below. The NRT gateway, the other requester, is already stopped, so
    // once the watchdog joins no new recovery can start; one already past its
    // mRunning check runs to completion and is waited on here.
    mWatchdogThread.request_stop();
    if (mWatchdogThread.joinable()) mWatchdogThread.join();
    mWatchdog.reset();   // a poll after this does nothing

    // No control command can run now. The device lane closes the devices it
    // opened, as its last work, and stops: nothing after this touches the
    // device manager or the egress it reports through.
    mDebounceSwitchStop.store(true);
    if (mDeviceManager) runOnDeviceLane([this] { teardownDeviceManager(); });
    mDeviceTaskStop.store(true);   // no more work accepted
    mDeviceTaskThread.request_stop();   // the lane wakes through the loop's stop_callback
    if (mDeviceTaskThread.joinable()) mDeviceTaskThread.join();

    mHeadlessDriver->signalThreadShouldExit();

    // Wake the readers so they can exit. Both wait on processCount; bump it so
    // wait() sees a change and returns.
    mProcessor.processCount.fetch_add(1, std::memory_order_release);
    mProcessor.processCount.notify_all();

    mHeadlessDriver->stopThread(2000);

    // Unpublish from the /clockwork/clock queries only if we're the current
    // publisher — never stomp another engine's pointer.
    {
        ClockworkClock* expected = &mClockworkClock;
        g_active_clockwork_clock.compare_exchange_strong(
            expected, nullptr, std::memory_order_release);
    }

    // Unpublish the audio-thread ingress (same single-publisher discipline).
    {
        OscSplit* expected = &mSplit;
        g_active_split.compare_exchange_strong(
            expected, nullptr, std::memory_order_release);
    }

    // And the clock out. The render path can no longer reach it, so the ring
    // is emptied here for the next boot of this engine object: the audio
    // device and the gateway are both stopped above, so nothing is producing
    // or consuming while it is cleared.
    {
        MidiClockOut* expected = &mMidiClockOut;
        g_active_midi_clock_out.compare_exchange_strong(
            expected, nullptr, std::memory_order_release);
        mMidiClockOut.reset();
    }

    // NRT-out is no longer drained (gateway stopped above) and every egress
    // producer plus the audio device are now stopped, so off-thread debug can
    // safely fall back to RT-out again — and the flag is reset for the next engine.
    g_nrt_egress_drained.store(false, std::memory_order_relaxed);

    // Tear down the DSP instance and the engine's global arena view while the
    // segment is still mapped — after this the lanes entry points reject,
    // so nothing can dereference the arena once it is unmapped below.
    teardown_memory();

    // The client handle addresses the arena, so it goes before the arena does.
    if (mClientHandle) { clockwork_client_close(mClientHandle); mClientHandle = nullptr; }

    // Destroy engine-owned shared memory (after the DSP is gone). The peer
    // plane lives in the segment: null the published slot first so a late
    // ShmTransport send observes null rather than an unmapped plane. Same for
    // the ClockworkClock sample-clock binding — a late pumpAudioBlock must
    // publish into nothing rather than the unmapped segment.
    mClockworkClock.bindSampleClockToShm(nullptr);
    mProcessor.setMetrics(nullptr);
    mHeadlessDriver->setMetrics(nullptr);
    mPeerPlane.store(nullptr, std::memory_order_release);
    g_external_segment = nullptr;
    // The clock's state lives in this arena; point it back at its own copy
    // before the arena goes, or a second shutdown() reads freed memory.
    mClockworkClock.unbindStateFromShm();
    mShmemCreator.reset();
}

// --- OSC send ---
//
// Nothing parses an outgoing message on the way past. A guest that wants to
// survive a rebuild writes what it needs into DspConfig::persistent, so
// clockwork never has to know which of a guest's messages carry state worth
// keeping.

void ClockworkEngine::sendOSC(const uint8_t* data, uint32_t size) {
    // The in-process entry point: origin 0 = an anonymous in-process caller,
    // assigned explicitly here (every entry point mints its own origin — UDP
    // interns the sender on recv).
    ingest(data, size, 0);
}

void ClockworkEngine::pumpAudioBlock() {
    pumpAudioCallback(static_cast<uint32_t>(get_audio_buffer_samples()));
}

void ClockworkEngine::pumpAudioCallback(uint32_t frames,
                                        const std::function<void()>& afterEachBlock) {
    // One renderer at a time. With a source active — the device callback or
    // the headless driver on its own thread — a block rendered here would be
    // a second concurrent caller of process_audio, a data race across the
    // whole engine. Refused and said; a caller that wants to pump stops the
    // source first or boots with manualAudioPump.
    const AudioSource active = mActiveSource.load(std::memory_order_acquire);
    if (active != AudioSource::None) {
        clockwork_log("[engine] manual pump refused: the %s is rendering — stop it first, "
                      "or boot with manualAudioPump",
                      active == AudioSource::RealCallback ? "device callback" : "headless driver");
        return;
    }

    // Anchor the audio-thread clock on the first manual block (mirrors what
    // HeadlessDriver::run does at thread start). Safe to call after stopping the
    // HeadlessDriver — picks up rendering from a clean time base.
    if (!mManualPumpStarted) {
        mManualSamplePos = 0.0;
        clockwork::resetAudioBlockClock(mClockworkClock, mLinkAudio, mManualSamplePos,
                                  mCurrentConfig.sampleRate);
        mManualPumpStarted = true;
    }

    // The manual pump renders with no live device (idle / device-loss), so
    // "audible" == render time (latency 0).
    const clockwork::BlockTime bt = clockwork::beginAudioBlock(
        mClockworkClock, mLinkAudio, mManualSamplePos,
        static_cast<double>(mCurrentConfig.sampleRate), 0, mMetrics);

    // The DSP's actual block size is authoritative here; the configured buffer
    // size can differ from the render quantum.
    const uint32_t blockSize = static_cast<uint32_t>(get_audio_buffer_samples());
    const uint32_t nOut = mCurrentConfig.numOutputChannels > 0
                              ? static_cast<uint32_t>(mCurrentConfig.numOutputChannels) : 2;
    const uint32_t nIn  = mCurrentConfig.numInputChannels  > 0
                              ? static_cast<uint32_t>(mCurrentConfig.numInputChannels)  : 0;

    // A callback is whole blocks, stamped as the device path stamps them
    // (ClockworkProcessor::process): from the one clock step, a block apart.
    const double   blockSeconds = static_cast<double>(blockSize) / mCurrentConfig.sampleRate;
    const uint64_t blockMicros  = static_cast<uint64_t>(blockSeconds * 1e6);
    double   ntp  = bt.ntp;
    uint64_t host = bt.hostMicros;
    for (uint32_t rendered = 0; rendered < std::max(frames, blockSize); rendered += blockSize) {
        renderAudioBlock(mLinkAudio, blockSize, nOut, nIn,
                         static_cast<uint32_t>(mCurrentConfig.sampleRate), ntp, host);
        mManualSamplePos += blockSize;
        ntp += blockSeconds;
        if (host != 0) host += blockMicros;

        mProcessor.processCount.fetch_add(1, std::memory_order_release);
        mProcessor.processCount.notify_all();
        if (afterEachBlock) afterEachBlock();
    }
}

// Copy the OSC address of the command about to be handled. Bounded copy off the
// raw packet: an address is NUL-terminated at the head of the message, so no
// decode is needed on this path.
void ClockworkEngine::noteInFlightCommand(const uint8_t* data, uint32_t size) {
    mInFlightCommand[0] = '\0';
    if (!data || size == 0 || data[0] != '/') return;
    const uint32_t max = std::min<uint32_t>(size, static_cast<uint32_t>(mInFlightCommand.size()) - 1);
    uint32_t i = 0;
    for (; i < max && data[i] != '\0'; ++i)
        mInFlightCommand[i] = static_cast<char>(data[i]);
    mInFlightCommand[i] = '\0';
}

void ClockworkEngine::openClient() {
    if (mClientHandle || !shared_memory) return;
    ClockworkStatus st = CLOCKWORK_OK;
    mClientHandle = clockwork_client_open_memory(shared_memory, TOTAL_BUFFER_SIZE, &st);
    if (!mClientHandle)
        clockwork_log("[engine] client boundary unavailable: %s",
                clockwork_client_status_text(st));
}

void ClockworkEngine::drainEgressNow() {
    ClockworkClient* client = mClientHandle;
    if (!client) return;
    // A batch at a time until the rings are empty. Bounded per call because
    // the array is the caller's, which is the ABI's rule.
    std::array<ClockworkClientMessage, 64> batch{};
    for (;;) {
        const uint32_t n = clockwork_client_poll(client, batch.data(), batch.size());
        for (uint32_t i = 0; i < n; ++i)
            mEgress.dispatchEgress(batch[i].origin, batch[i].route,
                                   batch[i].bytes, batch[i].length);
        if (n < batch.size()) break;
    }
}

ClockworkStatus ClockworkEngine::ingest(const uint8_t* data, uint32_t size, uint32_t originToken) {
    // THROUGH THE CLIENT BOUNDARY, not past it. A datagram from another machine
    // and a call from a GUI in this process arrive here alike, and both go on
    // through clockwork_client_send — so the ring arithmetic a remote peer's
    // traffic meets is the same code an embedder's client runs, rather than a
    // second copy that happens to agree today.
    //
    // The audio thread drains, classifies, and either performs the audio plane
    // inline or forwards control to the NRT thread, which resolves the token
    // back to a reply address. Token 0 is in-process and replies via onReply.
    ClockworkClient* client = mClientHandle;
    const ClockworkStatus status = client ? clockwork_client_send(client, data, size, originToken)
                                          : CLOCKWORK_E_CLOSED;
    const bool written = status == CLOCKWORK_OK;
    if (mMetrics) {
        if (written) {
            mMetrics->osc_out_messages_sent.fetch_add(1, std::memory_order_relaxed);
            mMetrics->osc_out_bytes_sent.fetch_add(size, std::memory_order_relaxed);
        } else {
            // Backpressure / oversize frame: the message is gone — count it
            // as a drop, never as sent.
            mMetrics->messages_dropped.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return status;
}

// --- The host's door onto the control pass (Config::hostDrivesControl). The
// pass itself is mNrtGateway's registry; this only guards it against
// shutdown, the way the gateway thread's join does. Open from the point
// init() would have started the thread; closed by shutdown(), which then
// waits for the count below to reach zero before tearing down what a pass
// reads. The re-check after the increment closes the race where shutdown
// clears the flag between a caller's first look and its increment.
//
// ONE THREAD, enforced rather than trusted: the pass is single-consumer
// through and through, so a second caller arriving while one is inside gets
// false and touches nothing. That is the difference between a host that
// misreads the contract and a corrupted ring.
bool ClockworkEngine::controlPass() {
    if (!mControlOpen.load(std::memory_order_acquire)) return false;
    mControlPassesInFlight.fetch_add(1, std::memory_order_acq_rel);
    bool ran = false;
    // The pass is taken, then the park looked at — both seq_cst, against the
    // park's store-then-look (see parkControlPass).
    if (mControlOpen.load(std::memory_order_acquire) && !mControlPassBusy.exchange(true)) {
        if (!mControlParked.load()) {
            mControlPassThread.store(std::this_thread::get_id());
            mNrtGateway.pass();
            mControlPassThread.store(std::thread::id{});
            ran = true;
        }
        mControlPassBusy.store(false);
    }
    mControlPassesInFlight.fetch_sub(1, std::memory_order_acq_rel);
    return ran;
}

// A cold swap tears down and rebuilds the arena the control pass drains. The
// engine's own gateway thread parks between passes (RingReader::pause). A
// host's pass is held off by the flag controlPass() reads once it has taken
// the pass: the flag is stored and then the pass looked at here, the pass is
// taken and then the flag looked at there, all seq_cst, so either the pass
// sees the park or the park waits for the pass. Bounded as the reader's park
// is, for the same reason: a pass wedged behind a foreign lock must not hang
// the device switch.
bool ClockworkEngine::parkControlPass() {
    if (mNrtGateway.running()) return mNrtGateway.pause();
    mControlParked.store(true);
    // A swap run from inside a pass is not draining beside it.
    if (mControlPassThread.load() == std::this_thread::get_id()) return true;
    for (int waited = 0; mControlPassBusy.load(); ++waited) {
        if (waited >= 2000) {
            clockwork_log("[control] park: the host's pass did not finish within 2s — "
                          "proceeding unparked");
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

void ClockworkEngine::resumeControlPass() {
    if (mNrtGateway.running()) {
        mNrtGateway.resume();
        return;
    }
    mControlParked.store(false);
}

// --- Audio-thread control route: forward clockwork's control verbs to the NRT
// ring. mAudioRoutes' fallback, so it takes everything under the reserved prefix
// the audio-thread half did not claim. Runs on the audio thread; the control
// pass drains the ring and runs the subsystems.
//
// THE FRAME CARRIES THE CALL'S TIME AS WELL AS ITS ORIGIN. The ring header has a
// slot for the token and none for `when`, so the message is prefixed with
// NrtEnvelope. A verb fired from the scheduler reaches the audio thread up to one
// block early; without the envelope the time is lost on this hop and every
// scheduled note leaves at block time (test_scheduled_out.cpp caught it).
bool ClockworkEngine::nrtForwardSink(void* ctx, const void* callCtx,
                                      const uint8_t* data, std::size_t len) {
    auto* self = static_cast<ClockworkEngine*>(ctx);
    auto* cc   = static_cast<const DrainCallCtx*>(callCtx);
    bool ok = false;
    if (len <= kNrtMaxCommand) {
        // mNrtFrame is this thread's: the audio thread is the ring's only
        // producer (see the header), so filling it before the locked write
        // races nothing.
        uint8_t* frame = self->mNrtFrame.data();
        NrtEnvelope env{ cc ? cc->when : 0, cc ? cc->blockTime : 0 };
        std::memcpy(frame, &env, sizeof env);
        std::memcpy(frame + sizeof env, data, len);
        ok = RingBufferWriter::write(
            self->mNrtBuffer.data(), kNrtRingSize,
            &self->mNrtHead, &self->mNrtTail, &self->mNrtSeq, &self->mNrtLock,
            frame, static_cast<uint32_t>(sizeof env + len), cc ? cc->sourceId : 0);
    } else {
        // Wider than the hop carries. Say so, rate-limited: the sender gets no
        // reply, and a count alone does not name the message.
        if (self->mNrtOversizeLogged.fetch_add(1, std::memory_order_relaxed) < 8) {
            uint32_t a = 0; while (a < len && data[a] != '\0') ++a;
            clockwork_log("WARNING: control message %.*s of %u bytes dropped — wider than the "
                          "audio-to-control hop carries (%u)",
                          static_cast<int>(a), reinterpret_cast<const char*>(data),
                          static_cast<unsigned>(len), static_cast<unsigned>(kNrtMaxCommand));
        }
    }
    // The control pass drains this ring — the gateway thread every audio
    // block (it waits on processCount), a host whenever it passes — so no
    // wake is needed here. A full ring (or an oversize command) drops the
    // control message — count it rather than losing it silently (sender gets
    // no reply).
    if (!ok && self->mMetrics)
        self->mMetrics->messages_dropped.fetch_add(1, std::memory_order_relaxed);
    return true;  // consumed — control never falls through to the DSP
}

// --- Device switch / reopen orchestration -------------------------------------

bool ClockworkEngine::postDeviceTask(std::function<void()> task) {
    if (!task) return false;
    {
        std::lock_guard<std::mutex> lock(mDeviceTaskMutex);
        if (mDeviceTaskStop.load()) return false;   // shutting down: drop rather than queue
        mDeviceTasks.push_back(std::move(task));
        if (!mDeviceTaskThread.joinable())
            mDeviceTaskThread = std::jthread([this](const std::stop_token& stop) { deviceTaskLoop(stop); });
    }
    mDeviceLane.wake();
    return true;
}

void ClockworkEngine::deviceTaskLoop(const std::stop_token& stop) {
    const std::stop_callback wake(stop, [this] { mDeviceLane.wake(); });
    mDeviceLane.enter();
    for (;;) {
        // A pass that is due runs before the next task: a task posted after
        // a change was reported finds it looked at.
        if (mDeviceChanges.load() & kPassQueued) {
            reconcileDevices();
            continue;
        }
        std::function<void()> task;
        {
            std::lock_guard<std::mutex> lock(mDeviceTaskMutex);
            if (!mDeviceTasks.empty()) {
                task = std::move(mDeviceTasks.front());
                mDeviceTasks.pop_front();
            } else if (stop.stop_requested()) {
                // Drained: work accepted before shutdown has run, and replied.
                break;
            }
        }
        if (task) task();
        else      mDeviceLane.sleep();
    }
    mDeviceLane.leave();
}

void ClockworkEngine::runOnDeviceLane(const std::function<void()>& work) {
    std::promise<void> done;
    auto finished = done.get_future();
    const bool queued = postDeviceTask([&work, &done] {
        try {
            work();
            done.set_value();
        } catch (...) {
            done.set_exception(std::current_exception());
        }
    });
    if (!queued) {
        work();
        return;
    }
    finished.get();
}

void ClockworkEngine::scheduleDeviceSwitch(const std::string& devName,
                                            const std::string& inputDevName,
                                            double sampleRate, int bufferSize) {
    // Rapid picks replace the pending one; only the last is switched to, after
    // the debounce worker's quiet period, and only it ends in a switch.done.
    bool startWorker = false;
    {
        std::lock_guard<std::mutex> lock(mPendingSwitchMutex);
        mPendingSwitch = { devName, inputDevName, sampleRate, bufferSize,
                           std::chrono::steady_clock::now(), true };
        // Decided under the lock the worker stops under, so a pick is never
        // left to a worker that has already decided to stop.
        startWorker = !mDebounceSwitchRunning;
        mDebounceSwitchRunning = true;
    }
    if (startWorker) postDeviceTask([this] { executePendingSwitch(); });
}

void ClockworkEngine::executePendingSwitch() {
    constexpr auto kDebounce = std::chrono::milliseconds(500);

    // Until nothing is pending: a pick made while another is switching is
    // debounced and switched to in its turn.
    for (;;) {
        PendingSwitch pick;
        for (;;) {
            if (!mDebounceSwitchStop.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            std::lock_guard<std::mutex> lock(mPendingSwitchMutex);
            if (mDebounceSwitchStop.load() || !mPendingSwitch.active) {
                mDebounceSwitchRunning = false;
                return;
            }
            if (std::chrono::steady_clock::now() - mPendingSwitch.timestamp >= kDebounce) {
                pick = mPendingSwitch;
                mPendingSwitch.active = false;
                break;
            }
        }

        clockwork_log("[device-setup] debounced switch: out='%s' in='%s' sr=%.0f buf=%d",
                pick.devName.c_str(), pick.inputDevName.c_str(), pick.sampleRate, pick.bufferSize);

        // An explicit pick of a device leaves system mode: the default-output
        // listener must not take the engine off it.
        if (!pick.devName.empty())
            forceDeviceMode(pick.devName);

        // Retry if a swap is already in progress (e.g. cascade from a
        // device-change notification). Give it up to ~3 seconds.
        SwapResult result;
        int attempts = 0;
        for (int attempt = 0; attempt < 30; ++attempt) {
            attempts = attempt + 1;
            result = switchDevice(pick.devName, pick.sampleRate, pick.bufferSize, false,
                                  pick.inputDevName);
            if (result.success || result.error != "swap already in progress") break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!result.success) {
            clockwork_log("[device-setup] debounced switch failed after %d attempts: %s",
                    attempts, result.error.c_str());
        }
        // No sendDeviceReport() here — switchDevice's printDeviceList already
        // broadcasts. A second call can race with JUCE's post-switch device-list
        // rescan and report an empty snapshot. Push the truthful outcome instead.
        sendSwitchDone(result, pick.devName, pick.inputDevName);
    }
}

bool ClockworkEngine::tryAcquireSwapGate(std::unique_lock<std::recursive_mutex>& lk,
                                          int attempts, int sleepMs) {
    lk = std::unique_lock<std::recursive_mutex>(mSwapMutex, std::defer_lock);
    for (int i = 0; i < attempts; ++i) {
        if (lk.try_lock()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
    }
    return false;
}

ClockworkEngine::TestSwapHold ClockworkEngine::testHoldSwapGate() {
    return TestSwapHold(mSwapMutex, mDevicePhase);
}

// Callback-starvation watchdog. The audio callback is the sole drain for synth
// commands, so a device whose callback thread wedges — DirectSound spinning in its
// cursor poll after a mid-reconfigure race — leaves the server deaf forever while
// its control socket stays up. Sample processCount; when it freezes past the stall
// window with a source nominally active and no swap in flight, restart the source
// or request a device reopen.
ClockworkEngine::WatchdogState::WatchdogState(const Config& cfg)
    : stallMs(std::max(1, cfg.watchdogStallMs)),
      pollMs(std::max(10, cfg.watchdogPollMs)),
      rateCheck(cfg.watchdogRateWindowMs > 0),
      liveness(stallMs, stallMs),
      rateSkew(cfg.watchdogRateWindowMs,
               /*maxGap*/ std::max<int64_t>(3LL * pollMs, 1000),
               cfg.watchdogRateTolerance,
               std::max(1, cfg.watchdogRateBadWindows)),
      skewPolicy(std::max(1, cfg.watchdogRateMaxRecoveries),
                 2.0 * cfg.watchdogRateTolerance),
      skewGoodRequired(std::max(1, cfg.watchdogRateBadWindows)) {}

int64_t ClockworkEngine::watchdogNowMs() const {
    if (mCurrentConfig.watchdogClockMs) return mCurrentConfig.watchdogClockMs();
    return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void ClockworkEngine::watchdogLoop(const std::stop_token& stop) {
    const int pollMs = mWatchdog->pollMs;
    while (!stop.stop_requested()) {
        // Sleep in small slices so shutdown joins quickly.
        for (int slept = 0; slept < pollMs && !stop.stop_requested(); slept += 20)
            std::this_thread::sleep_for(
                std::chrono::milliseconds(std::min(20, pollMs - slept)));
        if (stop.stop_requested()) return;
        watchdogPoll();
    }
}

void ClockworkEngine::watchdogPoll() {
    if (!mWatchdog) return;
    WatchdogState& w = *mWatchdog;

    // Publish control-thread blocking from here, off the gateway: a gateway
    // stuck in a handler cannot report its own stall.
    // µs, not ms: healthy gateway passes are tens of µs, so integer ms
    // rounds every reading a user ever sees to 0.
    clockwork_publish_nrt_blocking(mNrtGateway.maxPassUs(),
                             mNrtGateway.recentMaxPassUs(),
                             mNrtGateway.inFlightUs());

    // Waiting for an audio device (no device open, not a headless / manual-
    // pump build). Keep trying to open one so the engine self-heals the
    // moment a device appears (plug in / wake). requestAudioRecovery is
    // cooldown-gated.
    if (waitingForAudioDevice() && !mReopenInProgress.load()) {
        // Skip while a swap holds the gate. A normal switchDevice passes through a
        // transient mActiveSource==None window with the gate held and brings a device up
        // itself; racing a recovery into that window would, on winning the gate, recreate
        // the device manager and revert to the system default — silently undoing the
        // switch. The next poll retries once the gate frees.
        const bool swapInFlight =
            mDevicePhase.load() != DevicePhase::Idle;
        if (!swapInFlight) {
            std::string reason;
            if (requestAudioRecovery(reason)) {
                mWatchdogRecoveries.fetch_add(1, std::memory_order_release);
                clockwork_log("[watchdog] no audio device — attempting to open one");
            }
        }
        return;
    }

    // Skip sampling where a tick gap is expected — not running, no source,
    // recovery in flight, a swap holding the gate — so a paused callback
    // cannot drift the monitor to a false stall.
    bool benign = !mRunning.load()
               || mActiveSource.load() == AudioSource::None
               || mReopenInProgress.load();
    if (!benign && mDevicePhase.load() != DevicePhase::Idle)
        benign = true;  // mutation in flight — callbacks legitimately paused
    if (benign) return;

    const int64_t  t     = watchdogNowMs();
    const uint32_t count = mProcessor.processCount.load(std::memory_order_acquire);
    w.liveness.observe(count, t);
    const auto ph = w.liveness.phase(t);

    if (mActiveSource.load() == AudioSource::Headless) {
        // Explicit-headless build (tests / non-JUCE backends). If its timer
        // thread died/wedged, restart it under the swap gate; otherwise it's
        // ticking fine and there's no real device to recover.
        if (ph == clockwork::audio::LivenessPhase::Stalled) {
            std::unique_lock<std::recursive_mutex> lk;
            if (tryAcquireSwapGate(lk, 1, 0)
                && mActiveSource.load() == AudioSource::Headless) {
                mWatchdogRecoveries.fetch_add(1, std::memory_order_release);
                clockwork_log("[watchdog] headless driver stalled — restarting");
                stopAudioSource();
                startAudioSource();
            }
        }
        return;
    }

    // Real device source.
    if (ph != clockwork::audio::LivenessPhase::Stalled) {
        // Ticking — but at the right rate? Only judge while Live (a
        // Confirming run is still settling) and not paused (stopRecording
        // pauses the callback briefly: ticks continue, frames freeze — a
        // false skew).
        if (w.rateCheck && ph == clockwork::audio::LivenessPhase::Live
            && !mProcessor.isPaused()) {
            const int nominal = mProcessor.nominalSampleRate();
            w.rateSkew.observe(mClockworkClock.engineFrames(),
                             static_cast<double>(nominal) / 1000.0, t);
            // Windows within tolerance end the skew episode for the
            // policy: a later verdict is a new fault, not the
            // continuation of this one. As many good windows in a row as
            // it takes bad ones to reach a verdict, for the same reason:
            // one window can be a transient. A 0.919x device on a loaded
            // macOS runner read one 300 ms window just after its reopen
            // as healthy (2026-09-19), which restarted the streak and
            // cost a fourth swap.
            if (w.rateSkew.windowsCompleted() != w.skewWindowsSeen) {
                w.skewWindowsSeen = w.rateSkew.windowsCompleted();
                w.skewGoodWindows = w.rateSkew.lastWindowGood() ? w.skewGoodWindows + 1 : 0;
                if (w.skewGoodWindows >= w.skewGoodRequired) w.skewPolicy.healthy();
            }
            if (w.rateSkew.skewed()) {
                const double ratio    = w.rateSkew.lastRatio();
                const double measured = ratio * static_cast<double>(nominal);
                const auto   action   = w.skewPolicy.next(ratio);
                if (action == clockwork::audio::RateSkewAction::None) {
                    // Given up this session: keep playing, keep quiet.
                } else if (action == clockwork::audio::RateSkewAction::GiveUp) {
                    // Skewing at the very rate it was measured delivering:
                    // no rate request corrects this device, and another
                    // swap would only stop the client's jobs again.
                    if (!w.skewGiveUpLogged) {
                        w.skewGiveUpLogged = true;
                        clockwork_log("[watchdog] rate skew persists at the adopted "
                               "rate (%.2fx of %d Hz) — no further recoveries this "
                               "session; playback continues at this rate",
                               ratio, nominal);
                    }
                    w.skewPolicy.acted(ratio);
                } else {
                    const bool adopt =
                        action == clockwork::audio::RateSkewAction::AdoptMeasuredRate;
                    if (!w.rateSkewLogged) {
                        w.rateSkewLogged = true;
                        clockwork_log("[watchdog] rate skew detected: device delivering "
                               "%.2fx real-time (nominal %d Hz) — clock cannot "
                               "converge, will %s",
                               ratio, nominal,
                               adopt ? "adopt the measured rate with a cold swap"
                                     : "recover with a cold swap");
                    }
                    RecoveryIntent intent;
                    intent.nominalRate  = nominal;
                    intent.measuredRate = measured;
                    if (adopt) intent.adoptRate = measured;
                    std::string reason;
                    if (requestAudioRecovery(reason, intent)) {
                        mWatchdogRecoveries.fetch_add(1, std::memory_order_release);
                        mRateSkewRecoveries.fetch_add(1, std::memory_order_release);
                        w.skewPolicy.acted(ratio);
                        if (adopt)
                            clockwork_log("[watchdog] rate skew persisted through %d "
                                   "recoveries: cold swap requesting the measured "
                                   "rate, ~%.0f Hz (%.2fx of %d Hz)",
                                   w.skewPolicy.streak() - 1, measured, ratio, nominal);
                        else
                            clockwork_log("[watchdog] rate skew: recovering with a cold "
                                   "swap (%.2fx real-time)", ratio);
                        w.rateSkew.reset();
                        w.skewGoodWindows = 0;
                        w.rateSkewLogged = false;
                    }
                }
            } else {
                w.rateSkewLogged = false;
            }
        }
        return;  // Live (healthy) or Confirming (give the ticks the window)
    }

    // The device stopped delivering. Recover with a cold swap on a fresh
    // connection. requestAudioRecovery is in-flight/cooldown gated, so only
    // log (and count) when one actually starts — not on every poll of the
    // stall.
    std::string reason;
    if (requestAudioRecovery(reason)) {
        mWatchdogRecoveries.fetch_add(1, std::memory_order_release);
        clockwork_log("[watchdog] audio device stalled (processCount=%u) — recovering", count);
    }
}

bool ClockworkEngine::requestAudioRecovery(std::string& reason) {
    return requestAudioRecovery(reason, RecoveryIntent{});
}

bool ClockworkEngine::requestAudioRecovery(std::string& reason,
                                           const RecoveryIntent& intent) {
    // Space recoveries: the cooldown covers the client's re-init after a
    // successful promotion — it has to re-establish whatever state it was
    // holding in the DSP, which takes seconds, and a tighter cooldown would
    // race a reinit still in progress. Config, not a constant, so a test can
    // walk a device through several recoveries in seconds rather than tens.
    const int kRecoveryCooldownMs = std::max(0, mCurrentConfig.watchdogRecoveryCooldownMs);

    // Claim the in-flight slot atomically: the watchdog and the /reopen control
    // thread can both reach here, and a load-then-store would let both pass and
    // launch two recoveries back-to-back (the second interrupting the first's
    // client re-init — exactly what the cooldown exists to prevent).
    bool expected = false;
    if (!mReopenInProgress.compare_exchange_strong(expected, true)) {
        reason = "already in progress";
        return false;
    }

    const int64_t now = watchdogNowMs();
    const int64_t last = mLastReopenFinishedAtMs.load();
    const int64_t sinceLast = last != kNeverMs ? now - last : kRecoveryCooldownMs;
    if (sinceLast < kRecoveryCooldownMs) {
        mReopenInProgress.store(false);   // release the slot we just claimed
        std::array<char, 96> msg{};
        snprintf(msg.data(), msg.size(), "cooldown (%lld ms since last)",
                 (long long)sinceLast);
        reason = msg.data();
        return false;
    }

    // Run on the device task lane, not the JUCE message thread: the Linux standalone
    // loop never pumps that queue, so a posted recovery would never run there. The
    // lane serialises recovery against every other deferred device mutation;
    // recoverAudio additionally holds the swap gate, which the message-thread device
    // handlers respect.
    postDeviceTask([this, intent]() { recoverAudio(intent); });
    reason = "started";
    return true;
}

void ClockworkEngine::recoverAudio(RecoveryIntent intent) {
    // A recovery can still be queued/starting when shutdown begins; skip rather
    // than operate on a torn-down engine. shutdown() sets mRunning=false and
    // then joins the device task lane: a recovery already past this check
    // completes and is waited on, while one that hasn't started yet
    // early-returns here.
    if (!mRunning.load()) {
        mReopenInProgress.store(false);
        return;
    }
    PhaseGuard phase(mDevicePhase, DevicePhase::Recovering);

    // Recovery is a full cold swap on a FRESH connection:
    //   1. recreateDeviceManager — a reopen reuses the hibernate-dead CoreAudio
    //      connection; only a brand-new AudioDeviceManager gets a live,
    //      coreaudiod-driven IO thread.
    //   2. reopenCurrentDevice — the cold swap on that fresh manager, rebuilding the
    //      DSP and emitting /clockwork/setup so a client drops the now-dead
    //      pre-outage timeline and re-establishes what it was tracking.
    //
    // Hold the swap gate across the whole swap. It is recursive, so reopenCurrentDevice
    // re-taking it is fine — but it MUST be released before the reopen.done broadcast,
    // which re-takes it. Hence the inner scope.
    std::string err;
    bool restored = false;
    SwapResult swap;   // device name/rate/buffer from the cold swap — reused below
    {
        std::unique_lock<std::recursive_mutex> lk;
        if (!tryAcquireSwapGate(lk, 20, 50)) {
            // Another thread genuinely holds the gate (a swap in flight) — it will
            // re-establish audio. Tell any /reopen caller it didn't run (so it
            // isn't left waiting), but DON'T stamp the cooldown: no recovery
            // happened, so the watchdog must stay free to retry the moment the
            // gate frees rather than sitting out a 3 s window for nothing.
            broadcastReopenDone(false, {}, 0, 0,
                                "device busy — another swap in progress");
            mReopenInProgress.store(false);
            return;
        }
        try {
            // The one message a user gets about a device that keeps time at a
            // rate other than the one it reports. Said here, under the gate,
            // where the device's name can be read; said once, because the
            // policy adopts once — anything after this is a plain log line.
            if (intent.adoptRate > 0) {
                std::string name;
                if (mDeviceManager)
                    if (auto* dev = mDeviceManager->getCurrentAudioDevice())
                        name = dev->getName().toStdString();
                clockwork_log("[watchdog] '%s' reports %.0f Hz but is delivering "
                       "~%.0f Hz. Playback continues at the measured rate; pitch "
                       "and timing may be slightly off. This is a fault in the "
                       "device or its driver, not in the engine.",
                       name.empty() ? "audio device" : name.c_str(),
                       intent.nominalRate, intent.measuredRate);
            }
            stopAudioSource();                      // detach the dead callback
            err = recreateDeviceManager();
            if (err.empty()) swap = reopenCurrentDevice(intent.adoptRate);
            // A pinned device that delivered nothing, with the engine back on
            // the default the fresh manager opened, is audio restored — there.
            restored = err.empty() && (swap.success || swap.fellBack)
                    && mActiveSource.load() == AudioSource::RealCallback;
        } catch (const std::exception& ex) {
            err = std::string("recovery exception: ") + ex.what();
        } catch (...) {
            err = "recovery exception (unknown)";
        }

        // If the swap couldn't bring a source up (recreate opened no device, or
        // an exception aborted mid-swap), run startAudioSource to settle into the
        // right idle state: an explicit-headless build restarts its driver; the
        // default build enters the "waiting for audio device" state, and the
        // watchdog keeps retrying until a device appears.
        if (mActiveSource.load() == AudioSource::None) {
            try {
                startAudioSource();
            } catch (const std::exception& e) {
                clockwork_log("[recovery] no audio source after the failed recovery: %s", e.what());
            } catch (...) {
                clockwork_log("[recovery] no audio source after the failed recovery");
            }
        }
    }   // release the gate before sendDeviceReport (which re-takes it)

    // The swap's own reason when the recreate went through: "No sound is coming
    // out of X…" says more than an empty err.
    if (err.empty() && !swap.success && !swap.error.empty())
        err = swap.error;
    clockwork_log("[recover] %s (err='%s')",
           restored ? "restored real audio" : "device down — watchdog will retry",
           err.c_str());
    if (restored && intent.adoptRate > 0)
        clockwork_log("[recover] session rate is now %.0f Hz (asked for the measured "
               "~%.0f Hz; the device reported %.0f Hz)",
               swap.sampleRate, intent.adoptRate, intent.nominalRate);

    // Report to the client/GUI over the reopen.done channel. On success a client runs
    // its cold-swap reinit; otherwise it's an informational failure. Report the
    // fields the cold swap already resolved (single source of truth, and no
    // ungated mDeviceManager read out here).
    if (restored)
        broadcastReopenDone(true, swap.deviceName, swap.sampleRate, swap.bufferSize, {});
    else
        broadcastReopenDone(false, {}, 0, 0,
                            err.empty() ? "audio device not yet recovered"
                                          : err);

    if (restored) sendDeviceReport();

    // ALWAYS clear the in-flight flag + stamp the cooldown, even after an
    // exception above — otherwise mReopenInProgress leaks true and the watchdog
    // (which treats it as benign) never recovers again for the rest of the session.
    mLastReopenFinishedAtMs.store(watchdogNowMs());
    mReopenInProgress.store(false);
}

void ClockworkEngine::broadcastReopenDone(bool success, const std::string& deviceName,
                                           double sampleRate, int bufferSize,
                                           const std::string& error) {
    const OscPacket packet = oscPacketOf(1024, [&](osc::OutboundPacketStream& s) {
        s << osc::BeginMessage(CLOCKWORK_SYS("devices/reopen.done"))
          << static_cast<osc::int32>(success ? 1 : 0)
          << deviceName.c_str()
          << static_cast<float>(sampleRate)
          << static_cast<osc::int32>(bufferSize)
          << (error.empty() ? "" : error.c_str())
          << osc::EndMessage;
    });
    mEgress.broadcastToTargets(packet.ptr(), packet.size());
}

void ClockworkEngine::sendSwitchDone(const SwapResult& result,
                                      const std::string& requestedOutput,
                                      const std::string& requestedInput) {
    if (!mEgress.hasSubscribers()) return;

    // Wire format (all fields present on every emit):
    //   success(int32),
    //   requestedOutput(str),  requestedInput(str),
    //   actualOutput(str),     actualInput(str),
    //   error(str),                  ← top-level error, "" on success
    //   inputUnavailable(int32),     ← 1 = output opened, input fell back
    //   inputUnavailableReason(str)  ← JUCE verbatim, "" otherwise
    const OscPacket packet = oscPacketOf(2048, [&](osc::OutboundPacketStream& s) {
        s << osc::BeginMessage(CLOCKWORK_SYS("devices/switch.done"))
          << static_cast<osc::int32>(result.success ? 1 : 0)
          << requestedOutput.c_str()
          << requestedInput.c_str()
          << result.deviceName.c_str()
          << result.inputDeviceName.c_str()
          << result.error.c_str()
          << static_cast<osc::int32>(result.inputUnavailable ? 1 : 0)
          << result.inputUnavailableReason.c_str()
          << osc::EndMessage;
    });
    mEgress.broadcastToTargets(packet.ptr(), packet.size());
}

void ClockworkEngine::sendDeviceReport() {
    if (!mEgress.hasSubscribers()) return;
    // Serialise mDeviceManager access against device mutations / recovery's
    // recreate. Recursive: the mutation paths call this while holding the gate.
    std::lock_guard<std::recursive_mutex> gate(mSwapMutex);

    // Skip rescan — sendDeviceReport() is usually called right after a
    // device switch, when rescanning would disrupt the freshly-opened device.
    auto allDevices = listDevices(false);
    auto current = currentDevice();
    auto mode    = deviceMode();

    // All list-shaping is policy: see selectReportedDevices for the filter order.
    std::vector<std::string> knownBadInputs;
    if (!current.name.empty())
        for (auto& dev : allDevices)
            if (dev.maxInputChannels > 0
                && isInputKnownBadFor(current.name, dev.name))
                knownBadInputs.push_back(dev.name);

    auto selection = clockwork::device::selectReportedDevices(
        allDevices, current.name, current.inputDeviceName,
        current.typeName, knownBadInputs);
    auto& outputDevices = selection.outputs;
    auto& inputDevices  = selection.inputs;

    // Per-driver device table: the same filtered devices grouped by driver and NOT
    // deduped, so a client can render any driver's list directly rather than inferring
    // it from the flat list below — whose dedupe keeps only the active driver's entry
    // for a shared name (Windows exposes identical endpoint names under WASAPI and
    // DirectSound). Group order follows enumeration order.
    struct DriverGroup {
        std::string driver;
        std::vector<std::string> outputs, inputs;
        std::vector<std::string> outputFlags, inputFlags;   // parallel, "" = none
    };
    std::vector<DriverGroup> deviceTable;
    {
        auto groupFor = [&](const std::string& t) -> DriverGroup& {
            for (auto& g : deviceTable)
                if (g.driver == t) return g;
            deviceTable.push_back({t, {}, {}, {}, {}});
            return deviceTable.back();
        };
        for (auto& dev : selection.outputsByDriver)
            groupFor(dev.typeName).outputs.push_back(dev.name);
        for (auto& dev : selection.inputsByDriver)
            groupFor(dev.typeName).inputs.push_back(dev.name);

        // Capability flags make the table the single source of truth for
        // client dropdowns: the GUI renders rows and their semantics from
        // here instead of synthesizing an "OS Default" entry client-side.
        // Drivers without a native default-follow device get a synthetic
        // flagged row; picking it is translated to system mode by
        // EngineControl (see isSyntheticDefaultPick).
#if defined(__linux__) && defined(CLOCKWORK_PIPEWIRE)
        const std::string nativeFollowDriver = "PipeWire";
        const std::string followName = pipeWireDefaultDeviceName();
        const std::string exclusiveName = pipeWirePatchbayDeviceName();
#else
        const std::string nativeFollowDriver;
        const std::string followName = clockwork::device::kSystemDefaultTableName;
        const std::string exclusiveName;
#endif
        for (auto& g : deviceTable) {
            auto out = clockwork::device::annotateDriverOutputs(
                g.driver, g.outputs, nativeFollowDriver, followName, exclusiveName);
            g.outputFlags = std::move(out.flags);
            if (out.insertSyntheticDefault) {
                g.outputs.insert(g.outputs.begin(), out.syntheticName);
                g.outputFlags.insert(g.outputFlags.begin(), out.syntheticFlags);
            }
            // Inputs carry the same per-device capabilities but never a
            // synthetic default row — "no input" is already an explicit
            // GUI choice, and input-follow semantics ride on the native
            // driver's own entry.
            auto in = clockwork::device::annotateDriverOutputs(
                g.driver, g.inputs, nativeFollowDriver, followName, exclusiveName);
            g.inputFlags = std::move(in.flags);
        }
    }

    // The flat lists (selection.outputs/inputs) arrive deduped by name,
    // active driver's entry winning — see selectReportedDevices step 6.
    clockwork_log("[device-list] outputs=%zu inputs=%zu currentIn='%s' currentOut='%s'",
            outputDevices.size(), inputDevices.size(),
            current.inputDeviceName.c_str(), current.name.c_str());
    if (selection.suppressReport) {
        clockwork_log("[device-list] skipping: currentIn set but inputDevices list empty "
                "(transient enumeration, probably mid-swap)");
        return;
    }

    // Build per-driver device table message
    // Format: currentDriver(str), intendedDriver(str), numDrivers(int32),
    //         then per driver: name(str),
    //         numOutputs(int32), then per output: name(str), flags(str),
    //         numInputs(int32),  then per input:  name(str), flags(str).
    //         Flags are comma-separated capability tokens
    //         ("follows-default", "exclusive-duplex", "synthetic"), "" = plain.
    // Counts-first throughout so parsers never type-sniff. The packet is the
    // size the table needs (oscPacketOf): a table too big for a fixed buffer
    // used to be dropped, and before that threw into the reporting thread.
    const OscPacket tablePacket = oscPacketOf(8192, [&](osc::OutboundPacketStream& tableMsg) {
        tableMsg << osc::BeginMessage(CLOCKWORK_SYS("device-table"))
                 << currentDriver().c_str()
                 << mIntendedDriver.c_str()
                 << static_cast<osc::int32>(deviceTable.size());
        for (auto& g : deviceTable) {
            tableMsg << g.driver.c_str()
                     << static_cast<osc::int32>(g.outputs.size());
            for (size_t i = 0; i < g.outputs.size(); ++i) {
                tableMsg << g.outputs[i].c_str();
                tableMsg << (i < g.outputFlags.size() ? g.outputFlags[i].c_str() : "");
            }
            tableMsg << static_cast<osc::int32>(g.inputs.size());
            for (size_t i = 0; i < g.inputs.size(); ++i) {
                tableMsg << g.inputs[i].c_str();
                tableMsg << (i < g.inputFlags.size() ? g.inputFlags[i].c_str() : "");
            }
        }
        tableMsg << osc::EndMessage;
    });

    // Build output device list message
    // Format: mode(str), current(str), device1(str), ..., deviceN(str),
    //         sampleRate(int32), compat1(int32), ..., compatN(int32),
    //         type1(str), ..., typeN(str)
    //   compat: 1 = device supports current rate, 0 = rate change needed
    //   type:   driver type per device, enables the GUI's per-driver
    //           dropdown filter
    const OscPacket devPacket = oscPacketOf(8192, [&](osc::OutboundPacketStream& devMsg) {
        devMsg << osc::BeginMessage(CLOCKWORK_SYS("devices"))
               << (mode.empty() ? "system" : mode.c_str())
               << current.name.c_str();
        for (auto& dev : outputDevices)
            devMsg << dev.name.c_str();
        devMsg << static_cast<osc::int32>(current.activeSampleRate);
        int curRate = static_cast<int>(current.activeSampleRate);
        for (auto& dev : outputDevices) {
            bool compat = false;
            for (auto r : dev.availableSampleRates)
                if (static_cast<int>(r) == curRate)
                    compat = true;
            devMsg << static_cast<osc::int32>(compat ? 1 : 0);
        }
        for (auto& dev : outputDevices)
            devMsg << dev.typeName.c_str();
        devMsg << osc::EndMessage;
    });

    // Build input device list message
    // Format: currentInput(str), numDevices(int32),
    //         name1(str), ..., nameN(str),
    //         type1(str), ..., typeN(str)
    const OscPacket inDevPacket = oscPacketOf(2048, [&](osc::OutboundPacketStream& inDevMsg) {
        inDevMsg << osc::BeginMessage(CLOCKWORK_SYS("input-devices"))
                 << current.inputDeviceName.c_str()
                 << static_cast<osc::int32>(inputDevices.size());
        for (auto& dev : inputDevices)
            inDevMsg << dev.name.c_str();
        for (auto& dev : inputDevices)
            inDevMsg << dev.typeName.c_str();
        inDevMsg << osc::EndMessage;
    });

    // Build hardware info message
    double sr = current.activeSampleRate;
    double outLatMs = sr > 0 ? (current.outputLatencySamples / sr) * 1000.0 : 0.0;
    double inLatMs  = sr > 0 ? (current.inputLatencySamples  / sr) * 1000.0 : 0.0;

    // The device's channel counts (maxOutputChannels / maxInputChannels),
    // falling back to the active ones when it gives none.
    int outCh = current.maxOutputChannels > 0 ? current.maxOutputChannels
                                              : current.activeOutputChannels;
    // Only report an input count when input channels are actually open —
    // maxInputChannels is the device's capability and is non-zero even
    // when inputs are disabled (the patchbay always exposes 16 port
    // names), which would show "in 16" on a device with no input active.
    int inCh  = current.activeInputChannels == 0 ? 0
              : current.maxInputChannels  > 0    ? current.maxInputChannels
                                                 : current.activeInputChannels;

    std::array<char, 1024> info{};
    if (!current.inputDeviceName.empty() && inCh > 0) {
        snprintf(info.data(), info.size(),
                 "Output:      %s (%d ch)\n"
                 "Input:       %s (%d ch)\n"
                 "Driver:      %s\n"
                 "Sample Rate: %.0f Hz\n"
                 "Buffer Size: %d samples\n"
                 "Latency:     %.1f / %.1f ms (out/in)",
                 current.name.c_str(), outCh,
                 current.inputDeviceName.c_str(), inCh,
                 current.typeName.c_str(),
                 sr,
                 current.activeBufferSize,
                 outLatMs, inLatMs);
    } else {
        snprintf(info.data(), info.size(),
                 "Output:      %s (%d ch)\n"
                 "Driver:      %s\n"
                 "Sample Rate: %.0f Hz\n"
                 "Buffer Size: %d samples\n"
                 "Latency:     %.1f ms",
                 current.name.c_str(), outCh,
                 current.typeName.c_str(),
                 sr,
                 current.activeBufferSize,
                 outLatMs);
    }

    // Info message with config data appended
    // Format: info_string, sampleRate(int32), bufferSize(int32),
    //         numRates(int32), rate1..rateN, numBufs(int32), buf1..bufN,
    //         numDrivers(int32), driver1..driverN, currentDriver(str),
    //         outputChannels(int32), inputChannels(int32),
    //         outputLatencySamples(int32),
    //         intendedDriver(str — pending switchDriver pick, "" = none)
    // Trailing fields are positional and optional: parsers pop in this
    // exact order, so new fields append at the END only.
    auto drivers = listDrivers();
    auto curDriver = currentDriver();

    // The rates the output and the input both offer (a paired device offers
    // its pair's already); the output's alone when no input is open.
    std::vector<double> usableRates = current.availableSampleRates;
    if (!current.inputDeviceName.empty()) {
        for (auto& dev : allDevices) {
            if (dev.name == current.inputDeviceName) {
                std::vector<double> intersection;
                for (auto r : current.availableSampleRates)
                    for (auto ir : dev.availableSampleRates)
                        if (static_cast<int>(r) == static_cast<int>(ir))
                            intersection.push_back(r);
                if (!intersection.empty())
                    usableRates = intersection;
                break;
            }
        }
    }

    const OscPacket infoPacket = oscPacketOf(4096, [&](osc::OutboundPacketStream& infoMsg) {
        infoMsg << osc::BeginMessage(CLOCKWORK_SYS("info"))
                << info.data()
                << static_cast<osc::int32>(current.activeSampleRate)
                << static_cast<osc::int32>(current.activeBufferSize)
                << static_cast<osc::int32>(usableRates.size());
        for (auto r : usableRates)
            infoMsg << static_cast<osc::int32>(r);

        // Same intersection for buffer sizes.
        std::vector<int> usableBufferSizes = current.availableBufferSizes;
        if (!current.inputDeviceName.empty()) {
            for (auto& dev : allDevices) {
                if (dev.name == current.inputDeviceName) {
                    std::vector<int> intersection;
                    for (auto b : current.availableBufferSizes)
                        for (auto ib : dev.availableBufferSizes)
                            if (b == ib)
                                intersection.push_back(b);
                    if (!intersection.empty())
                        usableBufferSizes = intersection;
                    break;
                }
            }
        }

        // Raw CoreAudio lists non-powers-of-two (14, 24, 48, 96) and extremes
        // (16, 8192); the powers of two from 16 to 2048 are the useful ones. (A
        // drift-compensated pair offers none below 256 already.)
        //
        // A DISPLAY filter, not a safety net: -Z / -z and sound_card_buffer_size
        // still force any value.
        {
            static const std::set<int> canonical = {16, 32, 64, 128, 256, 512, 1024, 2048};
            std::vector<int> filtered;
            for (int b : usableBufferSizes)
                if (canonical.count(b)) filtered.push_back(b);
            if (!filtered.empty()) usableBufferSizes = std::move(filtered);
            // Also ensure the currently-active buffer size is represented so
            // the GUI dropdown can display the correct selection when a
            // non-canonical size was forced via CLI / TOML.
            if (current.activeBufferSize > 0) {
                bool present = false;
                for (int b : usableBufferSizes)
                    if (b == current.activeBufferSize) { present = true; break; }
                if (!present) {
                    usableBufferSizes.insert(usableBufferSizes.begin(), current.activeBufferSize);
                }
            }
        }

        infoMsg << static_cast<osc::int32>(usableBufferSizes.size());
        for (auto b : usableBufferSizes)
            infoMsg << static_cast<osc::int32>(b);
        infoMsg << static_cast<osc::int32>(drivers.size());
        for (auto& d : drivers)
            infoMsg << d.c_str();
        infoMsg << curDriver.c_str();
        // The device's counts (outCh/inCh), as the banner has them, so a GUI
        // can render them directly.
        infoMsg << static_cast<osc::int32>(outCh);
        infoMsg << static_cast<osc::int32>(inCh);
        // Device output latency: DSP-computed audio reaches the speaker this
        // many samples later.
        infoMsg << static_cast<osc::int32>(current.outputLatencySamples);
        // Pending driver pick (switchDriver with no openable device yet).
        // curDriver stays truthful about the audio path; this lets the GUI
        // keep its driver dropdown on the user's uncommitted choice — and,
        // when empty, tells it no pick is pending so a stale local override
        // must follow curDriver instead of pinning forever.
        infoMsg << mIntendedDriver.c_str();
        infoMsg << osc::EndMessage;
    });

    // Fan out to all registered notify subscribers via the transport. The
    // table goes first so a client already holds the grouped lists when the
    // flat messages trigger its UI rebuild (relay order is not guaranteed;
    // clients must still tolerate either order).
    mEgress.broadcastToTargets(tablePacket.ptr(), tablePacket.size());
    mEgress.broadcastToTargets(devPacket.ptr(), devPacket.size());
    mEgress.broadcastToTargets(inDevPacket.ptr(), inDevPacket.size());
    mEgress.broadcastToTargets(infoPacket.ptr(), infoPacket.size());
}

bool ClockworkEngine::interceptBufferFreed(const uint8_t* /*data*/, uint32_t /*size*/) {
    /*
     * Egress-thread heap maintenance, and nothing else.
     *
     * A guest hands blocks back through DspHost::free_bytes, which carries a
     * pointer and no name — nothing here parses a message to find one. The hook
     * stays because this is a natural off-audio-thread moment for the
     * deferred-free queue.
     */
    clockwork_heap_foreign_maintenance();
    return false;
}

// --- Purge ---

void ClockworkEngine::purge() {
    // Discard pending IN-ring messages. Callers arrive on two threads — the
    // wake hook pre-tick on the audio thread, cold swap on a control thread
    // while audio is still live — so this only requests; the audio thread,
    // the IN ring's single consumer, applies the flush at the top of its
    // next drain.
    clockwork_ingress_flush_request();

    clear_scheduler();
}

// --- Variadic send helpers ---

void ClockworkEngine::sendBundle(double ntpTimeSec, std::initializer_list<OscPacket> messages) {
    const uint64_t tag =
        static_cast<uint64_t>(clockwork::ntpToOscTimetag(ntpTimeSec));
    auto pkt = OscBuilder::bundle(tag, messages);
    sendOSC(pkt.ptr(), pkt.size());
}

// --- Device management ---

std::vector<DeviceInfo> ClockworkEngine::listDevices(bool rescan) const {
    // Serialise mDeviceManager access against device mutations / recovery's
    // recreate (see mSwapMutex). Recursive: mutation paths call this under the
    // gate. Lock order is always gate-then-mListDevicesMutex.
    std::lock_guard<std::recursive_mutex> gate(mSwapMutex);
    std::vector<DeviceInfo> result;
    if (!mDeviceManager) return result;

    // Cache hit — see ClockworkEngine.h for the rationale (JUCE WASAPI
    // probing is ~10 s for a typical device set, called multiple times
    // during boot). rescan=true refreshes it.
    if (!rescan) {
        std::lock_guard<std::mutex> lk(mListDevicesMutex);
        if (!mCachedDevices.empty())
            return mCachedDevices;
    }

    // What the driver says of a device without opening it (its traits), and
    // its channels where no probe answered: the driver's count when it can
    // say, else a placeholder of 2 out / 1 in that decisions don't trust
    // (out/inChannelsProbed stay false).
    auto takeTraits = [](DeviceInfo& info, const juce::AudioIODeviceType::DeviceTraits& t) {
        info.wireless       = t.wireless;
        info.isVirtual      = t.isVirtual;
        info.aggregateClass = t.aggregateClass;
        info.pairs          = t.pairs;
        info.kind           = t.kind.toStdString();
    };
    auto fillOutputs = [](DeviceInfo& info, const juce::AudioIODeviceType::DeviceTraits& t) {
        if (info.maxOutputChannels != 0) return;
        info.maxOutputChannels = t.numOutputChannels >= 0 ? t.numOutputChannels : 2;
        info.outChannelsProbed = t.numOutputChannels > 0;
    };
    auto takeTraitRates = [](DeviceInfo& info, const juce::AudioIODeviceType::DeviceTraits& t) {
        if (t.sampleRates.isEmpty()) return false;
        info.availableSampleRates.assign(t.sampleRates.begin(), t.sampleRates.end());
        info.availableBufferSizes.assign(t.bufferSizes.begin(), t.bufferSizes.end());
        return true;
    };
    auto fillInputs = [](DeviceInfo& info, const juce::AudioIODeviceType::DeviceTraits& t) {
        if (info.maxInputChannels != 0) return;
        info.maxInputChannels = t.numInputChannels >= 0 ? t.numInputChannels : 1;
        info.inChannelsProbed = t.numInputChannels > 0;
    };

    // A device its driver describes without opening it (traits with its
    // rates — CoreAudio's do) is never opened to be asked: a probe of a
    // device a live aggregate holds can stop the aggregate. Others are
    // probed, once each, never the one open — its handle is live.
    std::string activeDeviceName;
    if (auto* dev = mDeviceManager->getCurrentAudioDevice())
        activeDeviceName = dev->getName().toStdString();

    // A device's rates, buffer sizes and channels, from a device created to
    // be asked (a probe), kept by driver, name and direction (mDeviceProbes):
    // each is probed once, not on every rescan.
    auto populateFromDevice = [](DeviceInfo& info, juce::AudioIODevice* dev) {
        for (auto r : dev->getAvailableSampleRates())
            info.availableSampleRates.push_back(r);
        for (auto b : dev->getAvailableBufferSizes())
            info.availableBufferSizes.push_back(b);
        info.maxOutputChannels = dev->getOutputChannelNames().size();
        info.maxInputChannels  = dev->getInputChannelNames().size();
        // Only the side the device was created on is authoritative: a
        // wrapper created output-only reports no input channels even on
        // a full-duplex device (the input merge below probes that side).
        info.outChannelsProbed = info.maxOutputChannels > 0;
        info.inChannelsProbed  = info.maxInputChannels > 0;
    };
    auto takeProbe = [](DeviceInfo& info, const DeviceInfo& probe) {
        info.availableSampleRates = probe.availableSampleRates;
        info.availableBufferSizes = probe.availableBufferSizes;
        info.maxOutputChannels    = probe.maxOutputChannels;
        info.maxInputChannels     = probe.maxInputChannels;
        info.outChannelsProbed    = probe.outChannelsProbed;
        info.inChannelsProbed     = probe.inChannelsProbed;
    };
    std::set<std::string> probesListed;   // the rest are forgotten below
    auto probe = [&](juce::AudioIODeviceType* type, const std::string& typeName,
                     const juce::String& devName, bool asInput) -> const DeviceInfo* {
        const std::string name = devName.toStdString();
        const std::string key  = typeName + '\x1f' + name + (asInput ? "\x1fin" : "\x1fout");
        probesListed.insert(key);
        if (auto it = mDeviceProbes.find(key); it != mDeviceProbes.end())
            return &it->second;
        if (name == activeDeviceName) return nullptr;
        std::unique_ptr<juce::AudioIODevice> dev(
            asInput ? type->createDevice(juce::String(), devName)
                    : type->createDevice(devName, juce::String()));
        if (!dev) return nullptr;              // asked again next time
        DeviceInfo probed;
        populateFromDevice(probed, dev.get());
        return &(mDeviceProbes[key] = std::move(probed));
    };

    auto& types = mDeviceManager->getAvailableDeviceTypes();
    for (auto* type : types) {
        if (rescan) type->scanForDevices();

        std::string typeNameStr = type->getTypeName().toStdString();

        // ASIO names come from the registry, so an installed-but-unloadable
        // driver is listed exactly like a working one. Drop the ones the
        // Windows loader rejects outright — probing them yields no channels,
        // and the 0 -> 2/1 fallback below would dress them up as usable.
        const std::set<std::string> unloadable =
            typeNameStr == "ASIO" ? clockwork::device::unloadableAsioDrivers()
                                  : std::set<std::string>{};

        auto outputNames = type->getDeviceNames(false);
        for (auto& devName : outputNames) {
            DeviceInfo info;
            info.name = devName.toStdString();
            if (unloadable.count(info.name)) continue;
            info.typeName = typeNameStr;
            const auto traits = type->getDeviceTraits(devName);
            takeTraits(info, traits);

            if (!takeTraitRates(info, traits))
                if (auto* probed = probe(type, typeNameStr, devName, false))
                    takeProbe(info, *probed);
            fillOutputs(info, traits);

            result.push_back(std::move(info));
        }

        // Enumerate input devices. A full-duplex device (e.g. a MOTU
        // soundcard with both playback and capture) shows up in both the
        // output and input enumerations with the same name. Merge the
        // input-side info into the existing entry so full-duplex inputs are
        // recorded and the GUI's input dropdown shows them.
        auto inputNames = type->getDeviceNames(true);
        for (auto& devName : inputNames) {
            std::string nameStr = devName.toStdString();
            if (unloadable.count(nameStr)) continue;

            DeviceInfo* existing = nullptr;
            for (auto& e : result) {
                if (e.name == nameStr && e.typeName == typeNameStr) {
                    existing = &e;
                    break;
                }
            }

            if (existing) {
                const auto traits = type->getDeviceTraits(devName);
                if (traits.numInputChannels < 0)
                    if (auto* probed = probe(type, typeNameStr, devName, true)) {
                        existing->maxInputChannels = probed->maxInputChannels;
                        existing->inChannelsProbed = existing->maxInputChannels > 0;
                    }
                fillInputs(*existing, traits);
                continue;
            }

            // Input-only device (e.g. MacBook Pro Microphone).
            DeviceInfo info;
            info.name = std::move(nameStr);
            info.typeName = typeNameStr;
            const auto traits = type->getDeviceTraits(devName);
            takeTraits(info, traits);

            if (!takeTraitRates(info, traits))
                if (auto* probed = probe(type, typeNameStr, devName, true))
                    takeProbe(info, *probed);
            fillInputs(info, traits);

            result.push_back(std::move(info));
        }
    }

    // A device no longer listed is forgotten: plugged back in, it is
    // probed as it is then.
    for (auto it = mDeviceProbes.begin(); it != mDeviceProbes.end();)
        it = probesListed.count(it->first) ? std::next(it) : mDeviceProbes.erase(it);

    {
        std::lock_guard<std::mutex> lk(mListDevicesMutex);
        mCachedDevices = result;
    }
    return result;
}

bool ClockworkEngine::isSyntheticDefaultPick(const std::string& name) const {
    if (name != clockwork::device::kSystemDefaultTableName)
        return false;
    // A driver with a real device of this name (PipeWire) makes it a
    // literal pick; everywhere else the row exists only in the table.
    const std::string driver = currentDriver();
    for (const auto& dev : listDevices(false))
        if (dev.typeName == driver && dev.name == name)
            return false;
    return true;
}

CurrentDeviceInfo ClockworkEngine::currentDevice() const {
    // Serialise mDeviceManager access against device mutations / recovery's
    // recreate (see mSwapMutex). Recursive: mutation paths call this under gate.
    std::lock_guard<std::recursive_mutex> gate(mSwapMutex);
    CurrentDeviceInfo info;
    if (!mDeviceManager) return info;

    auto* dev = mDeviceManager->getCurrentAudioDevice();
    if (!dev) return info;

    info.name     = dev->getName().toStdString();
    info.typeName = dev->getTypeName().toStdString();
    info.activeSampleRate    = dev->getCurrentSampleRate();
    info.activeBufferSize    = dev->getCurrentBufferSizeSamples();
    info.controlBlockSize    = mProcessor.bufferLength();
    info.activeOutputChannels = dev->getActiveOutputChannels().countNumberOfSetBits();
    info.activeInputChannels  = dev->getActiveInputChannels().countNumberOfSetBits();
    info.outputLatencySamples = dev->getOutputLatencyInSamples();
    info.inputLatencySamples  = dev->getInputLatencyInSamples();

    juce::AudioDeviceManager::AudioDeviceSetup setup;
    mDeviceManager->getAudioDeviceSetup(setup);
    info.inputDeviceName = setup.inputDeviceName.toStdString();

    // ASIO drivers are full-duplex single-device by spec — one
    // AudioIODevice carries both directions. JUCE has no separate
    // input-device concept on ASIO; setup.inputDeviceName echoes
    // back whatever the caller passed in, regardless of what's
    // actually open. Derive the truthful input name from the
    // output device when on ASIO and the input is active.
    if (info.typeName == "ASIO")
        info.inputDeviceName = (info.activeInputChannels > 0) ? info.name : "";

    // With zero active input channels there is no usable input, whatever
    // name the setup echoes back (JUCE fills in the driver's default-input
    // name even when the device was opened output-only). Report none so
    // the GUI's input dropdown matches reality.
    if (info.activeInputChannels == 0)
        info.inputDeviceName.clear();

    for (auto r : dev->getAvailableSampleRates())
        info.availableSampleRates.push_back(r);
    for (auto b : dev->getAvailableBufferSizes())
        info.availableBufferSizes.push_back(b);

    info.maxOutputChannels = dev->getOutputChannelNames().size();
    info.maxInputChannels  = dev->getInputChannelNames().size();

    return info;
}

// ── Audio source state machine ──────────────────────────────────────────────
//
// See the contract on the enum/helpers in ClockworkEngine.h.

ClockworkEngine::AudioSource ClockworkEngine::desiredAudioSource() const {
    if (mDeviceManager && mDeviceManager->getCurrentAudioDevice())
        return AudioSource::RealCallback;
    // Headless is an EXPLICIT mode only — the test harness and future non-JUCE /
    // WASM backends. For the default engine, no open device means None: a waiting
    // state the watchdog recovers from when a device appears.
    if (mHeadless)
        return AudioSource::Headless;
    return AudioSource::None;
}

bool ClockworkEngine::startAudioSource() {
    if (mActiveSource.load() != AudioSource::None) {
        // Should be unreachable: every caller stops before starting.
        // Assert in debug so a regression fails loudly; log+return in
        // release so we don't crash a user session.
        jassertfalse;
        clockwork_log(
                "[engine] BUG: startAudioSource called while %s already active",
                mActiveSource.load() == AudioSource::RealCallback ? "RealCallback" : "Headless");
        return true;
    }

    // Manual-pump mode (tests): start no audio source at all. The caller owns
    // process_audio() on its own thread, and an autonomous source here would give two
    // concurrent callers — a data race on the whole engine state. mActiveSource stays
    // None so shutdown's stopAudioSource() no-ops.
    if (mCurrentConfig.manualAudioPump) {
        clockwork_log(
                "[engine] manual audio pump — no audio source started; "
                "caller drives process_audio()");
        mActiveSource.store(AudioSource::None, std::memory_order_release);
        return true;
    }

    uint32_t before = mProcessor.processCount.load(std::memory_order_acquire);

    const AudioSource desired = desiredAudioSource();
    if (desired == AudioSource::RealCallback) {
        // Bracket the attach with log lines: a silent process death in this
        // window (seen in the wild on a virtual 8-out device) is only
        // localisable if the log shows exactly how far we got — nothing after
        // "attaching" = died inside the driver's start path; "attached" but no
        // "[juce] first audio callback" = died before the device's IO thread
        // reached our callback. The gateway thread drains the ring on its own,
        // so the lines reach the host even when this thread never returns.
        {
            auto* dev = mDeviceManager->getCurrentAudioDevice();
            clockwork_log("[engine] attaching audio callback to device '%s'",
                    dev ? dev->getName().toRawUTF8() : "(none)");
        }
        mDeviceManager->addAudioCallback(mDeviceCallback.get());
        clockwork_log("[engine] audio callback attached — waiting for first tick");
        mActiveSource.store(AudioSource::RealCallback, std::memory_order_release);
    } else if (desired == AudioSource::Headless) {
        // Explicit headless (mHeadless): tests and future non-JUCE backends.
        mHeadlessDriver->configure(&mProcessor,
                                   mCurrentConfig.sampleRate,
                                   mCurrentConfig.bufferSize,
                                   mCurrentConfig.numOutputChannels,
                                   mCurrentConfig.numInputChannels);
        mHeadlessDriver->setClockworkClock(&mClockworkClock);
        mHeadlessDriver->setLinkAudio(&mLinkAudio);
        mHeadlessDriver->setMetrics(mMetrics);
        mHeadlessDriver->startThread(juce::Thread::Priority::highest);
        mActiveSource.store(AudioSource::Headless, std::memory_order_release);
    } else {
        // No audio device on the default (JUCE) engine: stay sourceless and
        // surface it. The watchdog keeps trying to open a device and cold-swaps
        // in when one appears (plug in / wake). audioSource()==None with a live
        // device manager is the "waiting for audio device" state.
        mActiveSource.store(AudioSource::None, std::memory_order_release);
        clockwork_log("[engine] no audio device available — engine is idle and will "
               "recover when one appears");
        sendDeviceReport();   // GUI sees an empty current device
        return true;          // nothing will tick; don't wait for a first block
    }

    return waitForFirstAudioTick(before);
}

void ClockworkEngine::stopAudioSource() {
    switch (mActiveSource.load()) {
    case AudioSource::None:
        return;
    case AudioSource::RealCallback:
        if (mDeviceManager)
            mDeviceManager->removeAudioCallback(mDeviceCallback.get());
        // Change listener is NOT removed here; it survives swaps and is
        // removed only in shutdown(). Removing it would lose hot-plug
        // events between stop and the next start.
        break;
    case AudioSource::Headless:
        mHeadlessDriver->signalThreadShouldExit();
        mHeadlessDriver->stopThread(2000);
        break;
    }
    mActiveSource.store(AudioSource::None, std::memory_order_release);
}

bool ClockworkEngine::waitForFirstAudioTick(uint32_t before) {
    constexpr int kTimeoutMs = 5000;
    auto start = std::chrono::steady_clock::now();
    auto deadline = start + std::chrono::milliseconds(kTimeoutMs);
    while (mProcessor.processCount.load(std::memory_order_acquire) == before
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    bool ticked = mProcessor.processCount.load(std::memory_order_acquire) != before;
    if (!ticked) {
        clockwork_log(
                "[engine] WARNING: audio callbacks not firing after %d ms, "
                "engine is alive but the audio thread has not started", kTimeoutMs);
    } else {
        auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        clockwork_log("[engine] audio callbacks started (%lld ms)",
                       static_cast<long long>(elapsedMs));
    }
    return ticked;
}

std::string ClockworkEngine::driverWithADefault() {
    // The boot's driver when it has a default, otherwise the first that does:
    // on Windows, back from ASIO to the driver it booted on, or to WASAPI if it
    // booted on ASIO. A driver's names are only known once it has looked; the
    // current one has, and is not looked through again under its open device.
    auto* current = mDeviceManager->getCurrentDeviceTypeObject();
    auto namesADefault = [current](juce::AudioIODeviceType* type) {
        if (type->getSystemDefaultDeviceName(false).isNotEmpty()) return true;
        if (type == current) return false;
        type->scanForDevices();
        return type->getSystemDefaultDeviceName(false).isNotEmpty();
    };
    auto& types = mDeviceManager->getAvailableDeviceTypes();
    for (auto* type : types)
        if (type->getTypeName().toStdString() == mBootDriver && namesADefault(type))
            return mBootDriver;
    for (auto* type : types)
        if (namesADefault(type)) return type->getTypeName().toStdString();
    return {};
}

std::string ClockworkEngine::reinitialiseWithDefaultsPreservingConfig() {
    // The session's, from the engine's own record: recovery calls this on a
    // device manager it has just made, which knows nothing of the session.
    int prevRate = mCurrentConfig.sampleRate;
    int prevBufSize = mCurrentConfig.bufferSize;

    // "System default" means nothing on a driver with none (ASIO): JUCE's
    // default-device init stops the current device and opens nothing. Ask a
    // driver that has one instead (driverWithADefault).
    if (auto* type = mDeviceManager->getCurrentDeviceTypeObject();
        type && type->getSystemDefaultDeviceName(false).isEmpty()) {
        const std::string current = type->getTypeName().toStdString();
        const std::string target = driverWithADefault();
        if (!target.empty() && target != current) {
            clockwork_log("[device-setup] system default requested from %s, which "
                    "has none — using %s's", current.c_str(), target.c_str());
            mDeviceManager->setCurrentAudioDeviceType(juce::String(target), true);
        }
    }

    auto err = mDeviceManager->initialiseWithDefaultDevices(0, 2);
    if (err.isNotEmpty())
        err = mDeviceManager->initialiseWithDefaultDevices(0, 0);
    if (err.isNotEmpty()) return err.toStdString();

    // An init that reports success but opens no device is still a failure
    // here: callers assume a current device afterwards, and without one the
    // audio callback never restarts and the engine produces no sound. Report
    // it so the caller can recover.
    if (!mDeviceManager->getCurrentAudioDevice())
        return "system default init opened no device";

    // Don't second-guess JUCE's negotiation on wireless devices. AirPlay and
    // Bluetooth negotiate a specific buffer size with the remote end; JUCE reports
    // many as available but only the negotiated one delivers audio, and forcing
    // another runs the IOProc — scope stays happy — while the speakers are silent.
    // Non-wireless devices keep the user's previous rate and buffer so GUI selections
    // survive System Output toggles.
    auto setup = mDeviceManager->getAudioDeviceSetup();
    bool isWireless = false;
    if (auto* type = mDeviceManager->getCurrentDeviceTypeObject())
        isWireless = type->getDeviceTraits(mDeviceManager->getCurrentAudioDevice()->getName()).wireless;
    if (!isWireless) {
        setup.sampleRate = static_cast<double>(prevRate);
        setup.bufferSize = prevBufSize;
        mDeviceManager->setAudioDeviceSetup(setup, true);
    } else {
        // Wireless device now active. AirPlay 1 negotiates 44.1 kHz, but AirPlay 2
        // receivers commonly support 48. Probe the device's rates and force prevRate if
        // it is there, so a modern receiver does not downgrade a 48 kHz session.
        bool forcedPrev = false;
        if (auto* dev = mDeviceManager->getCurrentAudioDevice()) {
            bool prevSupported = false;
            for (auto r : dev->getAvailableSampleRates()) {
                if (static_cast<int>(r) == prevRate) {
                    prevSupported = true;
                    break;
                }
            }
            if (prevSupported && static_cast<int>(setup.sampleRate) != prevRate) {
                setup.sampleRate = static_cast<double>(prevRate);
                auto err2 = mDeviceManager->setAudioDeviceSetup(setup, true);
                if (err2.isEmpty()) {
                    forcedPrev = true;
                    clockwork_log("[device-setup] reinit: wireless device supports "
                            "prev rate %d — forcing it (was %.0f)",
                            prevRate, dev->getCurrentSampleRate());
                } else {
                    clockwork_log("[device-setup] reinit: setAudioDeviceSetup at "
                            "prev rate %d failed on wireless (%s), keeping negotiated",
                            prevRate, err2.toRawUTF8());
                }
            }
        }
        if (!forcedPrev) {
            clockwork_log("[device-setup] reinit: keeping JUCE's negotiated rate=%.0f buf=%d "
                    "for wireless device (prev rate=%d buf=%d)",
                    setup.sampleRate, setup.bufferSize, prevRate, prevBufSize);
        }
    }
    return {};
}

SwapResult ClockworkEngine::switchDevice(const std::string& rawOutputName,
                                           double sampleRate,
                                           int bufferSize,
                                           bool forceCold,
                                           const std::string& rawInputName,
                                           SwapOrigin origin) {
    // Normalise raw CoreAudio names to JUCE's disambiguated form. A raw name
    // handed to setAudioDeviceSetup errors "No such device" whenever CoreAudio
    // has duplicate base names — two identical USB interfaces, two AirPlay
    // endpoints. GUI names are already JUCE-form and sentinels ("__system__",
    // "__none__") match nothing; both pass through.
    std::string deviceName = rawOutputName;
    std::string inputDeviceName = rawInputName;
    if (mDeviceManager && (!deviceName.empty() || !inputDeviceName.empty())) {
        std::vector<std::string> visibleNames;
        for (auto& d : listDevices(false)) visibleNames.push_back(d.name);
        deviceName      = clockwork::device::resolveJuceDeviceName(deviceName, visibleNames);
        inputDeviceName = clockwork::device::resolveJuceDeviceName(inputDeviceName, visibleNames);
    }

#if defined(__linux__) && defined(CLOCKWORK_PIPEWIRE)
    // The patchbay's two sides live on one filter node, so a mixed patchbay/stream
    // pair can never open as requested — JUCE would hand both sides to whichever
    // device the type resolves, silently overriding the side the user changed.
    // Resolve the pair that will really open before any destructive work. Gated on a
    // NAMED request: rate/buffer-only swaps carry no pairing intent, and
    // currentDevice() takes the swap gate.
    if (!deviceName.empty() || !inputDeviceName.empty()) {
        auto cur = currentDevice();
        const auto resolved = clockwork::device::resolveExclusiveDuplexPair(
            deviceName, inputDeviceName, cur.name, cur.inputDeviceName,
            pipeWirePatchbayDeviceName(), pipeWireDefaultDeviceName());
        if (resolved.output != deviceName || resolved.input != inputDeviceName) {
            clockwork_log(
                    "[device-setup] exclusive-pair resolve: out '%s' -> '%s', in '%s' -> '%s'",
                    deviceName.c_str(), resolved.output.c_str(),
                    inputDeviceName.c_str(), resolved.input.c_str());
            deviceName = resolved.output;
            inputDeviceName = resolved.input;
        }
    }
#endif

    SwapResult result;
    result.deviceName = deviceName;
    bool recovered = false;

    // No-op detection: destroying and recreating an identical device is
    // fragile — CoreAudio sometimes stops a recreated aggregate device within
    // a callback or two, and on Linux the ALSA close+reopen races PipeWire's
    // client-side node setup and can SIGSEGV inside libspa-audioconvert
    // (a GUI's boot-time saved-prefs restore sends exactly
    // such a same-device switch). If the caller asked for exactly what we
    // already have, short-circuit. A requested input counts as satisfied
    // only when it names the input that is currently open — enabling a
    // closed input is a real change and must reopen.
    if (!deviceName.empty() && sampleRate <= 0 && bufferSize <= 0 && !forceCold) {
        std::string activeReal = mDeviceManager && mDeviceManager->getCurrentAudioDevice()
            ? mDeviceManager->getCurrentAudioDevice()->getName().toStdString() : "";
        bool inputSatisfied = inputDeviceName.empty();
        if (!inputSatisfied) {
            auto cur = currentDevice();
            inputSatisfied = cur.activeInputChannels > 0
                          && !cur.inputDeviceName.empty()
                          && cur.inputDeviceName == inputDeviceName;
        }
        if (activeReal == deviceName && inputSatisfied) {
            result.success = true;
            result.type = SwapType::Hot;
            result.deviceName = deviceName;
            result.sampleRate = mCurrentConfig.sampleRate;
            result.bufferSize = mCurrentConfig.bufferSize;
            // Nothing to open, but a user's pick of the device already
            // playing is their pick all the same: hot-plug follows it.
            recordSwapPreferences(deviceName, inputDeviceName, result.sampleRate, origin);
            return result;
        }
    }

    // Refuse "add an input to an output that plays alone" upfront, BEFORE
    // any cold-swap work: the pairing check later in this function would
    // drop the input anyway, after a cold swap for nothing.
    if (auto err = refuseUnpairableInput(deviceName, inputDeviceName);
        !err.empty()) {
        result.error = err;
        return result;
    }

    // Reject swap with an unknown output / input device name BEFORE any
    // destructive state mutation. Without this, switchDevice mutates
    // mCurrentConfig.numInputChannels, tears down the DSP instance and
    // pauses the audio callback before discovering at setAudioDeviceSetup
    // time that the name doesn't resolve — leaving half-built state that
    // the client's next cold-swap re-init then inherits.
    if (auto err = refuseUnknownDeviceName(deviceName, inputDeviceName);
        !err.empty()) {
        result.error = err;
        clockwork_log("[switchDevice] refused: %s", err.c_str());
        return result;
    }

    // Acquire the swap gate (non-blocking) — tryAcquireSwapGate is the
    // single way any code takes it.
    std::unique_lock<std::recursive_mutex> guard;
    if (!tryAcquireSwapGate(guard, 1, 0)) {
        result.error = "swap already in progress";
        clockwork_log("[switchDevice] refused: %s", result.error.c_str());
        return result;
    }
    PhaseGuard phase(mDevicePhase, DevicePhase::Swapping);

    // ── Plan ────────────────────────────────────────────────────────────
    // All decisions live in DevicePolicy::planSwap; this function gathers the
    // snapshot the planner asks for and executes the plan. Two passes: the first
    // resolves names and scope so the probes can target the right devices, and is
    // skipped when the first pass already forced a cold swap or decided a rate.
    clockwork::device::SwapSnapshot snap;
    snap.hasDeviceManager = mDeviceManager != nullptr;
    if (mDeviceManager) {
        if (auto* dev = mDeviceManager->getCurrentAudioDevice()) {
            snap.juceCurrentType   = dev->getTypeName().toStdString();
            snap.currentOutputName = dev->getName().toStdString();
            snap.currentRate       = dev->getCurrentSampleRate();
        } else {
            snap.juceCurrentType =
                mDeviceManager->getCurrentAudioDeviceType().toStdString();
        }
        for (auto& d : listDevices(false)) {
            snap.deviceTable.emplace_back(d.typeName, d.name);
            if (d.wireless)
                snap.wirelessDeviceNames.push_back(d.name);
        }
    } else {
        snap.currentRate = static_cast<double>(mCurrentConfig.sampleRate);
    }
    snap.intendedDriver        = mIntendedDriver;
    snap.deviceMode            = mDeviceMode;
    snap.currentOutputChannels = mCurrentConfig.numOutputChannels;
    snap.currentInputChannels  = mCurrentConfig.numInputChannels;
    snap.bootInputChannels     = mBootInputChannels;
    snap.preWirelessRate       = mPreWirelessRate;
    if (auto it = mDeviceRateMemory.find(deviceName);
        it != mDeviceRateMemory.end())
        snap.rememberedRate = it->second;

    // Where to go back to if the target opens but never delivers a block: the
    // device playing now, at its rate and buffer.
    struct { std::string output, input; double rate = 0; int buffer = 0; } playing;
    if (mDeviceManager) {
        if (auto* dev = mDeviceManager->getCurrentAudioDevice()) {
            playing.output = snap.currentOutputName;
            playing.rate   = snap.currentRate;
            playing.buffer = dev->getCurrentBufferSizeSamples();
            if (mCurrentConfig.numInputChannels > 0) {
                juce::AudioDeviceManager::AudioDeviceSetup setup;
                mDeviceManager->getAudioDeviceSetup(setup);
                playing.input = setup.inputDeviceName.toStdString();
            }
        }
    }
    if (playing.input.empty()) playing.input = "__none__";

    clockwork::device::SwapPlanRequest planReq;
    planReq.outputName    = deviceName;
    planReq.inputName     = inputDeviceName;
    planReq.sampleRate    = sampleRate;
    planReq.bufferSize    = bufferSize;
    planReq.forceCold     = forceCold;
    planReq.userInitiated = origin == SwapOrigin::User;

    auto plan = clockwork::device::planSwap(planReq, snap);
    if (plan.error.empty()) {
        // Fill in the probes the first pass showed we need, then re-plan.
        if (sampleRate <= 0 && !plan.restoredPreWirelessRate) {
            snap.outputDeviceRates = probeDeviceSampleRates(deviceName, false);
            snap.inputDeviceRates  = probeDeviceSampleRates(plan.inputName, true);
        }
        if (plan.enableInputWidth >= 0)
            snap.probedInputChannels = probeDeviceChannelCount(
                plan.inputName, true, probeDriverTypeName(plan.scope));
        else if (mDeviceManager && !forceCold) {
            const std::string probeType = probeDriverTypeName(plan.scope);
            snap.probedTargetOut =
                probeDeviceChannelCount(deviceName, false, probeType);
            snap.probedTargetIn =
                probeDeviceChannelCount(plan.inputName, true, probeType);
        }
        plan = clockwork::device::planSwap(planReq, snap);
    }
    if (!plan.error.empty()) {
        result.error = plan.error;
        // Every way out of here says why, through the funnel: the client's
        // log pane sees it, and so does a test's captured debug stream. A swap
        // that failed silently read as a flake on a CI runner, and was — until
        // this line — indistinguishable from one.
        clockwork_log("[switchDevice] refused: %s", result.error.c_str());
        return result;
    }

    // Execute the plan's bookkeeping decisions + the logs the old inline
    // code emitted (provenance flags exist for exactly this).
    if (plan.abandonDriverIntent) {
        clockwork_log(
            "[device-setup] abandoning pending driver intent '%s' "
            "— picks resolve under '%s'",
            mIntendedDriver.c_str(), snap.juceCurrentType.c_str());
        mIntendedDriver.clear();
    }
    clockwork::device::SwapScope scope = plan.scope;
    if (scope.crossDriver) {
        clockwork_log(
            "[device-setup] cross-driver: '%s' -> '%s' (device '%s')",
            snap.juceCurrentType.c_str(), scope.targetDriver.c_str(),
            scope.targetDevice.c_str());
    }
    if (plan.inputName != inputDeviceName) {
        clockwork_log(
            "[device-setup] ASIO full-duplex: mirroring '%s' to input",
            plan.inputName.c_str());
    }
    inputDeviceName = plan.inputName;
    if (plan.restoredPreWirelessRate) {
        clockwork_log("[device-setup] restoring pre-wireless rate %d "
                "(current=%.0f)", mPreWirelessRate, snap.currentRate);
    }
    if (plan.rateAdjustedToNearest) {
        clockwork_log(
            "[device-setup] current rate %.0f not supported "
            "by the target device, will use %.0f (cold swap)",
            snap.currentRate, plan.sampleRate);
    }
    sampleRate = plan.sampleRate;

    if (plan.enableInputWidth >= 0) {
#ifdef __APPLE__
        // Log mic permission status for diagnostics — but don't refuse
        // enabling inputs. Our TCC query may return notDetermined
        // when launched as a child of the GUI, while CoreAudio's actual
        // mic stream honours the GUI's grant via responsible-process
        // attribution. Trying anyway may work; if buffers come back zero,
        // we'll know TCC really is denying.
        std::string micStat = MicPermission::status();
        if (micStat != "authorized") {
            clockwork_log("[device-setup] mic permission status=%s (proceeding anyway; "
                    "CoreAudio may still grant via GUI's responsible process)",
                    micStat.c_str());
        }
#endif
        if (plan.enableInputWidth != plan.enableInputRequested) {
            clockwork_log("[device-setup] auto-enabling %d input channels for '%s' "
                    "(requested %d, device max %d)",
                    plan.enableInputWidth, inputDeviceName.c_str(),
                    plan.enableInputRequested, plan.enableInputProbed);
        } else {
            clockwork_log("[device-setup] auto-enabling %d input channels for '%s'",
                    plan.enableInputWidth, inputDeviceName.c_str());
        }
        mCurrentConfig.numInputChannels = plan.enableInputWidth;
        clockwork_set_channel_ceilings(static_cast<uint32_t>(plan.enableInputWidth),
                                 static_cast<uint32_t>(mCurrentConfig.numOutputChannels));
    }

    if (plan.coldForChannels) {
        clockwork_log("[device-setup] channel-count change detected "
                "(probedOut=%d probedIn=%d currentOut=%d currentIn=%d) "
                "— forcing cold swap so the DSP rebuilds at the new count",
                snap.probedTargetOut, snap.probedTargetIn,
                snap.currentOutputChannels, snap.currentInputChannels);
    }

    bool inputWasDropped = false;
    const bool   isCold      = plan.isCold;
    const double currentRate = snap.currentRate;
    result.type = isCold ? SwapType::Cold : SwapType::Hot;

    if (isCold) setEngineState(EngineState::Restarting, "rate-change");
    if (onSwapEvent) onSwapEvent("swap:start", result);

    // --- Pause and optionally capture state ---
    if (isCold) purge();
    mProcessor.pause();

    // Cold swaps tear down and re-init the shared-memory arena (destroy_dsp
    // / rebuild_dsp → init_memory → clockwork_lanes_reset_drains) that the
    // control pass drains — park it until the arena is back, whoever runs it.
    // Scope guard so every exit path below resumes it; the happy paths
    // resume explicitly just before audio restarts.
    struct ControlPark {
        ClockworkEngine* engine = nullptr;
        bool parked = false;
        void park(ClockworkEngine* e) { parked = e->parkControlPass(); engine = e; }
        void resumeNow() { if (engine) engine->resumeControlPass(); engine = nullptr; }
        ControlPark() = default;
        ~ControlPark()   { resumeNow(); }
        ControlPark(const ControlPark&) = delete;
        ControlPark& operator=(const ControlPark&) = delete;
    } controlPark;
    if (isCold) {
        controlPark.park(this);
        if (testControlParked) testControlParked();
    }

    // --- Stop audio ---
    stopAudioSource();

    // What the engine has said so far reaches its clients before the rings
    // go down with the arena — the user's notice before a recovery, this
    // swap's own account of itself. The pass is parked, so this thread is the
    // rings' only reader now. (A host that drains the egress itself is a
    // reader the engine cannot park, and drains its own.)
    if (isCold && controlPark.parked && !mCurrentConfig.hostDrainsEgress) drainEgressNow();

    if (isCold) destroy_dsp();

    // Snapshot for rollback: a failed setAudioDeviceSetup can leave JUCE's
    // AudioDeviceManager bound to NO device, and the rollback below would then
    // re-attach the callback to a manager with nothing in it. Symptom is
    // currentIn=''/currentOut='', prefs reading 0hz | 0buf | 0out | 0in, and
    // every DSP reply timing out because the audio thread is not ticking.
    juce::AudioDeviceManager::AudioDeviceSetup prevSetup;
    if (mDeviceManager) mDeviceManager->getAudioDeviceSetup(prevSetup);

    // --- Apply new device configuration ---
    std::string errStr;
    if (mDeviceManager) {
        // Cross-driver: move JUCE to the new AudioIODeviceType before
        // reading the setup. setCurrentAudioDeviceType internally calls
        // setAudioDeviceSetup with the new type's saved (often empty)
        // config; insertDefaultDeviceNames fills the empty field with
        // the alphabetical-first device of the type. The transient
        // open is discardable — outputDeviceName is overridden below
        // and setAudioDeviceSetup is re-run authoritatively. On
        // Windows ASIO an unplugged-but-registered driver can hang
        // here in IASIO::init().
        if (scope.crossDriver) {
            // A cross-driver change costs ~1.5 s, and essentially all of it
            // is JUCE: setCurrentAudioDeviceType closes the open device and
            // then does a hardcoded Thread::sleep(1500): "allow a moment for
            // OS devices to sort themselves out, to help avoid things like
            // DirectSound/ASIO clashes" (juce_AudioDeviceManager.cpp).
            //
            // Closing the device first would make JUCE take neither branch,
            // but leaves the manager in a state its destructor cannot
            // handle: the process faults on teardown (0xC000041D). The safe
            // lever is shortening the sleep in the vendored JUCE via a
            // FetchContent patch, which changes the duration without
            // reordering the device lifecycle.
            mDeviceManager->setCurrentAudioDeviceType(
                juce::String(scope.targetDriver), false);
        }

        juce::AudioDeviceManager::AudioDeviceSetup setup;
        mDeviceManager->getAudioDeviceSetup(setup);

        if (scope.crossDriver) {
            // Override the names left by the transient open with the resolved values. An
            // empty inputDeviceName here would re-trigger insertDefaultDeviceNames, which
            // picks the alphabetical-first input of the new type.
            setup.outputDeviceName = juce::String(scope.targetDevice);
            setup.inputDeviceName  =
                (inputDeviceName.empty() || inputDeviceName == "__none__")
                    ? juce::String()
                    : juce::String(inputDeviceName);
        }

        if (!deviceName.empty()) {
            setup.outputDeviceName = juce::String(deviceName);
        } else if (!mDeviceMode.empty()) {
            // An input-only switch re-asserts the user's chosen output — when
            // it is there. Gone (unplugged, the engine fallen back to another),
            // the input joins what is playing, and the chosen output's return
            // is the hot-plug pass's to act on.
            std::vector<std::string> outputs;
            if (auto* type = mDeviceManager->getCurrentDeviceTypeObject())
                for (auto& n : type->getDeviceNames(false)) outputs.push_back(n.toStdString());
            if (clockwork::device::deviceNameVisible(mDeviceMode, outputs))
                setup.outputDeviceName = juce::String(mDeviceMode);
        }
        // "__none__" can arrive here with numInputChannels still > 0 (the
        // exclusive-pair resolver yields a carried-over patchbay input when
        // the output moves away) — it is a disable request, never a device
        // name, so it must take the disable branch below.
        if (mCurrentConfig.numInputChannels > 0 && inputDeviceName != "__none__") {
            if (!inputDeviceName.empty()) {
                // Explicit input device requested — save for future re-enable
                setup.useDefaultInputChannels = false;
                setup.inputDeviceName = juce::String(inputDeviceName);
                mLastInputDeviceName = inputDeviceName;
                juce::BigInteger inputBits;
                inputBits.setRange(0, mCurrentConfig.numInputChannels, true);
                setup.inputChannels = inputBits;
            } else {
                // Re-enable inputs — must explicitly set the input device name.
                // useDefaultInputChannels won't auto-fill the device name when
                // numInputChansNeeded was 0 at init() time (boot with -i 0).
                setup.useDefaultInputChannels = false;
                juce::BigInteger inputBits;
                inputBits.setRange(0, mCurrentConfig.numInputChannels, true);
                setup.inputChannels = inputBits;

                if (setup.inputDeviceName.isEmpty()) {
                    if (!mLastInputDeviceName.empty()) {
                        setup.inputDeviceName = juce::String(mLastInputDeviceName);
                    } else {
                        auto* currentType = mDeviceManager->getCurrentDeviceTypeObject();
                        if (currentType) {
                            auto inputNames = currentType->getDeviceNames(true);
                            if (!inputNames.isEmpty())
                                setup.inputDeviceName = inputNames[0];
                        }
                    }
                }
            }
        } else {
            // Disable inputs — save the current input device name, then release
            if (!setup.inputDeviceName.isEmpty())
                mLastInputDeviceName = setup.inputDeviceName.toStdString();
            setup.useDefaultInputChannels = false;
            setup.inputChannels.clear();
            setup.inputDeviceName = "";
        }
        // Set output bits explicitly rather than relying on useDefaultOutputChannels.
        // JUCE derives its default from numOutputChannelsNeeded at init() time, but under
        // some swap sequences re-evaluates and reports 0 active outputs (symptom:
        // activeOut=0 on Loopback despite four channels). Setting kRequestMaxChannels
        // bits lets CoreAudio clamp to the device's real count — same as the input side.
        setup.useDefaultOutputChannels = false;
        {
            juce::BigInteger outputBits;
            outputBits.setRange(0, kRequestMaxChannels, true);
            setup.outputChannels = outputBits;
        }
        if (sampleRate > 0) setup.sampleRate = sampleRate;
        if (bufferSize > 0) setup.bufferSize = bufferSize;

        // Two devices play as one only when both pair (the driver's word —
        // CoreAudio won't aggregate a wireless device, whose codec would drop
        // the pair to 16 kHz, #3555). Otherwise the output plays alone and
        // the input is remembered for the next output that pairs.
        if (setup.outputDeviceName.isNotEmpty() && setup.inputDeviceName.isNotEmpty()
            && setup.outputDeviceName != setup.inputDeviceName) {
            if (auto* type = mDeviceManager->getCurrentDeviceTypeObject()) {
                for (const auto& name : { setup.outputDeviceName, setup.inputDeviceName }) {
                    if (type->getDeviceTraits(name).pairs) continue;
                    clockwork_log("[device-setup] '%s' plays only on its own — "
                            "clearing input (was '%s')",
                            name.toRawUTF8(), setup.inputDeviceName.toRawUTF8());
                    mLastInputDeviceName = setup.inputDeviceName.toStdString();
                    setup.inputDeviceName = "";
                    setup.inputChannels.clear();
                    inputWasDropped = true;
                    break;
                }
            }
        }

        clockwork_log("[device-setup] calling setAudioDeviceSetup: out='%s' in='%s' sr=%.0f buf=%d",
                setup.outputDeviceName.toRawUTF8(),
                setup.inputDeviceName.toRawUTF8(),
                setup.sampleRate, setup.bufferSize);
        juce::String err = mDeviceManager->setAudioDeviceSetup(setup, true);
        clockwork_log("[device-setup] setAudioDeviceSetup returned: '%s'",
                err.isEmpty() ? "OK" : err.toRawUTF8());
        if (err.isNotEmpty()) errStr = err.toStdString();

        // Input-fallback: the setup failed while an input was requested
        // (Windows mic privacy denied, exclusive-mode contention, …) —
        // retry with the input cleared and let the RETRY attribute the
        // fault: success means the input was the problem (output keeps
        // working, prefs show an empty input instead of the whole rate
        // change rolling back into a cold-swap rebuild loop); failure
        // propagates the original class of error. NOT by substring-matching
        // the device name inside JUCE's error text: that breaks silently
        // whenever the wording changes.
        if (!errStr.empty() && setup.inputDeviceName.isNotEmpty()) {
            const std::string firstError = errStr;
            const std::string failedInputName = setup.inputDeviceName.toStdString();
            const std::string pairedOutputName = setup.outputDeviceName.toStdString();
            juce::AudioDeviceManager::AudioDeviceSetup outOnly = setup;
            outOnly.inputDeviceName = juce::String();
            outOnly.useDefaultInputChannels = false;
            outOnly.inputChannels.clear();
            clockwork_log(
                    "[device-setup] input '%s' failed when paired with output '%s' "
                    "(%s) — retrying output-only",
                    failedInputName.c_str(), pairedOutputName.c_str(),
                    firstError.c_str());
            juce::String retryErr = mDeviceManager->setAudioDeviceSetup(outOnly, true);
            if (retryErr.isEmpty()) {
                setup = outOnly;
                errStr.clear();
                result.inputUnavailable = true;
                result.inputUnavailableReason = firstError;
                // Remember the (output, input) pair as known-bad so
                // sendDeviceReport hides this input from the dropdown
                // while pairedOutputName is the active output. Per-
                // output scoping: the same input can pair fine with a
                // different output (typical with WASAPI Shared vs
                // ASIO on the same hardware).
                {
                    std::lock_guard<std::mutex> lock(mUngatableInputPairsMutex);
                    mUngatableInputPairs.emplace(pairedOutputName, failedInputName);
                }
            } else {
                errStr = retryErr.toStdString();
                clockwork_log(
                        "[device-setup] output-only retry also failed: %s",
                        errStr.c_str());
            }
        }

    } else {
        // Headless: no real device to configure; use failure hook for testing.
        // Mirrors the real-device input-fallback above so the same code path
        // can be exercised by unit tests via the testSwapFailure hook.
        if (testSwapFailure) {
            const bool inputRequested = !inputDeviceName.empty();
            errStr = testSwapFailure(inputRequested);
            // Same retry-attributes-the-fault contract as the real path.
            if (!errStr.empty() && inputRequested) {
                const std::string firstError = errStr;
                std::string retryErr = testSwapFailure(false);
                if (retryErr.empty()) {
                    errStr.clear();
                    result.inputUnavailable = true;
                    result.inputUnavailableReason = firstError;
                } else {
                    errStr = retryErr;
                }
            }
        }
    }

    if (!errStr.empty()) {
        if (isCold) { rebuild_dsp(currentRate); mDspRebuilt = true; }
        // --- Restart audio (failure path) ---
        if (mDeviceManager) {
            // Restore the previous device setup. A failed setAudioDeviceSetup
            // typically leaves the manager with no bound device. If we can't
            // restore (and the default-device fallback below also fails),
            // startAudioSource() sees no current device and brings up the
            // headless driver so the engine stays responsive (the client's /done
            // syncs return) instead of silently dead with the audio thread
            // never ticking.
            clockwork_log(
                    "[device-setup] swap failed (%s), restoring previous setup: out='%s' in='%s' sr=%.0f buf=%d",
                    errStr.c_str(),
                    prevSetup.outputDeviceName.toRawUTF8(),
                    prevSetup.inputDeviceName.toRawUTF8(),
                    prevSetup.sampleRate, prevSetup.bufferSize);
            juce::String restoreErr = mDeviceManager->setAudioDeviceSetup(prevSetup, true);
            if (restoreErr.isNotEmpty()) {
                clockwork_log(
                        "[device-setup] WARNING: failed to restore previous setup: %s",
                        restoreErr.toRawUTF8());
                // Last-resort recovery: try the system default with output-only.
                // If even this fails, startAudioSource() will choose the headless
                // fallback (no current device, so Headless) and the engine
                // stays up; the next user-driven swap can take it from there.
                juce::String fallbackErr =
                    mDeviceManager->initialiseWithDefaultDevices(0, mCurrentConfig.numOutputChannels);
                if (fallbackErr.isNotEmpty()) {
                    clockwork_log(
                            "[device-setup] WARNING: default-device fallback also failed: %s, "
                            "engine will run via headless driver",
                            fallbackErr.toRawUTF8());
                } else {
                    clockwork_log(
                            "[device-setup] recovered to system default after rollback failure");
                    // Drop input — fallback is output-only. Caller can
                    // re-enable inputs explicitly afterward.
                    mCurrentConfig.numInputChannels = 0;
                    clockwork_set_channel_ceilings(
                        0, static_cast<uint32_t>(mCurrentConfig.numOutputChannels));
                }
            }
        }
        controlPark.resumeNow();
        startAudioSource();
        mProcessor.resume();
        result.error = errStr;
        if (isCold) {
            // The rollback rebuilt the guest too, and that can fail like any build.
            if (const char* why = clockwork_boot_error())
                setEngineState(EngineState::Error, why);
            else
                setEngineState(EngineState::Running, "swap-failed-rollback");
        }
        if (onSwapEvent) onSwapEvent("swap:failed", result);
        return result;
    }

    if (isCold) {
        double newRate = (sampleRate > 0) ? sampleRate : currentRate;
        if (mDeviceManager) {
            auto* newDev = mDeviceManager->getCurrentAudioDevice();
            newRate = newDev ? newDev->getCurrentSampleRate() : newRate;
        }
        mCurrentConfig.sampleRate = static_cast<int>(newRate);

        // Aim the DSP's channel ceilings at the new device. The rebuild is
        // built at whatever these say; without the update it stays at the
        // boot-time count, so audio written to a higher channel (e.g. channel
        // 2 with 4-channel Loopback) lands on an internal private channel
        // instead of on hardware. The new sample rate reaches the DSP as
        // rebuild_dsp's argument.
        if (mDeviceManager) {
            if (auto* dev = mDeviceManager->getCurrentAudioDevice()) {
                int newOut = dev->getActiveOutputChannels().countNumberOfSetBits();
                int newIn  = dev->getActiveInputChannels().countNumberOfSetBits();
                if (newOut > 0) mCurrentConfig.numOutputChannels = newOut;
                // Respect inputWasDropped: when we've dropped input because
                // the new output doesn't pair with it, keep the previously-
                // remembered input count in config but build the DSP with
                // zero inputs for this rebuild.
                if (!inputWasDropped || newIn > 0) {
                    mCurrentConfig.numInputChannels = newIn;
                }
                clockwork_set_channel_ceilings(
                    static_cast<uint32_t>(inputWasDropped ? 0 : newIn),
                    static_cast<uint32_t>(mCurrentConfig.numOutputChannels));
            }
        }

        try {
            if (testRebuildFailure) {
                std::string failMsg = testRebuildFailure();
                if (!failMsg.empty())
                    throw std::runtime_error(failMsg);
            }
            rebuild_dsp(newRate);
            mDspRebuilt = true;
        } catch (const std::exception& e) {
            clockwork_log("[engine] rebuild_dsp failed: %s — recovering with safe defaults",
                    e.what());

            double safeRate = currentRate;
            int safeBuffer = 128;
            mCurrentConfig.sampleRate = static_cast<int>(safeRate);
            mCurrentConfig.bufferSize = safeBuffer;

            try {
                rebuild_dsp(safeRate);
                mDspRebuilt = true;
                recovered = true;
                result.error = std::string("rebuild failed (") + e.what()
                             + "), recovered at safe defaults";
                result.sampleRate = safeRate;
                result.bufferSize = safeBuffer;
            } catch (const std::exception& e2) {
                clockwork_log("[engine] rebuild recovery ALSO failed: %s", e2.what());
                result.error = std::string("rebuild failed and recovery failed: ") + e2.what();
                setEngineState(EngineState::Error, "rebuild-failed");
                if (onSwapEvent) onSwapEvent("swap:failed", result);
                return result;
            }
        }
    }

    // --- Restart audio (success path) ---
    controlPark.resumeNow();
    const bool delivered = startAudioSource();
    mProcessor.resume();

    // A device that opened and started but never delivered a block is a failed
    // swap, as one that refused to open is: nothing is playing on it, whatever
    // the device manager says, and while nothing ticks the engine hears no
    // commands. Go back to the device that was playing — unless that is this
    // device at this rate (a recovery reopening it: the watchdog retries), or
    // this swap is itself the way back.
    if (!delivered && mActiveSource.load() == AudioSource::RealCallback) {
        auto* dead = mDeviceManager ? mDeviceManager->getCurrentAudioDevice() : nullptr;
        const std::string deadName = dead ? dead->getName().toStdString() : deviceName;
        const double deadRate = dead ? dead->getCurrentSampleRate() : sampleRate;
        const bool goBack = !mNoAudioRollbackInFlight && !playing.output.empty()
            && !(sameDeviceName(playing.output, deadName)
                 && std::abs(playing.rate - deadRate) < 1.0);
        result.success = false;
        // The error is what a person who picked the device reads: what they
        // heard, where the sound is now, and what to try. Detail is the log's.
        clockwork_log("[switchDevice] '%s' started but delivered no audio in 5 s%s",
                deadName.c_str(),
                goBack ? (" — going back to '" + playing.output + "'").c_str() : "");
        result.error = "No sound is coming out of " + deadName
                     + ". Try choosing it again, or pick another output.";
        if (goBack) {
            mNoAudioRollbackInFlight = true;
            auto back = switchDevice(playing.output, playing.rate, playing.buffer,
                                     false, playing.input, SwapOrigin::Internal);
            mNoAudioRollbackInFlight = false;
            if (back.success) {
                result.fellBack   = true;
                result.deviceName = back.deviceName;
                result.sampleRate = back.sampleRate;
                result.bufferSize = back.bufferSize;
                result.error = "No sound came out of " + deadName
                             + ", so audio has gone back to " + back.deviceName + ".";
            } else {
                clockwork_log("[switchDevice] going back to '%s' failed: %s",
                        playing.output.c_str(), back.error.c_str());
                result.error = "No sound is coming out of " + deadName
                             + ", and switching back to " + playing.output
                             + " didn't work. Try choosing an output again.";
            }
        }
        // A cold swap left Restarting; the way back, when it was cold too, has
        // already said Running (this is then no transition, and no second
        // /clockwork/setup).
        if (isCold) {
            if (const char* why = clockwork_boot_error())
                setEngineState(EngineState::Error, why);
            else
                setEngineState(EngineState::Running, "swap-no-audio");
        }
        if (onSwapEvent) onSwapEvent("swap:failed", result);
        clockwork_log("[switchDevice] EXIT success=0 type=%s err='%s'",
                (result.type == SwapType::Cold) ? "Cold" : "Hot", result.error.c_str());
        return result;
    }

    // On a cold swap, don't restore definitions, buffers, or module state
    // here. The client receives /clockwork/setup and handles
    // all reinitialisation — reloading definitions, clearing sample
    // caches, recreating groups/mixer/scope.  Restoring from the
    // StateCache would create duplicate state and cause distortion.

    if (mDeviceManager) {
        auto* finalDev = mDeviceManager->getCurrentAudioDevice();
        if (finalDev) {
            result.sampleRate = finalDev->getCurrentSampleRate();
            result.bufferSize = finalDev->getCurrentBufferSizeSamples();
            mCurrentConfig.sampleRate = static_cast<int>(result.sampleRate);
            mCurrentConfig.bufferSize = result.bufferSize;
            mCurrentConfig.numOutputChannels = finalDev->getActiveOutputChannels().countNumberOfSetBits();
            // Preserve the user's desired input channel count when we had to
            // drop inputs for an output that doesn't pair. Without this, a
            // detour through e.g. AirPlay would permanently erase the mic
            // setting — switching back to speakers wouldn't pair it again.
            int actualIn = finalDev->getActiveInputChannels().countNumberOfSetBits();
            if (!inputWasDropped || actualIn > 0) {
                mCurrentConfig.numInputChannels = actualIn;
            }

            juce::AudioDeviceManager::AudioDeviceSetup finalSetup;
            mDeviceManager->getAudioDeviceSetup(finalSetup);
            result.inputDeviceName = finalSetup.inputDeviceName.toStdString();

            // Report the device that actually opened, not the request:
            // JUCE keeps the requested name in its setup even when the
            // device type resolved it elsewhere, and subscribers (the GUI)
            // must never be told a fiction.
            result.deviceName = finalDev->getName().toStdString();

            clockwork_log("[device-setup] switched to %s: %s %.0fHz buf=%d %dch",
                    finalDev->getTypeName().toRawUTF8(),
                    finalDev->getName().toRawUTF8(),
                    result.sampleRate, result.bufferSize,
                    mCurrentConfig.numOutputChannels);

            // Scopes draw the window being heard, which this device's output
            // latency puts behind the writer. A ring that cannot hold that
            // window leaves every scope flat while the audio is fine — say
            // so here, where the latency is first known, rather than let it
            // read as a scope bug.
            {
                const int outLat = finalDev->getOutputLatencyInSamples();
                if (outLat > 0 && result.sampleRate > 0
                    && !shm_scope_ring_covers(static_cast<uint32_t>(outLat), result.sampleRate))
                    clockwork_log("[device-setup] WARNING: output latency %d frames (%.0f ms) "
                            "is more than the scope ring covers (%u frames): scopes "
                            "will show nothing on this device",
                            outLat, outLat * 1000.0 / result.sampleRate,
                            static_cast<unsigned>(SHM_SCOPE_RING_FRAMES));
            }

            // Remember the rate of the last non-wireless settle so a
            // future detour through AirPlay/Bluetooth doesn't leave the
            // engine stuck at the wireless receiver's negotiated rate.
            if (result.sampleRate > 0)
                if (auto* type = mDeviceManager->getCurrentDeviceTypeObject())
                    if (!type->getDeviceTraits(finalDev->getName()).wireless)
                        mPreWirelessRate = static_cast<int>(result.sampleRate);
        }
    } else {
        if (!recovered) {
            result.sampleRate = isCold ? sampleRate : currentRate;
        }
        result.bufferSize = mCurrentConfig.bufferSize;
    }
    result.success = true;
    recordSwapPreferences(deviceName, inputDeviceName, result.sampleRate, origin);
    if (isCold) {
        if (const char* why = clockwork_boot_error()) {
            // The device swap went through; the guest did not come back on
            // it. The same failure as a boot's, reported the same way.
            result.error = why;
            setEngineState(EngineState::Error, why);
            if (onSwapEvent) onSwapEvent("swap:complete", result);
        } else if (recovered) {
            setEngineState(EngineState::Running, "swap-recovered");
            if (onSwapEvent) onSwapEvent("swap:recovered", result);
        } else {
            setEngineState(EngineState::Running, "rate-change");
            if (onSwapEvent) onSwapEvent("swap:complete", result);
        }
    } else {
        if (onSwapEvent) onSwapEvent("swap:complete", result);
    }
    clockwork_log("[switchDevice] EXIT success=%d type=%s sr=%.0f buf=%d out=%d in=%d err='%s'",
            result.success ? 1 : 0,
            (result.type == SwapType::Cold) ? "Cold" : "Hot",
            result.sampleRate, result.bufferSize,
            mCurrentConfig.numOutputChannels, mCurrentConfig.numInputChannels,
            result.error.c_str());
    printDeviceList();
    return result;
}

void ClockworkEngine::teardownDeviceManager() {
    if (mDeviceManager) {
        // The sink stays to the end: a change reported while the manager goes
        // down only notes it for a pass on the lane, which finds no manager.
        mDeviceManager->removeAudioCallback(mDeviceCallback.get());
        mDeviceManager->closeAudioDevice();
        mDeviceManager.reset();
    }
}

void ClockworkEngine::restoreBootDriver() {
    if (mCurrentConfig.deviceManagerFactory || !mDeviceManager) return;
#if defined(__linux__) && defined(CLOCKWORK_PIPEWIRE)
    // As at boot: PipeWire registered, and preferred unless boot honoured
    // another driver (--audio-driver). The scan must land before the
    // default-device open that follows, or the fresh manager cannot see
    // PipeWire's devices at all.
    registerPipeWireDriver(*mDeviceManager);
    if (mBootDriver.empty() || mBootDriver == "PipeWire") {
        preferPipeWireDriverIfAvailable(*mDeviceManager);
    } else {
        mDeviceManager->setCurrentAudioDeviceType(juce::String(mBootDriver), true);
    }
#elif defined(_WIN32)
    // A fresh manager starts on WASAPI; boot's driver, or DirectSound when
    // boot named none.
    const std::string driver = !mBootDriver.empty() ? mBootDriver : std::string("DirectSound");
    for (auto* t : mDeviceManager->getAvailableDeviceTypes()) {
        if (t->getTypeName().toStdString() == driver) {
            mDeviceManager->setCurrentAudioDeviceType(juce::String(driver), true);
            break;
        }
    }
#endif
}

std::string ClockworkEngine::recreateDeviceManager() {
    // Release the stale, hibernate-killed CoreAudio/HAL client completely, then
    // build a fresh manager. A reopen keeps this same dead connection; only a
    // new manager gets a new IsolatedCoreAudioClient + AudioObjectIDs that
    // coreaudiod will actually drive with a live IO thread. Runs on the device task lane
    // (the recovery worker — see requestAudioRecovery), holding the swap gate:
    // the thread that made the old one, on every platform.
    teardownDeviceManager();
    mDeviceManager = makeDeviceManager();
    restoreBootDriver();

    // Open the device via the shared system-default reinit: it preserves the
    // session sample rate.
    const std::string err = reinitialiseWithDefaultsPreservingConfig();
    if (!err.empty()) return err;
    if (!mDeviceManager->getCurrentAudioDevice())
        return "recreate: opened no device";

    // Device is open but no callback/listener is attached yet — the caller
    // (recoverAudio) promotes to it via startAudioSource() once it's confirmed
    // this fresh connection actually ticks. Recovery falls back to the default
    // output with no inputs (a pinned device / mic is not restored), but the
    // cache cleared above keeps the rest of the engine consistent with that.
    return {};
}

SwapResult ClockworkEngine::reopenCurrentDevice(double sampleRate) {
    SwapResult result;

    if (!mDeviceManager) {
        result.error = "no audio device manager (headless)";
        return result;
    }

    // Always delegate to switchDevice with forceCold=true, so the device opens
    // afresh and picks up any channel-count change; an open input carries
    // over from the setup. Aim at the pinned device, then the current one.
    std::string outName;
    // After recovery's recreate the "current" device is whatever the
    // system-default reinit opened — NOT necessarily the user's pinned
    // choice (#3555 follow-on: Testy wedged, recovery reopened the
    // wireless default and stayed there). Aim at the pin while it's
    // still attached; a pin that's genuinely gone falls through to
    // the current device.
    if (!mPreferredOutputDevice.empty()) {
        std::vector<std::string> outputNames;
        for (auto& d : listDevices(false))
            if (d.maxOutputChannels > 0) outputNames.push_back(d.name);
        outName = clockwork::device::selectRecoveryTarget(
            mPreferredOutputDevice, outputNames);
        if (!outName.empty()
            && mDeviceManager->getCurrentAudioDevice()
            && outName != mDeviceManager->getCurrentAudioDevice()
                              ->getName().toStdString()) {
            clockwork_log("[reopen] retargeting pinned device '%s' "
                    "(current is '%s')", outName.c_str(),
                    mDeviceManager->getCurrentAudioDevice()
                        ->getName().toRawUTF8());
        }
    }
    if (outName.empty()) {
        if (auto* dev = mDeviceManager->getCurrentAudioDevice())
            outName = dev->getName().toStdString();
    }
    if (outName.empty()) {
        result.error = "no current output device to reopen";
        return result;
    }
    // An explicit rate sits at the top of planSwap's precedence, above
    // "keep the session rate when the target advertises it" — which is what
    // a rate-skew recovery must get past: the device advertises the rate it
    // cannot deliver. Snap to what it advertises rather than asking for a
    // raw measurement (44,03x Hz), and log the list, because a driver can
    // back-fill the nominal rate into it whether or not the device claimed
    // it (JUCE 7's CoreAudio does).
    if (sampleRate > 0) {
        const auto rates = probeDeviceSampleRates(outName, false);
        std::string advertised;
        for (double r : rates) advertised += (advertised.empty() ? "" : ", ")
                                          + std::to_string(static_cast<int>(r));
        const double snapped = clockwork::device::resolveTargetRate(rates, sampleRate);
        if (snapped != 0 && snapped != sampleRate) {
            clockwork_log("[reopen] requested %.0f Hz snaps to %.0f Hz (device advertises: %s)",
                    sampleRate, snapped, advertised.empty() ? "nothing" : advertised.c_str());
            sampleRate = snapped;
        } else {
            clockwork_log("[reopen] requesting %.0f Hz (device advertises: %s)",
                    sampleRate, advertised.empty() ? "nothing" : advertised.c_str());
        }
    }
    clockwork_log("[reopen] forceCold switch out='%s' mode='%s' rate=%.0f",
            outName.c_str(),
            mDeviceMode.empty() ? "system" : mDeviceMode.c_str(), sampleRate);
    return switchDevice(outName, sampleRate, 0, /*forceCold=*/true, {},
                        SwapOrigin::Internal);
}

// --- Input channel management ---

SwapResult ClockworkEngine::enableInputChannels(int numChannels) {
    // Resolve WHICH input device before the channel width, so the width can be
    // clamped against that device's probed capacity. When enabling, prefer an
    // explicit name over switchDevice's "first in JUCE's input list" fallback — that
    // can pick a virtual device over the real hardware mic and produce silent zeros.
    // Order: saved mLastInputDeviceName, the system default input, then empty.
    std::string inputName;
    const char* inputSource = "disable";
    if (numChannels != 0) {
        inputName = mLastInputDeviceName;
        inputSource = inputName.empty() ? "none" : "mLastInputDeviceName";
    }
    if (numChannels != 0 && inputName.empty() && mDeviceManager) {
        if (auto* type = mDeviceManager->getCurrentDeviceTypeObject()) {
            inputName = type->getSystemDefaultDeviceName(true).toStdString();
            if (!inputName.empty()) inputSource = "the system default input";
        }
    }

    // -1 means "re-enable inputs": resolved against the boot -i flag and
    // clamped to the device's probed input capacity. The clamp is what
    // keeps the auto-max request off WASAPI, which rejects
    // setAudioDeviceSetup outright when asked for more inputs than exist
    // (CoreAudio silently clamps instead, which is how the unclamped
    // path went unnoticed on mac).
    if (numChannels != 0) {
        int probed = -1;
        if (mDeviceManager && !inputName.empty())
            probed = probeDeviceChannelCount(inputName, true,
                                             probeDriverTypeName());
        numChannels = clockwork::device::resolveInputWidth(
            numChannels, mBootInputChannels, probed);
    }

    if (numChannels == mCurrentConfig.numInputChannels) {
        SwapResult result;
        result.success = true;
        result.type = SwapType::Hot;  // no-op
        result.sampleRate = mCurrentConfig.sampleRate;
        result.bufferSize = mCurrentConfig.bufferSize;
        return result;
    }

    // Refuse "disable inputs" on ASIO. ASIO drivers are full-duplex
    // single-device by spec — one stream owns both directions.
    // Reconfiguring with input=0 while keeping output crashes real
    // drivers (MOTU Pro Audio observed). Output-only is served by
    // switching driver to Windows Audio / DirectSound.
    if (numChannels == 0 && mDeviceManager) {
        if (auto* dev = mDeviceManager->getCurrentAudioDevice()) {
            if (dev->getTypeName().toStdString() == "ASIO") {
                SwapResult result;
                result.error = "Cannot disable input on ASIO — ASIO drivers "
                               "are full-duplex by spec. Switch driver to "
                               "Windows Audio / DirectSound to run output-only.";
                clockwork_log("[enable-inputs] refusing disable on ASIO "
                        "(would crash the driver)");
                return result;
            }
        }
    }

    int oldNumInputChannels = mCurrentConfig.numInputChannels;

    // Aim the ceiling before the cold swap, so the rebuild is built at the
    // requested width.
    mCurrentConfig.numInputChannels = numChannels;
    clockwork_set_channel_ceilings(static_cast<uint32_t>(numChannels),
                             static_cast<uint32_t>(mCurrentConfig.numOutputChannels));

    clockwork_log("[enable-inputs] resolved input='%s' (source=%s) channels=%d",
            inputName.c_str(), inputSource, numChannels);
    // For disable, pass __none__ sentinel so switchDevice takes the disable
    // path (clears setup.inputDeviceName + inputChannels) instead of trying
    // to treat an empty string as "re-enable with last known input".
    auto result = switchDevice("", 0, 0, true, numChannels > 0 ? inputName : std::string("__none__"));

    if (!result.success) {
        mCurrentConfig.numInputChannels = oldNumInputChannels;
        clockwork_set_channel_ceilings(static_cast<uint32_t>(oldNumInputChannels),
                                 static_cast<uint32_t>(mCurrentConfig.numOutputChannels));
    }

    return result;
}

// --- Audio driver management ---

std::vector<std::string> ClockworkEngine::listDrivers() const {
    // Serialise mDeviceManager access against device mutations / recovery's
    // recreate (see mSwapMutex). Recursive: mutation paths call this under the
    // gate. Lock order is always gate-then-mListDriversMutex.
    std::lock_guard<std::recursive_mutex> gate(mSwapMutex);
    if (!mDeviceManager) return {};

    // Cache hit — skip the rescan. sendDeviceReport() is called many
    // times during boot (notify registration, first info push, device
    // change settles, input pairing) and once per user-initiated
    // switch; re-running scanForDevices() on every call is wasteful
    // and on Linux without a JACK server produces libjack connect()
    // stderr spam. Short TTL so a freshly-started jackd shows up.
    {
        std::lock_guard<std::mutex> lk(mListDriversMutex);
        auto now = std::chrono::steady_clock::now();
        if (!mCachedDrivers.empty()
            && (now - mCachedDriversAt) < std::chrono::seconds(3))
            return mCachedDrivers;
    }

    std::vector<std::string> result;
    auto& types = mDeviceManager->getAvailableDeviceTypes();
    for (auto* type : types) {
        // JUCE registers every compiled-in device type regardless of
        // runtime availability (e.g. JackAudioIODeviceType appears in
        // getAvailableDeviceTypes() whenever JUCE_JACK=1, even if no
        // jackd / pipewire-jack server is running). Offering a driver
        // the user can't switch to is worse than hiding it, so rescan
        // and only advertise drivers that enumerate at least one output
        // device right now. ALSA / CoreAudio / WASAPI always have the
        // hardware on their side so they pass; JACK / ASIO only show
        // when a server or driver is actually reachable.
        type->scanForDevices();
        auto names = type->getDeviceNames(false);
        if (names.isEmpty()) continue;

        // Same rule as listDevices: an ASIO driver the loader rejects can
        // never be opened, so it doesn't count towards the type being
        // reachable. When every installed ASIO driver is unloadable (e.g.
        // x64-only drivers on an ARM64 host) the whole type is hidden
        // rather than offering a driver with nothing selectable under it.
        const std::string typeName = type->getTypeName().toStdString();
        if (typeName == "ASIO") {
            const auto unloadable = clockwork::device::unloadableAsioDrivers();
            bool anyUsable = false;
            for (const auto& n : names) {
                if (!unloadable.count(n.toStdString())) { anyUsable = true; break; }
            }
            if (!anyUsable) continue;
        }

        result.push_back(typeName);
    }

    {
        std::lock_guard<std::mutex> lk(mListDriversMutex);
        mCachedDrivers = result;
        mCachedDriversAt = std::chrono::steady_clock::now();
    }
    return result;
}

std::string ClockworkEngine::currentDriver() const {
    // Serialise mDeviceManager access against device mutations / recovery's
    // recreate (see mSwapMutex). Recursive: mutation paths call this under gate.
    std::lock_guard<std::recursive_mutex> gate(mSwapMutex);
    if (!mDeviceManager) return "";
    if (auto* dev = mDeviceManager->getCurrentAudioDevice())
        return dev->getTypeName().toStdString();
    // No device open: fall back to the active type so the GUI's
    // driver dropdown stays on the right entry instead of going
    // blank. The type can be set without a device during cross-
    // driver swaps and after open failures.
    return mDeviceManager->getCurrentAudioDeviceType().toStdString();
}

std::string ClockworkEngine::intendedDriver() const {
    return mIntendedDriver;
}

bool ClockworkEngine::isInputKnownBadFor(const std::string& outputName,
                                          const std::string& inputName) const {
    if (outputName.empty() || inputName.empty()) return false;
    std::lock_guard<std::mutex> lock(mUngatableInputPairsMutex);
    return mUngatableInputPairs.count({outputName, inputName}) > 0;
}

SwapResult ClockworkEngine::switchDriver(const std::string& driverName) {
    SwapResult result;
    result.deviceName = driverName;

    // ── Real-driver path ────────────────────────────────────────────────
    // Always carry an explicit device name into setAudioDeviceSetup —
    // never let JUCE's insertDefaultDeviceNames pick alphabetical-first
    // for the new type. On Windows ASIO that's the registered-but-
    // unplugged-driver hang hazard (IASIO::init() can block in COM);
    // on every driver it's a quiet UX surprise (the device dropdown
    // shows one thing, the audio is routed through another).
    if (mDeviceManager) {
        // (a) Saved per-driver preference → delegate to switchDevice
        //     with the remembered name. switchDevice's cross-driver
        //     path moves JUCE atomically.
        auto pref = mPreferredDeviceByDriver.find(driverName);
        if (pref != mPreferredDeviceByDriver.end() && !pref->second.empty()) {
            clockwork_log("[device-setup] switchDriver('%s'): delegating to "
                    "switchDevice('%s') (saved preference)",
                    driverName.c_str(), pref->second.c_str());
            mIntendedDriver = driverName;
            return switchDevice(pref->second);
        }

        // (b) No saved preference, non-ASIO driver with at least one
        //     device visible → pick the driver's system-default device
        //     and delegate. Keeps the transition atomic (one cold swap)
        //     and avoids leaving the GUI in a "driver=X but no device"
        //     limbo for drivers that have a sensible default.
        if (driverName != "ASIO") {
            auto& types = mDeviceManager->getAvailableDeviceTypes();
            for (auto* type : types) {
                if (type->getTypeName().toStdString() != driverName) continue;
                type->scanForDevices();
                auto names = type->getDeviceNames(false);
                if (names.isEmpty()) break;  // fall through to (c)
                int idx = type->getDefaultDeviceIndex(false);
                if (idx < 0 || idx >= names.size()) idx = 0;
                std::string defaultName = names[idx].toStdString();
                clockwork_log("[device-setup] switchDriver('%s'): no saved pref, "
                        "auto-selecting default '%s'",
                        driverName.c_str(), defaultName.c_str());
                mIntendedDriver = driverName;
                return switchDevice(defaultName);
            }
        }

        // (c) ASIO with no saved preference, or any driver with no
        //     visible devices → don't touch JUCE. setCurrentAudio-
        //     DeviceType + initialiseWithDefaultDevices stops the
        //     audio callback, so the DSP stops ticking, and anything
        //     awaiting a reply from it before a follow-up switchDevice
        //     would wait forever. Record intent and wait for the
        //     caller's explicit device pick.
        mIntendedDriver                 = driverName;
        result.success                  = true;
        result.requiresDeviceSelection  = true;
        clockwork_log("[device-setup] switchDriver('%s'): intent recorded, "
                "no device opened — caller must follow with switchDevice",
                driverName.c_str());
        if (onSwapEvent) onSwapEvent("swap:complete", result);
        return result;
    }

    // ── Headless mode ───────────────────────────────────────────────────
    // No real audio driver to switch. If the test hook is set, simulate
    // the rate the new driver's default device would report.
    if (!testDriverSwitchRate) {
        result.error = "no audio device in headless mode";
        return result;
    }
    double newRate = testDriverSwitchRate();
    if (static_cast<int>(newRate) == mCurrentConfig.sampleRate) {
        result.success    = true;
        result.type       = SwapType::Hot;
        result.sampleRate = newRate;
        result.bufferSize = mCurrentConfig.bufferSize;
        if (onSwapEvent) onSwapEvent("swap:start", result);
        if (onSwapEvent) onSwapEvent("swap:complete", result);
        return result;
    }
    return switchDevice("", newRate);
}

// --- Device change detection ---

void ClockworkEngine::devicesChanged(unsigned changes) {
    // Any thread, at the OS's moment — a driver's own audio thread among
    // them: a note and a wake, no allocation. A pass already due has yet to
    // look, so it will see this change too.
    if (!(mDeviceChanges.fetch_or(changes | kPassQueued) & kPassQueued))
        mDeviceLane.wake();
}

void ClockworkEngine::reconcileDevices() {
    // Taken before looking: a change from here on queues another pass.
    const unsigned changes = mDeviceChanges.exchange(0) & ~kPassQueued;
    // Booting: init runs a pass once the engine is running, and the changes
    // wait for it. The flag is set before running is looked at again, and
    // init sets running before it reads the flag, so at least one of them
    // sees the other — both, at worst, and the second pass finds nothing to
    // do.
    if (!mRunning.load()) {
        mDeviceChanges.fetch_or(changes);
        mReconcileAfterBoot.store(true);
        if (!mRunning.load()) return;
    }

    // Held from looking through acting, so what is decided is what is done:
    // the swap and the reopen below take the gate again (it is recursive)
    // rather than racing a reader for it. Readers hold it only briefly; a
    // mutation outside the lane holding it for 3 s is a fault, and this
    // change must not be lost to it — look again once the lane comes round.
    std::unique_lock<std::recursive_mutex> gate;
    if (!tryAcquireSwapGate(gate, 30, 100)) {
        clockwork_log("[devices] changed, gate busy for 3 s — looking again");
        devicesChanged(changes);
        return;
    }
    if (!mDeviceManager) return;

    // MIDI hot-plug is the MIDI subsystem's own (rust/clockwork-midi's
    // watcher); this is audio devices only. The lists are rescanned (they are
    // this thread's) when they changed, or the default moved: a device just
    // plugged in can become the default before the OS says the list changed,
    // and a default the lists don't have yet would be refused. Not for a
    // change to the open device alone.
    auto devices = listDevices((changes & (kListChanged | kDefaultChanged)) != 0);
    auto* dev = mDeviceManager->getCurrentAudioDevice();

    const std::string currentOutput = dev ? dev->getName().toStdString() : std::string();
    const int currentActiveIn =
        dev ? dev->getActiveInputChannels().countNumberOfSetBits() : 0;
    std::vector<std::string> visibleNames;
    visibleNames.reserve(devices.size());
    for (auto& d : devices) visibleNames.push_back(d.name);

    // -H's words that have not found a device yet: matched as -H matches them,
    // against the outputs there now. A device they find is the preferred one
    // from here on, by its name.
    if (mPreferredOutputIsWords) {
        std::vector<std::pair<std::string, std::string>> outputs;
        for (auto& d : devices)
            if (d.maxOutputChannels > 0) outputs.emplace_back(d.typeName, d.name);
        const std::string matched = clockwork::device::resolveBootHardwareMatch(
            mPreferredOutputDevice, dev ? dev->getTypeName().toStdString() : std::string(), outputs);
        if (const auto at = matched.find(" : "); at != std::string::npos) {
            mPreferredOutputDevice  = matched.substr(at + 3);
            mPreferredOutputIsWords = false;
            clockwork_log("[hotplug] -H's '%s' found '%s'", matched.c_str(), mPreferredOutputDevice.c_str());
        }
    }

    const auto decision = clockwork::device::decideHotplugAction(
        mPreferredOutputDevice, mPreferredInputDevice,
        currentOutput, currentActiveIn, visibleNames);

    // The list as last reported, so a change that leaves it as it was isn't
    // re-sent. The field/record separators are control chars that can't
    // appear in a name.
    std::string fingerprint;
    for (auto& d : devices) {
        fingerprint += d.name;
        fingerprint += '\x1f' + std::to_string(d.maxOutputChannels);
        fingerprint += '\x1f' + std::to_string(d.maxInputChannels);
        fingerprint += '\x1f' + d.kind + '\x1e';
    }
    const bool listChanged = fingerprint != mLastAudioDeviceFingerprint;
    mLastAudioDeviceFingerprint = fingerprint;

    // A swap or a reopen reports for itself. An output the user chose comes
    // first; only without one does the system default decide.
    if (decision.reopen) {
        recoverLostDevice(currentOutput, "has gone from the list");
    } else if (decision.switchOutput) {
        clockwork_log("[hotplug] preferred output '%s' is here — switching to it "
                "(preferred input='%s')", decision.outputName.c_str(),
                decision.inputName.c_str());
        switchDevice(decision.outputName, 0, 0, false, decision.inputName,
                     SwapOrigin::Internal);
    } else {
        bool acted = false;
        if (decision.switchInput) {
            clockwork_log("[hotplug] preferred input '%s' is here — adding it",
                    decision.inputName.c_str());
            switchDevice("", 0, 0, false, decision.inputName, SwapOrigin::Internal);
            acted = true;
        }
        // A follow opens another device; only without one is the device
        // played on looked at as it stands.
        if (followDefaultOutput() || reconcileOpenDevice()) acted = true;
        if (!acted && listChanged) printDeviceList();
    }
}

void ClockworkEngine::recoverLostDevice(const std::string& name, const char* how) {
    // A recovery already claimed is queued behind this pass and will find
    // the device gone itself.
    bool expected = false;
    if (mReopenInProgress.compare_exchange_strong(expected, true)) {
        clockwork_log("[devices] '%s' %s — reopening", name.c_str(), how);
        recoverAudio({});
    }
}

bool ClockworkEngine::reconcileOpenDevice() {
    // A queued recovery opens a device anew anyway.
    if (!mDeviceManager || mReopenInProgress.load()) return false;
    auto* dev = mDeviceManager->getCurrentAudioDevice();
    if (!dev) return false;

    const std::string name = dev->getName().toStdString();
    const auto live = dev->readLiveState();
    if (!live.alive) {
        recoverLostDevice(name, "has stopped where it stands");
        return true;
    }
    // Its driver says it cannot carry on as opened (a stream a format change
    // invalidated, a reset the driver asked for): the same device, opened
    // again, at a rate it offers now.
    if (live.mustReopen) {
        clockwork_log("[devices] '%s' cannot carry on as opened — opening it again",
                name.c_str());
        reopenCurrentDevice(0);
        return true;
    }

    // Its channels as opened, against its driver's now (-1: can't say).
    const int outs = dev->getOutputChannelNames().size();
    const int ins  = dev->getInputChannelNames().size();
    const bool channelsChanged =
        (live.numOutputChannels >= 0 && live.numOutputChannels != outs)
        || (live.numInputChannels >= 0 && live.numInputChannels != ins);

    // Its rate is not taken from the report: CoreAudio reports a rate the
    // engine has itself just set only some time later, and a report read
    // in between would switch the engine back to the rate it left. A rate
    // another app sets is caught by the watchdog, which measures the rate
    // the device delivers (rate skew) and reopens at it. Said here, for the
    // log.
    const int liveRate = static_cast<int>(live.sampleRate);
    if (liveRate > 0 && liveRate != mCurrentConfig.sampleRate)
        clockwork_log("[devices] '%s' reports %d Hz; the engine runs at %d Hz",
                name.c_str(), liveRate, mCurrentConfig.sampleRate);

    if (!channelsChanged) return false;
    clockwork_log("[devices] '%s' now has %d out / %d in (opened with %d / %d) "
            "— opening it again", name.c_str(), live.numOutputChannels,
            live.numInputChannels, outs, ins);
    reopenCurrentDevice(0);
    return true;
}

bool ClockworkEngine::followDefaultOutput() {
    // A queued recovery opens the default itself.
    if (!mDeviceManager || mReopenInProgress.load()) return false;

    const SystemDefaultOutput def = systemDefaultOutput();
    auto* dev = mDeviceManager->getCurrentAudioDevice();
    const std::string currentOutput = dev ? dev->getName().toStdString() : std::string();
    const bool moved = def.name != mDefaultSeen;
    mDefaultSeen = def.name;
    if (def.name.empty() || def.name == currentOutput) {
        mUnopenableDefault.clear();
        return false;
    }
    if (def.name != mUnopenableDefault) mUnopenableDefault.clear();

    // Not our own aggregates, not virtual devices (an app may spawn one and
    // macOS make it the default), and never away from a device the user
    // chose: following would turn a hardware event into a switch they never
    // asked for. A device mode with no pin is boot's stand-in for a wireless
    // default, which is not followed either.
    const std::string& chosen =
        mPreferredOutputDevice.empty() ? mDeviceMode : mPreferredOutputDevice;
    if (!clockwork::device::shouldFollowDefaultOutputChange(
            def.name, currentOutput, def.isVirtual, chosen)) {
        if (moved)
            clockwork_log("[device-setup] system default is '%s' (virtual=%d, "
                    "pinned='%s'); not following (staying on '%s')",
                    def.name.c_str(), def.isVirtual ? 1 : 0, chosen.c_str(),
                    currentOutput.c_str());
        return false;
    }
    if (def.name == mUnopenableDefault) return false;   // tried since it moved here

    clockwork_log("[device-setup] system default output is now '%s' — following "
            "(was '%s')", def.name.c_str(), currentOutput.c_str());
    const std::string err = openSystemDefault();
    if (!err.empty()) {
        mUnopenableDefault = def.name;
        clockwork_log("[device-setup] could not open the default '%s' (%s) — "
                "trying again when the default moves", def.name.c_str(), err.c_str());
    }
    return true;
}

ClockworkEngine::SystemDefaultOutput ClockworkEngine::systemDefaultOutput() {
    // The current driver's word for it, and for what kind of device it is
    // (juce::AudioIODeviceType): CoreAudio asks the HAL, WASAPI lists the
    // default first, a driver with no default (ASIO) names none. Under the
    // gate, as every mDeviceManager read is: recovery's recreate resets it.
    SystemDefaultOutput out;
    std::lock_guard<std::recursive_mutex> gate(mSwapMutex);
    if (!mDeviceManager) return out;
    if (auto* type = mDeviceManager->getCurrentDeviceTypeObject()) {
        const juce::String name = type->getSystemDefaultDeviceName(false);
        if (name.isNotEmpty()) {
            const auto traits = type->getDeviceTraits(name);
            out.name      = name.toStdString();
            out.wireless  = traits.wireless;
            out.isVirtual = traits.isVirtual;
        }
    }
    return out;
}

std::string ClockworkEngine::openSystemDefault() {
    if (!mDeviceManager) return {};
    // The default as its driver names it. A wired one is opened by that name,
    // like any device the engine switches to: switchDevice keeps the input,
    // rebuilds when the rate differs, and goes back when it won't open. The
    // driver opens a wireless one itself (AirPlay, Bluetooth): CoreAudio's
    // default route reaches AirPlay, which opening by name never did
    // reliably. So it does when it names none (ASIO), by way of a driver that
    // has one. The driver's answer decides, on every platform: Linux and
    // Windows used to take the driver's way for every default, and no Mac run
    // went that way (test_no_audio_device.cpp).
    const SystemDefaultOutput def = systemDefaultOutput();
    if (!def.name.empty() && !def.wireless) {
        std::string inputName;
        if (mCurrentConfig.numInputChannels > 0)
            inputName = mDeviceManager->getAudioDeviceSetup().inputDeviceName.toStdString();
        // Internal: following the default is not choosing it. A user-origin
        // swap would pin the default it landed on, and every later move of
        // the default would then be refused.
        const auto result = switchDevice(def.name, 0, 0, false, inputName,
                                         SwapOrigin::Internal);
        return result.success ? std::string() : result.error;
    }
    if (!def.name.empty())
        clockwork_log("[device-setup] system default '%s' is wireless; its driver "
                "opens it", def.name.c_str());
    return openDriverDefault();
}

std::string ClockworkEngine::openDriverDefault() {
    // The init and what follows it are one mutation: under the gate
    // throughout, so no other swap drives the manager in between (two at once
    // wedged DirectSound's callback thread in a cursor-poll spin).
    std::unique_lock<std::recursive_mutex> swapGate;
    if (!tryAcquireSwapGate(swapGate, 30, 100)) {
        clockwork_log("[device-setup] system mode init refused: "
                "swap already in progress");
        return "swap already in progress";
    }

    // What it plays on now, to go back to: the init stops it to try the
    // default.
    const bool wasOpen = mDeviceManager->getCurrentAudioDevice() != nullptr;
    const auto wasSetup = mDeviceManager->getAudioDeviceSetup();
    const juce::String wasDriver = mDeviceManager->getCurrentAudioDeviceType();

    auto err = reinitialiseWithDefaultsPreservingConfig();
    if (!err.empty()) {
        clockwork_log("[device-setup] system mode init failed: %s",
                err.c_str());
        // Back on what it played on, as switchDevice goes back when a device
        // won't open: the device, its rate and buffer, and its driver.
        if (wasOpen) {
            if (mDeviceManager->getCurrentAudioDeviceType() != wasDriver)
                mDeviceManager->setCurrentAudioDeviceType(wasDriver, true);
            const auto back = mDeviceManager->setAudioDeviceSetup(wasSetup, true);
            if (back.isEmpty())
                clockwork_log("[device-setup] back on '%s'",
                        wasSetup.outputDeviceName.toRawUTF8());
            else
                clockwork_log("[device-setup] could not go back to '%s': %s",
                        wasSetup.outputDeviceName.toRawUTF8(), back.toRawUTF8());
        }
        // The engine plays on what is open now: that device, or none, and then
        // it waits for one (waitingForAudioDevice).
        stopAudioSource();
        startAudioSource();
        return err;
    }

    std::string newDevName;
    double newRate = 0.0;
    if (auto* dev = mDeviceManager->getCurrentAudioDevice()) {
        mCurrentConfig.numOutputChannels =
            dev->getActiveOutputChannels().countNumberOfSetBits();
        mCurrentConfig.numInputChannels =
            dev->getActiveInputChannels().countNumberOfSetBits();
        newRate = dev->getCurrentSampleRate();
        newDevName = dev->getName().toStdString();
    }

    // The engine plays on what the init opened: at its rate, and when it was
    // waiting for a device too, where nothing else would attach it.
    if (newRate > 0 && static_cast<int>(newRate) != mCurrentConfig.sampleRate) {
        clockwork_log(
                "[device-setup] system default has different rate "
                "(%d -> %.0f Hz) — performing cold swap",
                mCurrentConfig.sampleRate, newRate);
        // Force cold even though JUCE is already at newRate (we
        // just opened it via reinitialiseWithDefaultsPreservingConfig).
        // Without forceCold, switchDevice sees currentRate ==
        // sampleRate and skips the rebuild, leaving the DSP
        // running at the old rate while JUCE delivers samples at
        // the new rate — mismatch, pitched-down audio. Internal, as
        // in openSystemDefault: the default it opened is not the user's pick.
        switchDevice(newDevName, newRate, 0, /*forceCold=*/true, "",
                     SwapOrigin::Internal);
    } else if (mActiveSource.load() == AudioSource::None) {
        clockwork_log("[device-setup] playing on '%s'", newDevName.c_str());
        startAudioSource();
    }
    printDeviceList();
    return {};
}

std::string ClockworkEngine::setDeviceMode(const std::string& mode) {
    std::string previousMode = mDeviceMode;

    if (mode == "system" || mode.empty()) {
        mDeviceMode.clear();
        // Entering system mode means "follow the system default" — the user
        // has opted out of sticking to a specific hardware device, so drop
        // the hot-plug preference that would otherwise pull them back.
        mPreferredOutputDevice.clear();
    } else {
        mDeviceMode = mode;
        mPreferredOutputDevice = mode;
    }
    mPreferredOutputIsWords = false;

    if (!mRunning.load()) return "";

    if (mDeviceMode.empty()) {
        // System mode: the default output, keeping the input.
        clockwork_log("[device-setup] switching to system default");
        return openSystemDefault();
    } else {
        // Manual mode: switch to the named device. setDeviceMode is called by clients
        // that cannot handle a cold swap, so pre-check whether the target supports the
        // current sample rate and reject if not. Callers that tolerate cold swaps should
        // use switchDevice() directly.
        if (mDeviceManager) {
            auto* curDev = mDeviceManager->getCurrentAudioDevice();
            double curRate = curDev ? curDev->getCurrentSampleRate() : 0.0;
            if (curRate > 0) {
                auto rates = probeDeviceSampleRates(mDeviceMode, false);
                bool rateOk = false;
                for (auto r : rates)
                    if (static_cast<int>(r) == static_cast<int>(curRate))
                        rateOk = true;
                if (!rateOk) {
                    clockwork_log(
                        "[device-setup] rejecting mode switch to %s: "
                        "current rate %.0f not supported — restart required",
                        mDeviceMode.c_str(), curRate);
                    mDeviceMode = previousMode;
                    printDeviceList();
                    return "device requires different sample rate — restart required";
                }
            }
        }

        auto result = switchDevice(mDeviceMode);
        if (!result.success) {
            mDeviceMode = previousMode;  // revert mode on failure
            printDeviceList();
            return result.error;
        }
    }

    printDeviceList();
    return "";
}

void ClockworkEngine::printDeviceList() {
    if (!mDeviceManager) return;

    // Skip rescan — calling scanForDevices() right after a device switch
    // can close the just-opened CoreAudio device. The switch path already
    // rescanned when needed. This path only reports state.
    auto devices = listDevices(false);
    auto current = currentDevice();

    clockwork_log("[audio-devices-start]");
    for (auto& dev : devices) {
        clockwork_log("[audio-device-entry] %s|%s|%d|%d|%s",
                dev.name.c_str(), dev.typeName.c_str(),
                dev.maxOutputChannels, dev.maxInputChannels,
                dev.kind.empty() ? "?" : dev.kind.c_str());
    }
    clockwork_log("[audio-device-current] %s|%s|%.0f|%d|%d|%d",
            current.name.c_str(), current.typeName.c_str(),
            current.activeSampleRate, current.activeBufferSize,
            current.activeOutputChannels, current.activeInputChannels);
    clockwork_log("[audio-device-mode] %s",
            mDeviceMode.empty() ? "system" : mDeviceMode.c_str());
    clockwork_log("[audio-devices-end]");

    sendDeviceReport();
}

// --- Recording ---




