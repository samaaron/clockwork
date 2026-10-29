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

class iOSAudioIODevice;

constexpr const char* const iOSAudioDeviceName = "iOS Audio";

#ifndef JUCE_IOS_AUDIO_EXPLICIT_SAMPLERATES
 #define JUCE_IOS_AUDIO_EXPLICIT_SAMPLERATES
#endif

constexpr std::initializer_list<double> iOSExplicitSampleRates { JUCE_IOS_AUDIO_EXPLICIT_SAMPLERATES };

//==============================================================================
struct AudioSessionHolder
{
    AudioSessionHolder();
    ~AudioSessionHolder();

    void handleStatusChange (bool enabled, const char* reason) const;
    void handleRouteChange (AVAudioSessionRouteChangeReason reason);

    Array<iOSAudioIODevice::Pimpl*> activeDevices;
    Array<iOSAudioIODeviceType*> activeDeviceTypes;

    id nativeSession;
};

static const char* getRoutingChangeReason (AVAudioSessionRouteChangeReason reason) noexcept
{
    switch (reason)
    {
        case AVAudioSessionRouteChangeReasonNewDeviceAvailable:         return "New device available";
        case AVAudioSessionRouteChangeReasonOldDeviceUnavailable:       return "Old device unavailable";
        case AVAudioSessionRouteChangeReasonCategoryChange:             return "Category change";
        case AVAudioSessionRouteChangeReasonOverride:                   return "Override";
        case AVAudioSessionRouteChangeReasonWakeFromSleep:              return "Wake from sleep";
        case AVAudioSessionRouteChangeReasonNoSuitableRouteForCategory: return "No suitable route for category";
        case AVAudioSessionRouteChangeReasonRouteConfigurationChange:   return "Route configuration change";
        case AVAudioSessionRouteChangeReasonUnknown:
        default:                                                        return "Unknown";
    }
}

JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wmissing-prototypes")

bool getNotificationValueForKey (NSNotification* notification, NSString* key, NSUInteger& value) noexcept
{
    if (notification != nil)
    {
        if (NSDictionary* userInfo = [notification userInfo])
        {
            if (NSNumber* number = [userInfo objectForKey: key])
            {
                value = [number unsignedIntegerValue];
                return true;
            }
        }
    }

    jassertfalse;
    return false;
}

JUCE_END_IGNORE_WARNINGS_GCC_LIKE

} // namespace juce

//==============================================================================
@interface iOSAudioSessionNative  : NSObject
{
@private
    juce::AudioSessionHolder* audioSessionHolder;
};

- (id) init: (juce::AudioSessionHolder*) holder;
- (void) dealloc;

- (void) audioSessionChangedInterruptionType: (NSNotification*) notification;
- (void) handleMediaServicesReset;
- (void) handleMediaServicesLost;
- (void) handleRouteChange: (NSNotification*) notification;
@end

@implementation iOSAudioSessionNative

- (id) init: (juce::AudioSessionHolder*) holder
{
    self = [super init];

    if (self != nil)
    {
        audioSessionHolder = holder;

        auto session = [AVAudioSession sharedInstance];
        auto centre = [NSNotificationCenter defaultCenter];

        [centre addObserver: self
                   selector: @selector (audioSessionChangedInterruptionType:)
                       name: AVAudioSessionInterruptionNotification
                     object: session];

        [centre addObserver: self
                   selector: @selector (handleMediaServicesLost)
                       name: AVAudioSessionMediaServicesWereLostNotification
                     object: session];

        [centre addObserver: self
                   selector: @selector (handleMediaServicesReset)
                       name: AVAudioSessionMediaServicesWereResetNotification
                     object: session];

        [centre addObserver: self
                   selector: @selector (handleRouteChange:)
                       name: AVAudioSessionRouteChangeNotification
                     object: session];
    }
    else
    {
        jassertfalse;
    }

    return self;
}

- (void) dealloc
{
    [[NSNotificationCenter defaultCenter] removeObserver: self];
    [super dealloc];
}

- (void) audioSessionChangedInterruptionType: (NSNotification*) notification
{
    NSUInteger value;

    if (juce::getNotificationValueForKey (notification, AVAudioSessionInterruptionTypeKey, value))
    {
        switch ((AVAudioSessionInterruptionType) value)
        {
            case AVAudioSessionInterruptionTypeBegan:
                audioSessionHolder->handleStatusChange (false, "AVAudioSessionInterruptionTypeBegan");
                break;

            case AVAudioSessionInterruptionTypeEnded:
                audioSessionHolder->handleStatusChange (true, "AVAudioSessionInterruptionTypeEnded");
                break;

            // No default so the code doesn't compile if this enum is extended.
        }
    }
}

- (void) handleMediaServicesReset
{
    audioSessionHolder->handleStatusChange (true, "AVAudioSessionMediaServicesWereResetNotification");
}

- (void) handleMediaServicesLost
{
    audioSessionHolder->handleStatusChange (false, "AVAudioSessionMediaServicesWereLostNotification");
}

