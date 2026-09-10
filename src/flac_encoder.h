// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * flac_encoder.h — a FLAC encoder, in the subset a recorder uses.
 *
 * dr_flac decodes; this writes. It exists because the permissive decoders are
 * decoders: every FLAC encoder that could have been vendored instead is either
 * copyleft or a sub-project with its own build, and clockwork's recorder needs
 * one file's worth of the format rather than all of it.
 *
 * WHAT IT PRODUCES is a compliant FLAC stream that any decoder reads: fixed
 * blocksize, independent channels, fixed polynomial predictors of order 0 to 4
 * chosen per subframe, and Rice-coded residuals in the partitioning that costs
 * the fewest bits.
 *
 * WHAT THAT COSTS, measured over a corpus of 206 sample files: 53.7% of raw
 * against libFLAC's 48.6%, so files
 * about a tenth larger than `flac` at its default setting. The gap is LPC and
 * stereo decorrelation, which this omits. Both buy compression alone — a
 * stream without them decodes identically on any player — so the price is
 * disk, and the same 206 files re-encoded here pass `flac -t` and decode to
 * PCM byte-identical with the originals.
 *
 * Integer samples only, 16 or 24 bits, because that is what FLAC carries.
 *
 * The encoder holds no file. Frames are appended to a caller's buffer, and the
 * stream info that needs totals is handed back at the end for the caller to
 * patch over the placeholder it wrote first. That keeps the format here and
 * the IO in clockwork_audio_file.cpp.
 */
#pragma once

#include <cstddef>   // size_t: used unqualified below, and <vector> happens
                     // to drag it in on libc++ and MSVC but not on libstdc++
#include <cstdint>
#include <vector>

class FlacEncoder {
public:
    // Frames per block. 4096 is what reference encoders use at these rates.
    static constexpr uint32_t kBlockSize = 4096;

    // Byte offset of the STREAMINFO body within header(): "fLaC" plus the
    // four-byte metadata block header that precedes it.
    static constexpr size_t kStreamInfoOffset = 8;
    static constexpr size_t kStreamInfoBytes  = 34;

    FlacEncoder(uint32_t channels, uint32_t sampleRate, uint32_t bitsPerSample);

    bool valid() const { return mValid; }

    // "fLaC" and a STREAMINFO whose totals are still unknown. Written once,
    // before any frame, and overwritten at the end by finalStreamInfo().
    std::vector<uint8_t> header() const;

    // Encode one block of up to kBlockSize interleaved frames, appending the
    // frame to `out`. Samples must already be within the signed range of
    // bitsPerSample.
    void encodeBlock(const int32_t* interleaved, uint32_t frames,
                     std::vector<uint8_t>& out);

    // The 34 STREAMINFO bytes with the totals filled in, for the caller to
    // write back over kStreamInfoOffset.
    std::vector<uint8_t> finalStreamInfo() const;

    uint64_t framesEncoded() const { return mTotalFrames; }

    // How many subframes were written with each shape, across the stream.
    // Fixed orders 0 to 4 are indexed by their order; kConstant and kVerbatim
    // count the two that carry no residual. This is the encoder saying what it
    // decided, which is the statistic a size alone cannot recover — two
    // streams of the same length can be built from entirely different choices.
    static constexpr int kConstant = 5;
    static constexpr int kVerbatim = 6;
    uint64_t subframeKind(int kind) const {
        return (kind >= 0 && kind <= kVerbatim) ? mKinds[kind] : 0;
    }

private:
    uint32_t mChannels = 0;
    uint32_t mSampleRate = 0;
    uint32_t mBps = 0;
    bool     mValid = false;

    uint64_t mTotalFrames = 0;
    uint32_t mFrameNumber = 0;
    uint32_t mMinBlock = 0, mMaxBlock = 0;
    uint32_t mMinFrame = 0, mMaxFrame = 0;
    uint64_t mKinds[7] = {0, 0, 0, 0, 0, 0, 0};

    // Scratch, reused across blocks so a recording does not allocate per frame.
    std::vector<int32_t> mChannel;
    std::vector<int64_t> mResidual;
    std::vector<int64_t> mBest;
};
