// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//
// Seeds for fuzz_ring_drain: rings written by the real writer
// (RingBufferWriter), in the shapes the drain has to walk, with their
// cursors in front as the target reads them.

#include "workers/RingBufferWriter.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kRing = 1024;

struct Seed {
    std::vector<uint8_t> ring = std::vector<uint8_t>(kRing, 0);
    std::atomic<int32_t> head{0}, tail{0}, seq{0}, lock{0};
    bool write(uint32_t payload, uint8_t fill) {
        std::vector<uint8_t> p(payload, fill);
        return RingBufferWriter::write(ring.data(), kRing, &head, &tail, &seq, &lock, p.data(), payload, 7);
    }
    void save(const std::filesystem::path& dir, const char* name, uint8_t maxFrames = 16, uint8_t retainEvery = 0) const {
        std::vector<uint8_t> out(10);
        const int32_t h = head.load(), t = tail.load();
        std::memcpy(out.data(), &h, 4);
        std::memcpy(out.data() + 4, &t, 4);
        out[8] = maxFrames;
        out[9] = retainEvery;
        out.insert(out.end(), ring.begin(), ring.end());
        std::ofstream f(dir / name, std::ios::binary);
        f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    }
};

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) { std::fprintf(stderr, "usage: ring_drain_seeds <dir>\n"); return 2; }
    const std::filesystem::path dir = argv[1];
    std::filesystem::create_directories(dir);

    { Seed s; s.save(dir, "empty"); }
    { Seed s; s.write(13, 'a'); s.save(dir, "one-frame"); }
    { Seed s; s.write(13, 'a'); s.write(100, 'b'); s.save(dir, "two-frames"); }
    { Seed s; s.write(0, 0); s.write(5, 'c'); s.save(dir, "header-only-then-a-frame"); }
    { Seed s; s.write(13, 'a'); s.write(100, 'b'); s.save(dir, "retain-the-second", 16, 2); }
    {   // a frame that would cross the end: the writer pads and restarts at 0,
        // the reader consumed the first frames, the tail follows the pad
        Seed s;
        s.write(900, 'd');
        s.tail.store(s.head.load());
        s.write(200, 'e');
        s.save(dir, "pad-and-wrap");
    }
    {   // the ring one byte from full, as a flooding peer leaves it
        Seed s;
        while (s.write(60, 'f')) {}
        s.save(dir, "full", 0);
    }
    std::printf("7 seeds in %s\n", dir.string().c_str());
    return 0;
}
