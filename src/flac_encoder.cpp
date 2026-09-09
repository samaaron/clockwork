// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
#include "flac_encoder.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace {

// ── Bit output ───────────────────────────────────────────────────────────────
// Subframes are packed without regard to byte boundaries; frame headers are
// byte-aligned. This appends to a caller's buffer so a frame lands in one
// allocation with its header.
class BitWriter {
public:
    explicit BitWriter(std::vector<uint8_t>& out) : mOut(out) {}

    void write(uint64_t value, int bits) {
        if (bits <= 0) return;
        if (bits < 64) value &= (uint64_t(1) << bits) - 1;
        while (bits > 0) {
            const int take = std::min(bits, 8 - mBitsInByte);
            const int shift = bits - take;
            const uint8_t chunk =
                static_cast<uint8_t>((value >> shift) & ((1u << take) - 1u));
            mByte = static_cast<uint8_t>((mByte << take) | chunk);
            mBitsInByte += take;
            bits -= take;
            if (mBitsInByte == 8) {
                mOut.push_back(mByte);
                mByte = 0;
                mBitsInByte = 0;
            }
        }
    }

    void writeSigned(int64_t value, int bits) {
        write(static_cast<uint64_t>(value), bits);
    }

    // Rice: `quotient` zeros, a stop bit, then the remainder.
    void rice(uint64_t folded, int param) {
        const uint64_t q = folded >> param;
        for (uint64_t i = 0; i < q; ++i) write(0, 1);
        write(1, 1);
        if (param > 0) write(folded & ((uint64_t(1) << param) - 1), param);
    }

    // Zero-pad to the next byte, as a frame requires before its CRC.
    void flushToByte() {
        if (mBitsInByte > 0) write(0, 8 - mBitsInByte);
    }

private:
    std::vector<uint8_t>& mOut;
    uint8_t mByte = 0;
    int     mBitsInByte = 0;
};

// ── Checksums ────────────────────────────────────────────────────────────────
uint8_t crc8(const uint8_t* data, size_t n) {
    uint8_t crc = 0;
    for (size_t i = 0; i < n; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b)
            crc = static_cast<uint8_t>((crc & 0x80) ? ((crc << 1) ^ 0x07)
                                                    : (crc << 1));
    }
    return crc;
}

uint16_t crc16(const uint8_t* data, size_t n) {
    uint16_t crc = 0;
    for (size_t i = 0; i < n; ++i) {
        crc ^= static_cast<uint16_t>(data[i]) << 8;
        for (int b = 0; b < 8; ++b)
            crc = static_cast<uint16_t>((crc & 0x8000) ? ((crc << 1) ^ 0x8005)
                                                       : (crc << 1));
    }
    return crc;
}

// The frame number, in the UTF-8-shaped coding FLAC borrows for it.
void putUtf8(std::vector<uint8_t>& out, uint64_t value) {
    if (value < 0x80u) { out.push_back(static_cast<uint8_t>(value)); return; }

    int bytes;
    if      (value < 0x800u)        bytes = 2;
    else if (value < 0x10000u)      bytes = 3;
    else if (value < 0x200000u)     bytes = 4;
    else if (value < 0x4000000u)    bytes = 5;
    else if (value < 0x80000000u)   bytes = 6;
    else                            bytes = 7;

    const uint8_t lead = static_cast<uint8_t>(0xFEu << (7 - bytes));
    out.push_back(static_cast<uint8_t>(lead | (value >> ((bytes - 1) * 6))));
    for (int i = bytes - 2; i >= 0; --i)
        out.push_back(static_cast<uint8_t>(0x80u | ((value >> (i * 6)) & 0x3Fu)));
}

// Rice folds a signed residual onto the naturals, small magnitudes first.
inline uint64_t fold(int64_t v) {
    return v < 0 ? (static_cast<uint64_t>(-(v + 1)) << 1) | 1u
                 : static_cast<uint64_t>(v) << 1;
}

