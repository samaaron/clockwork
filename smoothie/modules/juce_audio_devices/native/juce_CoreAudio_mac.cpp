/*
  ==============================================================================

   This file is part of the JUCE library.
   Copyright (c) 2022 - Raw Material Software Limited

   JUCE is an open source library subject to commercial or open-source
   licensing.

   The code included in this file is provided under the terms of the ISC license
   http://www.isc.org/downloads/software-support-policy/isc-license. Permission
   To use, copy, modify, and/or distribute this software for any purpose with or
   without fee is hereby granted provided that the above copyright notice and
   this permission notice appear in all copies.

   JUCE IS PROVIDED "AS IS" WITHOUT ANY WARRANTY, AND ALL WARRANTIES, WHETHER
   EXPRESSED OR IMPLIED, INCLUDING MERCHANTABILITY AND FITNESS FOR PURPOSE, ARE
   DISCLAIMED.

  ==============================================================================
*/

namespace juce
{

#if JUCE_COREAUDIO_LOGGING_ENABLED
 #define JUCE_COREAUDIOLOG(a) { String camsg ("CoreAudio: "); camsg << a; Logger::writeToLog (camsg); }
#else
 #define JUCE_COREAUDIOLOG(a)
#endif

JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wnonnull")

constexpr auto juceAudioObjectPropertyElementMain =
       #if defined (MAC_OS_VERSION_12_0)
        kAudioObjectPropertyElementMain;
       #else
        kAudioObjectPropertyElementMaster;
       #endif

//==============================================================================
class ManagedAudioBufferList final : public AudioBufferList
{
public:
    struct Deleter
    {
        void operator() (ManagedAudioBufferList* p) const
        {
            if (p != nullptr)
                p->~ManagedAudioBufferList();

            delete[] reinterpret_cast<std::byte*> (p);
        }
    };

    using Ref = std::unique_ptr<ManagedAudioBufferList, Deleter>;

    //==============================================================================
    static Ref create (std::size_t numBuffers)
    {
        static_assert (alignof (ManagedAudioBufferList) <= alignof (std::max_align_t));

        if (std::unique_ptr<std::byte[]> storage { new std::byte[storageSizeForNumBuffers (numBuffers)] })
            return Ref { new (storage.release()) ManagedAudioBufferList (numBuffers) };

        return nullptr;
    }

    //==============================================================================
    static std::size_t storageSizeForNumBuffers (std::size_t numBuffers) noexcept
    {
        return audioBufferListHeaderSize + (numBuffers * sizeof (::AudioBuffer));
    }

    static std::size_t numBuffersForStorageSize (std::size_t bytes) noexcept
    {
        bytes -= audioBufferListHeaderSize;

        // storage size ends between to buffers in AudioBufferList
        jassert ((bytes % sizeof (::AudioBuffer)) == 0);

        return bytes / sizeof (::AudioBuffer);
    }

private:
    // Do not call the base constructor here as this will zero-initialize the first buffer,
    // for which no storage may be available though (when numBuffers == 0).
    explicit ManagedAudioBufferList (std::size_t numBuffers)
    {
        mNumberBuffers = static_cast<UInt32> (numBuffers);
    }

    static constexpr auto audioBufferListHeaderSize = sizeof (AudioBufferList) - sizeof (::AudioBuffer);

    JUCE_DECLARE_NON_COPYABLE (ManagedAudioBufferList)
    JUCE_DECLARE_NON_MOVEABLE (ManagedAudioBufferList)
};

//==============================================================================
struct IgnoreUnused
{
    template <typename... Ts>
    void operator() (Ts&&...) const {}
};

template <typename T>
static auto getDataPtrAndSize (T& t)
{
    static_assert (std::is_pod_v<T>);
    return std::make_tuple (&t, (UInt32) sizeof (T));
}

static auto getDataPtrAndSize (ManagedAudioBufferList::Ref& t)
{
    const auto size = t.get() != nullptr
                    ? ManagedAudioBufferList::storageSizeForNumBuffers (t->mNumberBuffers)
                    : 0;
    return std::make_tuple (t.get(), (UInt32) size);
}

//==============================================================================
[[nodiscard]] static bool audioObjectHasProperty (AudioObjectID objectID, const AudioObjectPropertyAddress address)
{
    return objectID != kAudioObjectUnknown && AudioObjectHasProperty (objectID, &address);
}

template <typename T, typename OnError = IgnoreUnused>
[[nodiscard]] static auto audioObjectGetProperty (AudioObjectID objectID,
                                                  const AudioObjectPropertyAddress address,
                                                  OnError&& onError = {})
{
    using Result = std::conditional_t<std::is_same_v<T, AudioBufferList>, ManagedAudioBufferList::Ref, std::optional<T>>;

    if (! audioObjectHasProperty (objectID, address))
        return Result{};

    auto result = [&]
    {
        if constexpr (std::is_same_v<T, AudioBufferList>)
        {
            UInt32 size{};

            if (auto status = AudioObjectGetPropertyDataSize (objectID, &address, 0, nullptr, &size); status != noErr)
            {
                onError (status);
                return Result{};
            }

            return ManagedAudioBufferList::create (ManagedAudioBufferList::numBuffersForStorageSize (size));
        }
        else
        {
            return T{};
        }
    }();

    auto [ptr, size] = getDataPtrAndSize (result);

    if (size == 0)
        return Result{};

    if (auto status = AudioObjectGetPropertyData (objectID, &address, 0, nullptr, &size, ptr); status != noErr)
    {
        onError (status);
        return Result{};
    }

    return Result { std::move (result) };
}

template <typename T, typename OnError = IgnoreUnused>
static bool audioObjectSetProperty (AudioObjectID objectID,
                                    const AudioObjectPropertyAddress address,
                                    const T value,
                                    OnError&& onError = {})
{
    if (! audioObjectHasProperty (objectID, address))
        return false;

    Boolean isSettable = NO;
    if (auto status = AudioObjectIsPropertySettable (objectID, &address, &isSettable); status != noErr)
    {
        onError (status);
        return false;
    }

    if (! isSettable)
        return false;

    if (auto status = AudioObjectSetPropertyData (objectID, &address, 0, nullptr, static_cast<UInt32> (sizeof (T)), &value); status != noErr)
    {
        onError (status);
        return false;
    }

    return true;
}

template <typename T, typename OnError = IgnoreUnused>
[[nodiscard]] static std::vector<T> audioObjectGetProperties (AudioObjectID objectID,
                                                              const AudioObjectPropertyAddress address,
                                                              OnError&& onError = {})
{
    if (! audioObjectHasProperty (objectID, address))
        return {};

    UInt32 size{};

    if (auto status = AudioObjectGetPropertyDataSize (objectID, &address, 0, nullptr, &size); status != noErr)
    {
        onError (status);
        return {};
    }

    // If this is hit, the number of results is not integral, and the following
    // AudioObjectGetPropertyData will probably write past the end of the result buffer.
    jassert ((size % sizeof (T)) == 0);
    std::vector<T> result (size / sizeof (T));

    if (auto status = AudioObjectGetPropertyData (objectID, &address, 0, nullptr, &size, result.data()); status != noErr)
    {
        onError (status);
        return {};
    }

    return result;
}

struct SystemVol
{
    explicit SystemVol (AudioObjectPropertySelector selector) noexcept
        : outputDeviceID (audioObjectGetProperty<AudioDeviceID> (kAudioObjectSystemObject, { kAudioHardwarePropertyDefaultOutputDevice,
                                                                                             kAudioObjectPropertyScopeGlobal,
                                                                                             juceAudioObjectPropertyElementMain }).value_or (kAudioObjectUnknown)),
          addr { selector, kAudioDevicePropertyScopeOutput, juceAudioObjectPropertyElementMain }
    {}

    float getGain() const noexcept
    {
        return audioObjectGetProperty<Float32> (outputDeviceID, addr).value_or (0.0f);
    }

    bool setGain (float gain) const noexcept
    {
        return audioObjectSetProperty (outputDeviceID, addr, static_cast<Float32> (gain));
    }

    bool isMuted() const noexcept
    {
        return audioObjectGetProperty<UInt32> (outputDeviceID, addr).value_or (0) != 0;
    }

    bool setMuted (bool mute) const noexcept
    {
        return audioObjectSetProperty (outputDeviceID, addr, static_cast<UInt32> (mute ? 1 : 0));
    }

private:
    AudioDeviceID outputDeviceID;
    AudioObjectPropertyAddress addr;
};

JUCE_END_IGNORE_WARNINGS_GCC_LIKE

constexpr auto juceAudioHardwareServiceDeviceProperty_VirtualMainVolume =
       #if defined (MAC_OS_VERSION_12_0)
        kAudioHardwareServiceDeviceProperty_VirtualMainVolume;
       #else
        kAudioHardwareServiceDeviceProperty_VirtualMasterVolume;
       #endif

#define JUCE_SYSTEMAUDIOVOL_IMPLEMENTED 1
float JUCE_CALLTYPE SystemAudioVolume::getGain()              { return SystemVol (juceAudioHardwareServiceDeviceProperty_VirtualMainVolume).getGain(); }
bool  JUCE_CALLTYPE SystemAudioVolume::setGain (float gain)   { return SystemVol (juceAudioHardwareServiceDeviceProperty_VirtualMainVolume).setGain (gain); }
bool  JUCE_CALLTYPE SystemAudioVolume::isMuted()              { return SystemVol (kAudioDevicePropertyMute).isMuted(); }
bool  JUCE_CALLTYPE SystemAudioVolume::setMuted (bool mute)   { return SystemVol (kAudioDevicePropertyMute).setMuted (mute); }

//==============================================================================
struct CoreAudioClasses
{

class CoreAudioIODeviceType;
class CoreAudioIODevice;

//==============================================================================
// smoothie addition: shared live-object fence for OS callbacks that can
// outlive their targets (listener bodies dereferencing dying internals
// during Bluetooth profile churn). See the header for the contract.
#include "juce_LiveObjectRegistry.h"
using LiveInternalRegistry = LiveObjectRegistry;

//==============================================================================
class CoreAudioInternal final
{
private:
    // members with deduced return types need to be defined before they
    // are used, so define it here. decltype doesn't help as you can't
    // capture anything in lambdas inside a decltype context.
    auto err2log() const { return [this] (OSStatus err) { OK (err); }; }

public:
    CoreAudioInternal (CoreAudioIODevice& d, AudioDeviceID id, bool hasInput, bool hasOutput)
        : owner (d),
          deviceID (id),
          inStream  (hasInput  ? new Stream (true,  *this, {}) : nullptr),
          outStream (hasOutput ? new Stream (false, *this, {}) : nullptr)
    {
        jassert (deviceID != 0);

        updateDetailsFromDevice();
        JUCE_COREAUDIOLOG ("Creating CoreAudioInternal\n"
                           << (inStream  != nullptr ? ("    inputDeviceId "  + String (deviceID) + "\n") : "")
                           << (outStream != nullptr ? ("    outputDeviceId " + String (deviceID) + "\n") : "")
                           << getDeviceDetails().joinIntoString ("\n    "));

        AudioObjectPropertyAddress pa;
        pa.mSelector = kAudioObjectPropertySelectorWildcard;
        pa.mScope = kAudioObjectPropertyScopeWildcard;
        pa.mElement = kAudioObjectPropertyElementWildcard;

        // smoothie: register before the listener can fire.
        LiveInternalRegistry::get().add (this);

        AudioObjectAddPropertyListener (deviceID, &pa, deviceListenerProc, this);
    }

