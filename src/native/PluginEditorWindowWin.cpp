// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * PluginEditorWindowWin.cpp — the plugin's own editor, in a window owned by
 * the ENGINE. The Win32 half of plugin_editor_window.h.
 *
 * PluginEditorWindow.mm is the macOS half and the reasoning lives there: a
 * native view can only be parented into a window belonging to the process
 * that created it, so the editor has to be shown by the process that is
 * making the sound, or a second instance has to be loaded to draw with and
 * every preset, meter and parameter mirrored by hand between the two. This
 * file follows the .mm entry point for entry point, and where the two differ
 * the difference is called out at the spot.
 *
 * WHY IT CAN LIVE HERE. The bridge's main thread already runs a message pump:
 * PluginBridgeMain.cpp's pumpEvents drains PeekMessageW/DispatchMessageW
 * between passes over the control ring, because that is what it takes to
 * own a window on Windows at all. A window created on that thread is drawn,
 * clicked and closed by that pump, with no new thread and no new loop — which
 * matters, because a Win32 window is bound to the thread that created it and
 * a VST3 view wants the same thread for everything.
 *
 * MAIN THREAD ONLY, and not negotiable. Every call below touches a window
 * that belongs to one thread, and a VST3 view says the same of itself. Win32
 * has no "is this the main thread" question to ask, so the thread that ran
 * this file's static initialisers is taken to be it — for an executable that
 * is the thread that goes on to call main(), and the bridge is an executable.
 * The OSC verbs that open and close an editor are handled on that thread in
 * the bridge; the _async entry points exist for a caller that is not, and
 * marshal by posting to a message-only window of their own, the way the .mm
 * uses dispatch_async. That window is created on the main thread the first
 * time any of this runs there, so a caller on another thread before then has
 * nowhere to post to and says so rather than touching a window it must not.
 *
 * WHAT THIS DELIBERATELY IS NOT. It is not an embedding of the editor inside
 * another application's window (SetParent across processes is possible on
 * Windows and a well-known source of hangs — input queues attach, and one
 * process blocking stalls the other). The window floats, as a DAW's plugin
 * editor does, and a plugin that crashes takes this process and not the GUI.
 *
 * WRITTEN AGAINST THE WIN32 API ON A MAC. This file has not yet been compiled
 * or run on Windows. docs/TRACKS.md says so; when it has been, that
 * sentence and this one should go.
 */
#if defined(_WIN32)

// Built as part of clockwork_plugin_host, which does not inherit clockwork's
// compile definitions; windows.h is tamed here as plugin_vst3.cpp tames it.
#ifndef NOMINMAX
#  define NOMINMAX 1
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN 1
#endif
#include <windows.h>

#include "plugin_editor_window.h"
#include "plugin_host.h"
#include "clockwork_path.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>