constexpr int kMaxRiceParam   = 30;   // the escape codes sit one above these
constexpr int kMaxRice1Param  = 14;
constexpr int kMaxPartOrder   = 6;
constexpr int kMaxFixedOrder  = 4;

// Bits a partition costs at a given Rice parameter.
uint64_t partitionBits(const int64_t* r, uint32_t n, int param) {
    uint64_t bits = static_cast<uint64_t>(n) * (1u + param);
    for (uint32_t i = 0; i < n; ++i) bits += fold(r[i]) >> param;
    return bits;
}

// The parameter that costs least, found from the mean and checked either side
// rather than by trying all thirty-one.
int bestParam(const int64_t* r, uint32_t n, uint64_t* bitsOut) {
    if (n == 0) { *bitsOut = 0; return 0; }
    uint64_t sum = 0;
    for (uint32_t i = 0; i < n; ++i) sum += fold(r[i]);

    int p = 0;
    while (p < kMaxRiceParam && (static_cast<uint64_t>(n) << p) < sum) ++p;

    int bestP = p;
    uint64_t bestBits = partitionBits(r, n, p);
    for (int d = -1; d <= 1; d += 2) {
        const int q = p + d;
        if (q < 0 || q > kMaxRiceParam) continue;
        const uint64_t bits = partitionBits(r, n, q);
        if (bits < bestBits) { bestBits = bits; bestP = q; }
    }
    *bitsOut = bestBits;
    return bestP;
}

struct Partitioning {
    int      order = 0;
    int      method = 0;              // 0 = 4-bit parameters, 1 = 5-bit
    uint64_t bits = std::numeric_limits<uint64_t>::max();
    std::vector<int> params;
};

// Split the residual every way the block size allows and keep the cheapest.
Partitioning choosePartitioning(const int64_t* residual, uint32_t blockSize,
                                int predictorOrder) {
    Partitioning best;
    std::vector<int> params;

    for (int order = 0; order <= kMaxPartOrder; ++order) {
        const uint32_t parts = 1u << order;
        if (blockSize % parts != 0) continue;
        const uint32_t psize = blockSize / parts;
        if (psize <= static_cast<uint32_t>(predictorOrder)) continue;

        params.clear();
        uint64_t total = 0;
        int maxParam = 0;
        uint32_t at = 0;
        for (uint32_t i = 0; i < parts; ++i) {
            const uint32_t n = (i == 0) ? psize - predictorOrder : psize;
            uint64_t bits = 0;
            const int p = bestParam(residual + at, n, &bits);
            params.push_back(p);
            maxParam = std::max(maxParam, p);
            total += bits;
            at += n;
        }

        const int method = (maxParam <= kMaxRice1Param) ? 0 : 1;
        // The parameters themselves, plus the two-bit method and four-bit order.
        total += 6 + static_cast<uint64_t>(parts) * (method == 0 ? 4 : 5);

        if (total < best.bits) {
            best.bits = total;
            best.order = order;
            best.method = method;
            best.params = params;
        }
    }
    return best;
}

// The fixed polynomial predictors, as differences of increasing order.
void fixedResidual(const int32_t* x, uint32_t n, int order, int64_t* out) {
    switch (order) {
    case 0:
        for (uint32_t i = 0; i < n; ++i) out[i] = x[i];
        break;
    case 1:
        for (uint32_t i = 1; i < n; ++i) out[i - 1] = int64_t(x[i]) - x[i - 1];
        break;
    case 2:
        for (uint32_t i = 2; i < n; ++i)
            out[i - 2] = int64_t(x[i]) - 2ll * x[i - 1] + x[i - 2];
        break;
    case 3:
        for (uint32_t i = 3; i < n; ++i)
            out[i - 3] = int64_t(x[i]) - 3ll * x[i - 1] + 3ll * x[i - 2] - x[i - 3];
        break;
    default:
        for (uint32_t i = 4; i < n; ++i)
            out[i - 4] = int64_t(x[i]) - 4ll * x[i - 1] + 6ll * x[i - 2]
                       - 4ll * x[i - 3] + x[i - 4];
        break;
    }
}

} // namespace