    // smoothie: stop HAL listener callbacks reaching this instance —
    // including bodies already in flight (the registry remove blocks on
    // them). Idempotent; called from ~CoreAudioIODevice (before teardown
    // starts), from the combiner's destructor (before it closes its
    // sub-devices), and from our own destructor as the backstop.
    void detachListener()
    {
        LiveInternalRegistry::get().remove (this);

        AudioObjectPropertyAddress pa;
        pa.mSelector = kAudioObjectPropertySelectorWildcard;
        pa.mScope = kAudioObjectPropertyScopeWildcard;
        pa.mElement = kAudioObjectPropertyElementWildcard;

        AudioObjectRemovePropertyListener (deviceID, &pa, deviceListenerProc, this);
    }

    ~CoreAudioInternal()
    {
        detachListener();

        stop (false);
    }

    auto getStreams() const { return std::array<Stream*, 2> { { inStream.get(), outStream.get() } }; }

    void allocateTempBuffers()
    {
        auto tempBufSize = bufferSize + 4;

        auto streams = getStreams();
        const auto total = std::accumulate (streams.begin(), streams.end(), 0,
                                            [] (int n, const auto& s) { return n + (s != nullptr ? s->channels : 0); });
        audioBuffer.calloc (total * tempBufSize);

        // smoothie: remember the per-channel capacity so audioCallback
        // can clamp against it — bufferSize is mutated in places that
        // don't reallocate (see reopen's "bodge").
        tempBufferSamples = tempBufSize;

        auto channels = 0;
        for (auto* stream : streams)
            channels += stream != nullptr ? stream->allocateTempBuffers (tempBufSize, channels, audioBuffer) : 0;
    }

    struct CallbackDetailsForChannel
    {
        int streamNum;
        int dataOffsetSamples;
        int dataStrideSamples;
    };

    Array<double> getSampleRatesFromDevice() const
    {
        Array<double> newSampleRates;

        if (auto ranges = audioObjectGetProperties<AudioValueRange> (deviceID,
                                                                     { kAudioDevicePropertyAvailableNominalSampleRates,
                                                                       kAudioObjectPropertyScopeWildcard,
                                                                       juceAudioObjectPropertyElementMain },
                                                                     err2log()); ! ranges.empty())
        {
            for (const auto rate : SampleRateHelpers::getAllSampleRates())
            {
                for (auto range = ranges.rbegin(); range != ranges.rend(); ++range)
                {
                    if (range->mMinimum - 2 <= rate && rate <= range->mMaximum + 2)
                    {
                        newSampleRates.add (rate);
                        break;
                    }
                }
            }
        }

        if (newSampleRates.isEmpty() && sampleRate > 0)
            newSampleRates.add (sampleRate);

        auto nominalRate = getNominalSampleRate();

        if ((nominalRate > 0) && ! newSampleRates.contains (nominalRate))
            newSampleRates.addUsingDefaultSort (nominalRate);

        return newSampleRates;
    }

    Array<int> getBufferSizesFromDevice() const
    {
        Array<int> newBufferSizes;

        if (auto ranges = audioObjectGetProperties<AudioValueRange> (deviceID, { kAudioDevicePropertyBufferFrameSizeRange,
                                                                                 kAudioObjectPropertyScopeWildcard,
                                                                                 juceAudioObjectPropertyElementMain },
                                                                     err2log()); ! ranges.empty())
        {
            newBufferSizes.add ((int) (ranges[0].mMinimum + 15) & ~15);

            for (int i = 32; i <= 2048; i += 32)
            {
                for (auto range = ranges.rbegin(); range != ranges.rend(); ++range)
                {
                    if (i >= range->mMinimum && i <= range->mMaximum)
                    {
                        newBufferSizes.addIfNotAlreadyThere (i);
                        break;
                    }
                }
            }

            if (bufferSize > 0)
                newBufferSizes.addIfNotAlreadyThere (bufferSize);
        }

        if (newBufferSizes.isEmpty() && bufferSize > 0)
            newBufferSizes.add (bufferSize);

        return newBufferSizes;
    }

    int getFrameSizeFromDevice() const
    {
        return static_cast<int> (audioObjectGetProperty<UInt32> (deviceID, { kAudioDevicePropertyBufferFrameSize,
                                                                             kAudioObjectPropertyScopeWildcard,
                                                                             juceAudioObjectPropertyElementMain }).value_or (0));
    }

    bool isDeviceAlive() const
    {
        return deviceID != 0
                 && audioObjectGetProperty<UInt32> (deviceID, { kAudioDevicePropertyDeviceIsAlive,
                                                                kAudioObjectPropertyScopeWildcard,
                                                                juceAudioObjectPropertyElementMain }, err2log()).value_or (0) != 0;
    }

    bool updateDetailsFromDevice (const BigInteger& activeIns, const BigInteger& activeOuts)
    {
        if (! isDeviceAlive())
            return false;

        // this collects all the new details from the device without any locking, then
        // locks + swaps them afterwards.

        auto newSampleRate = getNominalSampleRate();
        auto newBufferSize = getFrameSizeFromDevice();

        auto newBufferSizes = getBufferSizesFromDevice();
        auto newSampleRates = getSampleRatesFromDevice();

        auto newInput  = rawToUniquePtr (inStream  != nullptr ? new Stream (true,  *this, activeIns)  : nullptr);
        auto newOutput = rawToUniquePtr (outStream != nullptr ? new Stream (false, *this, activeOuts) : nullptr);

        auto newBitDepth = jmax (getBitDepth (newInput), getBitDepth (newOutput));

       #if JUCE_AUDIOWORKGROUP_TYPES_AVAILABLE
        audioWorkgroup = [this]() -> AudioWorkgroup
        {
            AudioObjectPropertyAddress pa;
            pa.mSelector = kAudioDevicePropertyIOThreadOSWorkgroup;
            pa.mScope    = kAudioObjectPropertyScopeWildcard;
            pa.mElement  = juceAudioObjectPropertyElementMain;

            if (auto* workgroup = audioObjectGetProperty<os_workgroup_t> (deviceID, pa).value_or (nullptr))
            {
                ScopeGuard scope { [&] { os_release (workgroup); } };
                return makeRealAudioWorkgroup (workgroup);
            }

            return {};
        }();
       #endif

        {
            const ScopedLock sl (callbackLock);

            bitDepth = newBitDepth > 0 ? newBitDepth : 32;

            if (newSampleRate > 0)
                sampleRate = newSampleRate;

            bufferSize = newBufferSize;

            sampleRates.swapWith (newSampleRates);
            bufferSizes.swapWith (newBufferSizes);

            std::swap (inStream,  newInput);
            std::swap (outStream, newOutput);

            allocateTempBuffers();
        }

        return true;
    }

    bool updateDetailsFromDevice()
    {
        return updateDetailsFromDevice (getActiveChannels (inStream), getActiveChannels (outStream));
    }

    StringArray getDeviceDetails()
    {
        StringArray result;

        String availableSampleRates ("Available sample rates:");

        for (auto& s : sampleRates)
            availableSampleRates << " " << s;

        result.add (availableSampleRates);
        result.add ("Sample rate: " + String (sampleRate));
        String availableBufferSizes ("Available buffer sizes:");

        for (auto& b : bufferSizes)
            availableBufferSizes << " " << b;

        result.add (availableBufferSizes);
        result.add ("Buffer size: " + String (bufferSize));
        result.add ("Bit depth: " + String (bitDepth));
        result.add ("Input latency: "  + String (getLatency (inStream)));
        result.add ("Output latency: " + String (getLatency (outStream)));
        result.add ("Input channel names: "  + getChannelNames (inStream));
        result.add ("Output channel names: " + getChannelNames (outStream));

        return result;
    }

    static auto getScope (bool input)
    {
        return input ? kAudioDevicePropertyScopeInput : kAudioDevicePropertyScopeOutput;
    }

    // smoothie: the channels a device has now, in one direction.
    static int getNumChannels (AudioDeviceID deviceID, bool input)
    {
        int total = 0;

        if (auto bufList = audioObjectGetProperty<AudioBufferList> (deviceID, { kAudioDevicePropertyStreamConfiguration,
                                                                                getScope (input),
                                                                                juceAudioObjectPropertyElementMain }))
        {
            auto numStreams = (int) bufList->mNumberBuffers;

            for (int i = 0; i < numStreams; ++i)
                total += bufList->mBuffers[i].mNumberChannels;
        }

        return total;
    }

    //==============================================================================
    StringArray getSources (bool input)
    {
        StringArray s;
        auto types = audioObjectGetProperties<OSType> (deviceID, { kAudioDevicePropertyDataSources,
                                                                   kAudioObjectPropertyScopeWildcard,
                                                                   juceAudioObjectPropertyElementMain });

        for (auto type : types)
        {
            AudioValueTranslation avt;
            char buffer[256];

            avt.mInputData = &type;
            avt.mInputDataSize = sizeof (UInt32);
            avt.mOutputData = buffer;
            avt.mOutputDataSize = 256;

            UInt32 transSize = sizeof (avt);

            AudioObjectPropertyAddress pa;
            pa.mSelector = kAudioDevicePropertyDataSourceNameForID;
            pa.mScope = getScope (input);
            pa.mElement = juceAudioObjectPropertyElementMain;

            if (OK (AudioObjectGetPropertyData (deviceID, &pa, 0, nullptr, &transSize, &avt)))
                s.add (buffer);
        }

        return s;
    }

    int getCurrentSourceIndex (bool input) const
    {
        if (deviceID != 0)
        {
            if (auto currentSourceID = audioObjectGetProperty<OSType> (deviceID, { kAudioDevicePropertyDataSource,
                                                                                   getScope (input),
                                                                                   juceAudioObjectPropertyElementMain }, err2log()))
            {
                auto types = audioObjectGetProperties<OSType> (deviceID, { kAudioDevicePropertyDataSources,
                                                                           kAudioObjectPropertyScopeWildcard,
                                                                           juceAudioObjectPropertyElementMain });

                if (auto it = std::find (types.begin(), types.end(), *currentSourceID); it != types.end())
                    return static_cast<int> (std::distance (types.begin(), it));
            }
        }

        return -1;
    }

