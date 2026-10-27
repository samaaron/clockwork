// SPDX-License-Identifier: ISC
// Copyright (c) 2026 Sam Aaron
/*
 * smoothie_devices.cpp — the DeviceSession over JUCE's device types (see
 * smoothie_devices.h), and the device callback through which every device it
 * opens runs a worklet Processor.
 */
#include "smoothie_juce_drivers.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#ifdef __linux__
#include <dlfcn.h>
#endif

namespace smoothie {

#ifdef __linux__
namespace {
void quiet(const char*) {}

// libjack writes a two-line connect-failed pair to stderr for every scan
// where no jackd or pipewire-jack server runs; the JACK driver hears the
// failed open all the same and lists no devices. The driver loads libjack
// when it is made, so this follows it and comes before any scan. No libjack
// installed: nothing to quiet. Never closed: the driver needs it resident.
void quietJackLogs() {
    void* jack = dlopen("libjack.so.0", RTLD_LAZY | RTLD_NOLOAD);
    if (!jack) return;
    using SetLog = void (*)(void (*)(const char*));
    if (auto setError = reinterpret_cast<SetLog>(dlsym(jack, "jack_set_error_function")))
        setError(quiet);
    if (auto setInfo = reinterpret_cast<SetLog>(dlsym(jack, "jack_set_info_function")))
        setInfo(quiet);
}
}  // namespace
#endif

Drivers platformDrivers() {
    // Every driver JUCE knows, in the order its device manager listed them;
    // the ones this platform or build doesn't have come back null.
    using Type = juce::AudioIODeviceType;
    Drivers drivers;
    for (Type* type : { Type::createAudioIODeviceType_WASAPI(juce::WASAPIDeviceMode::shared),
                        Type::createAudioIODeviceType_WASAPI(juce::WASAPIDeviceMode::exclusive),
                        Type::createAudioIODeviceType_WASAPI(juce::WASAPIDeviceMode::sharedLowLatency),
                        Type::createAudioIODeviceType_DirectSound(),
                        Type::createAudioIODeviceType_ASIO(),
                        Type::createAudioIODeviceType_CoreAudio(),
                        Type::createAudioIODeviceType_iOSAudio(),
                        Type::createAudioIODeviceType_Bela(),
                        Type::createAudioIODeviceType_ALSA(),
                        Type::createAudioIODeviceType_JACK(),
                        Type::createAudioIODeviceType_Oboe(),
                        Type::createAudioIODeviceType_OpenSLES(),
                        Type::createAudioIODeviceType_Android() })
        if (type != nullptr) drivers.emplace_back(type);
#ifdef __linux__
    quietJackLogs();
#endif
    return drivers;
}

namespace {

std::vector<std::string> toStrings(const juce::StringArray& names) {
    std::vector<std::string> out;
    out.reserve(static_cast<size_t>(names.size()));
    for (const auto& n : names) out.push_back(n.toStdString());
    return out;
}

bool listed(const juce::AudioIODeviceType& type, bool asInput, const std::string& name) {
    const juce::String wanted = juce::String(name).trim();
    for (const auto& n : type.getDeviceNames(asInput))
        if (n.trim().equalsIgnoreCase(wanted)) return true;
    return false;
}

// What JUCE's AudioDeviceManager chose from a request, kept as it was: the
// rate asked for when the device offers it, else the one it runs at, else
// the lowest from 44.1 kHz up; the buffer asked for when offered, else the
// device's default.
double chooseRate(juce::AudioIODevice& device, double wanted) {
    const auto rates = device.getAvailableSampleRates();
    if (wanted > 0 && rates.contains(wanted)) return wanted;
    const double now = device.getCurrentSampleRate();
    if (now > 0 && rates.contains(now)) return now;
    double lowestFrom44 = 0;
    for (double r : rates)
        if (r >= 44100.0 && (lowestFrom44 < 1.0 || r < lowestFrom44)) lowestFrom44 = r;
    return lowestFrom44 > 0 ? lowestFrom44 : rates[0];
}

int chooseBuffer(juce::AudioIODevice& device, int wanted) {
    if (wanted > 0 && device.getAvailableBufferSizes().contains(wanted)) return wanted;
    return device.getDefaultBufferSize();
}

// ── The device callback ──────────────────────────────────────────────────────

// What a running device reports about itself, as a Processor takes it.
DeviceInfo describe(juce::AudioIODevice& device) {
    DeviceInfo d;
    d.driver              = device.getTypeName().toStdString();
    d.name                = device.getName().toStdString();
    d.sampleRate          = device.getCurrentSampleRate();
    d.bufferFrames        = device.getCurrentBufferSizeSamples();
    d.inputChannels       = device.getActiveInputChannels().countNumberOfSetBits();
    d.outputChannels      = device.getActiveOutputChannels().countNumberOfSetBits();
    d.inputLatencyFrames  = device.getInputLatencyInSamples();
    d.outputLatencyFrames = device.getOutputLatencyInSamples();
    const auto names  = device.getOutputChannelNames();
    const auto active = device.getActiveOutputChannels();
    d.outputs.reserve(static_cast<size_t>(names.size()));
    for (int i = 0; i < names.size(); ++i)
        d.outputs.push_back({ names[i].toStdString(), active[i] });
    return d;
}

// What every device the session opens calls: the attached Processor, or
// silence. The processor is taken by the block without a lock — the audio
// thread waits for nothing — and the device's start and stop, which come on
// the threads that start and stop it, are told under one.
class Callback final : public juce::AudioIODeviceCallback {
public:
    void attach(Processor& processor, juce::AudioIODevice* device) {
        std::lock_guard<std::mutex> lock(mLifecycle);
        if (device != nullptr) processor.deviceStarting(describe(*device));
        mLifecycleProcessor = &processor;
        mProcessor.store(&processor);
    }