- (void) handleRouteChange: (NSNotification*) notification
{
    NSUInteger value;

    if (juce::getNotificationValueForKey (notification, AVAudioSessionRouteChangeReasonKey, value))
        audioSessionHolder->handleRouteChange ((AVAudioSessionRouteChangeReason) value);
}

@end

//==============================================================================
namespace juce {

#ifndef JUCE_IOS_AUDIO_LOGGING
 #define JUCE_IOS_AUDIO_LOGGING 0
#endif

#if JUCE_IOS_AUDIO_LOGGING
 #define JUCE_IOS_AUDIO_LOG(x)  DBG(x)
#else
 #define JUCE_IOS_AUDIO_LOG(x)
#endif

static void logNSError (NSError* e)
{
    if (e != nil)
    {
        JUCE_IOS_AUDIO_LOG ("iOS Audio error: " << [e.localizedDescription UTF8String]);
        jassertfalse;
    }
}

#define JUCE_NSERROR_CHECK(X)     { NSError* error = nil; X; logNSError (error); }

//==============================================================================
class iOSAudioIODeviceType final : public AudioIODeviceType,
                                   public AsyncUpdater
{
public:
    iOSAudioIODeviceType();
    ~iOSAudioIODeviceType() override;

    void scanForDevices() override;
    std::vector<std::string> getDeviceNames (bool) const override;
    int getDefaultDeviceIndex (bool) const override;
    int getIndexOfDevice (AudioIODevice*, bool) const override;
    bool hasSeparateInputsAndOutputs() const override;
    AudioIODevice* createDevice (const std::string&, const std::string&) override;

private:
    void handleRouteChange (AVAudioSessionRouteChangeReason);

    void handleAsyncUpdate() override;

    friend struct AudioSessionHolder;
    friend struct iOSAudioIODevice::Pimpl;

    SharedResourcePointer<AudioSessionHolder> sessionHolder;

    JUCE_DECLARE_WEAK_REFERENCEABLE (iOSAudioIODeviceType)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (iOSAudioIODeviceType)
};

//==============================================================================
struct iOSAudioIODevice::Pimpl final : public AsyncUpdater
{
    Pimpl (iOSAudioIODeviceType* ioDeviceType, iOSAudioIODevice& ioDevice)
        : deviceType (ioDeviceType),
          owner (ioDevice)
    {
        JUCE_IOS_AUDIO_LOG ("Creating iOS audio device");

        // We need to activate the audio session here to obtain the available sample rates and buffer sizes,
        // but if we don't set a category first then background audio will always be stopped. This category
        // may be changed later.
        setAudioSessionCategory (AVAudioSessionCategoryPlayAndRecord);

        setAudioSessionActive (true);
        updateHardwareInfo();
        channelData.reconfigure ({}, {});
        setAudioSessionActive (false);

        sessionHolder->activeDevices.add (this);
    }

    ~Pimpl() override
    {
        sessionHolder->activeDevices.removeFirstMatchingValue (this);

        close();
    }

    static void setAudioSessionCategory (NSString* category)
    {
        NSUInteger options = 0;

       #if ! JUCE_DISABLE_AUDIO_MIXING_WITH_OTHER_APPS
        options |= AVAudioSessionCategoryOptionMixWithOthers; // Alternatively AVAudioSessionCategoryOptionDuckOthers
       #endif

        if (category == AVAudioSessionCategoryPlayAndRecord)
        {
            options |= AVAudioSessionCategoryOptionDefaultToSpeaker
                     | AVAudioSessionCategoryOptionAllowBluetoothHFP
                     | AVAudioSessionCategoryOptionAllowAirPlay;

            if (@available (iOS 10.0, *))
                options |= AVAudioSessionCategoryOptionAllowBluetoothA2DP;
        }

        JUCE_NSERROR_CHECK ([[AVAudioSession sharedInstance] setCategory: category
                                                             withOptions: options
                                                                   error: &error]);
    }

    static void setAudioSessionActive (bool enabled)
    {
        JUCE_NSERROR_CHECK ([[AVAudioSession sharedInstance] setActive: enabled
                                                                 error: &error]);
    }

    int getBufferSize (const double currentSampleRate)
    {
        return roundToInt (currentSampleRate * [AVAudioSession sharedInstance].IOBufferDuration);
    }

    int tryBufferSize (const double currentSampleRate, const int newBufferSize)
    {
        NSTimeInterval bufferDuration = currentSampleRate > 0 ? (NSTimeInterval) ((newBufferSize + 1) / currentSampleRate) : 0.0;

        auto session = [AVAudioSession sharedInstance];
        JUCE_NSERROR_CHECK ([session setPreferredIOBufferDuration: bufferDuration
                                                            error: &error]);

        return getBufferSize (currentSampleRate);
    }

