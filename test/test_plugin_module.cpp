// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_plugin_module.cpp — a host that loads two plugin binaries built on
 * Clockwork into one process (docs/ENGINES.md). Each module exports its
 * entry point and nothing else, so each runs the Clockwork inside it: the
 * first core either makes is its own Clockwork's first, and none of
 * Clockwork's own names can be found from outside.
 *
 * This executable links no Clockwork of its own: what it finds, it finds in
 * the modules. The first copy is loaded into the global scope, where a
 * symbol it exported would be what the second copy's calls bound to.
 */
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

namespace fs = std::filesystem;

struct Module {
    void* handle = nullptr;
    Module(const fs::path& path, bool global) {
#if defined(_WIN32)
        (void)global;
        handle = reinterpret_cast<void*>(LoadLibraryW(path.c_str()));
#else
        handle = dlopen(path.c_str(), RTLD_NOW | (global ? RTLD_GLOBAL : RTLD_LOCAL));
#endif
    }
    ~Module() {
        if (!handle) return;
#if defined(_WIN32)
        FreeLibrary(reinterpret_cast<HMODULE>(handle));
#else
        dlclose(handle);
#endif
    }
    void* find(const char* name) const {
#if defined(_WIN32)
        return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(handle), name));
#else
        return dlsym(handle, name);
#endif
    }
};

// A second copy of the module under another name, so the loader maps a
// second image rather than handing the first back.
fs::path copyOf(const fs::path& module) {
    const fs::path copy = fs::temp_directory_path() / ("clockwork_test_plugin_copy" + module.extension().string());
    fs::copy_file(module, copy, fs::copy_options::overwrite_existing);
    return copy;
}

using Entry = int (*)();

} // namespace

TEST_CASE("plugin module: two loaded in one process each run their own Clockwork, and show nothing but their entry",
          "[plugin][module]") {
    const fs::path path = CLOCKWORK_TEST_PLUGIN;
    const fs::path copy = copyOf(path);
    {
        Module first(path, true), second(copy, false);
        REQUIRE(first.handle != nullptr);
        REQUIRE(second.handle != nullptr);
        for (const Module* m : { &first, &second }) {
            const auto entry = reinterpret_cast<Entry>(m->find("clockwork_test_plugin_entry"));
            REQUIRE(entry != nullptr);
            // 1 in each: two registries, two engines, each answering its pong.
            CHECK(entry() == 1);
            // Clockwork's own names, C++ and Rust, are not the host's to find.
            for (const char* name : { "clockwork_core_new", "clockwork_init", "clockwork_tick",
                                      "clockwork_embed_attach", "clockwork_client_open_memory",
                                      "clockwork_heap_take", "clockwork_profile_default",
                                      "dsp_new", "dsp_describe" }) {
                INFO(name);
                CHECK(m->find(name) == nullptr);
            }
        }
    }
    std::error_code ignored;
    fs::remove(copy, ignored);
}
