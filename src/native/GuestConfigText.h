// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
#pragma once
/*
 * GuestConfigText.h — how the hosts in this repository spell the guest's
 * config block: one `name=value` per line.
 *
 * Clockwork never reads the block. It reserves a region, copies whatever
 * bytes a host hands it (ClockworkEngine::Config::guestConfig, or the bytes
 * the web client writes), and gives the guest base and length
 * (DspConfig::guest_config). What the bytes mean is between a host and the
 * guest it boots.
 *
 * The hosts here — the native engine, the NIF, and the web client — agree on
 * TEXT: lines of `name=value`, terminated by NUL. It replaces a positional
 * block of uint32 slots that two writers and one reader had to agree on
 * index by index, and did not: a native write at slot 7 was read as slot 10
 * once, and every test passed over it because a wrong slot held a plausible
 * number. A name cannot be off by one. What the names ARE is the guest's to
 * say (a guest's own header lists them); a host that wants to set one writes
 * the name it was given and its value, and a guest that does not know the
 * name refuses the boot with the line, rather than booting with a silently
 * different configuration.
 *
 * The two helpers keep one line per name.
 */

#include <string>
#include <string_view>

namespace clockwork::guest_config_text {

// Set `name` to `value`, replacing an earlier line for the same name.
inline void set(std::string& text, std::string_view name, std::string_view value) {
    std::string line;
    line.reserve(name.size() + 1 + value.size() + 1);
    line.append(name).append("=").append(value).append("\n");
    // Drop an existing line for this name.
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t end = text.find('\n', pos);
        const size_t lineEnd = end == std::string::npos ? text.size() : end + 1;
        const std::string_view existing(text.data() + pos, lineEnd - pos);
        const size_t eq = existing.find('=');
        if (eq != std::string_view::npos && existing.substr(0, eq) == name) {
            text.erase(pos, lineEnd - pos);
            continue;
        }
        pos = lineEnd;
    }
    text.append(line);
}

// The value set for `name`, or an empty string when there is none.
inline std::string get(const std::string& text, std::string_view name) {
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t end = text.find('\n', pos);
        const size_t lineEnd = end == std::string::npos ? text.size() : end;
        const std::string_view line(text.data() + pos, lineEnd - pos);
        const size_t eq = line.find('=');
        if (eq != std::string_view::npos && line.substr(0, eq) == name)
            return std::string(line.substr(eq + 1));
        pos = lineEnd + 1;
    }
    return {};
}

}  // namespace clockwork::guest_config_text