    void updateAvailableBufferSizes()
    {
        availableBufferSizes.clear();

        auto newBufferSize = tryBufferSize (sampleRate, 64);
        jassert (newBufferSize > 0);

        const auto longestBufferSize  = tryBufferSize (sampleRate, 4096);

        while (newBufferSize <= longestBufferSize)
        {
            availableBufferSizes.add (newBufferSize);
            newBufferSize *= 2;
        }

        // Sometimes the largest supported buffer size is not a power of 2
        availableBufferSizes.addIfNotAlreadyThere (longestBufferSize);

        bufferSize = tryBufferSize (sampleRate, bufferSize);

       #if JUCE_IOS_AUDIO_LOGGING
        {
            String info ("Available buffer sizes:");

            for (auto size : availableBufferSizes)
                info << " " << size;

            JUCE_IOS_AUDIO_LOG (info);
        }
       #endif

        JUCE_IOS_AUDIO_LOG ("Buffer size after detecting available buffer sizes: " << bufferSize);
    }

    double trySampleRate (double rate)
    {
        auto session = [AVAudioSession sharedInstance];
        JUCE_NSERROR_CHECK ([session setPreferredSampleRate: rate
                                                      error: &error]);

        return session.sampleRate;
    }

    // Important: the supported audio sample rates change on the iPhone 6S
    // depending on whether the headphones are plugged in or not!
    void updateAvailableSampleRates()
    {
        if (iOSExplicitSampleRates.size() != 0)
        {
            availableSampleRates = Array<double> (iOSExplicitSampleRates);
            return;
        }

        availableSampleRates.clear();

        AudioUnitRemovePropertyListenerWithUserData (audioUnit,
                                                     kAudioUnitProperty_StreamFormat,
                                                     dispatchAudioUnitPropertyChange,
                                                     this);

        const double lowestRate = trySampleRate (4000);
        availableSampleRates.add (lowestRate);
        const double highestRate = trySampleRate (192000);

        JUCE_IOS_AUDIO_LOG ("Lowest supported sample rate: "  << lowestRate);
        JUCE_IOS_AUDIO_LOG ("Highest supported sample rate: " << highestRate);

        for (double rate = lowestRate + 1000; rate < highestRate; rate += 1000)
        {
            const double supportedRate = trySampleRate (rate);
            JUCE_IOS_AUDIO_LOG ("Trying a sample rate of " << rate << ", got " << supportedRate);
            availableSampleRates.addIfNotAlreadyThere (supportedRate);
            rate = jmax (rate, supportedRate);
        }

        availableSampleRates.addIfNotAlreadyThere (highestRate);

        // Restore the original values.
        sampleRate = trySampleRate (sampleRate);
        bufferSize = tryBufferSize (sampleRate, bufferSize);

        AudioUnitAddPropertyListener (audioUnit,
                                      kAudioUnitProperty_StreamFormat,
                                      dispatchAudioUnitPropertyChange,
                                      this);

        // Check the current stream format in case things have changed whilst we
        // were going through the sample rates
        handleStreamFormatChange();

       #if JUCE_IOS_AUDIO_LOGGING
        {
            String info ("Available sample rates:");

            for (auto rate : availableSampleRates)
                info << " " << rate;

            JUCE_IOS_AUDIO_LOG (info);
        }
       #endif

        JUCE_IOS_AUDIO_LOG ("Sample rate after detecting available sample rates: " << sampleRate);
    }

    void updateHardwareInfo (bool forceUpdate = false)
    {
        if (! forceUpdate && ! hardwareInfoNeedsUpdating.compareAndSetBool (false, true))
            return;

        JUCE_IOS_AUDIO_LOG ("Updating hardware info");

        updateAvailableSampleRates();
        updateAvailableBufferSizes();

        if (deviceType != nullptr)
            deviceType->reportDeviceChange (AudioIODeviceType::DeviceChange::list);
    }

    void setTargetSampleRateAndBufferSize()
    {
        JUCE_IOS_AUDIO_LOG ("Setting target sample rate: " << targetSampleRate);
        sampleRate = trySampleRate (targetSampleRate);
        JUCE_IOS_AUDIO_LOG ("Actual sample rate: " << sampleRate);

        JUCE_IOS_AUDIO_LOG ("Setting target buffer size: " << targetBufferSize);
        bufferSize = tryBufferSize (sampleRate, targetBufferSize);
        JUCE_IOS_AUDIO_LOG ("Actual buffer size: " << bufferSize);
    }

