// SPDX-License-Identifier: ISC
// Copyright (c) 2025-2026 Sam Aaron
/*
 * smoothie_pipewire.h — the PipeWire driver (Linux), Smoothie's own
 *
 * A driver (a juce::AudioIODeviceType) named "PipeWire" that talks to the PipeWire
 * daemon directly via libpipewire-0.3 instead of going through the ALSA or
 * JACK compatibility layers. libpipewire is dlopen'd at runtime (the same
 * strategy JUCE uses for JACK) so the binary carries no hard PipeWire
 * dependency: without the library the factory returns nullptr and the
 * driver simply doesn't exist; without a running daemon the driver exists
 * but enumerates no devices.
 */
#pragma once

#if defined(__linux__) && defined(CLOCKWORK_PIPEWIRE)

#include <memory>
#include <string>

namespace smoothie { struct DriverFactory; }   // smoothie_juce_drivers.h

// The platform's drivers (smoothie::platformDrivers) and, when libpipewire
// loads, the PipeWire driver after them: what a device session is made of on
// a Linux build with PipeWire. `clientName` is what the daemon shows for
// this process's streams and nodes.
std::shared_ptr<const smoothie::DriverFactory> platformDriversWithPipeWire(std::string clientName);

// Names the engine's device-switch policy needs: the patchbay is an
// exclusive duplex device (both sides on one filter node — see
// DevicePolicy's resolveExclusiveDuplexPair), and "System Default" is the
// driver's fallback output when a carried-over patchbay side must yield.
const char* pipeWirePatchbayDeviceName();
const char* pipeWireDefaultDeviceName();

#endif
