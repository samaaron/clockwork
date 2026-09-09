// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * ClockworkTestGain.cpp — the smallest VST3 plugin that is still worth hosting.
 *
 * Exists so `src/plugin_vst3.cpp` can be tested in-tree without a third-party
 * plugin on the box. Its behaviour is pinned to the letter and is IDENTICAL to
 * plugins/clap's test plugin, so a later differential test can assert both
 * formats produce the same samples:
 *
 *   out[c][i] = in[c][i] * gain      (output channels beyond the inputs: silent)
 *   one parameter, id 0, "gain", plain range 0.0 .. 2.0, default 0.5
 *   0 latency
 *   state = the plain gain as a little-endian double, 8 bytes, nothing else
 *
 * THE NORMALISATION IS THE POINT. VST3 has no parameter range: every parameter
 * is normalised 0..1 on the wire and the plugin's own IEditController is the
 * only thing that knows what that means. So gain 0.5 reaches this plugin as
 * normalised 0.25, and clockwork — which speaks plain 0..2 through
 * plugin_host.h — never sees that. Hiding it is the adapter's job, and the
 * tests bite on exactly this.
 *
 * TWO CLASSES, not one. A single object implementing IComponent and
 * IEditController together is legal VST3 and is what src/native/vst3 in
 * tau-next does, but it exercises none of the host work that actually
 * has bugs in it: creating the controller from its own class id, connecting
 * the two connection points, priming the controller from the component's
 * state. This plugin is deliberately the two-object shape so the adapter's
 * real path is the one under test.
 *
 * NO EDITOR. createView returns null — permitted by VST3 and required by
 * docs/TRACKS.md's headless posture. A host that cannot cope with
 * that is broken, and this is what proves ours copes. What an editor WOULD
 * do — set the controller's value and report the edit through the host's
 * IComponentHandler — is reachable through ClockworkTestGainTurn, so the host's
 * side of that handshake can be tested without a window.
 *
 * Hand-rolled COM: interface headers only, no SDK base/ or public.sdk/.
 */

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/vstspeaker.h"
#include "pluginterfaces/vst/vsttypes.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

// ── IDs ──────────────────────────────────────────────────────────────────────
// Stable, generated once for this plugin. Never reuse for anything else.
const TUID kGainProcessorUID =
    INLINE_UID (0x7A55A140, 0x9C1F4B22, 0xA0E31D57, 0x54455301);
const TUID kGainControllerUID =
    INLINE_UID (0x7A55A140, 0x9C1F4B22, 0xA0E31D57, 0x54455302);

constexpr ParamID  kGainParamId = 0;
constexpr double   kGainMin     = 0.0;
constexpr double   kGainMax     = 2.0;
constexpr double   kGainDefault = 0.5;

// void* signature so both TUID (int8[16]) and FIDString (char*) callers fit.
inline bool sameIID (const void* a, const void* b)
{
    return a && b && std::memcmp (a, b, sizeof (TUID)) == 0;
}

void asciiToString128 (const char* src, String128 dst)
{
    int i = 0;
    for (; src[i] != '\0' && i < 127; ++i)
        dst[i] = static_cast<TChar> (src[i]);
    dst[i] = 0;
}

inline double normToPlain (double n)
{
    if (n < 0.0) n = 0.0;
    if (n > 1.0) n = 1.0;
    return kGainMin + n * (kGainMax - kGainMin);
}

inline double plainToNorm (double p)
{
    const double n = (p - kGainMin) / (kGainMax - kGainMin);
    return n < 0.0 ? 0.0 : (n > 1.0 ? 1.0 : n);
}

// The musical time the host handed the last block, read back through the
// ClockworkTestGainLastContext boundary below: what the host SAYS the block is, which
// no output sample can show. Written and read on the test's own thread.
struct LastContext {
    bool   any = false;      // a block has been processed
    uint32 state = 0;        // ProcessContext::state, as given
    double tempo = 0.0;
    double projectTimeMusic = 0.0;
    double barPositionMusic = 0.0;
    int32  timeSigNumerator = 0, timeSigDenominator = 0;
} gLastContext;