    std::string open (const ChannelMask& inputChannelsWanted,
                 const ChannelMask& outputChannelsWanted,
                 double sampleRateWanted, int bufferSizeWanted)
    {
        close();

        firstHostTime = true;
        lastNumFrames = 0;
        xrun = 0;
        lastError.clear();

        requestedInputChannels  = inputChannelsWanted;
        requestedOutputChannels = outputChannelsWanted;
        targetSampleRate = sampleRateWanted;
        targetBufferSize = bufferSizeWanted > 0 ? bufferSizeWanted : defaultBufferSize;

        JUCE_IOS_AUDIO_LOG ("Opening audio device:"
                            <<  " inputChannelsWanted: "  << requestedInputChannels .toString (2)
                            << ", outputChannelsWanted: " << requestedOutputChannels.toString (2)
                            << ", targetSampleRate: " << targetSampleRate
                            << ", targetBufferSize: " << targetBufferSize);

        setAudioSessionActive (true);
        setAudioSessionCategory (! requestedInputChannels.isZero() ? AVAudioSessionCategoryPlayAndRecord
                                                            : AVAudioSessionCategoryPlayback);
        channelData.reconfigure (requestedInputChannels, requestedOutputChannels);
        updateHardwareInfo (true);
        setTargetSampleRateAndBufferSize();
        fixAudioRouteIfSetToReceiver();

        isRunning = true;

        if (! createAudioUnit())
        {
            lastError = "Couldn't open the device";
            return lastError.toStdString();
        }

        const ScopedLock sl (callbackLock);

        AudioOutputUnitStart (audioUnit);

        if (callback != nullptr)
            callback->audioDeviceAboutToStart (&owner);

        return lastError.toStdString();
    }

    void close()
    {
        stop();

        if (isRunning)
        {
            isRunning = false;

            if (audioUnit != nullptr)
            {
                AudioOutputUnitStart (audioUnit);
                AudioComponentInstanceDispose (audioUnit);
                audioUnit = nullptr;
            }

            setAudioSessionActive (false);
        }
    }

    void start (AudioIODeviceCallback* newCallback)
    {
        if (isRunning && callback != newCallback)
        {
            if (newCallback != nullptr)
                newCallback->audioDeviceAboutToStart (&owner);

            const ScopedLock sl (callbackLock);
            callback = newCallback;
        }
    }

    void stop()
    {
        if (isRunning)
        {
            AudioIODeviceCallback* lastCallback;

            {
                const ScopedLock sl (callbackLock);
                lastCallback = callback;
                callback = nullptr;
            }

            if (lastCallback != nullptr)
                lastCallback->audioDeviceStopped();
        }
    }

    bool setAudioPreprocessingEnabled (bool enable)
    {
        auto session = [AVAudioSession sharedInstance];

        NSString* mode = (enable ? AVAudioSessionModeDefault
                                 : AVAudioSessionModeMeasurement);

        JUCE_NSERROR_CHECK ([session setMode: mode
                                       error: &error]);

        return session.mode == mode;
    }

    //==============================================================================
    void invokeAudioDeviceErrorCallback (const String& reason)
    {
        const ScopedLock sl (callbackLock);

        if (callback != nullptr)
            callback->audioDeviceError (reason.toStdString());
    }

    void handleStatusChange (bool enabled, const char* reason)
    {
        const ScopedLock myScopedLock (callbackLock);

        JUCE_IOS_AUDIO_LOG ("handleStatusChange: enabled: " << (int) enabled << ", reason: " << reason);

        isRunning = enabled;
        setAudioSessionActive (enabled);

        if (enabled)
            AudioOutputUnitStart (audioUnit);
        else
            AudioOutputUnitStop (audioUnit);

        if (! enabled)
            invokeAudioDeviceErrorCallback (reason);
    }

    void handleRouteChange (AVAudioSessionRouteChangeReason reason)
    {
        const ScopedLock myScopedLock (callbackLock);

        const String reasonString (getRoutingChangeReason (reason));
        JUCE_IOS_AUDIO_LOG ("handleRouteChange: " << reasonString);

        if (isRunning)
            invokeAudioDeviceErrorCallback (reasonString);

        switch (reason)
        {
        case AVAudioSessionRouteChangeReasonCategoryChange:
        case AVAudioSessionRouteChangeReasonRouteConfigurationChange:
            break;
        case AVAudioSessionRouteChangeReasonOverride:
        case AVAudioSessionRouteChangeReasonUnknown:
        case AVAudioSessionRouteChangeReasonNewDeviceAvailable:
        case AVAudioSessionRouteChangeReasonOldDeviceUnavailable:
        case AVAudioSessionRouteChangeReasonWakeFromSleep:
        case AVAudioSessionRouteChangeReasonNoSuitableRouteForCategory:
        {
            hardwareInfoNeedsUpdating = true;
            triggerAsyncUpdate();
            break;
        }

        // No default so the code doesn't compile if this enum is extended.
        }
    }

    void handleAudioUnitPropertyChange (AudioUnit,
                                        AudioUnitPropertyID propertyID,
                                        [[maybe_unused]] AudioUnitScope scope,
                                        [[maybe_unused]] AudioUnitElement element)
    {
        JUCE_IOS_AUDIO_LOG ("handleAudioUnitPropertyChange: propertyID: " << String (propertyID)
                                                            << " scope: " << String (scope)
                                                          << " element: " << String (element));

        switch (propertyID)
        {
            case kAudioUnitProperty_StreamFormat:
                handleStreamFormatChange();
                return;
            default:
                jassertfalse;
        }
    }

