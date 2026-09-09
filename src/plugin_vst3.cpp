// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * plugin_vst3.cpp — the VST3 adapter behind plugin_host.h.
 *
 * Hand-rolled COM, against Steinberg's interface headers only: no SDK base/
 * or public.sdk/ is fetched or compiled, so nothing here inherits from
 * FObject, uses IPtr, or links a hosting helper. The headers are MIT as of
 * VST3 3.8.1 (v3.8.1_build_84 is the first tag carrying an MIT LICENSE.txt);
 * everything at 3.8.0 and earlier is Steinberg-proprietary OR GPLv3.
 *
 * WHAT THE HOST SIDE ACTUALLY OWES A VST3 PLUGIN, and what each entry point
 * therefore does:
 *
 *   scan   load the module, call its entry point (see the loader note
 *          below — both differ by platform), GetPluginFactory, then every class whose
 *          category is "Audio Module Class" — a module also advertises its
 *          controller class, which is not a plugin and must not be listed.
 *          Strings are copied and the module is closed again: a scan of a
 *          directory opens a great deal that it must not keep open.
 *
 *   open   createInstance(IComponent) → initialize(host context) →
 *          getControllerClassId → createInstance(IEditController) →
 *          connect the two IConnectionPoints → prime the controller from the
 *          component's own state → negotiate bus arrangements →
 *          setupProcessing → setActive → setProcessing. That order is the
 *          specification's, and getting it wrong mostly produces silence
 *          rather than an error.
 *
 *   process  one ProcessData per block, with an IParameterChanges carrying
 *          everything plugin_param_set queued since the last block, each at
 *          its own sample offset. The plugin's buses have fixed channel
 *          counts; clockwork's arrive per block. Scratch buffers (allocated
 *          at open) bridge the two, zero-filling inputs clockwork did not
 *          supply and silencing outputs the plugin did not fill.
 *
 *   state  IComponent::getState / setState through a memory IBStream.
 *   latency IAudioProcessor::getLatencySamples, asked per block.
 *
 * EDITORS ARE CREATED, on macOS and Windows. IEditController::createView is
 * called by plugin_editor_open, and the view is attached to a window the
 * engine itself owns (src/native/PluginEditorWindow.mm, or
 * PluginEditorWindowWin.cpp) so the editor belongs to the instance that is
 * actually making sound. VST3 permits createView to return null, so
 * plugin_editor_has answering false is an answer and not a failure. Linux
 * gets plugin_editor_window_stub.cpp, which answers "no window".
 *
 * THE NORMALISATION IS HIDDEN HERE. Every VST3 parameter is 0..1 on the wire
 * and only the plugin's controller knows what that means, so this adapter
 * derives the plain range by asking normalizedParamToPlain at 0 and 1, and
 * converts in both directions. plugin_param_info therefore reports a real
 * range and plugin_param_set takes a real value — a caller never learns that
 * VST3 works in normalised units, which is the entire point of the boundary.
 *
 * THE RT GUARD IS SUSPENDED, DELIBERATELY AND VISIBLY, across every call into
 * plugin code (process and getLatencySamples). See plugin_host.h: a third
 * party allocates on the audio thread and clockwork cannot stop it, so the
 * honest thing is to name the window rather than weaken rt_alloc::Guard
 * everywhere. RtSuspend below is that window and nothing else uses it.
 *
 * MODULE LOADING IS THE ONE PLATFORM-SPECIFIC PART, and it is three loaders
 * behind one interface (Module, loadModule, unloadModule, resolveModulePath).
 * Linux and macOS share dlopen and differ in two ways: the binary inside a
 * .vst3 bundle is Contents/<arch>-linux/*.so on one and Contents/MacOS/<name>
 * with NO EXTENSION on the other, and the initialiser is ModuleEntry(void*)
 * against bundleEntry(CFBundleRef). The CFBundle is not decorative — a plugin
 * uses it to find the resources beside its binary, and Surge will not produce
 * sound without its wavetables.
 *
 * Windows is Contents/<arch>-win/<name>.vst3 — a DLL that keeps the bundle's
 * suffix — loaded with LoadLibraryExW and initialised through InitDll(),
 * which is optional in the specification (a module without it is still a
 * module), with ExitDll() before FreeLibrary. Older plugins ship as a bare
 * .vst3 DLL with no bundle around it, and that is accepted too. THE WINDOWS
 * BRANCH WAS WRITTEN AGAINST THE SPECIFICATION ON A MAC and has not yet been
 * compiled or run on Windows; docs/TRACKS.md says so as well.
 */

#include "plugin_vst3.h"
#include "clockwork_product.h"

