// SPDX-License-Identifier: ISC
// Copyright (c) 2026 Sam Aaron
/*
 * smoothie_devices.h — the devices a machine has, and the one open.
 *
 * The other half of what Smoothie offers the code above it. The worklet
 * (smoothie_worklet.h) is what a device does with a processor once it runs;
 * a DeviceSession is how one comes to run: the drivers there are, the
 * devices each lists, what a driver says of a device without opening it, and
 * opening one exactly as asked.
 *
 * Exactly as asked: a request names its driver, its output, its input, its
 * rate, its buffer and its channels, and nothing is filled in from a default
 * the caller did not name. Choosing what to open — the user's pick, the
 * system default, what to fall back to — is the caller's; the session opens
 * it, or says why it could not and leaves nothing open.
 *
 * Nothing here includes JUCE. The drivers behind it are JUCE's device types
 * (smoothie_juce_drivers.h says which), and a caller that hands it its own —
 * a test's fakes — does so through that header.
 *
 * One caller at a time: a session is driven from one thread, or under the
 * caller's own lock. The processor it runs is called on the device's thread,
 * which the session keeps apart from the caller's (attach, detach).
 */
#pragma once

#include "smoothie_worklet.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace smoothie {

struct DriverFactory;   // smoothie_juce_drivers.h

// What a driver says of a device without opening it. What it can't say is
// left as it is here.
struct DeviceTraits {
    bool wireless       = false;   // its link negotiates its own rate and buffer (AirPlay, Bluetooth)
    bool isVirtual      = false;   // made in software (Loopback, BlackHole)
    bool aggregateClass = false;   // made of other devices (an aggregate, a multi-output device)
    bool pairs          = true;    // plays as one with a different device: output on one, input on the other
    int  outputChannels = -1;      // -1: can't say without opening it
    int  inputChannels  = -1;
    std::vector<double> sampleRates;   // empty: can't say without opening it
    std::vector<int>    bufferSizes;
    std::string kind;              // the driver's own word for how it is connected, for logs
};

// What a device created to be asked says it offers. Only the side it was
// created on is authoritative: one created as an output reports no inputs,
// even when it has them.
struct DeviceProbe {
    std::vector<double> sampleRates;
    std::vector<int>    bufferSizes;
    int outputChannels = 0;
    int inputChannels  = 0;
};

// What to open.
struct DeviceRequest {
    // Every channel the device has, up to this many: more than it has opens
    // all it has.
    static constexpr int kAllChannels = 64;

    std::string driver;         // empty: the session's driver (DeviceSession::driver)
    std::string output;         // empty: no output
    std::string input;          // empty: no input. The output's name: one duplex device
    double sampleRate   = 0;    // 0, or one it doesn't offer: the rate it runs at now,
                                // else the lowest it offers from 44.1 kHz up
    int    bufferFrames = 0;    // 0, or one it doesn't offer: its default
    int    outputChannels = kAllChannels;   // channels 0 to n-1 of the output
    int    inputChannels  = kAllChannels;   // channels 0 to n-1 of the input

    bool operator==(const DeviceRequest& o) const {
        return driver == o.driver && output == o.output && input == o.input
            && sampleRate == o.sampleRate && bufferFrames == o.bufferFrames
            && outputChannels == o.outputChannels && inputChannels == o.inputChannels;
    }
    bool operator!=(const DeviceRequest& o) const { return !(*this == o); }
};

// The device open now, as its driver reports it.
struct OpenDevice {
    std::string driver;
    std::string name;           // the output's name; the input's when it opened input alone
    std::string inputName;      // where the input comes from: "" with no input channel active
    double sampleRate     = 0;
    int    bufferFrames   = 0;
    int    activeOutputs  = 0;
    int    activeInputs   = 0;
    int    outputs        = 0;  // every channel it has
    int    inputs         = 0;
    int    outputLatencyFrames = 0;
    int    inputLatencyFrames  = 0;
    std::vector<double> sampleRates;   // what it offers
    std::vector<int>    bufferSizes;
};