namespace {

// ── The main thread ──────────────────────────────────────────────────────────
// Captured by a static initialiser: see the header comment for why that is
// the main thread here and what it would take for it not to be.
const DWORD g_mainThread = GetCurrentThreadId();
bool onMainThread() { return GetCurrentThreadId() == g_mainThread; }

// ── One window per plugin ────────────────────────────────────────────────────

struct Entry {
    HWND window = nullptr;
};

std::map<struct HostedPlugin*, Entry>& registry() {
    static std::map<struct HostedPlugin*, Entry> r;
    return r;
}

const wchar_t* const kEditorClass  = L"ClockworkPluginEditorWindow";
const wchar_t* const kMarshalClass = L"ClockworkPluginEditorMarshal";

// The two messages the _async entry points post. WM_APP upwards is the range
// reserved for an application's own use; the offset keeps clear of anything
// a plugin's child window might also choose from the bottom of it.
enum : UINT {
    kMsgShow = WM_APP + 0x40,   // wParam: HostedPlugin*; lParam: std::string* title, owned
    kMsgHide = WM_APP + 0x41,   // wParam: HostedPlugin*
};

std::wstring widen(const char* utf8) { return clockwork_path::to_wide(utf8 ? utf8 : ""); }

// The window's own procedure. GWLP_USERDATA carries the plugin, so that a
// close arriving from the title bar can find it; forget() clears it, so a
// close after the plugin has gone finds nothing rather than a dangling one.
LRESULT CALLBACK editorProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CLOSE: {
        // Detach BEFORE the window goes away — the .mm's windowShouldClose:
        // exactly. A plugin whose view outlives its parent is the standard way
        // to crash a host, and here that would take the audio engine with it.
        auto* p = reinterpret_cast<struct HostedPlugin*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (p) plugin_editor_close(p);
        ShowWindow(hwnd, SW_HIDE);
        return 0;   // hide rather than destroy, so reopening is instant
    }
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

// The message-only window's procedure: the receiving end of the _async calls,
// running on the main thread because that is where the window was made.
LRESULT CALLBACK marshalProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case kMsgShow: {
        auto* p = reinterpret_cast<struct HostedPlugin*>(wParam);
        auto* title = reinterpret_cast<std::string*>(lParam);
        clockwork_editor_window_open(p, title ? title->c_str() : nullptr);
        delete title;
        return 0;
    }
    case kMsgHide:
        clockwork_editor_window_close(reinterpret_cast<struct HostedPlugin*>(wParam));
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

/* Both classes, registered once. RegisterClassExW is per process, and a
   second registration of the same name fails with ERROR_CLASS_ALREADY_EXISTS,
   which is why this remembers rather than retries. */
/* Bring the editor to the front. Windows lets only the foreground process
   (or one it has granted the right to) take the foreground; this bridge is
   a background process the engine spawned, so a bare SetForegroundWindow is
   refused and the window sits behind the host, findable only from the
   taskbar. The permitted way round it is the documented one: attach this
   thread's input queue to the foreground window's thread for the duration
   of the call, which makes the two count as one for the foreground rule.
   Belt and braces, the window is also pushed through the topmost band and
   back, which raises its z-order even where activation is still refused. */
void bringToFront(HWND w) {
    ShowWindow(w, SW_SHOWNORMAL);
    if (SetForegroundWindow(w)) return;
    const HWND fg = GetForegroundWindow();
    const DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    const DWORD me = GetCurrentThreadId();
    const bool attached = fgThread && fgThread != me && AttachThreadInput(fgThread, me, TRUE);
    SetWindowPos(w, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetWindowPos(w, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    BringWindowToTop(w);
    SetForegroundWindow(w);
    SetActiveWindow(w);
    if (attached) AttachThreadInput(fgThread, me, FALSE);
}

bool ensureClasses() {
    static bool done = false, ok = false;
    if (done) return ok;
    done = true;

    WNDCLASSEXW editor {};
    editor.cbSize        = sizeof editor;
    editor.lpfnWndProc   = editorProc;
    editor.hInstance     = GetModuleHandleW(nullptr);
    // IDC_ARROW is MAKEINTRESOURCE, which is the ANSI form unless UNICODE is
    // defined — and this file calls the W entry points explicitly rather than
    // relying on that macro. The value is an atom, not a string, so widening
    // the pointer type is all that is meant here.
    editor.hCursor       = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    // The product's icon, if the build gave this binary one (resource id 1,
    // clockwork_version.rc.in), else the stock application icon rather than
    // the blank the taskbar shows for a class with none.
    editor.hIcon         = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
    if (!editor.hIcon) editor.hIcon = LoadIconW(nullptr, reinterpret_cast<LPCWSTR>(IDI_APPLICATION));
    editor.hIconSm       = editor.hIcon;
    editor.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_WINDOW + 1));
    editor.lpszClassName = kEditorClass;

    WNDCLASSEXW marshal {};
    marshal.cbSize        = sizeof marshal;
    marshal.lpfnWndProc   = marshalProc;
    marshal.hInstance     = GetModuleHandleW(nullptr);
    marshal.lpszClassName = kMarshalClass;

    ok = RegisterClassExW(&editor) != 0 && RegisterClassExW(&marshal) != 0;
    if (!ok) fprintf(stderr, "[plugin-editor] RegisterClassExW failed (error %lu)\n",
                     static_cast<unsigned long>(GetLastError()));
    return ok;
}

/* The message-only window the _async calls post to. Created lazily and only
   on the main thread, since the thread that creates a window is the thread
   whose pump delivers to it. Atomic because the _async callers read it from
   other threads. */
std::atomic<HWND> g_marshal{nullptr};

HWND marshalWindow() {
    HWND h = g_marshal.load(std::memory_order_acquire);
    if (h) return h;
    if (!onMainThread() || !ensureClasses()) return nullptr;
    h = CreateWindowExW(0, kMarshalClass, L"", 0, 0, 0, 0, 0,
                        HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!h) {
        fprintf(stderr, "[plugin-editor] cannot create the marshal window (error %lu)\n",
                static_cast<unsigned long>(GetLastError()));
        return nullptr;
    }
    g_marshal.store(h, std::memory_order_release);
    return h;
}

/* Size the window so its CLIENT area is the view's size — the numbers a
   plugin gives are its view's, and the title bar and frame are not part of
   them (the .mm's setContentSize) — and centre it on the monitor's work area,
   which is what [NSWindow center] does on the other side. */
void fitToView(HWND hwnd, uint32_t vw, uint32_t vh) {
    RECT r { 0, 0, static_cast<LONG>(vw), static_cast<LONG>(vh) };
    const DWORD style   = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    const DWORD exStyle = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    AdjustWindowRectEx(&r, style, FALSE, exStyle);
    const int w = r.right - r.left;
    const int h = r.bottom - r.top;

    int x = 0, y = 0;
    UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;
    MONITORINFO mi {};
    mi.cbSize = sizeof mi;
    if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
        x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - w) / 2;
        y = mi.rcWork.top  + ((mi.rcWork.bottom - mi.rcWork.top) - h) / 2;
    } else {
        flags |= SWP_NOMOVE;   // wherever CreateWindow put it is better than (0,0)
    }
    SetWindowPos(hwnd, nullptr, x, y, w, h, flags);
}

}  // namespace

