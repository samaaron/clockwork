// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * DeviceInfo.h — Transport-neutral POD structs for device management
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct DeviceInfo {
    std::string name;
    std::string typeName;                    // "Windows Audio", "ASIO", "CoreAudio", "ALSA"
    std::vector<double> availableSampleRates;
    std::vector<int>    availableBufferSizes;
    int  maxOutputChannels = 0;
    int  maxInputChannels  = 0;
    // Whether each channel count came from an actual device probe (or an
    // authoritative CoreAudio query) rather than the 2-out/1-in placeholder
    // used when probing is skipped (device currently open, aggregate live).
    // Placeholder counts are fine for populating a device list but must not
    // feed decisions like hot-vs-cold swap or input re-enable width.
    bool outChannelsProbed = false;
    bool inChannelsProbed  = false;

    // What its driver says it is (juce::AudioIODeviceType::DeviceTraits):
    // a wireless link negotiates its own rate and buffer (Bluetooth, AirPlay
    // — codecs unsuited to low-latency work: HFP's 16 kHz mono, AirPlay's
    // buffering); a virtual device is made in software (Loopback,
    // BlackHole); an aggregate-class one is made of other devices; one that
    // pairs plays as one with a different device, output on one and input
    // on the other. `kind` is the driver's own word for how it is
    // connected, for logs.
    bool wireless       = false;
    bool isVirtual      = false;
    bool aggregateClass = false;
    bool pairs          = true;
    std::string kind;

    // Suitable for input: not wireless (it forces a low-quality codec).
    bool isSuitableForInput() const { return !wireless; }

    // Hide platform-specific clutter from the GUI dropdown list. On macOS
    // the wireless / virtual predicates above cover everything. On Linux
    // JUCE's ALSA backend exposes a raft of redundant PCM nodes (surround
    // 2.1/4.0/5.1/7.1, Direct sample mixing / snooping) that users don't
    // want to scroll through. Additionally, when PipeWire is active
    // (pipewireActive=true), direct-hardware ALSA PCMs cannot be opened
    // — PipeWire holds the card exclusive — so selecting them cascades
    // to "device didn't start" + engine rollback into a broken state.
    // Hide them too. Engine still accepts any name via switchDevice —
    // this only affects the push list used by dropdowns.
    bool isPlatformClutter(bool pipewireActive = false) const {
#ifdef __linux__
        if (typeName != "ALSA") return false;
        // Always hidden — never useful from a GUI:
        if (name.find("Surround") != std::string::npos) return true;
        if (name.find("Direct sample mixing device") != std::string::npos) return true;
        if (name.find("Direct sample snooping device") != std::string::npos) return true;
        // Hidden only when PipeWire owns the card (the common case on
        // modern Linux desktops): direct-hardware PCMs would fail to
        // open. On pure-ALSA boxes (no PipeWire) these are the main
        // hardware entry points and must stay visible.
        if (pipewireActive) {
            if (name.find("Direct hardware device") != std::string::npos) return true;
            if (name.find("Front output / input") != std::string::npos) return true;
        }
        return false;
#else
        (void)pipewireActive;
        return false;
#endif
    }
};

struct CurrentDeviceInfo : DeviceInfo {
    double activeSampleRate    = 0.0;
    int    activeBufferSize    = 0;   // hardware callback buffer
    int    controlBlockSize    = 0;   // the DSP's control block (decoupled from the HW buffer)
    int    activeOutputChannels = 0;
    int    activeInputChannels  = 0;
    int    outputLatencySamples = 0;
    int    inputLatencySamples  = 0;
    std::string inputDeviceName;
};

enum class SwapType { Hot, Cold };

// Who asked for a device swap. User swaps carry intent: they update the
// preferred-device memory and can consume a pending switchDriver pick.
// Internal swaps (recovery reopen after a failed swap, hotplug re-attach,
// system-default follows) must do neither — they keep the engine alive
// without impersonating the user.
enum class SwapOrigin { User, Internal };

struct SwapResult {
    bool        success = false;
    SwapType    type    = SwapType::Hot;
    std::string error;
    std::string deviceName;
    std::string inputDeviceName;
    double      sampleRate  = 0.0;
    int         bufferSize  = 0;
    // Set when the caller asked for an input device but it couldn't be
    // opened (e.g. Windows microphone privacy denied). The swap still
    // succeeded for the output, so we don't roll back the whole thing —
    // we just clear the input. The client surface this so the user knows
    // why their mic isn't live.
    bool        inputUnavailable = false;
    std::string inputUnavailableReason;
    // Set by switchDriver when the driver was selected but no device
    // was opened — the caller (GUI) must follow up with switchDevice
    // naming a specific device. Used on ASIO and on any driver with
    // no remembered preferred device, where letting JUCE auto-pick
    // the alphabetical-first device of the new type is unsafe.
    bool        requiresDeviceSelection = false;
    // Set when the target opened but delivered no audio and the engine went
    // back to the device it was playing on, which does: deviceName, rate and
    // buffer are that device's. success stays false — the request failed —
    // but the engine is not silent.
    bool        fellBack = false;
};