    void detach(bool deviceExists) {
        Processor* processor = nullptr;
        {
            std::lock_guard<std::mutex> lock(mLifecycle);
            processor = mLifecycleProcessor;
            mLifecycleProcessor = nullptr;
            mProcessor.store(nullptr);
        }
        // A block that took the processor before it was let go finishes
        // first. Both sides are sequentially consistent: a block that read
        // the old processor marked itself before it read it, so this wait
        // sees the mark, or the block's own clearing of it.
        while (mInBlock.load()) std::this_thread::yield();
        if (processor != nullptr && deviceExists) processor->deviceStopped();
    }

    bool attached() const { return mProcessor.load() != nullptr; }

    void audioDeviceAboutToStart(juce::AudioIODevice* device) override {
        std::lock_guard<std::mutex> lock(mLifecycle);
        if (mLifecycleProcessor != nullptr) mLifecycleProcessor->deviceStarting(describe(*device));
    }

    void audioDeviceIOCallbackWithContext(const float* const* inputs, int numInputs,
                                          float* const* outputs, int numOutputs, int frames,
                                          const juce::AudioIODeviceCallbackContext& context) override {
        // Denormal flush-to-zero is a per-thread flag, and this is the thread
        // that renders: armed here, every block of every device has it.
        const juce::ScopedNoDenormals noDenormals;
        mInBlock.store(true);
        if (Processor* processor = mProcessor.load()) {
            BlockTime time;
            if (context.hostTimeNs != nullptr) time.hostTimeNs = *context.hostTimeNs;
            processor->process(inputs, numInputs, outputs, numOutputs, frames, time);
        } else {
            for (int c = 0; c < numOutputs; ++c)
                if (outputs[c] != nullptr) juce::FloatVectorOperations::clear(outputs[c], frames);
        }
        mInBlock.store(false);
    }

    void audioDeviceStopped() override {
        std::lock_guard<std::mutex> lock(mLifecycle);
        if (mLifecycleProcessor != nullptr) mLifecycleProcessor->deviceStopped();
    }

private:
    std::mutex              mLifecycle;
    Processor*              mLifecycleProcessor = nullptr;   // under mLifecycle
    std::atomic<Processor*> mProcessor{nullptr};              // what a block calls
    std::atomic<bool>       mInBlock{false};
};

}  // namespace

// ── The session ──────────────────────────────────────────────────────────────

struct DeviceSession::Impl {
    Options                                options;
    Drivers                                types;
    std::string                            driver;     // a request that names none opens here
    std::unique_ptr<juce::AudioIODevice>   device;
    DeviceRequest                          settled;    // while device: what it was opened as
    Callback                               callback;

    juce::AudioIODeviceType* find(const std::string& name) const {
        for (const auto& t : types)
            if (t->getTypeName().toStdString() == name) return t.get();
        return nullptr;
    }