// ── The processor ────────────────────────────────────────────────────────────
class GainProcessor : public IComponent,
                      public IAudioProcessor,
                      public IConnectionPoint
{
public:
    // ── FUnknown ─────────────────────────────────────────────────────────────
    tresult PLUGIN_API queryInterface (const TUID iid, void** obj) override
    {
        if (!obj)
            return kInvalidArgument;
        if (sameIID (iid, FUnknown_iid) || sameIID (iid, IPluginBase_iid)
            || sameIID (iid, IComponent_iid))
            *obj = static_cast<IComponent*> (this);
        else if (sameIID (iid, IAudioProcessor_iid))
            *obj = static_cast<IAudioProcessor*> (this);
        else if (sameIID (iid, IConnectionPoint_iid))
            *obj = static_cast<IConnectionPoint*> (this);
        else {
            *obj = nullptr;
            return kNoInterface;
        }
        addRef ();
        return kResultOk;
    }
    uint32 PLUGIN_API addRef () override { return static_cast<uint32> (++mRefCount); }
    uint32 PLUGIN_API release () override
    {
        const uint32 rc = static_cast<uint32> (--mRefCount);
        if (rc == 0) delete this;
        return rc;
    }

    // ── IPluginBase ──────────────────────────────────────────────────────────
    tresult PLUGIN_API initialize (FUnknown*) override { return kResultOk; }
    tresult PLUGIN_API terminate () override { return kResultOk; }

    // ── IComponent ───────────────────────────────────────────────────────────
    tresult PLUGIN_API getControllerClassId (TUID classId) override
    {
        std::memcpy (classId, kGainControllerUID, sizeof (TUID));
        return kResultOk;
    }
    tresult PLUGIN_API setIoMode (IoMode) override { return kResultOk; }

    int32 PLUGIN_API getBusCount (MediaType type, BusDirection) override
    {
        return type == kAudio ? 1 : 0;   // no event bus: this is an effect
    }

    tresult PLUGIN_API getBusInfo (MediaType type, BusDirection dir, int32 index,
                                   BusInfo& bus) override
    {
        if (type != kAudio || index != 0)
            return kInvalidArgument;
        std::memset (&bus, 0, sizeof (bus));
        bus.mediaType    = kAudio;
        bus.direction    = dir;
        bus.channelCount = 2;
        bus.busType      = kMain;
        bus.flags        = BusInfo::kDefaultActive;
        asciiToString128 (dir == kInput ? "In" : "Out", bus.name);
        return kResultOk;
    }

    tresult PLUGIN_API getRoutingInfo (RoutingInfo&, RoutingInfo&) override
    {
        return kNotImplemented;
    }

    tresult PLUGIN_API activateBus (MediaType type, BusDirection dir, int32 index,
                                    TBool state) override
    {
        if (type != kAudio || index != 0)
            return kInvalidArgument;
        (dir == kInput ? mInBusActive : mOutBusActive) = (state != 0);
        return kResultOk;
    }

    tresult PLUGIN_API setActive (TBool state) override
    {
        mActive = (state != 0);
        return kResultOk;
    }

    // State: the plain gain, one little-endian double, 8 bytes. Anything else
    // is rejected — which is what makes plugin_state_load's "the blob came
    // from a different plugin" case actually return false.
    tresult PLUGIN_API setState (IBStream* state) override
    {
        if (!state)
            return kInvalidArgument;
        uint8_t raw[8];
        int32 got = 0;
        if (state->read (raw, 8, &got) != kResultOk || got != 8)
            return kResultFalse;
        double v = 0.0;
        std::memcpy (&v, raw, 8);      // little-endian host; see file header
        if (!(v >= kGainMin && v <= kGainMax))
            return kResultFalse;
        mGain.store (v, std::memory_order_relaxed);
        return kResultOk;
    }

    tresult PLUGIN_API getState (IBStream* state) override
    {
        if (!state)
            return kInvalidArgument;
        const double v = mGain.load (std::memory_order_relaxed);
        uint8_t raw[8];
        std::memcpy (raw, &v, 8);
        int32 put = 0;
        if (state->write (raw, 8, &put) != kResultOk || put != 8)
            return kResultFalse;
        return kResultOk;
    }

    // ── IConnectionPoint ─────────────────────────────────────────────────────
    // Accepted and remembered; no messages are ever sent. A host that connects
    // the pair must not be told it failed.
    tresult PLUGIN_API connect (IConnectionPoint* other) override
    {
        mPeer = other;
        return kResultOk;
    }
    tresult PLUGIN_API disconnect (IConnectionPoint*) override
    {
        mPeer = nullptr;
        return kResultOk;
    }
    tresult PLUGIN_API notify (IMessage*) override { return kResultOk; }

    // ── IAudioProcessor ──────────────────────────────────────────────────────
    tresult PLUGIN_API setBusArrangements (SpeakerArrangement* inputs, int32 numIns,
                                           SpeakerArrangement* outputs,
                                           int32 numOuts) override
    {
        if (numIns == 1 && numOuts == 1 && inputs && outputs
            && inputs[0] == SpeakerArr::kStereo && outputs[0] == SpeakerArr::kStereo)
            return kResultOk;
        return kResultFalse;
    }

    tresult PLUGIN_API getBusArrangement (BusDirection, int32 index,
                                          SpeakerArrangement& arr) override
    {
        if (index != 0)
            return kInvalidArgument;
        arr = SpeakerArr::kStereo;
        return kResultOk;
    }

    tresult PLUGIN_API canProcessSampleSize (int32 s) override
    {
        return s == kSample32 ? kResultTrue : kResultFalse;
    }

    uint32 PLUGIN_API getLatencySamples () override { return 0; }

    tresult PLUGIN_API setupProcessing (ProcessSetup& setup) override
    {
        if (setup.symbolicSampleSize != kSample32)
            return kResultFalse;
        mSetup = setup;
        return kResultOk;
    }

    tresult PLUGIN_API setProcessing (TBool state) override
    {
        mProcessing = (state != 0);
        return kResultOk;
    }

    uint32 PLUGIN_API getTailSamples () override { return 0; }

    tresult PLUGIN_API process (ProcessData& data) override
    {
        gLastContext = LastContext {};
        gLastContext.any = true;
        if (const ProcessContext* c = data.processContext) {
            gLastContext.state              = c->state;
            gLastContext.tempo              = c->tempo;
            gLastContext.projectTimeMusic   = c->projectTimeMusic;
            gLastContext.barPositionMusic   = c->barPositionMusic;
            gLastContext.timeSigNumerator   = c->timeSigNumerator;
            gLastContext.timeSigDenominator = c->timeSigDenominator;
        }
        const int32 n = data.numSamples;
        if (n <= 0)
            return kResultOk;
        if (data.numOutputs < 1 || !data.outputs || !data.outputs[0].channelBuffers32)
            return kResultOk;

        float** out    = data.outputs[0].channelBuffers32;
        const int32 nOut = data.outputs[0].numChannels;
        float** in     = nullptr;
        int32 nIn      = 0;
        if (data.numInputs >= 1 && data.inputs && data.inputs[0].channelBuffers32) {
            in  = data.inputs[0].channelBuffers32;
            nIn = data.inputs[0].numChannels;
        }

        // The one parameter queue we care about, if the host sent one.
        IParamValueQueue* q = nullptr;
        if (data.inputParameterChanges) {
            const int32 nq = data.inputParameterChanges->getParameterCount ();
            for (int32 i = 0; i < nq; ++i) {
                IParamValueQueue* cand = data.inputParameterChanges->getParameterData (i);
                if (cand && cand->getParameterId () == kGainParamId) { q = cand; break; }
            }
        }
        const int32 nPoints = q ? q->getPointCount () : 0;
        int32 pi = 0;

        double gain = mGain.load (std::memory_order_relaxed);
        for (int32 i = 0; i < n; ++i) {
            // A point takes effect AT its own frame and not before. No
            // smoothing, no ramp: clockwork test asserts the step lands on
            // the exact sample, and a ramp would make that untestable.
            while (pi < nPoints) {
                int32 off = 0;
                ParamValue v = 0.0;
                if (q->getPoint (pi, off, v) != kResultOk) { ++pi; continue; }
                if (off > i) break;
                gain = normToPlain (v);
                ++pi;
            }
            for (int32 c = 0; c < nOut; ++c) {
                if (!out[c]) continue;
                // Outputs beyond the inputs are silent, not copied.
                out[c][i] = (c < nIn && in && in[c])
                                ? in[c][i] * static_cast<float> (gain)
                                : 0.0f;
            }
        }
        mGain.store (gain, std::memory_order_relaxed);
        return kResultOk;
    }

private:
    std::atomic<int32> mRefCount {1};
    std::atomic<double> mGain {kGainDefault};
    ProcessSetup mSetup {kRealtime, kSample32, 0, 48000.0};
    bool mActive = false, mProcessing = false;
    bool mInBusActive = true, mOutBusActive = true;
    IConnectionPoint* mPeer = nullptr;
};