    void setCurrentSourceIndex (int index, bool input)
    {
        if (deviceID != 0)
        {
            auto types = audioObjectGetProperties<OSType> (deviceID, { kAudioDevicePropertyDataSources,
                                                                       kAudioObjectPropertyScopeWildcard,
                                                                       juceAudioObjectPropertyElementMain });

            if (isPositiveAndBelow (index, static_cast<int> (types.size())))
            {
                audioObjectSetProperty<OSType> (deviceID, { kAudioDevicePropertyDataSource,
                                                            getScope (input),
                                                            juceAudioObjectPropertyElementMain },
                                                types[static_cast<std::size_t> (index)], err2log());
            }
        }
    }

    double getNominalSampleRate() const
    {
        return static_cast<double> (audioObjectGetProperty <Float64> (deviceID, { kAudioDevicePropertyNominalSampleRate,
                                                                                  kAudioObjectPropertyScopeGlobal,
                                                                                  juceAudioObjectPropertyElementMain },
                                                                      err2log()).value_or (0.0));
    }

    bool setNominalSampleRate (double newSampleRate) const
    {
        if (std::abs (getNominalSampleRate() - newSampleRate) < 1.0)
            return true;

        return audioObjectSetProperty (deviceID, { kAudioDevicePropertyNominalSampleRate,
                                                   kAudioObjectPropertyScopeGlobal,
                                                   juceAudioObjectPropertyElementMain },
                                       static_cast<Float64> (newSampleRate), err2log());
    }

    //==============================================================================
    String reopen (const BigInteger& ins, const BigInteger& outs, double newSampleRate, int bufferSizeSamples)
    {
        stop (false);

        if (! setNominalSampleRate (newSampleRate))
        {
            updateDetailsFromDevice (ins, outs);
            return "Couldn't change sample rate";
        }

        if (! audioObjectSetProperty (deviceID, { kAudioDevicePropertyBufferFrameSize,
                                                  kAudioObjectPropertyScopeGlobal,
                                                  juceAudioObjectPropertyElementMain },
                                      static_cast<UInt32> (bufferSizeSamples), err2log()))
        {
            updateDetailsFromDevice (ins, outs);
            return "Couldn't change buffer size";
        }

        // Annoyingly, after changing the rate and buffer size, some devices fail to
        // correctly report their new settings until some random time in the future, so
        // after calling updateDetailsFromDevice, we need to manually bodge these values
        // to make sure we're using the correct numbers..
        updateDetailsFromDevice (ins, outs);

        {
            // smoothie: the bodge trusts the requested size, but
            // updateDetailsFromDevice just allocated temp buffers for the
            // size the device REPORTED. When a device genuinely settled on
            // a different size (an aggregate whose Bluetooth sub-device
            // renegotiated, rather than one that merely lags its
            // reporting), every later callback would write bufferSize
            // frames through capacity sized for the smaller report — the
            // ASan-confirmed heap overflow. Re-allocate for the trusted
            // values, under the same lock updateDetailsFromDevice uses.
            const ScopedLock sl (callbackLock);
            sampleRate = newSampleRate;
            bufferSize = bufferSizeSamples;
            allocateTempBuffers();
        }

        if (sampleRates.size() == 0)
            return "Device has no available sample-rates";

        if (bufferSizes.size() == 0)
            return "Device has no available buffer-sizes";

        return {};
    }

    bool start (AudioIODeviceCallback* callbackToNotify)
    {
        const ScopedLock sl (callbackLock);

        if (callback == nullptr && callbackToNotify != nullptr)
        {
            callback = callbackToNotify;
            callback->audioDeviceAboutToStart (&owner);
        }

        for (auto* stream : getStreams())
            if (stream != nullptr)
                stream->previousSampleTime = invalidSampleTime;

        owner.hadDiscontinuity = false;

        if (scopedProcID.get() == nullptr && deviceID != 0)
        {
            scopedProcID = [&self = *this,
                            &lock = callbackLock,
                            nextProcID = ScopedAudioDeviceIOProcID { *this, deviceID, audioIOProc },
                            dID = deviceID]() mutable -> ScopedAudioDeviceIOProcID
            {
                // It *looks* like AudioDeviceStart may start the audio callback running, and then
                // immediately lock an internal mutex.
                // The same mutex is locked before calling the audioIOProc.
                // If we get very unlucky, then we can end up with thread A taking the callbackLock
                // and calling AudioDeviceStart, followed by thread B taking the CoreAudio lock
                // and calling into audioIOProc, which waits on the callbackLock. When thread A
                // continues it attempts to take the CoreAudio lock, and the program deadlocks.

                if (auto* procID = nextProcID.get())
                {
                    const ScopedUnlock su (lock);

                    if (self.OK (AudioDeviceStart (dID, procID)))
                        return std::move (nextProcID);
                }

                return {};
            }();
        }

        playing = scopedProcID.get() != nullptr && callback != nullptr;

        return scopedProcID.get() != nullptr;
    }

    AudioIODeviceCallback* stop (bool leaveInterruptRunning)
    {
        const ScopedLock sl (callbackLock);

        auto result = std::exchange (callback, nullptr);

        if (scopedProcID.get() != nullptr && (deviceID != 0) && ! leaveInterruptRunning)
        {
            audioDeviceStopPending = true;

            // wait until AudioDeviceStop() has been called on the IO thread
            for (int i = 40; --i >= 0;)
            {
                if (audioDeviceStopPending == false)
                    break;

                const ScopedUnlock ul (callbackLock);
                Thread::sleep (50);
            }

            scopedProcID = {};
            playing = false;
        }

        return result;
    }

    double getSampleRate() const  { return sampleRate; }
    int getBufferSize() const     { return bufferSize; }

    void audioCallback (const AudioTimeStamp* inputTimestamp,
                        const AudioTimeStamp* outputTimestamp,
                        const AudioBufferList* inInputData,
                        AudioBufferList* outOutputData)
    {
        const ScopedLock sl (callbackLock);

        if (audioDeviceStopPending)
        {
            if (OK (AudioDeviceStop (deviceID, scopedProcID.get())))
                audioDeviceStopPending = false;

            return;
        }

        const auto numInputChans  = getChannels (inStream);
        const auto numOutputChans = getChannels (outStream);

        // smoothie: the frame count for THIS callback. `bufferSize` is
        // what we negotiated, but (a) reopen() trusts the requested size
        // before the device reports it (see the "bodge" there) while the
        // temp buffers may have been sized from what the device actually
        // reported, and (b) an aggregate whose sub-device renegotiated
        // (Bluetooth HFP dropping to 16 kHz) delivers a different frame
        // count than negotiated. Writing `bufferSize` frames through
        // either mismatch overflows our temp buffers or the HAL's own
        // buffer lists — heap corruption (ASan-confirmed). Trust only
        // what this callback's buffer lists actually carry, clamped to
        // what we allocated.
        const auto framesFromList = [] (const AudioBufferList* list) -> int
        {
            if (list == nullptr)
                return 0;

            auto frames = std::numeric_limits<int>::max();
            bool any = false;

            for (UInt32 i = 0; i < list->mNumberBuffers; ++i)
            {
                const auto& b = list->mBuffers[i];

                if (b.mNumberChannels > 0)
                {
                    frames = jmin (frames, (int) (b.mDataByteSize
                                   / (b.mNumberChannels * sizeof (float))));
                    any = true;
                }
            }

            return any ? frames : 0;
        };

        auto numFrames = bufferSize;

        if (const auto inFrames = framesFromList (inInputData); inFrames > 0)
            numFrames = jmin (numFrames, inFrames);

        if (const auto outFrames = framesFromList (outOutputData); outFrames > 0)
            numFrames = jmin (numFrames, outFrames);

        if (tempBufferSamples > 0)
            numFrames = jmin (numFrames, tempBufferSamples);

        if (callback != nullptr)
        {
            for (int i = numInputChans; --i >= 0;)
            {
                auto& info = inStream->channelInfo.getReference (i);
                auto dest = inStream->tempBuffers[i];
                auto src = ((const float*) inInputData->mBuffers[info.streamNum].mData) + info.dataOffsetSamples;
                auto stride = info.dataStrideSamples;

                if (stride != 0) // if this is zero, info is invalid
                {
                    for (int j = numFrames; --j >= 0;)
                    {
                        *dest++ = *src;
                        src += stride;
                    }
                }
            }

            for (auto* stream : getStreams())
                if (stream != nullptr)
                    owner.hadDiscontinuity |= stream->checkTimestampsForDiscontinuity (stream == inStream.get() ? inputTimestamp
                                                                                                                : outputTimestamp);

            const auto* timeStamp = numOutputChans > 0 ? outputTimestamp : inputTimestamp;
            const auto nanos = timeStamp != nullptr ? timeConversions.hostTimeToNanos (timeStamp->mHostTime) : 0;
            const AudioIODeviceCallbackContext context
            {
                timeStamp != nullptr ? &nanos : nullptr,
            };

            callback->audioDeviceIOCallbackWithContext (getTempBuffers (inStream),  numInputChans,
                                                        getTempBuffers (outStream), numOutputChans,
                                                        numFrames,
                                                        context);

            for (int i = numOutputChans; --i >= 0;)
            {
                auto& info = outStream->channelInfo.getReference (i);
                auto src = outStream->tempBuffers[i];
                auto dest = ((float*) outOutputData->mBuffers[info.streamNum].mData) + info.dataOffsetSamples;
                auto stride = info.dataStrideSamples;

                if (stride != 0) // if this is zero, info is invalid
                {
                    for (int j = numFrames; --j >= 0;)
                    {
                        *dest = *src++;
                        dest += stride;
                    }
                }
            }
        }
        else
        {
            for (UInt32 i = 0; i < outOutputData->mNumberBuffers; ++i)
                zeromem (outOutputData->mBuffers[i].mData,
                         outOutputData->mBuffers[i].mDataByteSize);
        }

        for (auto* stream : getStreams())
            if (stream != nullptr)
                stream->previousSampleTime += static_cast<Float64> (bufferSize);
    }

    bool isPlaying() const { return playing.load(); }