extern "C" {

int clockwork_editor_window_open(struct HostedPlugin* p, const char* title) {
    if (!p) return 0;
    if (!onMainThread()) {
        fprintf(stderr, "[plugin-editor] refused: not the main thread\n");
        return 0;
    }
    fprintf(stderr, "[plugin-editor] open '%s'\n", title ? title : "?");
    if (!ensureClasses()) return 0;
    marshalWindow();   // so a later _async call from any thread has somewhere to go

    Entry& e = registry()[p];
    if (e.window) {                            // already built: just show it
        if (!plugin_editor_open(p, e.window)) {
            // Reattaching can fail if the plugin tore its view down; the window
            // is still valid, so say so rather than leaving an empty frame up.
            return 0;
        }
        bringToFront(e.window);
        return 1;
    }

    if (!plugin_editor_has(p)) {
        fprintf(stderr, "[plugin-editor] plugin reports no editor\n");
        return 0;
    }

    // Titled, closable, minimisable and NOT resizable — the .mm's style mask,
    // which is WS_OVERLAPPEDWINDOW without the thick frame and the maximise
    // box. A plugin's view is attached at the size it asks for and the host
    // does not offer to change it; a resizable frame would invite a drag the
    // view then ignores.
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;

    // Opened at a nominal size and corrected below once the view reports what
    // it actually wants — asking before attaching gives no useful answer.
    RECT r { 0, 0, 800, 500 };
    AdjustWindowRectEx(&r, style, FALSE, 0);
    const std::wstring wtitle = widen(title ? title : "Plugin");
    HWND w = CreateWindowExW(0, kEditorClass, wtitle.c_str(), style,
                             CW_USEDEFAULT, CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top,
                             nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!w) {
        fprintf(stderr, "[plugin-editor] CreateWindowExW failed (error %lu)\n",
                static_cast<unsigned long>(GetLastError()));
        return 0;
    }

    // The parent handed to the plugin is the window itself: on Windows a VST3
    // view creates its own child HWND inside whatever it is given
    // (kPlatformTypeHWND), so there is no separate content view to offer.
    if (!plugin_editor_open(p, w)) {
        fprintf(stderr, "[plugin-editor] view refused to attach\n");
        DestroyWindow(w);
        return 0;
    }

    uint32_t vw = 0, vh = 0;
    if (plugin_editor_size(p, &vw, &vh) && vw > 0 && vh > 0)
        fitToView(w, vw, vh);

    SetWindowLongPtrW(w, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(p));
    e.window = w;

    bringToFront(w);
    fprintf(stderr, "[plugin-editor] window up (%ux%u)\n", vw, vh);
    return 1;
}

void clockwork_editor_window_close(struct HostedPlugin* p) {
    if (!p || !onMainThread()) return;
    auto it = registry().find(p);
    if (it == registry().end()) return;
    plugin_editor_close(p);
    ShowWindow(it->second.window, SW_HIDE);
}

/* Called when the plugin itself is going away, so the window must not outlive
   it holding a dangling pointer.

   ONE STEP FURTHER THAN THE .mm, which hides and lets the NSWindow go with
   the registry entry. A Win32 window that is never destroyed is a leak for
   the life of the process, so this one is destroyed — but only after the view
   has been detached, because DestroyWindow takes every child with it and a
   plugin's view torn out from under it that way is the crash the WM_CLOSE
   comment warns of. plugin_editor_close is idempotent, so the plugin_close
   that follows this call finds nothing left to do. */
void clockwork_editor_window_forget(struct HostedPlugin* p) {
    if (!p) return;
    auto it = registry().find(p);
    if (it == registry().end()) return;
    if (onMainThread()) {
        plugin_editor_close(p);
        SetWindowLongPtrW(it->second.window, GWLP_USERDATA, 0);
        DestroyWindow(it->second.window);
    }
    // Off the main thread the window cannot be touched; the entry goes and
    // the hidden window stays, which is a leak and not a crash.
    registry().erase(it);
}

/* Marshal onto the main thread and return at once. The bridge's main loop
   pumps messages every couple of milliseconds, so the window appears within
   that. The title is copied into the message, since the caller's buffer need
   not outlive the call. */
void clockwork_editor_window_show_async(struct HostedPlugin* p, const char* title) {
    if (!p) return;
    HWND m = marshalWindow();
    if (!m) {
        fprintf(stderr, "[plugin-editor] no way to reach the main thread from here\n");
        return;
    }
    auto* t = new std::string(title ? title : "Plugin");
    if (!PostMessageW(m, kMsgShow, reinterpret_cast<WPARAM>(p), reinterpret_cast<LPARAM>(t)))
        delete t;
}

void clockwork_editor_window_hide_async(struct HostedPlugin* p) {
    if (!p) return;
    HWND m = marshalWindow();
    if (!m) return;
    PostMessageW(m, kMsgHide, reinterpret_cast<WPARAM>(p), 0);
}

int clockwork_editor_window_is_open(struct HostedPlugin* p) {
    if (!p) return 0;
    auto it = registry().find(p);
    return (it != registry().end() && it->second.window && IsWindowVisible(it->second.window)) ? 1 : 0;
}

/* Nothing to do: a Win32 process is a capture target by its window handles,
   and OBS finds those by executable name. */
void clockwork_editor_window_register_app(void) {}

}  // extern "C"

#endif  // _WIN32