// The open device as its driver reports it now, read fresh: not what it
// was opened with.
struct LiveState {
    bool   alive      = true;   // false once the driver says it has gone
    bool   mustReopen = false;  // the driver says it cannot carry on as opened
    double sampleRate = 0;      // 0: the driver can't say
    int    outputs    = -1;     // -1: the driver can't say
    int    inputs     = -1;
};

// What a driver says changed (DeviceSession::Options::onChange).
enum class DeviceChange {
    list,           // devices came or went: the lists need scanning again
    openDevice,     // the open device changed where it stands (liveState says how)
    systemDefault,  // the operating system's default device moved
};

class DeviceSession {
public:
    struct Options {
        // What the devices are opened for, in what a driver makes in the
        // operating system's name (a CoreAudio aggregate, a PipeWire node).
        std::string clientName;
        // Told what changed, on whatever thread the driver hears of it, the
        // moment it does: a note to look, never the place to act. Nothing in
        // the session acts on a change by itself.
        std::function<void(DeviceChange)> onChange;
        // A driver changing with a device open waits this long between
        // closing the one and opening the other, so the operating system's
        // devices settle first (DirectSound and ASIO on one interface clash
        // without it).
        int driverSettleMs = 1500;
    };

    // The drivers `drivers` makes, or the platform's own when it is null.
    // Every driver's devices are listed once here, and the session's driver
    // is the first that has any.
    explicit DeviceSession(Options options,
                           const std::shared_ptr<const DriverFactory>& drivers = nullptr);
    ~DeviceSession();   // the open device closed, no change told after it returns

    DeviceSession(const DeviceSession&) = delete;
    DeviceSession& operator=(const DeviceSession&) = delete;
    DeviceSession(DeviceSession&&) = delete;
    DeviceSession& operator=(DeviceSession&&) = delete;

    // ── What there is ────────────────────────────────────────────────────
    std::vector<std::string> drivers() const;
    bool hasDriver(const std::string& driver) const;
    // List a driver's devices again: what deviceNames and the rest answer
    // from. A driver's lists are as it last looked until then.
    void scan(const std::string& driver);
    void scanAll();
    std::vector<std::string> deviceNames(const std::string& driver, bool inputs) const;
    // The operating system's default among a driver's devices; "" when the
    // driver has none (ASIO) or can't say.
    std::string defaultDevice(const std::string& driver, bool input) const;
    DeviceTraits traits(const std::string& driver, const std::string& device) const;
    // A device created to be asked, and closed again; nullopt when the
    // driver would not create it. Costs what creating it costs: on Windows,
    // an activation.
    std::optional<DeviceProbe> probe(const std::string& driver,
                                     const std::string& device, bool asInput);

    // ── What is open ─────────────────────────────────────────────────────
    // The open device's driver, or with none open the one a request that
    // names none opens on.
    std::string driver() const;
    // With nothing open, make `driver` the one a request that names none
    // opens on; it opens nothing. False for a driver there isn't, and with a
    // device open — its driver is the driver: open on another to change it.
    bool selectDriver(const std::string& driver);

    // Open `request`, in place of whatever is open: "" when it is open and
    // running, else why not, with nothing open. A request the open device
    // was opened with and settled on leaves it as it is. One that keeps the
    // devices and changes the rest opens the same device again; one that
    // changes them closes it first. Whatever comes of it, the request's
    // driver is the session's driver after.
    std::string open(const DeviceRequest& request);
    void close();
    bool isOpen() const;
    std::optional<OpenDevice> current() const;
    // The open device's request as it settled: its driver, its rate and
    // buffer, its active channels. Opened again, it is the same device.
    std::optional<DeviceRequest> settled() const;
    LiveState liveState() const;   // { alive = false } with nothing open

    // ── The processor ────────────────────────────────────────────────────
    // An open device runs from the moment it opens, silent until a
    // processor is attached. Attached, the processor is told the device is
    // starting and is called from the next block; it stays attached through
    // whatever the session opens and closes after, told of each device's
    // stop and start. Detached, it is told the device stopped, and is
    // never called again once detach returns.
    void attach(Processor& processor);
    void detach();
    bool attached() const;

private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

// The device layer's own diagnostics, on `sink`: once per process, and
// never over a logger the embedder set first.
void routeDeviceLog(std::function<void(const std::string&)> sink);

}  // namespace smoothie