    //==============================================================================
    struct Stream
    {
        Stream (bool isInput, CoreAudioInternal& parent, const BigInteger& activeRequested)
            : input (isInput),
              latency (getLatencyFromDevice (isInput, parent)),
              bitDepth (getBitDepthFromDevice (isInput, parent)),
              chanNames (getChannelNames (isInput, parent)),
              activeChans ([&activeRequested, clearFrom = chanNames.size()]
                           {
                               auto result = activeRequested;
                               result.setRange (clearFrom, result.getHighestBit() + 1 - clearFrom, false);
                               return result;
                           }()),
              channelInfo (getChannelInfos (isInput, parent, activeChans)),
              channels (static_cast<int> (channelInfo.size()))
        {}

        int allocateTempBuffers (int tempBufSize, int channelCount, HeapBlock<float>& buffer)
        {
            tempBuffers.calloc (channels + 2);

            for (int i = 0; i < channels;  ++i)
                tempBuffers[i] = buffer + channelCount++ * tempBufSize;

            return channels;
        }

        template <typename Visitor>
        static auto visitChannels (bool isInput, CoreAudioInternal& parent, Visitor&& visitor)
        {
            struct Args { int stream, channelIdx, chanNum, streamChannels; };
            using VisitorResultType = typename std::invoke_result_t<Visitor, const Args&>::value_type;
            Array<VisitorResultType> result;
            int chanNum = 0;

            if (auto bufList = audioObjectGetProperty<AudioBufferList> (parent.deviceID, { kAudioDevicePropertyStreamConfiguration,
                                                                                           getScope (isInput),
                                                                                           juceAudioObjectPropertyElementMain }, parent.err2log()))
            {
                const int numStreams = static_cast<int> (bufList->mNumberBuffers);

                for (int i = 0; i < numStreams; ++i)
                {
                    auto& b = bufList->mBuffers[i];

                    for (unsigned int j = 0; j < b.mNumberChannels; ++j)
                    {
                        // Passing an anonymous struct ensures that callback can't confuse the argument order
                        if (auto opt = visitor (Args { i, static_cast<int> (j), chanNum++, static_cast<int> (b.mNumberChannels) }))
                            result.add (std::move (*opt));
                    }
                }
            }

            return result;
        }

        static Array<CallbackDetailsForChannel> getChannelInfos (bool isInput, CoreAudioInternal& parent, const BigInteger& active)
        {
            return visitChannels (isInput, parent,
                                  [&] (const auto& args) -> std::optional<CallbackDetailsForChannel>
                                  {
                                      if (! active[args.chanNum])
                                          return {};

                                      return CallbackDetailsForChannel { args.stream, args.channelIdx, args.streamChannels };
                                  });
        }

        static StringArray getChannelNames (bool isInput, CoreAudioInternal& parent)
        {
            auto names = visitChannels (isInput, parent,
                                        [&] (const auto& args) -> std::optional<String>
                                        {
                                            String name;
                                            const auto element = static_cast<AudioObjectPropertyElement> (args.chanNum + 1);

                                            if (auto nameNSString = audioObjectGetProperty<NSString*> (parent.deviceID, { kAudioObjectPropertyElementName,
                                                                                                                          getScope (isInput),
                                                                                                                          element }).value_or (nullptr))
                                            {
                                                name = nsStringToJuce (nameNSString);
                                                [nameNSString release];
                                            }

                                            if (name.isEmpty())
                                                name << (isInput ? "Input " : "Output ") << (args.chanNum + 1);

                                            return name;
                                        });

            return { names };
        }

        static int getBitDepthFromDevice (bool isInput, CoreAudioInternal& parent)
        {
            return static_cast<int> (audioObjectGetProperty<AudioStreamBasicDescription> (parent.deviceID, { kAudioStreamPropertyPhysicalFormat,
                                                                                                             getScope (isInput),
                                                                                                             juceAudioObjectPropertyElementMain }, parent.err2log())
                                                                                         .value_or (AudioStreamBasicDescription{}).mBitsPerChannel);
        }

        static int getLatencyFromDevice (bool isInput, CoreAudioInternal& parent)
        {
            const auto scope = getScope (isInput);

            const auto deviceLatency  = audioObjectGetProperty<UInt32> (parent.deviceID, { kAudioDevicePropertyLatency,
                                                                                           scope,
                                                                                           juceAudioObjectPropertyElementMain }).value_or (0);

            const auto safetyOffset   = audioObjectGetProperty<UInt32> (parent.deviceID, { kAudioDevicePropertySafetyOffset,
                                                                                           scope,
                                                                                           juceAudioObjectPropertyElementMain }).value_or (0);

            const auto framesInBuffer = audioObjectGetProperty<UInt32> (parent.deviceID, { kAudioDevicePropertyBufferFrameSize,
                                                                                           kAudioObjectPropertyScopeWildcard,
                                                                                           juceAudioObjectPropertyElementMain }).value_or (0);

            UInt32 streamLatency = 0;

            if (auto streams = audioObjectGetProperties<AudioStreamID> (parent.deviceID, { kAudioDevicePropertyStreams,
                                                                                           scope,
                                                                                           juceAudioObjectPropertyElementMain }); ! streams.empty())
                streamLatency = audioObjectGetProperty<UInt32> (streams.front(), { kAudioStreamPropertyLatency,
                                                                                   scope,
                                                                                   juceAudioObjectPropertyElementMain }).value_or (0);

            return static_cast<int> (deviceLatency + safetyOffset + framesInBuffer + streamLatency);
        }

        bool checkTimestampsForDiscontinuity (const AudioTimeStamp* timestamp) noexcept
        {
            if (channels > 0)
            {
                jassert (timestamp == nullptr || (((timestamp->mFlags & kAudioTimeStampSampleTimeValid) != 0)
                                               && ((timestamp->mFlags & kAudioTimeStampHostTimeValid)   != 0)));

                if (exactlyEqual (previousSampleTime, invalidSampleTime))
                    previousSampleTime = timestamp != nullptr ? timestamp->mSampleTime : 0.0;

                if (timestamp != nullptr && std::fabs (previousSampleTime - timestamp->mSampleTime) >= 1.0)
                {
                    previousSampleTime = timestamp->mSampleTime;
                    return true;
                }
            }

            return false;
        }

        //==============================================================================
        const bool input;
        const int latency;
        const int bitDepth;
        const StringArray chanNames;
        const BigInteger activeChans;
        const Array<CallbackDetailsForChannel> channelInfo;
        const int channels = 0;
        Float64 previousSampleTime;

        HeapBlock<float*> tempBuffers;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Stream)
    };

    template <typename Callback>
    static auto getWithDefault (const std::unique_ptr<Stream>& ptr, Callback&& callback)
    {
        return ptr != nullptr ? callback (*ptr) : decltype (callback (*ptr)) {};
    }

    template <typename Value>
    static auto getWithDefault (const std::unique_ptr<Stream>& ptr, Value (Stream::* member))
    {
        return getWithDefault (ptr, [&] (Stream& s) { return s.*member; });
    }

    static int          getLatency          (const std::unique_ptr<Stream>& ptr) { return getWithDefault (ptr, &Stream::latency); }
    static int          getBitDepth         (const std::unique_ptr<Stream>& ptr) { return getWithDefault (ptr, &Stream::bitDepth); }
    static int          getChannels         (const std::unique_ptr<Stream>& ptr) { return getWithDefault (ptr, &Stream::channels); }
    static int          getNumChannelNames  (const std::unique_ptr<Stream>& ptr) { return getWithDefault (ptr, &Stream::chanNames).size(); }
    static String       getChannelNames     (const std::unique_ptr<Stream>& ptr) { return getWithDefault (ptr, &Stream::chanNames).joinIntoString (" "); }
    static BigInteger   getActiveChannels   (const std::unique_ptr<Stream>& ptr) { return getWithDefault (ptr, &Stream::activeChans); }
    static float**      getTempBuffers      (const std::unique_ptr<Stream>& ptr) { return getWithDefault (ptr, [] (auto& s) { return s.tempBuffers.get(); }); }

    //==============================================================================
    static constexpr Float64 invalidSampleTime = std::numeric_limits<Float64>::max();

    CoreAudioIODevice& owner;
    int bitDepth = 32;
    int xruns = 0;
    Array<double> sampleRates;
    Array<int> bufferSizes;
    AudioDeviceID deviceID;
    std::unique_ptr<Stream> inStream, outStream;

    AudioWorkgroup audioWorkgroup;

private:
    class ScopedAudioDeviceIOProcID
    {
    public:
        ScopedAudioDeviceIOProcID() = default;

        ScopedAudioDeviceIOProcID (CoreAudioInternal& coreAudio, AudioDeviceID d, AudioDeviceIOProc audioIOProc)
            : deviceID (d)
        {
            if (! coreAudio.OK (AudioDeviceCreateIOProcID (deviceID, audioIOProc, &coreAudio, &proc)))
                proc = {};
        }

        ~ScopedAudioDeviceIOProcID() noexcept
        {
            if (proc != AudioDeviceIOProcID{})
                AudioDeviceDestroyIOProcID (deviceID, proc);
        }

        ScopedAudioDeviceIOProcID (ScopedAudioDeviceIOProcID&& other) noexcept
        {
            swap (other);
        }

        ScopedAudioDeviceIOProcID& operator= (ScopedAudioDeviceIOProcID&& other) noexcept
        {
            ScopedAudioDeviceIOProcID { std::move (other) }.swap (*this);
            return *this;
        }

        AudioDeviceIOProcID get() const { return proc; }

    private:
        void swap (ScopedAudioDeviceIOProcID& other) noexcept
        {
            std::swap (other.deviceID, deviceID);
            std::swap (other.proc, proc);
        }

        AudioDeviceID deviceID = {};
        AudioDeviceIOProcID proc = {};
    };

    //==============================================================================
    ScopedAudioDeviceIOProcID scopedProcID;
    CoreAudioTimeConversions timeConversions;
    AudioIODeviceCallback* callback = nullptr;
    CriticalSection callbackLock;
    bool audioDeviceStopPending = false;
    std::atomic<bool> playing { false };
    double sampleRate = 0;
    int bufferSize = 0;
    // smoothie: per-channel capacity of audioBuffer's slices, set by
    // allocateTempBuffers — the callback clamp reads it (see there).
    int tempBufferSamples = 0;
    HeapBlock<float> audioBuffer;

    static OSStatus audioIOProc (AudioDeviceID /*inDevice*/,
                                 [[maybe_unused]] const AudioTimeStamp* inNow,
                                 const AudioBufferList* inInputData,
                                 const AudioTimeStamp* inInputTime,
                                 AudioBufferList* outOutputData,
                                 const AudioTimeStamp* inOutputTime,
                                 void* device)
    {
        static_cast<CoreAudioInternal*> (device)->audioCallback (inInputTime, inOutputTime, inInputData, outOutputData);
        return noErr;
    }