    //==============================================================================
    OSStatus process (AudioUnitRenderActionFlags* flags, const AudioTimeStamp* time,
                      const UInt32 numFrames, AudioBufferList* data)
    {
        OSStatus err = noErr;

        recordXruns (time, numFrames);

        const bool useInput = channelData.areInputChannelsAvailable();

        if (useInput)
            err = AudioUnitRender (audioUnit, flags, time, 1, numFrames, data);

        const auto channelDataSize = sizeof (float) * numFrames;

        const ScopedTryLock stl (callbackLock);

        if (stl.isLocked() && callback != nullptr)
        {
            if ((int) numFrames > channelData.getFloatBufferSize())
                channelData.setFloatBufferSize ((int) numFrames);

            float* const* const inputData = channelData.audioData.getArrayOfWritePointers();
            float* const* const outputData = inputData + channelData.inputs->numActiveChannels;

            if (useInput)
            {
                for (int c = 0; c < channelData.inputs->numActiveChannels; ++c)
                {
                    auto channelIndex = channelData.inputs->activeChannelIndices[c];
                    memcpy (inputData[c], (float*) data->mBuffers[channelIndex].mData, channelDataSize);
                }
            }
            else
            {
                for (int c = 0; c < channelData.inputs->numActiveChannels; ++c)
                    zeromem (inputData[c], channelDataSize);
            }

            const auto nanos = time != nullptr ? timeConversions.hostTimeToNanos (time->mHostTime) : 0;

            callback->audioDeviceIOCallbackWithContext ((const float**) inputData,
                                                        channelData.inputs ->numActiveChannels,
                                                        outputData,
                                                        channelData.outputs->numActiveChannels,
                                                        (int) numFrames,
                                                        { (time != nullptr && (time->mFlags & kAudioTimeStampHostTimeValid) != 0) ? &nanos : nullptr });

            for (int c = 0; c < channelData.outputs->numActiveChannels; ++c)
            {
                auto channelIndex = channelData.outputs->activeChannelIndices[c];
                memcpy (data->mBuffers[channelIndex].mData, outputData[c], channelDataSize);
            }

            for (auto c : channelData.outputs->inactiveChannelIndices)
                zeromem (data->mBuffers[c].mData, channelDataSize);
        }
        else
        {
            for (uint32 c = 0; c < data->mNumberBuffers; ++c)
                zeromem (data->mBuffers[c].mData, channelDataSize);
        }

        return err;
    }

    void recordXruns (const AudioTimeStamp* time, UInt32 numFrames)
    {
        if (time != nullptr && (time->mFlags & kAudioTimeStampSampleTimeValid) != 0)
        {
            if (! firstHostTime)
            {
                if (! approximatelyEqual ((time->mSampleTime - lastSampleTime), (double) lastNumFrames))
                    xrun++;
            }
            else
                firstHostTime = false;

            lastSampleTime = time->mSampleTime;
        }
        else
            firstHostTime = true;

        lastNumFrames = numFrames;
    }

    //==============================================================================
    static OSStatus processStatic (void* client, AudioUnitRenderActionFlags* flags, const AudioTimeStamp* time,
                                   UInt32 /*busNumber*/, UInt32 numFrames, AudioBufferList* data)
    {
        return static_cast<Pimpl*> (client)->process (flags, time, numFrames, data);
    }

    //==============================================================================
    bool createAudioUnit()
    {
        JUCE_IOS_AUDIO_LOG ("Creating the audio unit");

        if (audioUnit != nullptr)
        {
            AudioComponentInstanceDispose (audioUnit);
            audioUnit = nullptr;
        }

        AudioComponentDescription desc;
        desc.componentType = kAudioUnitType_Output;
        desc.componentSubType = kAudioUnitSubType_RemoteIO;
        desc.componentManufacturer = kAudioUnitManufacturer_Apple;
        desc.componentFlags = 0;
        desc.componentFlagsMask = 0;

        AudioComponent comp = AudioComponentFindNext (nullptr, &desc);
        AudioComponentInstanceNew (comp, &audioUnit);

        if (audioUnit == nullptr)
            return false;

        if (channelData.areInputChannelsAvailable())
        {
            const UInt32 one = 1;
            AudioUnitSetProperty (audioUnit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &one, sizeof (one));
        }

        {
            AURenderCallbackStruct inputProc;
            inputProc.inputProc = processStatic;
            inputProc.inputProcRefCon = this;
            AudioUnitSetProperty (audioUnit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &inputProc, sizeof (inputProc));
        }

        {
            AudioStreamBasicDescription format;
            zerostruct (format);
            format.mSampleRate = sampleRate;
            format.mFormatID = kAudioFormatLinearPCM;
            format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsNonInterleaved | kAudioFormatFlagsNativeEndian | kLinearPCMFormatFlagIsPacked;
            format.mBitsPerChannel = 8 * sizeof (float);
            format.mFramesPerPacket = 1;
            format.mChannelsPerFrame = (UInt32) jmax (channelData.inputs->numHardwareChannels, channelData.outputs->numHardwareChannels);
            format.mBytesPerFrame = format.mBytesPerPacket = sizeof (float);

            AudioUnitSetProperty (audioUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input,  0, &format, sizeof (format));
            AudioUnitSetProperty (audioUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &format, sizeof (format));
        }

        AudioUnitInitialize (audioUnit);

        {
            // Querying the kAudioUnitProperty_MaximumFramesPerSlice property after calling AudioUnitInitialize
            // seems to be more reliable than calling it before.
            UInt32 framesPerSlice, dataSize = sizeof (framesPerSlice);

            if (AudioUnitGetProperty (audioUnit, kAudioUnitProperty_MaximumFramesPerSlice,
                                      kAudioUnitScope_Global, 0, &framesPerSlice, &dataSize) == noErr
                    && dataSize == sizeof (framesPerSlice)
                    && static_cast<int> (framesPerSlice) != bufferSize)
            {
                JUCE_IOS_AUDIO_LOG ("Internal buffer size: " << String (framesPerSlice));
                channelData.setFloatBufferSize (static_cast<int> (framesPerSlice));
            }
        }

       #if JUCE_AUDIOWORKGROUP_TYPES_AVAILABLE
        workgroup = [this]
        {
            UInt32 dataSize = sizeof (os_workgroup_t);
            os_workgroup_t wgHandle = nullptr;

            AudioUnitGetProperty (audioUnit, kAudioOutputUnitProperty_OSWorkgroup,
                                  kAudioUnitScope_Global, 0, &wgHandle, &dataSize);

            return makeRealAudioWorkgroup (wgHandle);
        }();
       #endif

        AudioUnitAddPropertyListener (audioUnit, kAudioUnitProperty_StreamFormat, dispatchAudioUnitPropertyChange, this);

        return true;
    }

