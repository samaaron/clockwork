// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//
// A main() for compilers without libFuzzer (GCC, MSVC): runs the target once
// on each file named, or on each file in each directory named. That is how a
// corpus, and every crash ever found, keeps being checked on every platform,
// under whatever sanitizers the build turned on. Fuzzing itself is clang's.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

namespace fs = std::filesystem;

static int runFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) { std::fprintf(stderr, "cannot read %s\n", p.string().c_str()); return 1; }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    LLVMFuzzerTestOneInput(bytes.data(), bytes.size());
    return 0;
}

int main(int argc, char** argv) {
    int failed = 0, ran = 0;
    for (int i = 1; i < argc; ++i) {
        const fs::path p(argv[i]);
        if (fs::is_directory(p)) {
            for (const auto& e : fs::recursive_directory_iterator(p))
                if (e.is_regular_file()) { failed += runFile(e.path()); ++ran; }
        } else {
            failed += runFile(p); ++ran;
        }
    }
    std::printf("%d inputs run\n", ran);
    return failed ? 1 : 0;
}