    static OSStatus deviceListenerProc (AudioDeviceID /*inDevice*/,
                                        UInt32 numAddresses,
                                        const AudioObjectPropertyAddress* pa,
                                        void* inClientData)
    {
        // smoothie: fenced — inClientData may belong to an instance
        // whose destruction has begun (see LiveInternalRegistry).
        LiveInternalRegistry::get().ifLive (inClientData, [&]
        {
            auto& intern = *static_cast<CoreAudioInternal*> (inClientData);

            const auto xruns = std::count_if (pa, pa + numAddresses, [] (const AudioObjectPropertyAddress& x)
            {
                return x.mSelector == kAudioDeviceProcessorOverload;
            });

            intern.xruns += xruns;

            const auto detailsChanged = std::any_of (pa, pa + numAddresses, [] (const AudioObjectPropertyAddress& x)
            {
                constexpr UInt32 selectors[]
                {
                    kAudioDevicePropertyBufferSize,
                    kAudioDevicePropertyBufferFrameSize,
                    kAudioDevicePropertyNominalSampleRate,
                    kAudioDevicePropertyStreamFormat,
                    kAudioDevicePropertyDeviceIsAlive,
                    kAudioStreamPropertyPhysicalFormat,
                };

                return std::find (std::begin (selectors), std::end (selectors), x.mSelector) != std::end (selectors);
            });

            const auto requestedRestart = std::any_of (pa, pa + numAddresses, [] (const AudioObjectPropertyAddress& x)
            {
                constexpr UInt32 selectors[]
                {
                    kAudioDevicePropertyDeviceHasChanged,
                    kAudioObjectPropertyOwnedObjects,
                };

                return std::find (std::begin (selectors), std::end (selectors), x.mSelector) != std::end (selectors);
            });

            // smoothie: the owner hears of it through the sink, here, on
            // CoreAudio's thread, and looks at the device itself
            // (readLiveState). The device does nothing about it.
            if (detailsChanged || requestedRestart)
                intern.owner.reportOpenDeviceChange();
        });

        return noErr;
    }

    //==============================================================================
    bool OK (const OSStatus errorCode) const
    {
        if (errorCode == noErr)
            return true;

        const String errorMessage ("CoreAudio error: " + String::toHexString ((int) errorCode));
        JUCE_COREAUDIOLOG (errorMessage);

        if (callback != nullptr)
            callback->audioDeviceError (errorMessage);

        return false;
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CoreAudioInternal)
};


//==============================================================================
class CoreAudioIODevice final : public AudioIODevice
{
public:
    CoreAudioIODevice (CoreAudioIODeviceType* dt,
                       const String& deviceName,
                       AudioDeviceID inputDeviceId,
                       AudioDeviceID outputDeviceId)
        : AudioIODevice (deviceName, "CoreAudio"),
          deviceType (dt)
    {
        internal = [this, &inputDeviceId, &outputDeviceId]
        {
            if (outputDeviceId == 0 || outputDeviceId == inputDeviceId)
            {
                jassert (inputDeviceId != 0);
                return std::make_unique<CoreAudioInternal> (*this, inputDeviceId, true, outputDeviceId != 0);
            }

            return std::make_unique<CoreAudioInternal> (*this, outputDeviceId, false, true);
        }();

        jassert (internal != nullptr);
    }

    ~CoreAudioIODevice() override
    {
        // smoothie: detach the HAL listener BEFORE teardown starts —
        // deregistration blocks on in-flight listener bodies, so none reaches
        // the sink through a half-destroyed device (Bluetooth churn fires the
        // listener exactly when devices are being torn down).
        internal->detachListener();

        close();
    }

    StringArray getOutputChannelNames() override        { return internal->outStream != nullptr ? internal->outStream->chanNames : StringArray(); }
    StringArray getInputChannelNames() override         { return internal->inStream  != nullptr ? internal->inStream ->chanNames : StringArray(); }

    bool isOpen() override                              { return isOpen_; }

    Array<double> getAvailableSampleRates() override    { return internal->sampleRates; }
    Array<int> getAvailableBufferSizes() override       { return internal->bufferSizes; }

    double getCurrentSampleRate() override              { return internal->getSampleRate(); }
    int getCurrentBitDepth() override                   { return internal->bitDepth; }
    int getCurrentBufferSizeSamples() override          { return internal->getBufferSize(); }
    int getXRunCount() const noexcept override          { return internal->xruns; }

    int getIndexOfDevice (bool asInput) const           { return deviceType->getDeviceNames (asInput).indexOf (getName()); }

    int getDefaultBufferSize() override
    {
        int best = 0;

        for (int i = 0; best < 512 && i < internal->bufferSizes.size(); ++i)
            best = internal->bufferSizes.getUnchecked (i);

        if (best == 0)
            best = 512;

        return best;
    }

    String open (const BigInteger& inputChannels,
                 const BigInteger& outputChannels,
                 double sampleRate, int bufferSizeSamples) override
    {
        isOpen_ = true;
        internal->xruns = 0;

        if (bufferSizeSamples <= 0)
            bufferSizeSamples = getDefaultBufferSize();

        if (sampleRate <= 0)
            sampleRate = internal->getNominalSampleRate();

        lastError = internal->reopen (inputChannels, outputChannels, sampleRate, bufferSizeSamples);
        JUCE_COREAUDIOLOG ("Opened: " << getName());

        isOpen_ = lastError.isEmpty();

        return lastError;
    }

    void close() override
    {
        isOpen_ = false;
        internal->stop (false);
    }

    BigInteger getActiveOutputChannels()      const override { return CoreAudioInternal::getActiveChannels (internal->outStream); }
    BigInteger getActiveInputChannels()       const override { return CoreAudioInternal::getActiveChannels (internal->inStream); }
    int getOutputLatencyInSamples()                 override { return CoreAudioInternal::getLatency (internal->outStream); }
    int getInputLatencyInSamples()                  override { return CoreAudioInternal::getLatency (internal->inStream); }

    void start (AudioIODeviceCallback* callback) override
    {
        internal->start (callback);
    }

    void stop() override
    {
        if (auto* lastCallback = internal->stop (true))
            lastCallback->audioDeviceStopped();
    }

    AudioWorkgroup getWorkgroup() const override
    {
        return internal->audioWorkgroup;
    }

    bool isPlaying() override
    {
        return internal->isPlaying();
    }

    String getLastError() override
    {
        return lastError;
    }

    bool setCurrentSampleRate (double newSampleRate)
    {
        return internal->setNominalSampleRate (newSampleRate);
    }

    // smoothie: a change to this device, to its type's sink. The type
    // outlives its devices, and the listener reaching here is fenced
    // against this device's teardown (LiveInternalRegistry).
    void reportOpenDeviceChange()
    {
        if (deviceType != nullptr)
            deviceType->reportOpenDeviceChange();
    }

    LiveState readLiveState() override
    {
        LiveState state;
        state.alive = internal->isDeviceAlive();

        if (state.alive)
        {
            state.sampleRate        = internal->getNominalSampleRate();
            state.numOutputChannels = internal->outStream != nullptr ? CoreAudioInternal::getNumChannels (internal->deviceID, false) : 0;
            state.numInputChannels  = internal->inStream  != nullptr ? CoreAudioInternal::getNumChannels (internal->deviceID, true)  : 0;
        }

        return state;
    }

    WeakReference<CoreAudioIODeviceType> deviceType;
    bool hadDiscontinuity;

private:
    std::unique_ptr<CoreAudioInternal> internal;
    bool isOpen_ = false;
    String lastError;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CoreAudioIODevice)
};


//==============================================================================
// smoothie addition: what CoreAudio says of a device by its ID alone — no
// CoreAudioIODevice, nothing opened.
struct CoreAudioDeviceFacts
{
    static Array<double> sampleRates (AudioDeviceID deviceID)
    {
        Array<double> rates;

        const auto ranges = audioObjectGetProperties<AudioValueRange> (deviceID, { kAudioDevicePropertyAvailableNominalSampleRates,
                                                                                   kAudioObjectPropertyScopeWildcard,
                                                                                   juceAudioObjectPropertyElementMain });
        for (const auto rate : SampleRateHelpers::getAllSampleRates())
            for (auto range = ranges.rbegin(); range != ranges.rend(); ++range)
                if (range->mMinimum - 2 <= rate && rate <= range->mMaximum + 2)
                {
                    rates.add (rate);
                    break;
                }

        if (const auto nominal = nominalSampleRate (deviceID); nominal > 0 && ! rates.contains (nominal))
            rates.add (nominal);

        return rates;
    }

    static Array<int> bufferSizes (AudioDeviceID deviceID)
    {
        Array<int> sizes;

        if (const auto ranges = audioObjectGetProperties<AudioValueRange> (deviceID, { kAudioDevicePropertyBufferFrameSizeRange,
                                                                                       kAudioObjectPropertyScopeWildcard,
                                                                                       juceAudioObjectPropertyElementMain });
            ! ranges.empty())
        {
            sizes.add ((int) (ranges[0].mMinimum + 15) & ~15);

            for (int i = 32; i <= 2048; i += 32)
                for (auto range = ranges.rbegin(); range != ranges.rend(); ++range)
                    if (i >= range->mMinimum && i <= range->mMaximum)
                    {
                        sizes.addIfNotAlreadyThere (i);
                        break;
                    }
        }

        return sizes;
    }

    static double nominalSampleRate (AudioDeviceID deviceID)
    {
        return audioObjectGetProperty<Float64> (deviceID, { kAudioDevicePropertyNominalSampleRate,
                                                            kAudioObjectPropertyScopeGlobal,
                                                            juceAudioObjectPropertyElementMain }).value_or (0.0);
    }

    static String uid (AudioDeviceID deviceID)
    {
        if (const auto uid = audioObjectGetProperty<CFStringRef> (deviceID, { kAudioDevicePropertyDeviceUID,
                                                                              kAudioObjectPropertyScopeGlobal,
                                                                              juceAudioObjectPropertyElementMain }))
            if (const CFUniquePtr<CFStringRef> owned { *uid })
                return String::fromCFString (owned.get());

        return {};
    }

    static UInt32 transport (AudioDeviceID deviceID)
    {
        return audioObjectGetProperty<UInt32> (deviceID, { kAudioDevicePropertyTransportType,
                                                           kAudioObjectPropertyScopeGlobal,
                                                           juceAudioObjectPropertyElementMain }).value_or (0);
    }

    static bool isAlive (AudioDeviceID deviceID)
    {
        return audioObjectGetProperty<UInt32> (deviceID, { kAudioDevicePropertyDeviceIsAlive,
                                                           kAudioObjectPropertyScopeWildcard,
                                                           juceAudioObjectPropertyElementMain }).value_or (0) != 0;
    }
};