    void closeDevice() {
        if (!device) return;
        device->stop();
        device->close();
        device.reset();
        settled = {};
    }
};

DeviceSession::DeviceSession(Options options, const std::shared_ptr<const DriverFactory>& drivers)
    : mImpl(std::make_unique<Impl>()) {
    mImpl->options = std::move(options);
    mImpl->types = drivers ? drivers->make() : platformDrivers();
    for (auto& type : mImpl->types) {
        if (mImpl->options.onChange)
            type->setDeviceChangeSink([sink = mImpl->options.onChange](juce::AudioIODeviceType::DeviceChange change) {
                using Change = juce::AudioIODeviceType::DeviceChange;
                sink(change == Change::list          ? DeviceChange::list
                   : change == Change::systemDefault ? DeviceChange::systemDefault
                                                     : DeviceChange::openDevice);
            });
        if (!mImpl->options.clientName.empty())
            type->setClientName(juce::String(mImpl->options.clientName));
        type->scanForDevices();
    }
    for (auto& type : mImpl->types)
        if (!type->getDeviceNames(false).isEmpty() || !type->getDeviceNames(true).isEmpty()) {
            mImpl->driver = type->getTypeName().toStdString();
            break;
        }
    if (mImpl->driver.empty() && !mImpl->types.empty())
        mImpl->driver = mImpl->types.front()->getTypeName().toStdString();
}

DeviceSession::~DeviceSession() {
    mImpl->closeDevice();
    // Waits for a change being told, so none is told once this returns.
    for (auto& type : mImpl->types) type->setDeviceChangeSink(nullptr);
}

std::vector<std::string> DeviceSession::drivers() const {
    std::vector<std::string> names;
    for (const auto& t : mImpl->types) names.push_back(t->getTypeName().toStdString());
    return names;
}

bool DeviceSession::hasDriver(const std::string& driver) const {
    return mImpl->find(driver) != nullptr;
}

void DeviceSession::scan(const std::string& driver) {
    if (auto* type = mImpl->find(driver)) type->scanForDevices();
}

void DeviceSession::scanAll() {
    for (auto& type : mImpl->types) type->scanForDevices();
}

std::vector<std::string> DeviceSession::deviceNames(const std::string& driver, bool inputs) const {
    auto* type = mImpl->find(driver);
    return type ? toStrings(type->getDeviceNames(inputs)) : std::vector<std::string>{};
}

std::string DeviceSession::defaultDevice(const std::string& driver, bool input) const {
    auto* type = mImpl->find(driver);
    return type ? type->getSystemDefaultDeviceName(input).toStdString() : std::string();
}

DeviceTraits DeviceSession::traits(const std::string& driver, const std::string& device) const {
    DeviceTraits out;
    auto* type = mImpl->find(driver);
    if (!type) return out;
    const auto t = type->getDeviceTraits(juce::String(device));
    out.wireless       = t.wireless;
    out.isVirtual      = t.isVirtual;
    out.aggregateClass = t.aggregateClass;
    out.pairs          = t.pairs;
    out.outputChannels = t.numOutputChannels;
    out.inputChannels  = t.numInputChannels;
    out.sampleRates.assign(t.sampleRates.begin(), t.sampleRates.end());
    out.bufferSizes.assign(t.bufferSizes.begin(), t.bufferSizes.end());
    out.kind = t.kind.toStdString();
    return out;
}

std::optional<DeviceProbe> DeviceSession::probe(const std::string& driver,
                                                const std::string& device, bool asInput) {
    auto* type = mImpl->find(driver);
    if (!type) return std::nullopt;
    const juce::String name(device);
    std::unique_ptr<juce::AudioIODevice> asked(asInput ? type->createDevice({}, name)
                                                       : type->createDevice(name, {}));
    if (!asked) return std::nullopt;
    DeviceProbe p;
    for (double r : asked->getAvailableSampleRates()) p.sampleRates.push_back(r);
    for (int b : asked->getAvailableBufferSizes()) p.bufferSizes.push_back(b);
    p.outputChannels = asked->getOutputChannelNames().size();
    p.inputChannels  = asked->getInputChannelNames().size();
    return p;
}

std::string DeviceSession::driver() const {
    return mImpl->device ? mImpl->settled.driver : mImpl->driver;
}

bool DeviceSession::selectDriver(const std::string& driver) {
    if (mImpl->device || !mImpl->find(driver)) return false;
    mImpl->driver = driver;
    return true;
}

std::string DeviceSession::open(const DeviceRequest& request) {
    Impl& s = *mImpl;
    DeviceRequest want = request;
    if (want.driver.empty()) want.driver = driver();
    auto* type = s.find(want.driver);
    if (!type) {
        s.closeDevice();
        return "No such driver: " + want.driver;
    }
    if (s.device && s.settled == want) return {};
    // Asked for on this driver, whatever comes of it, this is the driver.
    const std::string driverBefore = driver();
    s.driver = want.driver;

    const bool keepDevice = s.device && s.settled.driver == want.driver
                         && s.settled.output == want.output && s.settled.input == want.input;
    if (keepDevice) {
        s.device->stop();
    } else {
        const bool driverChanges = s.device && driverBefore != want.driver;
        s.closeDevice();
        if (want.output.empty() && want.input.empty())
            return "Nothing to open: the request names no output and no input";
        if (driverChanges && s.options.driverSettleMs > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(s.options.driverSettleMs));
        if (!want.output.empty() && !listed(*type, false, want.output))
            return "No such device: " + want.output;
        if (!want.input.empty() && !listed(*type, true, want.input))
            return "No such device: " + want.input;
        s.device.reset(type->createDevice(juce::String(want.output), juce::String(want.input)));
        if (!s.device)
            return "Can't open '" + (want.output.empty() ? want.input : want.output)
                 + "' on " + want.driver;
        if (const auto err = s.device->getLastError(); err.isNotEmpty()) {
            s.device.reset();
            return err.toStdString();
        }
    }
    s.settled = {};

    juce::ChannelMask outs, ins;
    if (!want.output.empty() && want.outputChannels > 0) outs.setRange(0, want.outputChannels, true);
    if (!want.input.empty() && want.inputChannels > 0) ins.setRange(0, want.inputChannels, true);
    if (outs.isZero() && ins.isZero()) {
        s.closeDevice();
        return "Nothing to open: the request asks for no channels";
    }

    juce::String err = s.device->open(ins, outs, chooseRate(*s.device, want.sampleRate),
                                      chooseBuffer(*s.device, want.bufferFrames));
    if (err.isEmpty()) {
        s.device->start(&s.callback);
        err = s.device->getLastError();
    }
    if (err.isNotEmpty()) {
        s.closeDevice();
        return err.toStdString();
    }

    s.settled = want;
    s.settled.sampleRate     = s.device->getCurrentSampleRate();
    s.settled.bufferFrames   = s.device->getCurrentBufferSizeSamples();
    s.settled.outputChannels = s.device->getActiveOutputChannels().countNumberOfSetBits();
    s.settled.inputChannels  = s.device->getActiveInputChannels().countNumberOfSetBits();
    return {};
}

void DeviceSession::close() {
    mImpl->closeDevice();
}

bool DeviceSession::isOpen() const {
    return mImpl->device != nullptr;
}

std::optional<OpenDevice> DeviceSession::current() const {
    auto* dev = mImpl->device.get();
    if (!dev) return std::nullopt;
    OpenDevice d;
    d.driver        = mImpl->settled.driver;
    d.name          = dev->getName().toStdString();
    d.sampleRate    = dev->getCurrentSampleRate();
    d.bufferFrames  = dev->getCurrentBufferSizeSamples();
    d.activeOutputs = dev->getActiveOutputChannels().countNumberOfSetBits();
    d.activeInputs  = dev->getActiveInputChannels().countNumberOfSetBits();
    d.outputs       = dev->getOutputChannelNames().size();
    d.inputs        = dev->getInputChannelNames().size();
    d.outputLatencyFrames = dev->getOutputLatencyInSamples();
    d.inputLatencyFrames  = dev->getInputLatencyInSamples();
    // Its inputs come from the input it was opened with — the output itself
    // when that is one duplex device — and from nowhere with none active.
    if (d.activeInputs > 0)
        d.inputName = mImpl->settled.input.empty() ? d.name : mImpl->settled.input;
    for (double r : dev->getAvailableSampleRates()) d.sampleRates.push_back(r);
    for (int b : dev->getAvailableBufferSizes()) d.bufferSizes.push_back(b);
    return d;
}

std::optional<DeviceRequest> DeviceSession::settled() const {
    if (!mImpl->device) return std::nullopt;
    return mImpl->settled;
}

LiveState DeviceSession::liveState() const {
    LiveState out;
    if (!mImpl->device) {
        out.alive = false;
        return out;
    }
    const auto live = mImpl->device->readLiveState();
    out.alive      = live.alive;
    out.mustReopen = live.mustReopen;
    out.sampleRate = live.sampleRate;
    out.outputs    = live.numOutputChannels;
    out.inputs     = live.numInputChannels;
    return out;
}

void DeviceSession::attach(Processor& processor) {
    mImpl->callback.attach(processor, mImpl->device.get());
}

void DeviceSession::detach() {
    mImpl->callback.detach(mImpl->device != nullptr);
}

bool DeviceSession::attached() const {
    return mImpl->callback.attached();
}

// ── The device layer's log ───────────────────────────────────────────────────

namespace {
struct DeviceLog final : juce::Logger {
    std::function<void(const std::string&)> sink;
    void logMessage(const juce::String& message) override {
        if (sink) sink(message.toStdString());
    }
};
}  // namespace

void routeDeviceLog(std::function<void(const std::string&)> sink) {
    static DeviceLog log;
    static std::once_flag once;
    std::call_once(once, [&] {
        if (juce::Logger::getCurrentLogger() != nullptr) return;
        log.sink = std::move(sink);
        juce::Logger::setCurrentLogger(&log);
    });
}

}  // namespace smoothie
