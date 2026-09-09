// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
//
// See plugin_discovery.h. Control thread only; every call here opens plugins.

#include "plugin_discovery.h"
#include "clockwork_path.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// Copy into a fixed field, always NUL-terminated. strncpy does not guarantee
// that when the source is longer, which is the whole reason this exists.
void put(char* dst, size_t cap, const char* src) {
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    const size_t n = std::min(cap - 1, std::strlen(src));
    std::memcpy(dst, src, n);
    dst[n] = '\0';
}

std::string home() {
    if (const char* h = std::getenv("HOME")) return h;
    return {};
}

PluginScanFn g_scan_fn  = nullptr;
void*        g_scan_ctx = nullptr;

// The format-defined locations, user scope first. Built once and kept, because
// plugin_search_paths hands out pointers that must outlive the call.
const std::vector<std::string>& searchPaths() {
    static const std::vector<std::string> paths = [] {
        std::vector<std::string> p;
        const std::string h = home();
#if defined(__APPLE__)
        // Apple's own layout. CLAP chose to live alongside VST3 here rather
        // than invent a location, which is why the two lists are parallel.
        if (!h.empty()) {
            p.push_back(h + "/Library/Audio/Plug-Ins/VST3");
            p.push_back(h + "/Library/Audio/Plug-Ins/CLAP");
        }
        p.push_back("/Library/Audio/Plug-Ins/VST3");
        p.push_back("/Library/Audio/Plug-Ins/CLAP");
#elif defined(_WIN32)
        // Per-user first, then the machine-wide Common Files — taken from the
        // environment rather than spelled out, because "C:\\Program Files" is
        // only the usual answer, not the guaranteed one.
        if (const char* la = std::getenv("LOCALAPPDATA")) {
            p.push_back(std::string(la) + "\\Programs\\Common\\VST3");
            p.push_back(std::string(la) + "\\Programs\\Common\\CLAP");
        }
        const char* cf = std::getenv("COMMONPROGRAMFILES");
        const std::string common = cf ? cf : "C:\\Program Files\\Common Files";
        p.push_back(common + "\\VST3");
        p.push_back(common + "\\CLAP");
#else
        if (!h.empty()) {
            p.push_back(h + "/.vst3");
            p.push_back(h + "/.clap");
        }
        p.push_back("/usr/lib/vst3");
        p.push_back("/usr/local/lib/vst3");
        p.push_back("/usr/lib/clap");
        p.push_back("/usr/local/lib/clap");
#endif
        return p;
    }();
    return paths;
}

// The user's own folders. Guarded because a GUI settings panel edits this
// while a scan may be reading it from another thread; the vector's strings
// are handed out by pointer, so removal invalidates what a caller was given —
// which the header says.
std::mutex g_extraMu;
std::vector<std::string> g_extra;

std::vector<std::string> allSearchPaths() {
    std::vector<std::string> p;
    {
        std::lock_guard<std::mutex> lk(g_extraMu);
        p = g_extra;
    }
    for (const std::string& s : searchPaths()) p.push_back(s);
    return p;
}

// Is this directory entry worth handing to plugin_scan?
//
// The extension test is not decoration. Without it every scan opens every file
// in the folder — including a plugin bundle's own resources — and "opens" here
// means running its module entry. Filtering first is what keeps a scan to the
// things that claim to be plugins.
//
// Compared without case, because Windows filesystems keep whatever case the
// installer wrote and a ".VST3" is the same plugin.
bool looksLikePlugin(const fs::path& p) {
    std::string ext = clockwork_path::to_utf8(p.extension());
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".vst3" || ext == ".clap";
}

}  // namespace