//==============================================================================
// smoothie addition: an aggregate device of one output device and one input
// device, so the two play to one clock — the output's channels first, a
// hardware clock as master, drift compensation across clock domains. Made
// when its CoreAudioPairedDevice opens and destroyed when it closes, after
// the device made on it.
//
// Public (the HAL lists it, as "<client>#<n>"): the private flag leaves
// drift compensation half-applied on some macOS versions. This type never
// lists one (isOurs), and one a crashed process left behind is destroyed by
// the next type made (destroyLeftovers).
class CoreAudioAggregate final
{
public:
    static std::unique_ptr<CoreAudioAggregate> create (AudioDeviceID outputID, AudioDeviceID inputID,
                                                       const String& clientName, String& error)
    {
        const auto outputUID = CoreAudioDeviceFacts::uid (outputID);
        const auto inputUID  = CoreAudioDeviceFacts::uid (inputID);

        if (outputUID.isEmpty() || inputUID.isEmpty())
        {
            error = "the devices to pair have no UIDs";
            return nullptr;
        }

        // A new UID every time: one reused makes a reopen look like the same
        // device, which then starts with stale state and calls back nothing.
        static std::atomic<int> counter { 0 };
        const auto n    = ++counter;
        const auto uid  = uidPrefix() + String ((int) getpid()) + "." + String (n);
        const auto name = (clientName.isNotEmpty() ? clientName : String ("smoothie")) + "#" + String (n);

        const void* keys[]   = { CFSTR (kAudioAggregateDeviceUIDKey), CFSTR (kAudioAggregateDeviceNameKey) };
        const void* values[] = { uid.toCFString(), name.toCFString() };
        CFUniquePtr<CFDictionaryRef> description { CFDictionaryCreate (nullptr, keys, values, 2,
                                                                       &kCFTypeDictionaryKeyCallBacks,
                                                                       &kCFTypeDictionaryValueCallBacks) };
        CFRelease ((CFStringRef) values[0]);
        CFRelease ((CFStringRef) values[1]);

        AudioObjectID aggregateID = kAudioObjectUnknown;

        if (const auto status = AudioHardwareCreateAggregateDevice (description.get(), &aggregateID); status != noErr)
        {
            error = "the aggregate device could not be made (" + String ((int) status) + ")";
            return nullptr;
        }

        std::unique_ptr<CoreAudioAggregate> aggregate (new CoreAudioAggregate (aggregateID, name));
        settle();

        // The output first: CoreAudio lays the aggregate's channels out in
        // sub-device order, and the engine writes from channel 0.
        CFUniquePtr<CFStringRef> outUID { outputUID.toCFString() }, inUID { inputUID.toCFString() };
        CFMutableArrayRef subDevices = CFArrayCreateMutable (nullptr, 0, &kCFTypeArrayCallBacks);
        CFArrayAppendValue (subDevices, outUID.get());
        CFArrayAppendValue (subDevices, inUID.get());
        const auto setList = audioObjectSetProperty (aggregateID, { kAudioAggregateDevicePropertyFullSubDeviceList,
                                                                    kAudioObjectPropertyScopeGlobal,
                                                                    juceAudioObjectPropertyElementMain },
                                                     (CFArrayRef) subDevices);
        CFRelease (subDevices);

        if (! setList)
        {
            error = "the aggregate device's sub-devices could not be set";
            return nullptr;
        }

        settle();

        // The master must keep a hardware clock: a virtual device's is the
        // scheduler's, and drift compensation crashes on it. The input is
        // tried first (it is usually the hardware side), unless it alone is
        // virtual.
        const bool inputVirtual  = CoreAudioDeviceFacts::transport (inputID)  == kAudioDeviceTransportTypeVirtual;
        const bool outputVirtual = CoreAudioDeviceFacts::transport (outputID) == kAudioDeviceTransportTypeVirtual;
        CFStringRef masterUID = (inputVirtual && ! outputVirtual) ? outUID.get() : inUID.get();
        const AudioObjectPropertyAddress masterAddress { kAudioAggregateDevicePropertyMainSubDevice,
                                                         kAudioObjectPropertyScopeGlobal,
                                                         juceAudioObjectPropertyElementMain };

        if (! audioObjectSetProperty (aggregateID, masterAddress, masterUID))
        {
            masterUID = masterUID == inUID.get() ? outUID.get() : inUID.get();

            if (! audioObjectSetProperty (aggregateID, masterAddress, masterUID))
            {
                error = "no master clock could be set on the aggregate device";
                return nullptr;
            }
        }

        settle();

        // Drift compensation where the two keep different clocks (separate
        // USB/PCI clocks), or can't say; not where they share one (built-in
        // speakers and microphone).
        const auto clockDomain = [] (AudioDeviceID id)
        {
            return audioObjectGetProperty<UInt32> (id, { kAudioDevicePropertyClockDomain,
                                                         kAudioObjectPropertyScopeGlobal,
                                                         juceAudioObjectPropertyElementMain });
        };
        const auto outClock = clockDomain (outputID), inClock = clockDomain (inputID);
        aggregate->driftCompensated = ! outClock || ! inClock || *outClock != *inClock;

        if (aggregate->driftCompensated)
            aggregate->compensateDrift (masterUID);

        settle();
        Logger::writeToLog ("[aggregate] made '" + name + "' of '" + String (outputUID) + "' (out) and '"
                            + String (inputUID) + "' (in)"
                            + (aggregate->driftCompensated ? ", drift compensated" : ""));
        return aggregate;
    }

    ~CoreAudioAggregate()
    {
        AudioHardwareDestroyAggregateDevice (aggregateID);
    }

    AudioDeviceID getID() const noexcept        { return aggregateID; }
    bool isDriftCompensated() const noexcept    { return driftCompensated; }

    // An aggregate device this code made, in this process or another.
    static bool isOurs (AudioDeviceID deviceID)
    {
        const auto uid = CoreAudioDeviceFacts::uid (deviceID);
        return uid.startsWith (uidPrefix()) || isLegacy (uid);
    }

    // Ours, made by a process no longer running (a crash leaves its
    // aggregate behind, in Audio MIDI Setup and in every app's device list).
    static void destroyLeftovers()
    {
        for (const auto device : audioObjectGetProperties<AudioDeviceID> (kAudioObjectSystemObject, { kAudioHardwarePropertyDevices,
                                                                                                      kAudioObjectPropertyScopeWildcard,
                                                                                                      juceAudioObjectPropertyElementMain }))
        {
            const auto uid = CoreAudioDeviceFacts::uid (device);
            const bool ours = uid.startsWith (uidPrefix());

            if (! ours && ! isLegacy (uid))
                continue;

            const auto pid = uid.fromFirstOccurrenceOf (uidPrefix(), false, false).upToFirstOccurrenceOf (".", false, false).getIntValue();

            if (ours && pid > 0 && (kill ((pid_t) pid, 0) == 0 || errno != ESRCH))
                continue;   // its process is still running

            // A legacy one names no process: one an older app is playing on now is its.
            if (! ours && audioObjectGetProperty<UInt32> (device, { kAudioDevicePropertyDeviceIsRunningSomewhere,
                                                                    kAudioObjectPropertyScopeGlobal,
                                                                    juceAudioObjectPropertyElementMain }).value_or (0) != 0)
                continue;

            Logger::writeToLog ("[aggregate] destroying a leftover aggregate device (" + uid + ")");
            AudioHardwareDestroyAggregateDevice (device);
        }
    }

private:
    CoreAudioAggregate (AudioDeviceID id, String nameIn) : aggregateID (id), name (std::move (nameIn)) {}

    static String uidPrefix()       { return "smoothie.aggregate."; }

    // What our aggregates were named before they were smoothie's: Clockwork's
    // "clockwork.<product>.aggregate.<n>", SuperSonic's
    // "com.sonicpi.supersonic.aggregate[.<n>]" (Sonic Pi's releases), and
    // "net.sonic-pi.<product>.aggregate.<n>" (development builds).
    static bool isLegacy (const String& uid)
    {
        return ((uid.startsWith ("clockwork.") || uid.startsWith ("net.sonic-pi.")) && uid.contains (".aggregate"))
            || uid.startsWith ("com.sonicpi.supersonic.aggregate");
    }

    // CoreAudio applies an aggregate's configuration asynchronously; the
    // next step waits this long, as every aggregate tool does.
    static void settle()    { Thread::sleep (100); }

    void compensateDrift (CFStringRef masterUID)
    {
        // Only the sub-devices (not clocks or taps), and every one but the
        // master: it is the clock the others are compensated to. They can
        // take a moment to appear, a virtual one longest.
        const AudioObjectPropertyAddress owned { kAudioObjectPropertyOwnedObjects,
                                                 kAudioObjectPropertyScopeGlobal,
                                                 juceAudioObjectPropertyElementMain };
        AudioClassID subDeviceClass = kAudioSubDeviceClassID;
        UInt32 size = 0;

        for (int i = 0; i < 10; ++i)
        {
            if (AudioObjectGetPropertyDataSize (aggregateID, &owned, sizeof (subDeviceClass), &subDeviceClass, &size) == noErr && size > 0)
                break;

            settle();
        }

        std::vector<AudioObjectID> subDevices (size / sizeof (AudioObjectID));

        if (subDevices.empty()
            || AudioObjectGetPropertyData (aggregateID, &owned, sizeof (subDeviceClass), &subDeviceClass, &size, subDevices.data()) != noErr)
        {
            Logger::writeToLog ("[aggregate] drift compensation skipped: no sub-devices after 1 s");
            return;
        }

        for (const auto subDevice : subDevices)
        {
            const auto uid = CoreAudioDeviceFacts::uid (subDevice);

            if (uid.isEmpty() || uid == String::fromCFString (masterUID))
                continue;

            audioObjectSetProperty (subDevice, { kAudioSubDevicePropertyDriftCompensation,
                                                 kAudioObjectPropertyScopeGlobal,
                                                 juceAudioObjectPropertyElementMain },
                                    (UInt32) 1);
        }
    }

    AudioDeviceID aggregateID;
    String name;
    bool driftCompensated = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CoreAudioAggregate)
};