// ── The controller ───────────────────────────────────────────────────────────
class GainController;
// The most recently created controller, for ClockworkTestGainTurn. A test's stand-in
// for the editor window this plugin deliberately has not got. Cleared when
// that controller goes, so the boundary can never reach a freed object.
GainController* gLastController = nullptr;

class GainController : public IEditController, public IConnectionPoint
{
public:
    tresult PLUGIN_API queryInterface (const TUID iid, void** obj) override
    {
        if (!obj)
            return kInvalidArgument;
        if (sameIID (iid, FUnknown_iid) || sameIID (iid, IPluginBase_iid)
            || sameIID (iid, IEditController_iid))
            *obj = static_cast<IEditController*> (this);
        else if (sameIID (iid, IConnectionPoint_iid))
            *obj = static_cast<IConnectionPoint*> (this);
        else {
            *obj = nullptr;
            return kNoInterface;
        }
        addRef ();
        return kResultOk;
    }
    uint32 PLUGIN_API addRef () override { return static_cast<uint32> (++mRefCount); }
    uint32 PLUGIN_API release () override
    {
        const uint32 rc = static_cast<uint32> (--mRefCount);
        if (rc == 0)
        {
            if (gLastController == this)
                gLastController = nullptr;
            delete this;
        }
        return rc;
    }