extern "C" {

uint32_t plugin_search_paths(const char** out, uint32_t cap) {
    const auto& paths = searchPaths();
    if (out) {
        const uint32_t k = std::min<uint32_t>(cap, static_cast<uint32_t>(paths.size()));
        for (uint32_t i = 0; i < k; ++i) out[i] = paths[i].c_str();
    }
    return static_cast<uint32_t>(paths.size());
}

void plugin_set_scan_listener(PluginScanFn fn, void* ctx) {
    g_scan_fn  = fn;
    g_scan_ctx = ctx;
}

int plugin_add_search_path(const char* dir) {
    if (!dir || !*dir) return 0;
    std::lock_guard<std::mutex> lk(g_extraMu);
    for (const std::string& s : g_extra) if (s == dir) return 1;
    g_extra.emplace_back(dir);
    return 1;
}

int plugin_remove_search_path(const char* dir) {
    if (!dir || !*dir) return 0;
    std::lock_guard<std::mutex> lk(g_extraMu);
    for (auto it = g_extra.begin(); it != g_extra.end(); ++it) {
        if (*it == dir) { g_extra.erase(it); return 1; }
    }
    return 0;
}

uint32_t plugin_extra_search_paths(const char** out, uint32_t cap) {
    std::lock_guard<std::mutex> lk(g_extraMu);
    if (out) {
        const uint32_t k = std::min<uint32_t>(cap, static_cast<uint32_t>(g_extra.size()));
        for (uint32_t i = 0; i < k; ++i) out[i] = g_extra[i].c_str();
    }
    return static_cast<uint32_t>(g_extra.size());
}

uint32_t plugin_scan_dir(const char* dir, PluginEntry* out, uint32_t cap) {
    if (!dir || !*dir) return 0;

    std::error_code ec;
    const fs::path root = clockwork_path::from_utf8(dir);
    if (!fs::is_directory(root, ec) || ec) return 0;

    // Collected before writing, so the count is right even when it exceeds cap
    // and so the caller can size a second call from the first.
    std::vector<PluginEntry> found;

    // Recursive, because vendors nest: Windows installers routinely put a
    // plugin at VST3\\Vendor\\Plugin.vst3, and a flat listing of Common Files
    // finds only the vendors who did not. A bundle is itself a directory and
    // is never descended into — its insides are not more plugins.
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    if (ec) return 0;
    const fs::recursive_directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) break;
        const auto& de = *it;
        if (!looksLikePlugin(de.path())) continue;
        if (de.is_directory(ec)) it.disable_recursion_pending();
        const std::string path = clockwork_path::to_utf8(de.path());

        // A bundle can hold several plugins, so ask for a handful rather than
        // one. Sixteen is well past what any real bundle ships and costs
        // nothing: the scan happens either way.
        PluginDesc descs[16] {};
        if (g_scan_fn) g_scan_fn(g_scan_ctx, path.c_str());
        const uint32_t n = plugin_scan(path.c_str(), descs, 16);
        if (g_scan_fn) g_scan_fn(g_scan_ctx, nullptr);
        const uint32_t k = std::min<uint32_t>(n, 16);
        for (uint32_t i = 0; i < k; ++i) {
            PluginEntry e {};
            put(e.path, sizeof e.path, path.c_str());
            e.index  = descs[i].index;
            e.format = descs[i].format;
            put(e.id,     sizeof e.id,     descs[i].id);
            put(e.name,   sizeof e.name,   descs[i].name);
            put(e.vendor, sizeof e.vendor, descs[i].vendor);
            e.is_instrument = descs[i].is_instrument;
            found.push_back(e);
        }
    }

    if (out) {
        const uint32_t k = std::min<uint32_t>(cap, static_cast<uint32_t>(found.size()));
        for (uint32_t i = 0; i < k; ++i) out[i] = found[i];
    }
    return static_cast<uint32_t>(found.size());
}

uint32_t plugin_scan_all(PluginEntry* out, uint32_t cap) {
    std::vector<PluginEntry> all;
    std::set<std::string> seen;   // "<format>:<id>", the identity a format states

    const std::vector<std::string> paths = allSearchPaths();   // extras first
    for (const std::string& dir : paths) {
        const uint32_t n = plugin_scan_dir(dir.c_str(), nullptr, 0);
        if (n == 0) continue;
        std::vector<PluginEntry> here(n);
        plugin_scan_dir(dir.c_str(), here.data(), n);
        for (const PluginEntry& e : here) {
            // Identity is the format's own id, never the path or the name: the
            // same plugin in the user and system folder has two paths, and two
            // different plugins can share a name.
            std::string key = std::to_string(static_cast<int>(e.format));
            key += ':';
            key += e.id[0] ? e.id : e.path;   // a format with no id falls back
            if (!seen.insert(key).second) continue;   // user scope already won
            all.push_back(e);
        }
    }

    // Stable order, so the browser reads the same way twice. Directory order is
    // filesystem order, which is neither sorted nor reproducible.
    std::sort(all.begin(), all.end(), [](const PluginEntry& a, const PluginEntry& b) {
        const int v = std::strcmp(a.vendor, b.vendor);
        if (v != 0) return v < 0;
        return std::strcmp(a.name, b.name) < 0;
    });

    if (out) {
        const uint32_t k = std::min<uint32_t>(cap, static_cast<uint32_t>(all.size()));
        for (uint32_t i = 0; i < k; ++i) out[i] = all[i];
    }
    return static_cast<uint32_t>(all.size());
}

}  // extern "C"