//==============================================================================
// smoothie addition: an output device and a separate input device as one
// device, on an aggregate device of the two (CoreAudioAggregate) — what
// CoreAudio offers for playing and recording on separate hardware, in place
// of JUCE's AudioIODeviceCombiner (which the #3554 crash came from). Named
// after its output; its outputs are the output device's, its inputs the
// input device's.
class CoreAudioPairedDevice final : public AudioIODevice
{
public:
    CoreAudioPairedDevice (CoreAudioIODeviceType* type, const String& outputName, const String& inputNameIn,
                           AudioDeviceID outputIDIn, AudioDeviceID inputIDIn, const String& clientNameIn)
        : AudioIODevice (outputName, "CoreAudio"),
          deviceType (type),
          inputName (inputNameIn),
          outputID (outputIDIn),
          inputID (inputIDIn),
          clientName (clientNameIn)
    {
        // Described from its two devices, before any aggregate exists.
        CoreAudioIODevice output (type, outputName, 0, outputID);
        CoreAudioIODevice input  (type, inputName, inputID, 0);
        outputChannelNames = output.getOutputChannelNames();
        inputChannelNames  = input.getInputChannelNames();
        defaultBufferSize  = output.getDefaultBufferSize();
        bufferSizes        = output.getAvailableBufferSizes();

        // The rates both offer; the output's, when they share none (the
        // engine's buffer converts the input's).
        const auto outRates = output.getAvailableSampleRates();
        const auto inRates  = input.getAvailableSampleRates();

        for (const auto rate : outRates)
            if (inRates.contains (rate))
                sampleRates.add (rate);

        if (sampleRates.isEmpty())
            sampleRates = outRates.isEmpty() ? inRates : outRates;

        // In the aggregate the output device's own inputs come first.
        inputOffset = CoreAudioInternal::getNumChannels (outputID, true);
    }

    ~CoreAudioPairedDevice() override { close(); }

    String getInputDeviceName() const       { return inputName; }

    StringArray getOutputChannelNames() override        { return outputChannelNames; }
    StringArray getInputChannelNames() override         { return inputChannelNames; }
    Array<double> getAvailableSampleRates() override    { return sampleRates; }
    int getDefaultBufferSize() override                 { return driftFloor (defaultBufferSize); }

    Array<int> getAvailableBufferSizes() override
    {
        // A drift-compensated aggregate needs 256 frames or more: below it,
        // CoreAudio's resampler starves.
        Array<int> sizes;

        for (const auto size : bufferSizes)
            if (size == driftFloor (size))
                sizes.add (size);

        return sizes.isEmpty() ? bufferSizes : sizes;
    }

    String open (const BigInteger& inputChannels, const BigInteger& outputChannels,
                 double sampleRate, int bufferSizeSamples) override
    {
        close();

        const auto rate = alignRates (sampleRate);
        lastError.clear();
        aggregate = CoreAudioAggregate::create (outputID, inputID, clientName, lastError);

        if (aggregate == nullptr)
            return lastError;

        inner = std::make_unique<CoreAudioIODevice> (deviceType, getName(), aggregate->getID(), aggregate->getID());

        BigInteger outs = outputChannels;
        outs.setRange (outputChannelNames.size(), 256, false);

        BigInteger ins;
        for (int i = 0; i < inputChannelNames.size(); ++i)
            if (inputChannels[i])
                ins.setBit (i + inputOffset);

        lastError = inner->open (ins, outs, rate, driftFloor (bufferSizeSamples));

        if (lastError.isNotEmpty())
            close();

        return lastError;
    }

    void close() override
    {
        inner.reset();       // the device before the aggregate it is on
        aggregate.reset();
    }

    bool isOpen() override                         { return inner != nullptr && inner->isOpen(); }
    void start (AudioIODeviceCallback* callback) override   { if (inner != nullptr) inner->start (callback); }
    void stop() override                           { if (inner != nullptr) inner->stop(); }
    bool isPlaying() override                      { return inner != nullptr && inner->isPlaying(); }
    String getLastError() override                 { return lastError; }

    int getCurrentBufferSizeSamples() override     { return inner != nullptr ? inner->getCurrentBufferSizeSamples() : 0; }
    double getCurrentSampleRate() override         { return inner != nullptr ? inner->getCurrentSampleRate() : 0.0; }
    int getCurrentBitDepth() override              { return inner != nullptr ? inner->getCurrentBitDepth() : 32; }
    int getOutputLatencyInSamples() override       { return inner != nullptr ? inner->getOutputLatencyInSamples() : 0; }
    int getInputLatencyInSamples() override        { return inner != nullptr ? inner->getInputLatencyInSamples() : 0; }
    int getXRunCount() const noexcept override     { return inner != nullptr ? inner->getXRunCount() : -1; }
    AudioWorkgroup getWorkgroup() const override   { return inner != nullptr ? inner->getWorkgroup() : AudioWorkgroup{}; }

    BigInteger getActiveOutputChannels() const override
    {
        return inner != nullptr ? inner->getActiveOutputChannels() : BigInteger();
    }

    BigInteger getActiveInputChannels() const override
    {
        BigInteger active;

        if (inner != nullptr)
            for (int i = 0; i < inputChannelNames.size(); ++i)
                if (inner->getActiveInputChannels()[i + inputOffset])
                    active.setBit (i);

        return active;
    }

    // Alive while both its devices are; their channels and the output's
    // rate as CoreAudio has them now.
    LiveState readLiveState() override
    {
        LiveState state;
        state.alive = CoreAudioDeviceFacts::isAlive (outputID) && CoreAudioDeviceFacts::isAlive (inputID);

        if (state.alive)
        {
            state.sampleRate        = CoreAudioDeviceFacts::nominalSampleRate (outputID);
            state.numOutputChannels = CoreAudioInternal::getNumChannels (outputID, false);
            state.numInputChannels  = CoreAudioInternal::getNumChannels (inputID, true);
        }

        return state;
    }

private:
    int driftFloor (int frames) const
    {
        return aggregate != nullptr && aggregate->isDriftCompensated() ? jmax (frames, 256) : frames;
    }

    // Both devices to the rate before the aggregate is made of them: one left
    // at another rate makes the aggregate resample inside its IO (audible
    // distortion) or stop within a callback. A device that refuses keeps its
    // own; the output's then rules, as the clock the music plays to.
    double alignRates (double wanted)
    {
        if (wanted <= 0)
            wanted = CoreAudioDeviceFacts::nominalSampleRate (outputID);

        for (const auto id : { inputID, outputID })
        {
            if (std::abs (CoreAudioDeviceFacts::nominalSampleRate (id) - wanted) < 1.0)
                continue;

            audioObjectSetProperty (id, { kAudioDevicePropertyNominalSampleRate,
                                          kAudioObjectPropertyScopeGlobal,
                                          juceAudioObjectPropertyElementMain },
                                    (Float64) wanted);

            for (int i = 0; i < 100 && std::abs (CoreAudioDeviceFacts::nominalSampleRate (id) - wanted) >= 1.0; ++i)
                Thread::sleep (10);
        }

        const auto out = CoreAudioDeviceFacts::nominalSampleRate (outputID);
        const auto in  = CoreAudioDeviceFacts::nominalSampleRate (inputID);

        if (std::abs (out - wanted) >= 1.0 || std::abs (in - wanted) >= 1.0)
            Logger::writeToLog ("[aggregate] asked " + String (wanted) + " Hz; output at " + String (out)
                                + ", input at " + String (in) + " — running at the output's");

        return out > 0 ? out : (in > 0 ? in : wanted);
    }

    CoreAudioIODeviceType* deviceType;
    String inputName;
    AudioDeviceID outputID, inputID;
    String clientName;
    StringArray outputChannelNames, inputChannelNames;
    Array<double> sampleRates;
    Array<int> bufferSizes;
    int defaultBufferSize = 512;
    int inputOffset = 0;
    String lastError;
    std::unique_ptr<CoreAudioAggregate> aggregate;   // declared first: destroyed last
    std::unique_ptr<CoreAudioIODevice> inner;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CoreAudioPairedDevice)
};


//==============================================================================
class CoreAudioIODeviceType final : public AudioIODeviceType
{
public:
    CoreAudioIODeviceType()  : AudioIODeviceType ("CoreAudio")
    {
        CoreAudioAggregate::destroyLeftovers();

        // smoothie: the HAL's notifications on its own threads, not a run
        // loop's — the listeners below deliver to the sink from there, and
        // nothing need pump a main run loop for them.
        CFRunLoopRef noRunLoop = nullptr;
        audioObjectSetProperty (kAudioObjectSystemObject, { kAudioHardwarePropertyRunLoop,
                                                            kAudioObjectPropertyScopeGlobal,
                                                            juceAudioObjectPropertyElementMain },
                                noRunLoop);

        AudioObjectPropertyAddress pa;
        pa.mSelector = kAudioHardwarePropertyDevices;
        pa.mScope = kAudioObjectPropertyScopeWildcard;
        pa.mElement = kAudioObjectPropertyElementWildcard;

        // smoothie: same fence as CoreAudioInternal — device types are
        // destroyed on device-manager rebuilds (the embedder's recovery
        // path) with listener bodies possibly in flight.
        LiveInternalRegistry::get().add (this);

        AudioObjectAddPropertyListener (kAudioObjectSystemObject, &pa, hardwareListenerProc, this);

        // smoothie: the system default moving is a change of its own.
        for (auto& address : defaultDeviceAddresses())
            AudioObjectAddPropertyListener (kAudioObjectSystemObject, &address, defaultDeviceListenerProc, this);
    }

    ~CoreAudioIODeviceType() override
    {
        LiveInternalRegistry::get().remove (this);

        AudioObjectPropertyAddress pa;
        pa.mSelector = kAudioHardwarePropertyDevices;
        pa.mScope = kAudioObjectPropertyScopeWildcard;
        pa.mElement = kAudioObjectPropertyElementWildcard;

        AudioObjectRemovePropertyListener (kAudioObjectSystemObject, &pa, hardwareListenerProc, this);

        for (auto& address : defaultDeviceAddresses())
            AudioObjectRemovePropertyListener (kAudioObjectSystemObject, &address, defaultDeviceListenerProc, this);
    }

    //==============================================================================
    // smoothie: the HAL's default output (or input), by the name this type
    // lists it under — or by its own name, when the lists don't have it yet
    // (nothing has scanned them, or the device has only just arrived). Never
    // one of our aggregates: macOS can make one the default for a moment as
    // it is made, and it is no device of this type's.
    String getSystemDefaultDeviceName (bool forInput) const override
    {
        const auto deviceID = defaultDeviceID (forInput);

        if (deviceID == kAudioObjectUnknown || CoreAudioAggregate::isOurs (deviceID))
            return {};

        const auto& ids   = forInput ? inputIds : outputIds;
        const auto& names = forInput ? inputDeviceNames : outputDeviceNames;

        for (int i = 0; i < ids.size(); ++i)
            if (ids.getUnchecked (i) == deviceID)
                return names[i];

        return deviceName (deviceID);
    }