    tresult PLUGIN_API initialize (FUnknown*) override { return kResultOk; }
    tresult PLUGIN_API terminate () override { return kResultOk; }

    // Primed from the component's own state blob — same 8 little-endian bytes.
    tresult PLUGIN_API setComponentState (IBStream* state) override
    {
        if (!state)
            return kResultFalse;
        uint8_t raw[8];
        int32 got = 0;
        if (state->read (raw, 8, &got) != kResultOk || got != 8)
            return kResultFalse;
        double v = 0.0;
        std::memcpy (&v, raw, 8);
        if (!(v >= kGainMin && v <= kGainMax))
            return kResultFalse;
        mNorm = plainToNorm (v);
        return kResultOk;
    }

    tresult PLUGIN_API setState (IBStream*) override { return kResultOk; }
    tresult PLUGIN_API getState (IBStream*) override { return kResultOk; }

    int32 PLUGIN_API getParameterCount () override { return 1; }

    tresult PLUGIN_API getParameterInfo (int32 index, ParameterInfo& info) override
    {
        if (index != 0)
            return kInvalidArgument;
        std::memset (&info, 0, sizeof (info));
        info.id = kGainParamId;
        asciiToString128 ("gain", info.title);
        asciiToString128 ("gain", info.shortTitle);
        asciiToString128 ("", info.units);
        info.stepCount = 0;                       // continuous
        info.defaultNormalizedValue = plainToNorm (kGainDefault);   // 0.25
        info.unitId = 0;   // kRootUnitId, without pulling in ivstunits.h
        info.flags  = ParameterInfo::kCanAutomate;
        return kResultOk;
    }

    tresult PLUGIN_API getParamStringByValue (ParamID id, ParamValue norm,
                                              String128 out) override
    {
        if (id != kGainParamId)
            return kInvalidArgument;
        char buf[32];
        std::snprintf (buf, sizeof (buf), "%.4f", normToPlain (norm));
        asciiToString128 (buf, out);
        return kResultOk;
    }

    tresult PLUGIN_API getParamValueByString (ParamID id, TChar* str,
                                              ParamValue& out) override
    {
        if (id != kGainParamId || !str)
            return kInvalidArgument;
        char buf[64];
        int i = 0;
        for (; str[i] != 0 && i < 63; ++i)
            buf[i] = static_cast<char> (str[i]);
        buf[i] = 0;
        char* end = nullptr;
        const double plain = std::strtod (buf, &end);
        if (end == buf)
            return kResultFalse;
        out = plainToNorm (plain);
        return kResultOk;
    }

    ParamValue PLUGIN_API normalizedParamToPlain (ParamID id, ParamValue v) override
    {
        return id == kGainParamId ? normToPlain (v) : v;
    }
    ParamValue PLUGIN_API plainParamToNormalized (ParamID id, ParamValue v) override
    {
        return id == kGainParamId ? plainToNorm (v) : v;
    }
    ParamValue PLUGIN_API getParamNormalized (ParamID id) override
    {
        return id == kGainParamId ? mNorm : 0.0;
    }
    tresult PLUGIN_API setParamNormalized (ParamID id, ParamValue v) override
    {
        if (id != kGainParamId)
            return kResultFalse;
        mNorm = v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
        return kResultOk;
    }
    tresult PLUGIN_API setComponentHandler (IComponentHandler* h) override
    {
        mHandler = h;
        return kResultOk;
    }