    // If the routing is set to go through the receiver (i.e. the speaker, but quiet), this re-routes it
    // to make it loud. Needed because by default when using an input + output, the output is kept quiet.
    static void fixAudioRouteIfSetToReceiver()
    {
        auto session = [AVAudioSession sharedInstance];
        auto route = session.currentRoute;

        for (AVAudioSessionPortDescription* port in route.outputs)
        {
            if ([port.portName isEqualToString: @"Receiver"])
            {
                JUCE_NSERROR_CHECK ([session overrideOutputAudioPort: AVAudioSessionPortOverrideSpeaker
                                                               error: &error]);
                setAudioSessionActive (true);
            }
        }
    }

    void restart()
    {
        const ScopedLock sl (callbackLock);

        updateHardwareInfo();
        setTargetSampleRateAndBufferSize();

        if (isRunning)
        {
            if (audioUnit != nullptr)
            {
                AudioComponentInstanceDispose (audioUnit);
                audioUnit = nullptr;

                if (callback != nullptr)
                    callback->audioDeviceStopped();
            }

            channelData.reconfigure (requestedInputChannels, requestedOutputChannels);

            createAudioUnit();

            if (audioUnit != nullptr)
            {
                isRunning = true;

                if (callback != nullptr)
                    callback->audioDeviceAboutToStart (&owner);

                AudioOutputUnitStart (audioUnit);
            }
        }
    }

    void handleAsyncUpdate() override
    {
        restart();
    }

    void handleStreamFormatChange()
    {
        AudioStreamBasicDescription desc;
        zerostruct (desc);
        UInt32 dataSize = sizeof (desc);
        AudioUnitGetProperty (audioUnit,
                              kAudioUnitProperty_StreamFormat,
                              kAudioUnitScope_Output,
                              0,
                              &desc,
                              &dataSize);

        if (! approximatelyEqual (desc.mSampleRate, 0.0) && ! approximatelyEqual (desc.mSampleRate, sampleRate))
        {
            JUCE_IOS_AUDIO_LOG ("Stream format has changed: Sample rate " << desc.mSampleRate);
            triggerAsyncUpdate();
        }
    }

    static void dispatchAudioUnitPropertyChange (void* data, AudioUnit unit, AudioUnitPropertyID propertyID,
                                                 AudioUnitScope scope, AudioUnitElement element)
    {
        static_cast<Pimpl*> (data)->handleAudioUnitPropertyChange (unit, propertyID, scope, element);
    }

    struct IOChannelData
    {
        class IOChannelConfig
        {
        public:
            IOChannelConfig (const bool isInput, const ChannelMask requiredChannels)
                : hardwareChannelNames (getHardwareChannelNames (isInput)),
                  numHardwareChannels (hardwareChannelNames.size()),
                  areChannelsAccessible ((! isInput) || [AVAudioSession sharedInstance].isInputAvailable),
                  activeChannels (limitRequiredChannelsToHardware (numHardwareChannels, requiredChannels)),
                  numActiveChannels (activeChannels.countNumberOfSetBits()),
                  activeChannelIndices (getActiveChannelIndices (activeChannels)),
                  inactiveChannelIndices (getInactiveChannelIndices (activeChannelIndices, numHardwareChannels))
            {
               #if JUCE_IOS_AUDIO_LOGGING
                {
                    String info;

                    info << "Number of hardware channels: " << numHardwareChannels
                         << ", Hardware channel names:";

                    for (auto& name : hardwareChannelNames)
                        info << " \"" << name << "\"";

                    info << ", Are channels available: " << (areChannelsAccessible ? "yes" : "no")
                         << ", Active channel indices:";

                    for (auto i : activeChannelIndices)
                        info << " " << i;

                    info << ", Inactive channel indices:";

                    for (auto i : inactiveChannelIndices)
                        info << " " << i;

                    JUCE_IOS_AUDIO_LOG ((isInput ? "Input" : "Output") << " channel configuration: {" << info << "}");
                }
               #endif
            }