FlacEncoder::FlacEncoder(uint32_t channels, uint32_t sampleRate,
                         uint32_t bitsPerSample)
    : mChannels(channels), mSampleRate(sampleRate), mBps(bitsPerSample) {
    mValid = channels >= 1 && channels <= 8
          && sampleRate >= 1 && sampleRate < (1u << 20)
          && (bitsPerSample == 16 || bitsPerSample == 24);
    if (!mValid) return;

    mChannel.resize(kBlockSize);
    mResidual.resize(kBlockSize);
    mBest.resize(kBlockSize);
}

std::vector<uint8_t> FlacEncoder::header() const {
    std::vector<uint8_t> out;
    out.reserve(4 + 4 + kStreamInfoBytes);
    out.push_back('f'); out.push_back('L'); out.push_back('a'); out.push_back('C');

    // STREAMINFO, and it is the last metadata block.
    out.push_back(0x80);
    out.push_back(0x00); out.push_back(0x00);
    out.push_back(static_cast<uint8_t>(kStreamInfoBytes));

    const auto body = finalStreamInfo();
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

std::vector<uint8_t> FlacEncoder::finalStreamInfo() const {
    std::vector<uint8_t> body;
    body.reserve(kStreamInfoBytes);
    BitWriter bw(body);

    // Before any frame these totals are zero, which is how a FLAC stream says
    // "unknown"; close writes this block again with them filled in.
    bw.write(mMinBlock ? mMinBlock : kBlockSize, 16);
    bw.write(mMaxBlock ? mMaxBlock : kBlockSize, 16);
    bw.write(mMinFrame, 24);
    bw.write(mMaxFrame, 24);
    bw.write(mSampleRate, 20);
    bw.write(mChannels - 1, 3);
    bw.write(mBps - 1, 5);
    bw.write(mTotalFrames & ((uint64_t(1) << 36) - 1), 36);
    // The MD5 of the unencoded audio, left at zero: the format defines that as
    // "not computed", and every decoder treats it so.
    for (int i = 0; i < 16; ++i) bw.write(0, 8);

    body.resize(kStreamInfoBytes, 0);
    return body;
}

void FlacEncoder::encodeBlock(const int32_t* interleaved, uint32_t frames,
                              std::vector<uint8_t>& out) {
    if (!mValid || frames == 0 || frames > kBlockSize) return;

    const size_t frameStart = out.size();

    // ── Frame header ────────────────────────────────────────────────────────
    // Sample rate and sample size are coded as "take it from STREAMINFO",
    // which every rate and depth this encoder accepts can use, so no lookup
    // table can disagree with the stream info written above.
    {
        std::vector<uint8_t> hdr;
        BitWriter bw(hdr);
        bw.write(0x3FFE, 14);          // sync
        bw.write(0, 1);                // reserved
        bw.write(0, 1);                // fixed blocksize: the number below is a frame index

        const bool wholeBlock = (frames == kBlockSize);
        bw.write(wholeBlock ? 0xC : 0x7, 4);   // 4096, or a 16-bit count at the end
        bw.write(0, 4);                // sample rate from STREAMINFO
        bw.write(mChannels - 1, 4);    // independent channels
        bw.write(0, 3);                // sample size from STREAMINFO
        bw.write(0, 1);                // reserved
        bw.flushToByte();

        putUtf8(hdr, mFrameNumber);
        if (!wholeBlock) {
            hdr.push_back(static_cast<uint8_t>((frames - 1) >> 8));
            hdr.push_back(static_cast<uint8_t>((frames - 1) & 0xFF));
        }
        hdr.push_back(crc8(hdr.data(), hdr.size()));
        out.insert(out.end(), hdr.begin(), hdr.end());
    }

    // ── Subframes ───────────────────────────────────────────────────────────
    BitWriter bw(out);
    for (uint32_t ch = 0; ch < mChannels; ++ch) {
        int32_t* x = mChannel.data();
        for (uint32_t i = 0; i < frames; ++i)
            x[i] = interleaved[static_cast<size_t>(i) * mChannels + ch];

        // A block that never changes is one sample and a type code.
        bool constant = true;
        for (uint32_t i = 1; i < frames && constant; ++i)
            constant = (x[i] == x[0]);
        if (constant) {
            bw.write(0, 1); bw.write(0x00, 6); bw.write(0, 1);
            bw.writeSigned(x[0], static_cast<int>(mBps));
            ++mKinds[kConstant];
            continue;
        }

        // Every fixed order, kept if it is cheaper than the last.
        const uint64_t verbatimBits = static_cast<uint64_t>(frames) * mBps;
        uint64_t bestBits = verbatimBits;
        int bestOrder = -1;
        Partitioning bestPart;

        const int maxOrder = std::min<int>(kMaxFixedOrder,
                                           static_cast<int>(frames) - 1);
        for (int order = 0; order <= maxOrder; ++order) {
            const uint32_t n = frames - static_cast<uint32_t>(order);
            fixedResidual(x, frames, order, mResidual.data());

            // A residual too wide for the coder is a residual this order will
            // not carry; the next order, or verbatim, takes it instead.
            bool codable = true;
            for (uint32_t i = 0; i < n && codable; ++i)
                codable = fold(mResidual[i]) <= (std::numeric_limits<uint64_t>::max() >> 1);
            if (!codable) continue;

            Partitioning part = choosePartitioning(mResidual.data(), frames, order);
            if (part.bits == std::numeric_limits<uint64_t>::max()) continue;

            const uint64_t total = part.bits
                                 + static_cast<uint64_t>(order) * mBps;  // warmup
            if (total < bestBits) {
                bestBits  = total;
                bestOrder = order;
                bestPart  = std::move(part);
                std::copy(mResidual.begin(), mResidual.begin() + n, mBest.begin());
            }
        }

        if (bestOrder < 0) {
            bw.write(0, 1); bw.write(0x01, 6); bw.write(0, 1);   // VERBATIM
            for (uint32_t i = 0; i < frames; ++i)
                bw.writeSigned(x[i], static_cast<int>(mBps));
            ++mKinds[kVerbatim];
            continue;
        }
        ++mKinds[bestOrder];

        bw.write(0, 1);
        bw.write(0x08u | static_cast<uint32_t>(bestOrder), 6);   // FIXED, order
        bw.write(0, 1);
        for (int i = 0; i < bestOrder; ++i)
            bw.writeSigned(x[i], static_cast<int>(mBps));

        bw.write(static_cast<uint32_t>(bestPart.method), 2);
        bw.write(static_cast<uint32_t>(bestPart.order), 4);

        const uint32_t parts = 1u << bestPart.order;
        const uint32_t psize = frames / parts;
        const int      pbits = bestPart.method == 0 ? 4 : 5;
        uint32_t at = 0;
        for (uint32_t i = 0; i < parts; ++i) {
            const uint32_t n = (i == 0) ? psize - static_cast<uint32_t>(bestOrder)
                                        : psize;
            const int p = bestPart.params[i];
            bw.write(static_cast<uint32_t>(p), pbits);
            for (uint32_t k = 0; k < n; ++k)
                bw.rice(fold(mBest[at + k]), p);
            at += n;
        }
    }
    bw.flushToByte();

    // ── Frame checksum ──────────────────────────────────────────────────────
    const uint16_t sum = crc16(out.data() + frameStart, out.size() - frameStart);
    out.push_back(static_cast<uint8_t>(sum >> 8));
    out.push_back(static_cast<uint8_t>(sum & 0xFF));

    const uint32_t frameBytes =
        static_cast<uint32_t>(out.size() - frameStart);
    mMinFrame = mMinFrame ? std::min(mMinFrame, frameBytes) : frameBytes;
    mMaxFrame = std::max(mMaxFrame, frameBytes);
    mMinBlock = mMinBlock ? std::min(mMinBlock, frames) : frames;
    mMaxBlock = std::max(mMaxBlock, frames);

    mTotalFrames += frames;
    ++mFrameNumber;
}