    // smoothie: how the device is connected (its HAL transport type) and
    // what that makes it; its channels from its stream configuration, so
    // nothing is opened to ask.
    DeviceTraits getDeviceTraits (const String& name) const override
    {
        DeviceTraits traits;
        const auto deviceID = deviceIDForName (name);

        if (deviceID == kAudioObjectUnknown)
            return traits;

        const auto transport = audioObjectGetProperty<UInt32> (deviceID, { kAudioDevicePropertyTransportType,
                                                                           kAudioObjectPropertyScopeGlobal,
                                                                           juceAudioObjectPropertyElementMain }).value_or (0);
        traits.wireless          = transport == kAudioDeviceTransportTypeBluetooth
                                || transport == kAudioDeviceTransportTypeBluetoothLE
                                || transport == kAudioDeviceTransportTypeAirPlay
                                || transport == 0x63637764;   // 'ccwd', Continuity Capture (wireless)
        traits.isVirtual         = transport == kAudioDeviceTransportTypeVirtual;
        traits.aggregateClass    = transport == kAudioDeviceTransportTypeAggregate
                                || transport == kAudioDeviceTransportTypeAutoAggregate;
        // A wireless device's codec and its own clock make an aggregate fail,
        // or drop the pair to 16 kHz (#3555); CoreAudio can't nest aggregates.
        traits.pairs             = ! (traits.wireless || traits.aggregateClass);
        traits.numOutputChannels = getNumChannels (deviceID, false);
        traits.numInputChannels  = getNumChannels (deviceID, true);
        traits.sampleRates       = CoreAudioDeviceFacts::sampleRates (deviceID);
        traits.bufferSizes       = CoreAudioDeviceFacts::bufferSizes (deviceID);

        if (transport != 0)
        {
            const char code[] = { (char) (transport >> 24), (char) (transport >> 16),
                                  (char) (transport >> 8),  (char) transport, 0 };
            traits.kind = code;
        }

        return traits;
    }

    //==============================================================================
    void scanForDevices() override
    {
        hasScanned = true;

        inputDeviceNames.clear();
        outputDeviceNames.clear();
        inputIds.clear();
        outputIds.clear();

        auto audioDevices = audioObjectGetProperties<AudioDeviceID> (kAudioObjectSystemObject, { kAudioHardwarePropertyDevices,
                                                                                                 kAudioObjectPropertyScopeWildcard,
                                                                                                 juceAudioObjectPropertyElementMain });

        for (const auto audioDevice : audioDevices)
        {
            // smoothie: our own aggregate devices are a paired device's
            // insides, not a device to offer.
            if (CoreAudioAggregate::isOurs (audioDevice))
                continue;

            if (const auto optionalName = audioObjectGetProperty<CFStringRef> (audioDevice, { kAudioDevicePropertyDeviceNameCFString,
                                                                                              kAudioObjectPropertyScopeWildcard,
                                                                                              juceAudioObjectPropertyElementMain }))
            {
                if (const CFUniquePtr<CFStringRef> name { *optionalName })
                {
                    const auto nameString = String::fromCFString (name.get());

                    if (const auto numIns  = getNumChannels (audioDevice, true); numIns > 0)
                    {
                        inputDeviceNames.add (nameString);
                        inputIds.add (audioDevice);
                    }

                    if (const auto numOuts = getNumChannels (audioDevice, false); numOuts > 0)
                    {
                        outputDeviceNames.add (nameString);
                        outputIds.add (audioDevice);
                    }
                }
            }
        }

        inputDeviceNames.appendNumbersToDuplicates (false, true);
        outputDeviceNames.appendNumbersToDuplicates (false, true);
    }

    StringArray getDeviceNames (bool wantInputNames) const override
    {
        jassert (hasScanned); // need to call scanForDevices() before doing this

        return wantInputNames ? inputDeviceNames
                              : outputDeviceNames;
    }

    int getDefaultDeviceIndex (bool forInput) const override
    {
        jassert (hasScanned); // need to call scanForDevices() before doing this

        // if they're asking for any input channels at all, use the default input, so we
        // get the built-in mic rather than the built-in output with no inputs..

        AudioObjectPropertyAddress pa;
        auto selector = forInput ? kAudioHardwarePropertyDefaultInputDevice
                                 : kAudioHardwarePropertyDefaultOutputDevice;
        pa.mScope    = kAudioObjectPropertyScopeWildcard;
        pa.mElement  = juceAudioObjectPropertyElementMain;

        if (auto deviceID = audioObjectGetProperty<AudioDeviceID> (kAudioObjectSystemObject, { selector,
                                                                                               kAudioObjectPropertyScopeWildcard,
                                                                                               juceAudioObjectPropertyElementMain }))
        {
            auto& ids = forInput ? inputIds : outputIds;

            if (auto it = std::find (ids.begin(), ids.end(), deviceID); it != ids.end())
                return static_cast<int> (std::distance (ids.begin(), it));
        }

        return 0;
    }

    int getIndexOfDevice (AudioIODevice* device, bool asInput) const override
    {
        jassert (hasScanned); // need to call scanForDevices() before doing this

        if (auto* d = dynamic_cast<CoreAudioIODevice*> (device))
            return d->getIndexOfDevice (asInput);

        if (auto* d = dynamic_cast<CoreAudioPairedDevice*> (device))
            return (asInput ? inputDeviceNames : outputDeviceNames)
                       .indexOf (asInput ? d->getInputDeviceName() : d->getName());

        return -1;
    }

    bool hasSeparateInputsAndOutputs() const override    { return true; }

    AudioIODevice* createDevice (const String& outputDeviceName,
                                 const String& inputDeviceName) override
    {
        jassert (hasScanned); // need to call scanForDevices() before doing this

        auto inputIndex  = inputDeviceNames.indexOf (inputDeviceName);
        auto outputIndex = outputDeviceNames.indexOf (outputDeviceName);

        auto inputDeviceID  = inputIds[inputIndex];
        auto outputDeviceID = outputIds[outputIndex];

        if (inputDeviceID == 0 && outputDeviceID == 0)
            return nullptr;

        auto combinedName = outputDeviceName.isEmpty() ? inputDeviceName
                                                       : outputDeviceName;

        if (inputDeviceID == outputDeviceID)
            return std::make_unique<CoreAudioIODevice> (this, combinedName, inputDeviceID, outputDeviceID).release();

        if (inputDeviceID == 0)
            return std::make_unique<CoreAudioIODevice> (this, outputDeviceName, 0, outputDeviceID).release();

        if (outputDeviceID == 0)
            return std::make_unique<CoreAudioIODevice> (this, inputDeviceName, inputDeviceID, 0).release();

        // smoothie: separate devices play as one on an aggregate of the two
        // (CoreAudioPairedDevice) — unless one of them doesn't pair (its
        // traits say why): refused, and the owner opens the output alone.
        for (const auto& name : { outputDeviceName, inputDeviceName })
        {
            if (! getDeviceTraits (name).pairs)
            {
                Logger::writeToLog ("[aggregate] '" + name + "' can't be paired with another device");
                return nullptr;
            }
        }

        return std::make_unique<CoreAudioPairedDevice> (this, outputDeviceName, inputDeviceName,
                                                        outputDeviceID, inputDeviceID, clientName).release();
    }

    void setClientName (const String& name) override    { clientName = name; }

    // smoothie: an open device's change, to the sink.
    void reportOpenDeviceChange()
    {
        reportDeviceChange (DeviceChange::openDevice);
    }

    //==============================================================================
private:
    StringArray inputDeviceNames, outputDeviceNames;
    Array<AudioDeviceID> inputIds, outputIds;
    String clientName;

    bool hasScanned = false;

    static int getNumChannels (AudioDeviceID deviceID, bool input)
    {
        return CoreAudioInternal::getNumChannels (deviceID, input);
    }

    static std::array<AudioObjectPropertyAddress, 2> defaultDeviceAddresses()
    {
        return { { { kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, juceAudioObjectPropertyElementMain },
                   { kAudioHardwarePropertyDefaultInputDevice,  kAudioObjectPropertyScopeGlobal, juceAudioObjectPropertyElementMain } } };
    }

    static AudioDeviceID defaultDeviceID (bool forInput)
    {
        return audioObjectGetProperty<AudioDeviceID> (kAudioObjectSystemObject,
                                                      defaultDeviceAddresses()[forInput ? 1 : 0])
                   .value_or (kAudioObjectUnknown);
    }

    static String deviceName (AudioDeviceID deviceID)
    {
        if (const auto name = audioObjectGetProperty<CFStringRef> (deviceID, { kAudioDevicePropertyDeviceNameCFString,
                                                                               kAudioObjectPropertyScopeWildcard,
                                                                               juceAudioObjectPropertyElementMain }))
            if (const CFUniquePtr<CFStringRef> owned { *name })
                return String::fromCFString (owned.get());

        return {};
    }

    // The device this type lists as `name` (its " (N)" form included), or —
    // not listed yet — the one the HAL calls that.
    AudioDeviceID deviceIDForName (const String& name) const
    {
        for (const auto* list : { &outputDeviceNames, &inputDeviceNames })
        {
            const auto& ids = list == &outputDeviceNames ? outputIds : inputIds;

            if (const auto index = list->indexOf (name); index >= 0)
                return ids[index];
        }

        for (const auto device : audioObjectGetProperties<AudioDeviceID> (kAudioObjectSystemObject, { kAudioHardwarePropertyDevices,
                                                                                                      kAudioObjectPropertyScopeWildcard,
                                                                                                      juceAudioObjectPropertyElementMain }))
            if (deviceName (device) == name)
                return device;

        return kAudioObjectUnknown;
    }

    static OSStatus defaultDeviceListenerProc (AudioObjectID, UInt32, const AudioObjectPropertyAddress*, void* clientData)
    {
        // smoothie: fenced, as hardwareListenerProc.
        LiveInternalRegistry::get().ifLive (clientData, [&]
        {
            static_cast<CoreAudioIODeviceType*> (clientData)->reportDeviceChange (DeviceChange::systemDefault);
        });

        return noErr;
    }

    static OSStatus hardwareListenerProc (AudioDeviceID, UInt32, const AudioObjectPropertyAddress*, void* clientData)
    {
        // smoothie: fenced — the type may be mid-destruction (see
        // LiveInternalRegistry). The sink's owner hears here, on CoreAudio's
        // thread, and rescans on its own.
        LiveInternalRegistry::get().ifLive (clientData, [&]
        {
            static_cast<CoreAudioIODeviceType*> (clientData)->reportDeviceChange (DeviceChange::list);
        });

        return noErr;
    }

    JUCE_DECLARE_WEAK_REFERENCEABLE (CoreAudioIODeviceType)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CoreAudioIODeviceType)
};

};

#undef JUCE_COREAUDIOLOG

} // namespace juce