            const StringArray hardwareChannelNames;
            const int numHardwareChannels;
            const bool areChannelsAccessible;

            const ChannelMask activeChannels;
            const int numActiveChannels;

            const Array<int> activeChannelIndices, inactiveChannelIndices;

        private:
            static StringArray getHardwareChannelNames (const bool isInput)
            {
                StringArray result;

                auto route = [AVAudioSession sharedInstance].currentRoute;

                for (AVAudioSessionPortDescription* port in (isInput ? route.inputs : route.outputs))
                {
                    for (AVAudioSessionChannelDescription* desc in port.channels)
                        result.add (nsStringToJuce (desc.channelName));
                }

                // A fallback for the iOS simulator and older iOS versions
                if (result.isEmpty())
                    return { "Left", "Right" };

                return result;
            }

            static ChannelMask limitRequiredChannelsToHardware (const int numHardwareChannelsAvailable,
                                                               ChannelMask requiredChannels)
            {
                requiredChannels.setRange (numHardwareChannelsAvailable,
                                           requiredChannels.getHighestBit() + 1,
                                           false);

                return requiredChannels;
            }

            static Array<int> getActiveChannelIndices (const ChannelMask activeChannelsToIndex)
            {
                Array<int> result;

                auto index = activeChannelsToIndex.findNextSetBit (0);

                while (index != -1)
                {
                    result.add (index);
                    index = activeChannelsToIndex.findNextSetBit (++index);
                }

                return result;
            }

            static Array<int> getInactiveChannelIndices (const Array<int>& activeIndices, int numChannels)
            {
                Array<int> result;

                auto nextActiveChannel = activeIndices.begin();

                for (int i = 0; i < numChannels; ++i)
                    if (nextActiveChannel != activeIndices.end() && i == *nextActiveChannel)
                        ++nextActiveChannel;
                    else
                        result.add (i);

                return result;
            }
        };

        void reconfigure (const ChannelMask requiredInputChannels,
                          const ChannelMask requiredOutputChannels)
        {
            inputs .reset (new IOChannelConfig (true,  requiredInputChannels));
            outputs.reset (new IOChannelConfig (false, requiredOutputChannels));

            audioData.setSize (inputs->numActiveChannels + outputs->numActiveChannels,
                               audioData.getNumSamples());
        }

        int getFloatBufferSize() const
        {
            return audioData.getNumSamples();
        }

        void setFloatBufferSize (const int newSize)
        {
            audioData.setSize (audioData.getNumChannels(), newSize);
        }

        bool areInputChannelsAvailable() const
        {
            return inputs->areChannelsAccessible && inputs->numActiveChannels > 0;
        }

        std::unique_ptr<IOChannelConfig> inputs;
        std::unique_ptr<IOChannelConfig> outputs;

        AudioBuffer<float> audioData { 0, 0 };
    };

    CoreAudioTimeConversions timeConversions;

    IOChannelData channelData;

    ChannelMask requestedInputChannels, requestedOutputChannels;

    bool isRunning = false;

    AudioIODeviceCallback* callback = nullptr;

    String lastError;

   #if TARGET_IPHONE_SIMULATOR
    static constexpr int defaultBufferSize = 512;
   #else
    static constexpr int defaultBufferSize = 256;
   #endif
    int targetBufferSize = defaultBufferSize, bufferSize = targetBufferSize;

    double targetSampleRate = 44100.0, sampleRate = targetSampleRate;

    Array<double> availableSampleRates;
    Array<int> availableBufferSizes;

    WeakReference<iOSAudioIODeviceType> deviceType;
    iOSAudioIODevice& owner;

    CriticalSection callbackLock;

    Atomic<bool> hardwareInfoNeedsUpdating { true };

    AudioUnit audioUnit {};
    AudioWorkgroup workgroup;

    SharedResourcePointer<AudioSessionHolder> sessionHolder;

    bool firstHostTime;
    Float64 lastSampleTime;
    unsigned int lastNumFrames;
    int xrun;