    // What the editor would do with a knob, had this plugin one: set its own
    // value, then tell the host through the handler. See ClockworkTestGainTurn below.
    void turn (ParamValue norm)
    {
        setParamNormalized (kGainParamId, norm);
        if (!mHandler)
            return;
        mHandler->beginEdit (kGainParamId);
        mHandler->performEdit (kGainParamId, mNorm);
        mHandler->endEdit (kGainParamId);
    }

    // Deliberately none. Hosts must cope; see the file header.
    IPlugView* PLUGIN_API createView (FIDString) override { return nullptr; }

    // ── IConnectionPoint ─────────────────────────────────────────────────────
    tresult PLUGIN_API connect (IConnectionPoint* other) override
    {
        mPeer = other;
        return kResultOk;
    }
    tresult PLUGIN_API disconnect (IConnectionPoint*) override
    {
        mPeer = nullptr;
        return kResultOk;
    }
    tresult PLUGIN_API notify (IMessage*) override { return kResultOk; }

private:
    std::atomic<int32> mRefCount {1};
    ParamValue mNorm = plainToNorm (kGainDefault);
    IConnectionPoint* mPeer = nullptr;
    IComponentHandler* mHandler = nullptr;
};

// ── Factory ──────────────────────────────────────────────────────────────────
// IPluginFactory2 so the vendor string exists at all: PClassInfo has no vendor
// field, and clockwork's PluginDesc reports one.
class GainFactory : public IPluginFactory2
{
public:
    tresult PLUGIN_API queryInterface (const TUID iid, void** obj) override
    {
        if (!obj)
            return kInvalidArgument;
        if (sameIID (iid, FUnknown_iid) || sameIID (iid, IPluginFactory_iid))
            *obj = static_cast<IPluginFactory*> (this);
        else if (sameIID (iid, IPluginFactory2_iid))
            *obj = static_cast<IPluginFactory2*> (this);
        else {
            *obj = nullptr;
            return kNoInterface;
        }
        addRef ();
        return kResultOk;
    }
    // Static singleton: ref counting is a formality.
    uint32 PLUGIN_API addRef () override { return 100; }
    uint32 PLUGIN_API release () override { return 100; }

    tresult PLUGIN_API getFactoryInfo (PFactoryInfo* info) override
    {
        if (!info)
            return kInvalidArgument;
        std::memset (info, 0, sizeof (*info));
        std::strncpy (info->vendor, "Clockwork", PFactoryInfo::kNameSize - 1);
        std::strncpy (info->url, "", PFactoryInfo::kURLSize - 1);
        info->flags = PFactoryInfo::kUnicode;
        return kResultOk;
    }

    // Two classes: the audio module and its controller. Only the first is an
    // "Audio Module Class", so a scan must report exactly one plugin — which
    // is the thing the scan test would otherwise never catch.
    int32 PLUGIN_API countClasses () override { return 2; }

    tresult PLUGIN_API getClassInfo (int32 index, PClassInfo* info) override
    {
        if (!info)
            return kInvalidArgument;
        std::memset (info, 0, sizeof (*info));
        if (index == 0) {
            std::memcpy (info->cid, kGainProcessorUID, sizeof (TUID));
            info->cardinality = PClassInfo::kManyInstances;
            std::strncpy (info->category, kVstAudioEffectClass, PClassInfo::kCategorySize - 1);
            std::strncpy (info->name, "ClockworkTestGain", PClassInfo::kNameSize - 1);
            return kResultOk;
        }
        if (index == 1) {
            std::memcpy (info->cid, kGainControllerUID, sizeof (TUID));
            info->cardinality = PClassInfo::kManyInstances;
            std::strncpy (info->category, kVstComponentControllerClass,
                          PClassInfo::kCategorySize - 1);
            std::strncpy (info->name, "ClockworkTestGain Controller", PClassInfo::kNameSize - 1);
            return kResultOk;
        }
        return kInvalidArgument;
    }