#include "plugin_transport.h"   // the block's musical position, shared with CLAP
#include "rt_alloc.h"
#include "shared_memory.h"   // ClockworkClockState

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"   // IMidiMapping — CC and bend land as parameters
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/ivstunits.h"   // IUnitInfo — the NAMES behind unitId
#include "pluginterfaces/vst/vsttypes.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
// The adapter is built in its own CMake directory and does not inherit
// clockwork's compile definitions, so windows.h is tamed here: without NOMINMAX
// its min/max macros break every std::min/std::max in this file.
#  ifndef NOMINMAX
#    define NOMINMAX 1
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN 1
#  endif
#  include <windows.h>
#  include <cwctype>      // towlower — matching ".VST3" as well as ".vst3"
#  include <filesystem>
#  include "clockwork_path.h"
#else
#  include <dirent.h>
#  include <dlfcn.h>
#  include <sys/stat.h>
#  if defined(__APPLE__)
#    include <CoreFoundation/CoreFoundation.h>
#  endif
#endif

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace clockwork_plugin_vst3 {

struct Instance;
// Defined with Instance below: queues an editor's edit for the processor.
void editor_edit(Instance* p, uint32_t id, double normalized);

namespace {

// ── The one deliberate hole in clockwork's RT guarantee ────────────────────
// rt_alloc::Guard sets g_in_rt; the test binary's operator new asserts on it.
// A hosted plugin will allocate inside process() and no host can prevent it,
// so the flag is cleared for exactly the duration of the call into plugin
// code and restored after — never wider, and never anywhere else.
struct RtSuspend {
    bool prev;
    RtSuspend() : prev(rt_alloc::g_in_rt) { rt_alloc::g_in_rt = false; }
    ~RtSuspend() { rt_alloc::g_in_rt = prev; }
};

// void* signature so both TUID (int8[16]) and FIDString (char*) callers fit.
inline bool sameIID(const void* a, const void* b) {
    return a && b && std::memcmp(a, b, sizeof(TUID)) == 0;
}

std::string tuidToHex(const TUID t) {
    static const char* kHex = "0123456789ABCDEF";
    std::string s(32, '0');
    for (int i = 0; i < 16; ++i) {
        const unsigned char b = static_cast<unsigned char>(t[i]);
        s[i * 2]     = kHex[b >> 4];
        s[i * 2 + 1] = kHex[b & 0x0F];
    }
    return s;
}

// String128 is UTF-16. Plugin names in practice are ASCII; anything above
// 0x7F is replaced rather than silently truncated to a wrong byte.
std::string string128ToUtf8(const TChar* s) {
    std::string out;
    if (!s) return out;
    for (int i = 0; i < 128 && s[i] != 0; ++i)
        out.push_back(s[i] < 0x80 ? static_cast<char>(s[i]) : '?');
    return out;
}

thread_local std::string g_err;
const char* fail(const char** err, const std::string& msg) {
    g_err = msg;
    if (err) *err = g_err.c_str();
    return g_err.c_str();
}

// ── A minimal IHostApplication ───────────────────────────────────────────────
// Passed to IPluginBase::initialize. Plugins query it for a name and for
// IMessage/IAttributeList factories; this host sends no messages, so those are
// refused honestly rather than half-implemented. A plugin that insists on
// them will fail at initialize, which is a real answer.
class HostContext : public IHostApplication {
public:
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (!obj) return kInvalidArgument;
        if (sameIID(iid, FUnknown_iid) || sameIID(iid, IHostApplication_iid)) {
            *obj = static_cast<IHostApplication*>(this);
            addRef();
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 100; }   // static singleton
    uint32 PLUGIN_API release() override { return 100; }

    tresult PLUGIN_API getName(String128 name) override {
        static const char* kName = CLOCKWORK_PRODUCT_NAME;
        int i = 0;
        for (; kName[i] != 0; ++i) name[i] = static_cast<TChar>(kName[i]);
        name[i] = 0;
        return kResultOk;
    }
    tresult PLUGIN_API createInstance(TUID /*cid*/, TUID /*iid*/, void** obj) override {
        if (obj) *obj = nullptr;
        return kNotImplemented;
    }
};
HostContext& hostContext() {
    static HostContext ctx;
    return ctx;
}

// ── A memory IBStream ────────────────────────────────────────────────────────
// Everything state save/load needs and nothing more. Host-owned, so addRef and
// release are formalities; a plugin that retained the stream past the call
// would be violating the contract anyway.
class MemStream : public IBStream {
public:
    MemStream() = default;
    explicit MemStream(const uint8_t* d, uint32_t n) : mBuf(d, d + n) {}

    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (!obj) return kInvalidArgument;
        if (sameIID(iid, FUnknown_iid) || sameIID(iid, IBStream_iid)) {
            *obj = static_cast<IBStream*>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 100; }
    uint32 PLUGIN_API release() override { return 100; }

    tresult PLUGIN_API read(void* buffer, int32 numBytes, int32* numRead) override {
        if (!buffer || numBytes < 0) return kInvalidArgument;
        const size_t avail = mBuf.size() - std::min(mPos, mBuf.size());
        const size_t n = std::min(static_cast<size_t>(numBytes), avail);
        if (n) std::memcpy(buffer, mBuf.data() + mPos, n);
        mPos += n;
        if (numRead) *numRead = static_cast<int32>(n);
        return kResultOk;
    }
    tresult PLUGIN_API write(void* buffer, int32 numBytes, int32* numWritten) override {
        if (!buffer || numBytes < 0) return kInvalidArgument;
        const size_t n = static_cast<size_t>(numBytes);
        if (mPos + n > mBuf.size()) mBuf.resize(mPos + n);
        std::memcpy(mBuf.data() + mPos, buffer, n);
        mPos += n;
        if (numWritten) *numWritten = static_cast<int32>(n);
        return kResultOk;
    }
    tresult PLUGIN_API seek(int64 pos, int32 mode, int64* result) override {
        int64 base = 0;
        if (mode == kIBSeekSet) base = 0;
        else if (mode == kIBSeekCur) base = static_cast<int64>(mPos);
        else if (mode == kIBSeekEnd) base = static_cast<int64>(mBuf.size());
        else return kInvalidArgument;
        const int64 want = base + pos;
        if (want < 0) return kInvalidArgument;
        mPos = static_cast<size_t>(want);
        if (result) *result = want;
        return kResultOk;
    }
    tresult PLUGIN_API tell(int64* pos) override {
        if (!pos) return kInvalidArgument;
        *pos = static_cast<int64>(mPos);
        return kResultOk;
    }

    void rewind() { mPos = 0; }
    const std::vector<uint8_t>& bytes() const { return mBuf; }

private:
    std::vector<uint8_t> mBuf;
    size_t mPos = 0;
};

// ── Host-side parameter changes ──────────────────────────────────────────────
// One queue per parameter that changed this block. Capacity is fixed at open
// so nothing here allocates during process, whatever the plugin does.
class HostParamQueue : public IParamValueQueue {
public:
    void init(uint32_t capacity) { mPts.reserve(capacity); mCap = capacity; }
    void reset(ParamID id) { mId = id; mPts.clear(); }
    bool full() const { return mPts.size() >= mCap; }
    void push(int32 offset, ParamValue v) { if (!full()) mPts.push_back({offset, v}); }
    ParamID id() const { return mId; }
    bool empty() const { return mPts.empty(); }

    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (!obj) return kInvalidArgument;
        if (sameIID(iid, FUnknown_iid) || sameIID(iid, IParamValueQueue_iid)) {
            *obj = static_cast<IParamValueQueue*>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 100; }
    uint32 PLUGIN_API release() override { return 100; }

    ParamID PLUGIN_API getParameterId() override { return mId; }
    int32 PLUGIN_API getPointCount() override { return static_cast<int32>(mPts.size()); }
    tresult PLUGIN_API getPoint(int32 index, int32& sampleOffset, ParamValue& value) override {
        if (index < 0 || static_cast<size_t>(index) >= mPts.size()) return kResultFalse;
        sampleOffset = mPts[static_cast<size_t>(index)].first;
        value        = mPts[static_cast<size_t>(index)].second;
        return kResultOk;
    }
    // Host-side queue: a plugin writing into an INPUT queue is out of contract.
    tresult PLUGIN_API addPoint(int32, ParamValue, int32&) override { return kNotImplemented; }

private:
    ParamID mId = 0;
    size_t mCap = 0;
    std::vector<std::pair<int32, ParamValue>> mPts;
};

/*
 * Where a plugin's OWN edits come back to the host.
 *
 * A plugin's editor does not change the sound by itself. Turn a knob and the
 * plugin calls IComponentHandler::performEdit on the host, and it is the host's
 * job to decide what that means. With no handler set, VST3 permits the plugin
 * to carry on — the controller updates and the host simply never hears about
 * it — which is exactly the failure that makes an embedded editor look alive
 * and change nothing.
 *
 * This exists so an embedder can be told. The callback is invoked on whatever
 * thread the plugin edits on, which for a GUI is the main thread.
 */
class HostComponentHandler : public IComponentHandler {
public:
    void setOwner(Instance* p) { mOwner = p; }
    void setListener(void (*fn)(void*, uint32_t, double), void* ctx) {
        mFn = fn; mCtx = ctx;
    }

    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (!obj) return kInvalidArgument;
        if (sameIID(iid, FUnknown_iid) || sameIID(iid, IComponentHandler_iid)) {
            *obj = static_cast<IComponentHandler*>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 100; }
    uint32 PLUGIN_API release() override { return 100; }

    // A gesture is starting/ending. Nothing to do: the host has no automation
    // recording to bracket, and answering kResultOk is what a plugin expects.
    tresult PLUGIN_API beginEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API endEdit(ParamID) override { return kResultOk; }

    // A knob turned in the plugin's own editor. The controller already holds
    // the value (it is the controller's edit), but the PROCESSOR does not: in
    // VST3 the two never share state, and relaying the edit through the next
    // block's IParameterChanges is the host's job. Miss it and the editor is
    // a picture — every control moves and nothing changes.
    tresult PLUGIN_API performEdit(ParamID id, ParamValue normalized) override {
        editor_edit(mOwner, static_cast<uint32_t>(id), normalized);
        if (mFn) mFn(mCtx, static_cast<uint32_t>(id), normalized);
        return kResultOk;
    }

    // The plugin is telling us its parameter list or latency changed. Accepted
    // rather than acted on: nothing here caches enough for a stale view to
    // matter, and refusing would make some plugins retry forever.
    tresult PLUGIN_API restartComponent(int32) override { return kResultOk; }

private:
    Instance* mOwner = nullptr;
    void (*mFn)(void*, uint32_t, double) = nullptr;
    void*  mCtx = nullptr;
};

/*
 * Note events for the block.
 *
 * A synth needs notes and the boundary had none: plugin_host.h enumerated
 * parameters and stopped there, which is enough for an effect and leaves an
 * instrument permanently silent. ProcessData::inputEvents was passed as
 * nullptr, so Surge XT loaded, reported 2855 parameters, and could not be
 * played.
 *
 * Fixed capacity, filled between blocks and drained by the plugin during
 * process. Overflow drops rather than grows: this is read on the audio thread,
 * and a note lost under a flood is better than an allocation there.
 */
class HostEventList : public IEventList {
public:
    void init(uint32_t cap) { mEvents.assign(std::max<uint32_t>(cap, 1), Event{}); mUsed = 0; }
    void clear() { mUsed = 0; }
    bool any() const { return mUsed != 0; }

    void addNoteOn(int16 channel, int16 pitch, float velocity, int32 offset, int32 noteId) {
        if (mUsed >= mEvents.size()) return;
        Event& e = mEvents[mUsed];
        std::memset(&e, 0, sizeof e);
        e.type = Event::kNoteOnEvent;
        e.sampleOffset = offset;
        e.noteOn.channel  = channel;
        e.noteOn.pitch    = pitch;
        e.noteOn.velocity = velocity;
        e.noteOn.noteId   = noteId;
        ++mUsed;
    }

    void addNoteOff(int16 channel, int16 pitch, float velocity, int32 offset, int32 noteId) {
        if (mUsed >= mEvents.size()) return;
        Event& e = mEvents[mUsed];
        std::memset(&e, 0, sizeof e);
        e.type = Event::kNoteOffEvent;
        e.sampleOffset = offset;
        e.noteOff.channel  = channel;
        e.noteOff.pitch    = pitch;
        e.noteOff.velocity = velocity;
        e.noteOff.noteId   = noteId;
        ++mUsed;
    }

    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (!obj) return kInvalidArgument;
        if (sameIID(iid, FUnknown_iid) || sameIID(iid, IEventList_iid)) {
            *obj = static_cast<IEventList*>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 100; }
    uint32 PLUGIN_API release() override { return 100; }

    int32 PLUGIN_API getEventCount() override { return static_cast<int32>(mUsed); }
    tresult PLUGIN_API getEvent(int32 index, Event& e) override {
        if (index < 0 || static_cast<size_t>(index) >= mUsed) return kResultFalse;
        e = mEvents[static_cast<size_t>(index)];
        return kResultOk;
    }
    // Host-owned input list; a plugin appending to it is out of contract.
    tresult PLUGIN_API addEvent(Event&) override { return kNotImplemented; }

private:
    std::vector<Event> mEvents;
    size_t mUsed = 0;
};

class HostParamChanges : public IParameterChanges {
public:
    void init(uint32_t maxParams, uint32_t pointsPerParam) {
        mQueues.resize(std::max<uint32_t>(maxParams, 1));
        for (auto& q : mQueues) q.init(pointsPerParam);
        mUsed = 0;
    }
    void clear() { mUsed = 0; }

    // Points for one id land in one queue, in the order they were queued —
    // which is what "sample offsets ascending" means for a host that only
    // ever appends.
    void add(ParamID id, int32 offset, ParamValue v) {
        for (size_t i = 0; i < mUsed; ++i)
            if (mQueues[i].id() == id) { mQueues[i].push(offset, v); return; }
        if (mUsed >= mQueues.size()) return;       // more distinct ids than params
        mQueues[mUsed].reset(id);
        mQueues[mUsed].push(offset, v);
        ++mUsed;
    }
    bool any() const { return mUsed != 0; }

    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (!obj) return kInvalidArgument;
        if (sameIID(iid, FUnknown_iid) || sameIID(iid, IParameterChanges_iid)) {
            *obj = static_cast<IParameterChanges*>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 100; }
    uint32 PLUGIN_API release() override { return 100; }

    int32 PLUGIN_API getParameterCount() override { return static_cast<int32>(mUsed); }
    IParamValueQueue* PLUGIN_API getParameterData(int32 index) override {
        if (index < 0 || static_cast<size_t>(index) >= mUsed) return nullptr;
        return &mQueues[static_cast<size_t>(index)];
    }
    IParamValueQueue* PLUGIN_API addParameterData(const ParamID&, int32&) override {
        return nullptr;   // host-owned input list; plugins do not extend it
    }

private:
    std::vector<HostParamQueue> mQueues;
    size_t mUsed = 0;
};

// ── Module loading ───────────────────────────────────────────────────────────
struct Module {
    void* handle = nullptr;   // dlopen's handle, or an HMODULE on Windows
    IPluginFactory* factory = nullptr;
    bool (*moduleExit)() = nullptr;
#if defined(__APPLE__)
    // macOS hands the plugin its own CFBundle at init so it can find the
    // resources sitting beside its binary — presets, wavetables, the editor's
    // images. Retained for the module's lifetime because the plugin keeps
    // using it long after bundleEntry returns.
    CFBundleRef bundle = nullptr;
#endif
};

#if defined(_WIN32)

// ── Windows ──────────────────────────────────────────────────────────────────
// Paths cross this file as UTF-8, because that is what the OSC surface and
// plugin_host.h speak; the Win32 calls that take them want UTF-16, and
// clockwork_path.h holds the conversions (LoadLibraryA would mangle a plugin under
// a user name with an accent in it). GetLastError becomes prose the way
// dlerror gives it on POSIX, with the code kept so a message the system has
// no text for is still something to search for.
using clockwork_path::to_wide;
using clockwork_path::from_wide;
std::string lastErrorText(DWORD code) {
    return clockwork_path::last_error_text(code) + " (error " + std::to_string(static_cast<unsigned long>(code)) + ")";
}

// The bundle folder for THIS build's architecture, in the specification's
// spelling. A plugin built for another architecture cannot be loaded by this
// process whatever folder it sits in, so this is the only folder that can
// hold a module worth trying first.
const char* windowsArchDir() {
#if defined(_M_ARM64) || defined(__aarch64__)
    return "arm64-win";
#elif defined(_M_X64) || defined(__x86_64__)
    return "x86_64-win";
#elif defined(_M_IX86) || defined(__i386__)
    return "x86-win";
#else
    return "x86_64-win";
#endif
}

// A VST3 "file" on Windows is usually a bundle directory —
// Name.vst3/Contents/<arch>-win/Name.vst3, the inner one a DLL that keeps the
// suffix — and occasionally, for plugins from before the bundle format, a
// bare .vst3 DLL with nothing around it. Accept either. The bundle's binary
// is looked for by the bundle's own name first, then by suffix within the
// architecture folder, then by suffix in ANY *-win folder: that last one will
// not load, but it makes the error "not a valid Win32 application" rather
// than a silent fall-through to a directory that LoadLibrary refuses with a
// message about access being denied.
std::string resolveModulePath(const std::string& path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    // A trailing separator ("Name.vst3\") would leave filename() empty below.
    std::string trimmed = path;
    while (trimmed.size() > 1 && (trimmed.back() == '\\' || trimmed.back() == '/'))
        trimmed.pop_back();
    const fs::path root(to_wide(trimmed));
    if (!fs::is_directory(root, ec)) return path;

    const fs::path contents = root / L"Contents";
    const fs::path archDir  = contents / to_wide(windowsArchDir());

    // The inner binary carries the bundle's own file name.
    const fs::path named = archDir / root.filename();
    if (fs::is_regular_file(named, ec)) return from_wide(named.wstring());

    // Windows file names are case-insensitive, so ".VST3" is the same suffix.
    auto isDll = [](const fs::path& f) {
        std::wstring ext = f.extension().wstring();
        for (auto& ch : ext) ch = static_cast<wchar_t>(std::towlower(ch));
        return ext == L".vst3";
    };
    auto firstDllIn = [&](const fs::path& dir) -> std::string {
        std::error_code e2;
        fs::directory_iterator it(dir, e2);
        if (e2) return std::string();
        for (const auto& entry : it) {
            if (entry.is_regular_file(e2) && isDll(entry.path()))
                return from_wide(entry.path().wstring());
        }
        return std::string();
    };

    std::string found = firstDllIn(archDir);
    if (!found.empty()) return found;

    fs::directory_iterator subs(contents, ec);
    if (ec) return path;
    for (const auto& sub : subs) {
        std::error_code e2;
        if (!sub.is_directory(e2)) continue;
        const std::wstring name = sub.path().filename().wstring();
        if (name.size() < 4 || name.compare(name.size() - 4, 4, L"-win") != 0) continue;
        found = firstDllIn(sub.path());
        if (!found.empty()) return found;
    }
    return path;
}

bool loadModule(const char* path, Module& m, std::string& err) {
    std::string real = resolveModulePath(path ? path : "");
    if (real.empty()) { err = "empty path"; return false; }

    // LOAD_WITH_ALTERED_SEARCH_PATH: the DLLs a plugin ships beside itself
    // are found relative to the module, not to the bridge's own directory.
    // The flag is only defined for an absolute path, so relative ones are
    // made absolute first rather than handed over and hoped for.
    {
        std::error_code ec;
        const std::filesystem::path abs =
            std::filesystem::absolute(std::filesystem::path(to_wide(real)), ec);
        if (!ec) real = from_wide(abs.wstring());
    }

    const std::wstring wide = to_wide(real);
    HMODULE dll = LoadLibraryExW(wide.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!dll) {
        err = "LoadLibrary failed: " + lastErrorText(GetLastError());
        return false;
    }
    m.handle = dll;

    // InitDll is Windows's initialiser and, like ModuleEntry on Linux, is
    // tolerated as absent: the specification makes it optional and older
    // modules omit it. Present and false is a refusal, and the module goes
    // straight back out.
    if (auto init = reinterpret_cast<bool (*)()>(GetProcAddress(dll, "InitDll"))) {
        if (!init()) {
            FreeLibrary(dll);
            m.handle = nullptr;
            err = "InitDll returned false";
            return false;
        }
    }
    m.moduleExit = reinterpret_cast<bool (*)()>(GetProcAddress(dll, "ExitDll"));

    auto getFactory = reinterpret_cast<IPluginFactory* (PLUGIN_API*)()>(
        GetProcAddress(dll, "GetPluginFactory"));
    if (!getFactory) {
        if (m.moduleExit) m.moduleExit();
        FreeLibrary(dll);
        m.handle = nullptr;
        m.moduleExit = nullptr;
        err = "no GetPluginFactory export — not a VST3 module";
        return false;
    }
    m.factory = getFactory();
    if (!m.factory) {
        if (m.moduleExit) m.moduleExit();
        FreeLibrary(dll);
        m.handle = nullptr;
        m.moduleExit = nullptr;
        err = "GetPluginFactory returned null";
        return false;
    }
    return true;
}

void unloadModule(Module& m) {
    if (m.factory) { m.factory->release(); m.factory = nullptr; }
    // ExitDll before FreeLibrary, for the same reason bundleExit precedes
    // CFRelease on macOS: the module's own shutdown runs while its code is
    // still mapped.
    if (m.moduleExit) m.moduleExit();
    if (m.handle) FreeLibrary(static_cast<HMODULE>(m.handle));
    m.handle = nullptr;
    m.moduleExit = nullptr;
}

#else  // POSIX: dlopen on Linux and macOS

// A VST3 "file" is usually a bundle directory: Name.vst3/Contents/<arch>/Name.so.
// Accept either that or a bare shared object, because a build tree holds the
// second and an installed plugin the first.
//
// macOS is the same idea with two differences that both matter: the binary
// lives in Contents/MacOS, and it carries NO EXTENSION — so the ".so" sweep
// below finds nothing and would silently hand back the bundle directory, which
// dlopen then refuses with a message about the path not being a mach-o file.
// Look for the executable by position instead of by suffix.
std::string resolveModulePath(const std::string& path) {
    struct stat st {};
    if (stat(path.c_str(), &st) != 0) return path;
    if (!S_ISDIR(st.st_mode)) return path;

#if defined(__APPLE__)
    {
        const std::string macos = path + "/Contents/MacOS";
        if (DIR* md = opendir(macos.c_str())) {
            std::string exe;
            while (struct dirent* e = readdir(md)) {
                const std::string n = e->d_name;
                if (n == "." || n == "..") continue;
                struct stat es {};
                const std::string cand = macos + "/" + n;
                // Regular file, executable bit set: a bundle holds exactly one.
                if (stat(cand.c_str(), &es) == 0 && S_ISREG(es.st_mode)
                    && (es.st_mode & S_IXUSR)) {
                    exe = cand;
                    break;
                }
            }
            closedir(md);
            if (!exe.empty()) return exe;
        }
    }
#endif

    const std::string contents = path + "/Contents";
    DIR* d = opendir(contents.c_str());
    if (!d) return path;
    std::string found;
    while (struct dirent* e = readdir(d)) {
        const std::string sub = e->d_name;
        if (sub == "." || sub == "..") continue;
        const std::string archDir = contents + "/" + sub;
        DIR* a = opendir(archDir.c_str());
        if (!a) continue;
        while (struct dirent* f = readdir(a)) {
            const std::string name = f->d_name;
            if (name.size() > 3 && name.compare(name.size() - 3, 3, ".so") == 0) {
                found = archDir + "/" + name;
                break;
            }
        }
        closedir(a);
        if (!found.empty()) break;
    }
    closedir(d);
    return found.empty() ? path : found;
}

bool loadModule(const char* path, Module& m, std::string& err) {
    const std::string real = resolveModulePath(path ? path : "");
    if (real.empty()) { err = "empty path"; return false; }

    m.handle = dlopen(real.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!m.handle) {
        const char* e = dlerror();
        err = std::string("dlopen failed: ") + (e ? e : "unknown");
        return false;
    }
#if defined(__APPLE__)
    /*
     * macOS initialises through bundleEntry(CFBundleRef), not ModuleEntry, and
     * the argument is not decorative: the plugin keeps the bundle to find the
     * resources next to its binary — Surge's wavetables and patches live in
     * Contents/Resources and it will not produce sound without them.
     *
     * So the CFBundle is built from the .vst3 DIRECTORY, not from the
     * executable inside it that dlopen was given. Retained until unload.
     */
    {
        // Walk back up from Contents/MacOS/<exe> to the bundle root.
        std::string root = real;
        for (int i = 0; i < 3; ++i) {
            const auto slash = root.find_last_of('/');
            if (slash == std::string::npos) break;
            root.erase(slash);
        }
        if (CFURLRef url = CFURLCreateFromFileSystemRepresentation(
                nullptr, reinterpret_cast<const UInt8*>(root.c_str()),
                (CFIndex)root.size(), true)) {
            m.bundle = CFBundleCreate(nullptr, url);
            CFRelease(url);
        }
        if (auto entry = reinterpret_cast<bool (*)(CFBundleRef)>(
                dlsym(m.handle, "bundleEntry"))) {
            if (!entry(m.bundle)) {
                if (m.bundle) { CFRelease(m.bundle); m.bundle = nullptr; }
                dlclose(m.handle);
                m.handle = nullptr;
                err = "bundleEntry returned false";
                return false;
            }
        }
        m.moduleExit = reinterpret_cast<bool (*)()>(dlsym(m.handle, "bundleExit"));
    }
#else
    // ModuleEntry is Linux's initialiser and is not optional for real plugins;
    // it is tolerated as absent so a plain .so built in-tree also loads.
    if (auto entry = reinterpret_cast<bool (*)(void*)>(dlsym(m.handle, "ModuleEntry"))) {
        if (!entry(m.handle)) {
            dlclose(m.handle);
            m.handle = nullptr;
            err = "ModuleEntry returned false";
            return false;
        }
    }
    m.moduleExit = reinterpret_cast<bool (*)()>(dlsym(m.handle, "ModuleExit"));
#endif

    auto getFactory = reinterpret_cast<IPluginFactory* (PLUGIN_API*)()>(
        dlsym(m.handle, "GetPluginFactory"));
    if (!getFactory) {
        if (m.moduleExit) m.moduleExit();
        dlclose(m.handle);
        m.handle = nullptr;
        err = "no GetPluginFactory export — not a VST3 module";
        return false;
    }
    m.factory = getFactory();
    if (!m.factory) {
        if (m.moduleExit) m.moduleExit();
        dlclose(m.handle);
        m.handle = nullptr;
        err = "GetPluginFactory returned null";
        return false;
    }
    return true;
}

void unloadModule(Module& m) {
    if (m.factory) { m.factory->release(); m.factory = nullptr; }
    if (m.moduleExit) m.moduleExit();
#if defined(__APPLE__)
    // After bundleExit: the plugin may still touch its bundle while shutting
    // down, and releasing first has it read a dead CFBundleRef.
    if (m.bundle) { CFRelease(m.bundle); m.bundle = nullptr; }
#endif
    if (m.handle) dlclose(m.handle);
    m.handle = nullptr;
    m.moduleExit = nullptr;
}
#endif  // _WIN32 / POSIX

// ── Scan cache ───────────────────────────────────────────────────────────────
// PluginDesc's strings are borrowed, and must survive until the same path is
// scanned again. One entry per path, replaced wholesale on rescan.
struct ScanEntry { std::string id, name, vendor; bool instrument = false; };
std::mutex g_scan_mu;
std::map<std::string, std::vector<ScanEntry>> g_scan_cache;

}  // namespace

// ── Instance ─────────────────────────────────────────────────────────────────
struct Instance {
    Module mod;
    IComponent*      component  = nullptr;
    IAudioProcessor* processor  = nullptr;
    IEditController* controller = nullptr;
    IConnectionPoint* cpComponent  = nullptr;
    IConnectionPoint* cpController = nullptr;
    bool controllerIsSeparate = false;

    double   sampleRate = 48000.0;
    uint32_t maxBlock   = 0;

    int32 busIn = 0, busOut = 0;         // channels on audio bus 0, each way
    std::vector<float>  inStore, outStore;   // channel-major, maxBlock stride
    std::vector<float*> inPtrs,  outPtrs;

    HostParamChanges changes;
    HostEventList    events;

    // The plugin's own editor, when it has one and someone asked for it.
    // Main-thread only: VST3 views are UI objects and the format says so.
    IPlugView* view = nullptr;

    // Where the plugin reports its own parameter edits — a knob turned in its
    // editor arrives here. Lives as long as the instance because the
    // controller keeps the pointer.
    HostComponentHandler handler;

    // plugin_note_on/off (control thread) → plugin_process (audio thread).
    // Same shape and same reasoning as the parameter ring below: fixed size,
    // lock-free, and a full ring drops rather than blocks.
    struct PendNote { uint16_t on; int16_t channel, pitch; float velocity; uint32_t offset; };
    static constexpr uint32_t kNoteCap = 256;
    PendNote note[kNoteCap];
    std::atomic<uint32_t> noteHead{0};
    std::atomic<uint32_t> noteTail{0};

    // VST3 has no MIDI CC event: a controller, a pitch bend or aftertouch is
    // delivered as a PARAMETER CHANGE, to whichever parameter the plugin's
    // IMidiMapping assigns it. Asked once at open, per channel and control,
    // so a CC on the audio thread is a table lookup and not a virtual call
    // into the controller. kNoParamId where the plugin maps nothing.
    static constexpr uint32_t kMidiChannels = 16;
    static constexpr uint32_t kMidiCtrls    = kCountCtrlNumber;   // 0..127 + aftertouch, bend, …
    uint32_t midiMap[kMidiChannels][kMidiCtrls];
    bool     hasMidiMap = false;

    // Cached parameter descriptions. Names are borrowed by PluginParam, so
    // they live as long as the instance; min/max come from the controller's
    // own normalised→plain mapping, asked once at open.
    struct ParamDesc { uint32_t id; std::string name; double min, max;
                       int32_t group; std::string groupName; bool automatable; };
    std::vector<ParamDesc> params;

    // plugin_param_set (control thread) → plugin_process (audio thread).
    // A fixed SPSC ring: no allocation, no lock, and a full ring drops rather
    // than blocks — one dropped automation point beats a stalled callback.
    struct Pending { uint32_t id; double norm; uint32_t offset; };
    static constexpr uint32_t kPendCap = 512;
    Pending pend[kPendCap];
    std::atomic<uint32_t> pendHead{0};
    std::atomic<uint32_t> pendTail{0};

    // The plugin's editor (main thread) → plugin_process (audio thread). Its
    // own ring, not the one above: that one is single-producer too, and the
    // editor is a second producer on a different thread. Offset is always 0
    // — a hand on a knob has no sample to be accurate to.
    static constexpr uint32_t kEditCap = 256;
    Pending edit[kEditCap];
    std::atomic<uint32_t> editHead{0};
    std::atomic<uint32_t> editTail{0};

    int64_t framesSeen = 0;

    // The session clock, or null. Read per block as one clockwork::Timeline
    // snapshot (Timeline::fromClockState) — never field by field, which is
    // how the coherence protocol gets broken.
    const ClockworkClockState* clock = nullptr;
    // The timeline bound for the coming blocks, when a track is following
    // one that is not the clock (set_timeline). Audio thread only.
    ClockworkTimeline timeline {};
    bool        hasTimeline = false;
};

namespace {

// An OSC timetag → nanoseconds since the Unix epoch, which is what
// ProcessContext::systemTime means. The epoch conversion is clock_math.h's,
// the same one the musical-time fields go through.
int64_t oscTimetagToSystemNs(int64_t tt) {
    if (tt == 0) return 0;
    const double unixSeconds = clockwork::oscTimetagToNtp(tt) - clockwork::kNtpEpochOffset;
    return static_cast<int64_t>(unixSeconds * 1e9);
}

void teardown(Instance* p) {
    if (!p) return;
    if (p->processor) { p->processor->setProcessing(false); }
    if (p->component) { p->component->setActive(false); }
    if (p->cpComponent && p->cpController) {
        p->cpComponent->disconnect(p->cpController);
        p->cpController->disconnect(p->cpComponent);
    }
    if (p->cpController) { p->cpController->release(); p->cpController = nullptr; }
    if (p->cpComponent)  { p->cpComponent->release();  p->cpComponent  = nullptr; }
    if (p->controller) {
        if (p->controllerIsSeparate) p->controller->terminate();
        p->controller->release();
        p->controller = nullptr;
    }
    if (p->processor) { p->processor->release(); p->processor = nullptr; }
    if (p->component) {
        p->component->terminate();
        p->component->release();
        p->component = nullptr;
    }
    unloadModule(p->mod);
}

}  // namespace

// ── Discovery ────────────────────────────────────────────────────────────────
uint32_t scan(const char* path, PluginDesc* out, uint32_t cap) {
    if (!path || !*path) return 0;

    Module m;
    std::string err;
    if (!loadModule(path, m, err))
        return 0;   // a refusal, not an error: see plugin_host.h

    PFactoryInfo fi {};
    std::string factoryVendor;
    if (m.factory->getFactoryInfo(&fi) == kResultOk)
        factoryVendor = fi.vendor;

    IPluginFactory2* f2 = nullptr;
    m.factory->queryInterface(IPluginFactory2_iid, reinterpret_cast<void**>(&f2));

    std::vector<ScanEntry> found;
    const int32 n = m.factory->countClasses();
    for (int32 i = 0; i < n; ++i) {
        PClassInfo ci {};
        if (m.factory->getClassInfo(i, &ci) != kResultOk) continue;
        // Only audio modules are plugins. A module also advertises its
        // controller class, and listing that would offer the host something
        // it can never instantiate as a processor.
        if (std::strcmp(ci.category, kVstAudioEffectClass) != 0) continue;

        ScanEntry e;
        e.id     = tuidToHex(ci.cid);
        e.name   = ci.name;
        e.vendor = factoryVendor;
        if (f2) {
            PClassInfo2 ci2 {};
            if (f2->getClassInfo2(i, &ci2) == kResultOk) {
                if (ci2.vendor[0] != '\0') e.vendor = ci2.vendor;
                // subCategories is a pipe-separated list — "Instrument|Synth",
                // "Fx|Reverb". The plugin states its own kind here, which is
                // why a host never has to ask the user for it.
                // "Instrument" alone, not kInstrumentSynth: the SDK spells out
                // Instrument|Synth, Instrument|Sampler, Instrument|Drum and
                // more, and matching the prefix catches every one of them
                // rather than the single flavour someone thought of.
                e.instrument = std::strstr(ci2.subCategories, "Instrument") != nullptr;
            }
        }
        found.push_back(std::move(e));
    }
    if (f2) f2->release();
    unloadModule(m);

    std::lock_guard<std::mutex> lk(g_scan_mu);
    auto& cached = g_scan_cache[path];
    cached = std::move(found);
    if (out) {
        const uint32_t k = std::min<uint32_t>(cap, static_cast<uint32_t>(cached.size()));
        for (uint32_t i = 0; i < k; ++i) {
            out[i].format = kPluginFormatVst3;
            out[i].id     = cached[i].id.c_str();
            out[i].name   = cached[i].name.c_str();
            out[i].vendor = cached[i].vendor.c_str();
            out[i].index  = i;
            out[i].is_instrument = cached[i].instrument ? 1 : 0;
        }
    }
    return static_cast<uint32_t>(cached.size());
}

// ── Lifecycle ────────────────────────────────────────────────────────────────
Instance* open(const char* path, uint32_t index, double sample_rate,
               uint32_t max_block, const char** err) {
    if (err) *err = nullptr;
    if (!path || !*path)      { fail(err, "no path given"); return nullptr; }
    if (max_block == 0)       { fail(err, "max_block must be non-zero"); return nullptr; }
    if (!(sample_rate > 0.0)) { fail(err, "sample_rate must be positive"); return nullptr; }

    auto* p = new Instance();
    p->sampleRate = sample_rate;
    p->maxBlock   = max_block;

    std::string loadErr;
    if (!loadModule(path, p->mod, loadErr)) {
        fail(err, std::string(path) + ": " + loadErr);
        delete p;
        return nullptr;
    }

    // Which class: the index-th AUDIO MODULE, matching what scan reported —
    // not the index-th class, which would count the controller too.
    TUID cid {};
    bool haveCid = false;
    {
        uint32_t seen = 0;
        const int32 n = p->mod.factory->countClasses();
        for (int32 i = 0; i < n; ++i) {
            PClassInfo ci {};
            if (p->mod.factory->getClassInfo(i, &ci) != kResultOk) continue;
            if (std::strcmp(ci.category, kVstAudioEffectClass) != 0) continue;
            if (seen == index) { std::memcpy(cid, ci.cid, sizeof(TUID)); haveCid = true; break; }
            ++seen;
        }
    }
    if (!haveCid) {
        fail(err, std::string(path) + ": no audio module at index "
                  + std::to_string(index));
        teardown(p); delete p; return nullptr;
    }

    if (p->mod.factory->createInstance(reinterpret_cast<FIDString>(cid),
                                       reinterpret_cast<FIDString>(IComponent_iid),
                                       reinterpret_cast<void**>(&p->component))
            != kResultOk || !p->component) {
        fail(err, std::string(path) + ": createInstance(IComponent) failed");
        teardown(p); delete p; return nullptr;
    }
    if (p->component->initialize(&hostContext()) != kResultOk) {
        fail(err, std::string(path) + ": IComponent::initialize failed");
        teardown(p); delete p; return nullptr;
    }
    if (p->component->queryInterface(IAudioProcessor_iid,
                                     reinterpret_cast<void**>(&p->processor))
            != kResultOk || !p->processor) {
        fail(err, std::string(path) + ": component is not an IAudioProcessor");
        teardown(p); delete p; return nullptr;
    }
    if (p->processor->canProcessSampleSize(kSample32) != kResultTrue) {
        fail(err, std::string(path) + ": plugin cannot process 32-bit float");
        teardown(p); delete p; return nullptr;
    }

    // The controller: its own class where the plugin declares one (the normal
    // two-object shape), otherwise the component itself (the single-object
    // shape, which is equally legal).
    TUID ctrlCid {};
    if (p->component->getControllerClassId(ctrlCid) == kResultOk) {
        if (p->mod.factory->createInstance(reinterpret_cast<FIDString>(ctrlCid),
                                           reinterpret_cast<FIDString>(IEditController_iid),
                                           reinterpret_cast<void**>(&p->controller))
                == kResultOk && p->controller) {
            p->controllerIsSeparate = true;
            // Before initialize: some plugins query the handler during it.
            p->handler.setOwner(p);
            p->controller->setComponentHandler(&p->handler);
            if (p->controller->initialize(&hostContext()) != kResultOk) {
                fail(err, std::string(path) + ": IEditController::initialize failed");
                teardown(p); delete p; return nullptr;
            }
        }
    }
    if (!p->controller) {
        p->component->queryInterface(IEditController_iid,
                                     reinterpret_cast<void**>(&p->controller));
        // The single-object shape has a handler to be given too, or its
        // editor's edits have nowhere to go.
        if (p->controller) {
            p->handler.setOwner(p);
            p->controller->setComponentHandler(&p->handler);
        }
    }

    // Connect the pair, and prime the controller from the component's state.
    // Both are the host's job and both are invisible when skipped — the plugin
    // simply reports stale parameter values for ever.
    if (p->controllerIsSeparate && p->controller) {
        p->component->queryInterface(IConnectionPoint_iid,
                                     reinterpret_cast<void**>(&p->cpComponent));
        p->controller->queryInterface(IConnectionPoint_iid,
                                      reinterpret_cast<void**>(&p->cpController));
        if (p->cpComponent && p->cpController) {
            p->cpComponent->connect(p->cpController);
            p->cpController->connect(p->cpComponent);
        }
        MemStream st;
        if (p->component->getState(&st) == kResultOk) {
            st.rewind();
            p->controller->setComponentState(&st);
        }
    }

    // Bus arrangements: ask the plugin what it wants and hand it straight
    // back. Inventing an arrangement is how a host ends up refused by a plugin
    // that would have been perfectly happy.
    SpeakerArrangement inArr = 0, outArr = 0;
    const int32 nInBuses  = p->component->getBusCount(kAudio, kInput);
    const int32 nOutBuses = p->component->getBusCount(kAudio, kOutput);
    if (nInBuses > 0) {
        BusInfo bi {};
        if (p->component->getBusInfo(kAudio, kInput, 0, bi) == kResultOk)
            p->busIn = bi.channelCount;
        p->processor->getBusArrangement(kInput, 0, inArr);
        p->component->activateBus(kAudio, kInput, 0, true);
    }
    if (nOutBuses > 0) {
        BusInfo bi {};
        if (p->component->getBusInfo(kAudio, kOutput, 0, bi) == kResultOk)
            p->busOut = bi.channelCount;
        p->processor->getBusArrangement(kOutput, 0, outArr);
        p->component->activateBus(kAudio, kOutput, 0, true);
    }
    if (p->busOut <= 0) {
        fail(err, std::string(path) + ": plugin has no audio output bus");
        teardown(p); delete p; return nullptr;
    }

    // EVENT INPUT BUSES, or the instrument never hears a note.
    //
    // A bus that is not activated receives nothing, and VST3 leaves every bus
    // inactive until the host says otherwise. An effect has no event bus and
    // this loop does nothing; a synth has one and without this it loads, reports
    // its parameters, renders silence, and gives no indication why.
    const int32 nEventIn = p->component->getBusCount(kEvent, kInput);
    for (int32 b = 0; b < nEventIn; ++b)
        p->component->activateBus(kEvent, kInput, b, true);
    // kResultFalse here is the plugin declining a layout, not a failure: it
    // keeps its own and getBusArrangement above already told us what that is.
    p->processor->setBusArrangements(nInBuses > 0 ? &inArr : nullptr,
                                     nInBuses > 0 ? 1 : 0,
                                     &outArr, 1);

    ProcessSetup setup {};
    setup.processMode        = kRealtime;
    setup.symbolicSampleSize = kSample32;
    setup.maxSamplesPerBlock = static_cast<int32>(max_block);
    setup.sampleRate         = sample_rate;
    if (p->processor->setupProcessing(setup) != kResultOk) {
        fail(err, std::string(path) + ": setupProcessing refused "
                  + std::to_string(sample_rate) + " Hz / "
                  + std::to_string(max_block) + " frames");
        teardown(p); delete p; return nullptr;
    }

    // Every buffer the audio thread will touch, allocated here and never again.
    p->inStore.assign(static_cast<size_t>(std::max(p->busIn, 0)) * max_block, 0.0f);
    p->outStore.assign(static_cast<size_t>(p->busOut) * max_block, 0.0f);
    p->inPtrs.resize(static_cast<size_t>(std::max(p->busIn, 0)));
    p->outPtrs.resize(static_cast<size_t>(p->busOut));
    for (size_t c = 0; c < p->inPtrs.size(); ++c)  p->inPtrs[c]  = p->inStore.data()  + c * max_block;
    for (size_t c = 0; c < p->outPtrs.size(); ++c) p->outPtrs[c] = p->outStore.data() + c * max_block;

    // Parameters, cached with their PLAIN range — derived from the
    // controller's own mapping, since VST3 states no range anywhere.
    if (p->controller) {
        // UNIT NAMES FIRST, because unitId on its own is unshowable. VST3 unit
        // ids are hashes — Surge XT's read 1065737767 — so a host that has only
        // the id can group correctly and still has nothing to write above the
        // group. IUnitInfo is where the names live, and it is optional: a
        // plugin that does not implement it simply has no names, which is a
        // plugin with one root unit and nothing to caption.
        //
        // Raw queryInterface, not FUnknownPtr: only the interface HEADERS are
        // compiled here (see plugins/vst3/CMakeLists.txt), so the SDK's FUID
        // class members — what FUnknownPtr resolves against — are never
        // defined. The TUID constant from the header always is.
        std::map<int32_t, std::string> unitNames;
        IUnitInfo* units = nullptr;
        if (p->controller->queryInterface(IUnitInfo_iid,
                                          reinterpret_cast<void**>(&units)) == kResultOk
            && units) {
            const int32 nu = units->getUnitCount();
            for (int32 u = 0; u < nu; ++u) {
                UnitInfo ui {};
                if (units->getUnitInfo(u, ui) != kResultOk) continue;
                unitNames[static_cast<int32_t>(ui.id)] = string128ToUtf8(ui.name);
            }
            units->release();
        }

        const int32 n = p->controller->getParameterCount();
        for (int32 i = 0; i < n; ++i) {
            ParameterInfo pi {};
            if (p->controller->getParameterInfo(i, pi) != kResultOk) continue;
            const double a = p->controller->normalizedParamToPlain(pi.id, 0.0);
            const double b = p->controller->normalizedParamToPlain(pi.id, 1.0);
            // unitId is the plugin's own grouping — "these four are the
            // filter". kCanAutomate marks what a host may sensibly show and
            // drive; the rest is hidden internals, read-outs and program slots,
            // which is most of what a 2855-parameter synth exposes.
            const auto un = unitNames.find(static_cast<int32_t>(pi.unitId));
            p->params.push_back({static_cast<uint32_t>(pi.id),
                                 string128ToUtf8(pi.title),
                                 std::min(a, b), std::max(a, b),
                                 static_cast<int32_t>(pi.unitId),
                                 un != unitNames.end() ? un->second : std::string(),
                                 (pi.flags & ParameterInfo::kCanAutomate) != 0});
        }
    }
    // The MIDI controller map (see Instance::midiMap). Optional in the
    // format: an effect with no interest in MIDI implements nothing and the
    // table stays all "no parameter".
    for (auto& row : p->midiMap)
        for (auto& id : row) id = kNoParamId;
    if (p->controller) {
        IMidiMapping* mm = nullptr;
        if (p->controller->queryInterface(IMidiMapping_iid,
                                          reinterpret_cast<void**>(&mm)) == kResultOk && mm) {
            for (uint32_t ch = 0; ch < Instance::kMidiChannels; ++ch)
                for (uint32_t cc = 0; cc < Instance::kMidiCtrls; ++cc) {
                    ParamID id = kNoParamId;
                    if (mm->getMidiControllerAssignment(0, static_cast<int16>(ch),
                                                        static_cast<CtrlNumber>(cc), id) == kResultOk)
                        p->midiMap[ch][cc] = static_cast<uint32_t>(id);
                }
            mm->release();
            p->hasMidiMap = true;
        }
    }

    p->events.init(Instance::kNoteCap);
    p->changes.init(static_cast<uint32_t>(std::max<size_t>(p->params.size(), 1)),
                    Instance::kPendCap);

    if (p->component->setActive(true) != kResultOk) {
        fail(err, std::string(path) + ": setActive(true) failed");
        teardown(p); delete p; return nullptr;
    }
    p->processor->setProcessing(true);
    return p;
}

// ── Editor ───────────────────────────────────────────────────────────────────
//
// The header used to say "NO GUI", and for a headless engine that was right:
// there is no window and no native event loop to give a plugin. Hosting one
// inside an application that HAS both is a different situation, and the boundary
// should let that application ask.
//
// Deliberately thin. Clockwork creates the view, attaches it to a native
// parent the caller already owns, and reports the size the plugin wants; it
// does not create windows, run an event loop, or know what a QWidget is. All
// of that belongs to whoever is embedding.
//
// MAIN THREAD ONLY. VST3 views are UI objects; calling any of this from the
// audio thread is undefined and would deadlock in most plugins.

bool editor_has(Instance* p) {
    if (!p || !p->controller) return false;
    if (p->view) return true;
    // createView is the only honest test: a controller may exist and still
    // offer no editor, which is why IEditController::createView is allowed to
    // return null.
    IPlugView* v = p->controller->createView(ViewType::kEditor);
    if (!v) return false;
    v->release();
    return true;
}

bool editor_open(Instance* p, void* parent) {
    if (!p || !p->controller || !parent) return false;
    if (p->view) return true;                       // already attached

    p->view = p->controller->createView(ViewType::kEditor);
    if (!p->view) return false;

    // kPlatformTypeNSView on macOS, kPlatformTypeHWND on Windows (the parent
    // is the window's HWND and the plugin creates its own child inside it),
    // kPlatformTypeX11EmbedWindowID on Linux. A plugin is entitled to refuse
    // a platform type it does not implement, which is a refusal rather than
    // a fault.
#if defined(__APPLE__)
    const FIDString platform = kPlatformTypeNSView;
#elif defined(_WIN32)
    const FIDString platform = kPlatformTypeHWND;
#else
    const FIDString platform = kPlatformTypeX11EmbedWindowID;
#endif
    if (p->view->isPlatformTypeSupported(platform) != kResultTrue) {
        p->view->release();
        p->view = nullptr;
        return false;
    }
    if (p->view->attached(parent, platform) != kResultOk) {
        p->view->release();
        p->view = nullptr;
        return false;
    }
    return true;
}

void editor_close(Instance* p) {
    if (!p || !p->view) return;
    p->view->removed();
    p->view->release();
    p->view = nullptr;
}

bool editor_size(Instance* p, uint32_t* w, uint32_t* h) {
    if (!p || !p->view || !w || !h) return false;
    ViewRect r {};
    if (p->view->getSize(&r) != kResultOk) return false;
    *w = static_cast<uint32_t>(r.right - r.left);
    *h = static_cast<uint32_t>(r.bottom - r.top);
    return true;
}

bool editor_set_size(Instance* p, uint32_t w, uint32_t h) {
    if (!p || !p->view) return false;
    ViewRect r {};
    r.left = 0; r.top = 0;
    r.right = static_cast<int32>(w); r.bottom = static_cast<int32>(h);
    // onSize is the host telling the view what it got, which is not the same
    // as the view agreeing — checkSizeConstraint is how a plugin says no.
    return p->view->onSize(&r) == kResultOk;
}

void close(Instance* p) {
    if (!p) return;
    // Before teardown: the view holds a reference back into the controller and
    // must not outlive it.
    editor_close(p);
    teardown(p);
    delete p;
}

// ── Audio ────────────────────────────────────────────────────────────────────
void process(Instance* p, const float* const* in, uint32_t n_in,
             float* const* out, uint32_t n_out, uint32_t frames,
             int64_t block_time) {
    if (!p || !p->processor || frames == 0) return;
    if (frames > p->maxBlock) frames = p->maxBlock;   // setupProcessing's promise

    // Drain everything plugin_param_set queued since the last block into this
    // block's IParameterChanges, each at its own sample offset.
    p->changes.clear();
    {
        uint32_t tail = p->pendTail.load(std::memory_order_relaxed);
        const uint32_t head = p->pendHead.load(std::memory_order_acquire);
        for (; tail != head; ++tail) {
            const Instance::Pending& e = p->pend[tail % Instance::kPendCap];
            const uint32_t off = e.offset < frames ? e.offset : frames - 1;
            p->changes.add(static_cast<ParamID>(e.id), static_cast<int32>(off), e.norm);
        }
        p->pendTail.store(tail, std::memory_order_release);
    }
    // Then what the plugin's own editor changed, at the start of the block.
    {
        uint32_t tail = p->editTail.load(std::memory_order_relaxed);
        const uint32_t head = p->editHead.load(std::memory_order_acquire);
        for (; tail != head; ++tail) {
            const Instance::Pending& e = p->edit[tail % Instance::kEditCap];
            p->changes.add(static_cast<ParamID>(e.id), 0, e.norm);
        }
        p->editTail.store(tail, std::memory_order_release);
    }

    // And everything plugin_note_on/off queued, into this block's IEventList.
    // Same drain, same clamp: an offset past the end of the block would be
    // rejected by the plugin and the note simply lost.
    p->events.clear();
    {
        uint32_t tail = p->noteTail.load(std::memory_order_relaxed);
        const uint32_t head = p->noteHead.load(std::memory_order_acquire);
        for (; tail != head; ++tail) {
            const Instance::PendNote& n = p->note[tail % Instance::kNoteCap];
            const uint32_t off = n.offset < frames ? n.offset : frames - 1;
            // noteId -1 is VST3's "no note id" — correct for plain MIDI-style
            // play, where pitch identifies the voice and there is no per-note
            // expression to address later.
            if (n.on) p->events.addNoteOn(n.channel, n.pitch, n.velocity,
                                          static_cast<int32>(off), -1);
            else      p->events.addNoteOff(n.channel, n.pitch, n.velocity,
                                           static_cast<int32>(off), -1);
        }
        p->noteTail.store(tail, std::memory_order_release);
    }

    // Clockwork channels → the plugin's fixed bus. Channels clockwork did not
    // supply are zero, not stale.
    const size_t stride = p->maxBlock;
    for (size_t c = 0; c < p->inPtrs.size(); ++c) {
        float* dst = p->inStore.data() + c * stride;
        if (in && c < n_in && in[c]) std::memcpy(dst, in[c], sizeof(float) * frames);
        else                         std::memset(dst, 0, sizeof(float) * frames);
    }
    for (size_t c = 0; c < p->outPtrs.size(); ++c)
        std::memset(p->outStore.data() + c * stride, 0, sizeof(float) * frames);

    AudioBusBuffers inBus {}, outBus {};
    inBus.numChannels      = static_cast<int32>(p->inPtrs.size());
    inBus.silenceFlags     = 0;
    inBus.channelBuffers32 = p->inPtrs.empty() ? nullptr : p->inPtrs.data();
    outBus.numChannels      = static_cast<int32>(p->outPtrs.size());
    outBus.silenceFlags     = 0;
    outBus.channelBuffers32 = p->outPtrs.data();

    ProcessContext ctx {};
    ctx.sampleRate = p->sampleRate;
    ctx.projectTimeSamples = p->framesSeen;
    ctx.continousTimeSamples = p->framesSeen;
    ctx.state |= ProcessContext::kContTimeValid;
    if (block_time != 0) {
        ctx.systemTime = oscTimetagToSystemNs(block_time);
        ctx.state |= ProcessContext::kSystemTimeValid;
    }

    /*
     * MUSICAL TIME, from the session clock.
     *
     * Without these a plugin is told a sample position and a wall time and
     * nothing else, so anything tempo-synced has nothing to sync to: a delay
     * set to 1/8 free-runs in milliseconds, an LFO ignores its note division,
     * an arpeggiator cannot find the grid. None of that fails loudly — it just
     * behaves as though the host had no tempo.
     *
     * The clock is clockwork's own, the same bytes the DSP reads, so a
     * hosted plugin and the engine cannot disagree about the beat; the
     * position is computed once for both formats (plugin_transport.h), so
     * a VST3 and a CLAP on the same block cannot disagree either. A track
     * following a midi timeline hands that in instead (set_timeline).
     */
    clockwork::Timeline grid;
    if (clockwork_plugin::blockTimeline(p->hasTimeline ? &p->timeline : nullptr, p->clock, grid)) {
        const clockwork_plugin::BlockTransport bt = clockwork_plugin::blockTransport(grid, block_time);

        ctx.tempo = bt.bpm;
        ctx.state |= ProcessContext::kTempoValid;

        // kPlaying reflects the transport rather than being asserted. A plugin
        // that gates on play/stop was previously told the transport was always
        // rolling.
        if (bt.playing) ctx.state |= ProcessContext::kPlaying;

        // The beat at the START of this block, in quarter notes — which is
        // what projectTimeMusic means (see blockTransport for why the block's
        // own timetag, and not "now", places it).
        ctx.projectTimeMusic = bt.beat;
        ctx.state |= ProcessContext::kProjectTimeMusicValid;

        /*
         * THE METER IS THE TIMELINE'S. It is 4/4 until something sets it
         * (/clockwork/clock/meter) — which is what every plugin asking for
         * "1/8 dotted" assumes by default, so the unset case matches what
         * they would have done anyway — and once set, the bar lines a
         * plugin sees are the ones the session declared. The denominator
         * does not change the beat unit: beats stay quarter notes, and
         * 7/8 is 3.5 of them to the bar.
         */
        ctx.timeSigNumerator   = bt.meterNum;
        ctx.timeSigDenominator = bt.meterDen;
        ctx.state |= ProcessContext::kTimeSigValid;

        ctx.barPositionMusic = bt.barStartBeat;
        ctx.state |= ProcessContext::kBarPositionValid;
    } else {
        // No clock: say the transport is rolling, which is what a plugin with
        // no host tempo has always been told here.
        ctx.state |= ProcessContext::kPlaying;
    }

    ProcessData data {};
    data.processMode           = kRealtime;
    data.symbolicSampleSize    = kSample32;
    data.numSamples            = static_cast<int32>(frames);
    data.numInputs             = p->inPtrs.empty() ? 0 : 1;
    data.numOutputs            = 1;
    data.inputs                = p->inPtrs.empty() ? nullptr : &inBus;
    data.outputs               = &outBus;
    data.inputParameterChanges = &p->changes;
    data.outputParameterChanges = nullptr;
    data.inputEvents           = p->events.any() ? &p->events : nullptr;
    data.outputEvents          = nullptr;
    data.processContext        = &ctx;

    {
        // The one window. See RtSuspend and plugin_host.h.
        RtSuspend suspend;
        p->processor->process(data);
    }

    for (uint32_t c = 0; c < n_out; ++c) {
        if (!out || !out[c]) continue;
        if (c < p->outPtrs.size())
            std::memcpy(out[c], p->outStore.data() + c * stride, sizeof(float) * frames);
        else
            std::memset(out[c], 0, sizeof(float) * frames);
    }
    p->framesSeen += frames;
}

uint32_t latency(const Instance* p) {
    if (!p || !p->processor) return 0;
    RtSuspend suspend;
    return p->processor->getLatencySamples();
}

// ── Parameters ───────────────────────────────────────────────────────────────
uint32_t param_count(const Instance* p) {
    return p ? static_cast<uint32_t>(p->params.size()) : 0;
}

int param_info(const Instance* p, uint32_t index, PluginParam* out) {
    if (!p || !out || index >= p->params.size()) return 0;
    const Instance::ParamDesc& d = p->params[index];
    out->id   = d.id;
    out->name = d.name.c_str();
    out->min  = d.min;
    out->max  = d.max;
    out->group       = d.group;
    // Borrowed, like `name`: it lives as long as the instance. Never NULL —
    // an empty string is "this plugin named no unit", which a caller can
    // test with one deref instead of a null check it will forget.
    out->group_name  = d.groupName.c_str();
    out->automatable = d.automatable ? 1 : 0;
    // Current value is asked live: automation and state loads both move it.
    out->value = p->controller
        ? p->controller->normalizedParamToPlain(
              static_cast<ParamID>(d.id),
              p->controller->getParamNormalized(static_cast<ParamID>(d.id)))
        : 0.0;
    return 1;
}

void set_clock(Instance* p, const ClockworkClockState* clock) {
    if (p) p->clock = clock;
}

void set_timeline(Instance* p, const ClockworkTimeline* timeline) {
    if (!p) return;
    p->hasTimeline = timeline != nullptr;
    if (timeline) p->timeline = *timeline;
}

void set_param_listener(Instance* p, void (*fn)(void*, uint32_t, double), void* ctx) {
    if (p) p->handler.setListener(fn, ctx);
}

void editor_edit(Instance* p, uint32_t id, double normalized) {
    if (!p) return;
    if (normalized < 0.0) normalized = 0.0;
    if (normalized > 1.0) normalized = 1.0;
    const uint32_t head = p->editHead.load(std::memory_order_relaxed);
    const uint32_t tail = p->editTail.load(std::memory_order_acquire);
    if (head - tail >= Instance::kEditCap) return;   // full: drop, never block
    p->edit[head % Instance::kEditCap] = {id, normalized, 0};
    p->editHead.store(head + 1, std::memory_order_release);
}

// Queue a note for the next block. Same ring discipline as param_set: the
// control thread appends, the audio thread drains, a full ring drops.
void note(Instance* p, bool on, int16_t channel, int16_t pitch,
          float velocity, uint32_t frame_offset) {
    if (!p) return;
    const uint32_t head = p->noteHead.load(std::memory_order_relaxed);
    const uint32_t tail = p->noteTail.load(std::memory_order_acquire);
    if (head - tail >= Instance::kNoteCap) return;   // full: drop, never block
    p->note[head % Instance::kNoteCap] =
        {static_cast<uint16_t>(on ? 1 : 0), channel, pitch, velocity, frame_offset};
    p->noteHead.store(head + 1, std::memory_order_release);
}

void param_set(Instance* p, uint32_t id, double value, uint32_t frame_offset) {
    if (!p || !p->controller) return;
    double norm = p->controller->plainParamToNormalized(static_cast<ParamID>(id), value);
    if (norm < 0.0) norm = 0.0;
    if (norm > 1.0) norm = 1.0;
    // The controller keeps the host's view of the value; the processor gets it
    // through the next block's IParameterChanges. That split is VST3's, not
    // ours: the two objects never share the value directly.
    p->controller->setParamNormalized(static_cast<ParamID>(id), norm);

    const uint32_t head = p->pendHead.load(std::memory_order_relaxed);
    const uint32_t tail = p->pendTail.load(std::memory_order_acquire);
    if (head - tail >= Instance::kPendCap) return;   // full: drop, never block
    p->pend[head % Instance::kPendCap] = {id, norm, frame_offset};
    p->pendHead.store(head + 1, std::memory_order_release);
}

// A MIDI controller, as the NORMALISED value of whatever parameter the plugin
// mapped it to. Nothing is queued when the plugin mapped nothing: a CC a
// plugin never asked for is not an error, it is a CC it does not have.
//
// Bypasses plainParamToNormalized on purpose — the value already IS
// normalised (the mapping is defined on 0..1) — but still tells the controller,
// so the editor's mod wheel moves when the code moves it.
void midi_control(Instance* p, int16_t channel, uint32_t ctrl, double norm,
                  uint32_t frame_offset) {
    if (!p || !p->hasMidiMap) return;
    if (channel < 0 || static_cast<uint32_t>(channel) >= Instance::kMidiChannels) return;
    if (ctrl >= Instance::kMidiCtrls) return;
    const uint32_t id = p->midiMap[channel][ctrl];
    if (id == static_cast<uint32_t>(kNoParamId)) return;
    if (norm < 0.0) norm = 0.0;
    if (norm > 1.0) norm = 1.0;
    if (p->controller) p->controller->setParamNormalized(static_cast<ParamID>(id), norm);

    const uint32_t head = p->pendHead.load(std::memory_order_relaxed);
    const uint32_t tail = p->pendTail.load(std::memory_order_acquire);
    if (head - tail >= Instance::kPendCap) return;
    p->pend[head % Instance::kPendCap] = {id, norm, frame_offset};
    p->pendHead.store(head + 1, std::memory_order_release);
}

void cc(Instance* p, int16_t channel, uint8_t number, float value, uint32_t frame_offset) {
    midi_control(p, channel, number < 128 ? number : 127u, value, frame_offset);
}

// -1..1 → 0..1, the pitch bend controller's own range (centre 0.5).
void pitch_bend(Instance* p, int16_t channel, float bend, uint32_t frame_offset) {
    midi_control(p, channel, kPitchBend, 0.5 + 0.5 * static_cast<double>(bend), frame_offset);
}

// A note-off for every pitch on every channel. Sixteen channels of 128 notes
// is far more than the ring holds, so this is done PER CHANNEL through the
// all-notes-off controller where the plugin maps one, and by explicit
// note-offs only on channel 0 — the channel clockwork's own notes use.
void all_notes_off(Instance* p, uint32_t frame_offset) {
    if (!p) return;
    if (p->hasMidiMap)
        for (uint32_t ch = 0; ch < Instance::kMidiChannels; ++ch)
            midi_control(p, static_cast<int16_t>(ch), kCtrlAllNotesOff, 1.0, frame_offset);
    for (int16_t pitch = 0; pitch < 128; ++pitch)
        note(p, false, 0, pitch, 0.0f, frame_offset);
}

// ── State ────────────────────────────────────────────────────────────────────
uint32_t state_save(Instance* p, uint8_t* out, uint32_t cap) {
    if (!p || !p->component) return 0;
    MemStream st;
    if (p->component->getState(&st) != kResultOk) return 0;
    const std::vector<uint8_t>& b = st.bytes();
    const uint32_t n = static_cast<uint32_t>(b.size());
    if (out && cap >= n && n > 0) std::memcpy(out, b.data(), n);
    return n;
}

int state_load(Instance* p, const uint8_t* bytes, uint32_t len) {
    if (!p || !p->component || !bytes || len == 0) return 0;
    MemStream st(bytes, len);
    if (p->component->setState(&st) != kResultOk) return 0;
    if (p->controller) {
        st.rewind();
        p->controller->setComponentState(&st);
    }
    return 1;
}

}  // namespace clockwork_plugin_vst3