    JUCE_DECLARE_NON_COPYABLE (Pimpl)
};

//==============================================================================
iOSAudioIODevice::iOSAudioIODevice (iOSAudioIODeviceType* ioDeviceType, const String&, const String&)
    : AudioIODevice (iOSAudioDeviceName, iOSAudioDeviceName),
      pimpl (new Pimpl (ioDeviceType, *this))
{
}

//==============================================================================
std::string iOSAudioIODevice::open (const ChannelMask& inChans, const ChannelMask& outChans,
                               double requestedSampleRate, int requestedBufferSize)
{
    return pimpl->open (inChans, outChans, requestedSampleRate, requestedBufferSize);
}

void iOSAudioIODevice::close()                                      { pimpl->close(); }

void iOSAudioIODevice::start (AudioIODeviceCallback* callbackToUse) { pimpl->start (callbackToUse); }
void iOSAudioIODevice::stop()                                       { pimpl->stop(); }

Array<double> iOSAudioIODevice::getAvailableSampleRates()           { return pimpl->availableSampleRates; }
Array<int> iOSAudioIODevice::getAvailableBufferSizes()              { return pimpl->availableBufferSizes; }

bool iOSAudioIODevice::setAudioPreprocessingEnabled (bool enabled)  { return pimpl->setAudioPreprocessingEnabled (enabled); }

bool iOSAudioIODevice::isPlaying()                                  { return pimpl->isRunning && pimpl->callback != nullptr; }
bool iOSAudioIODevice::isOpen()                                     { return pimpl->isRunning; }
std::string iOSAudioIODevice::getLastError()                        { return pimpl->lastError.toStdString(); }

std::vector<std::string> iOSAudioIODevice::getOutputChannelNames() { return toStdStrings (pimpl->channelData.outputs->hardwareChannelNames); }
std::vector<std::string> iOSAudioIODevice::getInputChannelNames()  { return toStdStrings (pimpl->channelData.inputs->hardwareChannelNames); }

int iOSAudioIODevice::getDefaultBufferSize()                        { return pimpl->defaultBufferSize; }
int iOSAudioIODevice::getCurrentBufferSizeSamples()                 { return pimpl->bufferSize; }

double iOSAudioIODevice::getCurrentSampleRate()                     { return pimpl->sampleRate; }

int iOSAudioIODevice::getCurrentBitDepth()                          { return 16; }

ChannelMask iOSAudioIODevice::getActiveInputChannels() const         { return pimpl->channelData.inputs->activeChannels; }
ChannelMask iOSAudioIODevice::getActiveOutputChannels() const        { return pimpl->channelData.outputs->activeChannels; }

int iOSAudioIODevice::getInputLatencyInSamples()                    { return roundToInt (pimpl->sampleRate * [AVAudioSession sharedInstance].inputLatency); }
int iOSAudioIODevice::getOutputLatencyInSamples()                   { return roundToInt (pimpl->sampleRate * [AVAudioSession sharedInstance].outputLatency); }
int iOSAudioIODevice::getXRunCount() const noexcept                 { return pimpl->xrun; }
AudioWorkgroup iOSAudioIODevice::getWorkgroup() const               { return pimpl->workgroup; }

//==============================================================================
iOSAudioIODeviceType::iOSAudioIODeviceType()
    : AudioIODeviceType (iOSAudioDeviceName)
{
    sessionHolder->activeDeviceTypes.add (this);
}

iOSAudioIODeviceType::~iOSAudioIODeviceType()
{
    sessionHolder->activeDeviceTypes.removeFirstMatchingValue (this);
}

// The list of devices is updated automatically
void iOSAudioIODeviceType::scanForDevices() {}
std::vector<std::string> iOSAudioIODeviceType::getDeviceNames (bool) const { return { iOSAudioDeviceName }; }
int iOSAudioIODeviceType::getDefaultDeviceIndex (bool) const              { return 0; }
int iOSAudioIODeviceType::getIndexOfDevice (AudioIODevice*, bool) const   { return 0; }
bool iOSAudioIODeviceType::hasSeparateInputsAndOutputs() const            { return false; }

AudioIODevice* iOSAudioIODeviceType::createDevice (const std::string& outputDeviceName, const std::string& inputDeviceName)
{
    return new iOSAudioIODevice (this, outputDeviceName, inputDeviceName);
}

void iOSAudioIODeviceType::handleRouteChange (AVAudioSessionRouteChangeReason)
{
    triggerAsyncUpdate();
}

void iOSAudioIODeviceType::handleAsyncUpdate()
{
    reportDeviceChange (AudioIODeviceType::DeviceChange::list);
}

//==============================================================================
AudioSessionHolder::AudioSessionHolder()    { nativeSession = [[iOSAudioSessionNative alloc] init: this]; }
AudioSessionHolder::~AudioSessionHolder()   { [nativeSession release]; }

void AudioSessionHolder::handleStatusChange (bool enabled, const char* reason) const
{
    for (auto device: activeDevices)
        device->handleStatusChange (enabled, reason);
}

void AudioSessionHolder::handleRouteChange (AVAudioSessionRouteChangeReason reason)
{
    for (auto device: activeDevices)
        device->handleRouteChange (reason);

    for (auto deviceType: activeDeviceTypes)
        deviceType->handleRouteChange (reason);
}

#undef JUCE_NSERROR_CHECK

} // namespace juce