    tresult PLUGIN_API getClassInfo2 (int32 index, PClassInfo2* info) override
    {
        if (!info)
            return kInvalidArgument;
        PClassInfo base;
        if (getClassInfo (index, &base) != kResultOk)
            return kInvalidArgument;
        std::memset (info, 0, sizeof (*info));
        std::memcpy (info->cid, base.cid, sizeof (TUID));
        info->cardinality = base.cardinality;
        std::strncpy (info->category, base.category, PClassInfo::kCategorySize - 1);
        std::strncpy (info->name, base.name, PClassInfo::kNameSize - 1);
        info->classFlags = 0;
        if (index == 0)
            std::strncpy (info->subCategories, PlugType::kFx,
                          PClassInfo2::kSubCategoriesSize - 1);
        std::strncpy (info->vendor, "Clockwork", PClassInfo2::kVendorSize - 1);
        std::strncpy (info->version, "1.0.0", PClassInfo2::kVersionSize - 1);
        std::strncpy (info->sdkVersion, kVstVersionString, PClassInfo2::kVersionSize - 1);
        return kResultOk;
    }

    tresult PLUGIN_API createInstance (FIDString cid, FIDString iid, void** obj) override
    {
        if (!obj)
            return kInvalidArgument;
        *obj = nullptr;
        if (!cid)
            return kNoInterface;
        tresult r = kNoInterface;
        if (sameIID (cid, kGainProcessorUID)) {
            auto* c = new GainProcessor ();
            r = c->queryInterface (iid, obj);
            c->release ();          // queryInterface holds the caller's reference
        } else if (sameIID (cid, kGainControllerUID)) {
            auto* c = new GainController ();
            gLastController = c;
            r = c->queryInterface (iid, obj);
            c->release ();
        }
        return r == kResultOk ? kResultOk : kNoInterface;
    }
};

} // namespace

// ── Module entry points ──────────────────────────────────────────────────────
// Each platform's way of putting a symbol in the export table: a DLL exports
// nothing unless told to, and an ELF/Mach-O built with hidden visibility (as
// this tree is) is the same. The names are the specification's, per platform.
#if defined(_WIN32)
#  define CLOCKWORK_TESTGAIN_EXPORT __declspec(dllexport)
#else
#  define CLOCKWORK_TESTGAIN_EXPORT __attribute__ ((visibility ("default")))
#endif

extern "C" {

CLOCKWORK_TESTGAIN_EXPORT IPluginFactory* PLUGIN_API GetPluginFactory ()
{
    static GainFactory factory;
    return &factory;
}

// Not VST3: a test boundary. Turns the gain knob the way an editor would — the
// controller takes the PLAIN value and reports the edit to its host — so a
// host can be checked for relaying the edit to the processor. Returns false
// when no controller has been created yet.
CLOCKWORK_TESTGAIN_EXPORT bool ClockworkTestGainTurn (double plain)
{
    if (!gLastController)
        return false;
    gLastController->turn (plainToNorm (plain));
    return true;
}

// Not VST3: a test boundary. The musical-time fields of the ProcessContext the
// host handed the last block — its state word as given, so a test can check
// which fields the host declared valid — or false if no block has run.
CLOCKWORK_TESTGAIN_EXPORT bool ClockworkTestGainLastContext (double* tempo, double* beat, double* barStart,
                                                 int* num, int* den, unsigned* state)
{
    if (!gLastContext.any)
        return false;
    if (tempo)    *tempo    = gLastContext.tempo;
    if (beat)     *beat     = gLastContext.projectTimeMusic;
    if (barStart) *barStart = gLastContext.barPositionMusic;
    if (num)      *num      = gLastContext.timeSigNumerator;
    if (den)      *den      = gLastContext.timeSigDenominator;
    if (state)    *state    = gLastContext.state;
    return true;
}

#if defined(__linux__)
CLOCKWORK_TESTGAIN_EXPORT bool ModuleEntry (void*) { return true; }
CLOCKWORK_TESTGAIN_EXPORT bool ModuleExit () { return true; }
#elif defined(__APPLE__)
CLOCKWORK_TESTGAIN_EXPORT bool bundleEntry (void*) { return true; }
CLOCKWORK_TESTGAIN_EXPORT bool bundleExit () { return true; }
#elif defined(_WIN32)
CLOCKWORK_TESTGAIN_EXPORT bool InitDll () { return true; }
CLOCKWORK_TESTGAIN_EXPORT bool ExitDll () { return true; }
#endif

} // extern "C"
