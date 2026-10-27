// SPDX-License-Identifier: ISC
// Copyright (c) 2026 Sam Aaron

namespace juce
{

//==============================================================================
/**
    Which channels of a device are in use: one bit per channel. The device
    interface and every driver spell channel selections with this.

    A fixed-size bit set, trivially copyable and heap-free, so it can be
    read on the audio thread. It replaces JUCE's arbitrary-precision
    BigInteger, which the device layer only ever used as a bit set; the
    method names are BigInteger's so the drivers read unchanged.

    @tags{Audio}
*/
class ChannelMask final
{
public:
    /** More channels than any device offers. A bit at or past this is
        ignored by set and read as clear. */
    static constexpr int maxChannels = 1024;

    ChannelMask() noexcept = default;

    /** The low 64 bits, as a Windows channel mask or a literal 0 arrives. */
    explicit ChannelMask (int64 bits) noexcept
    {
        for (int i = 0; i < 64; ++i)
            if ((static_cast<uint64> (bits) >> i) & 1)
                bitsSet.set (static_cast<size_t> (i));
    }

    //==============================================================================
    bool isZero() const noexcept                                  { return bitsSet.none(); }
    ChannelMask& clear() noexcept                                 { bitsSet.reset(); return *this; }
    ChannelMask& clearBit (int bit) noexcept                      { return setBit (bit, false); }
    ChannelMask& setBit (int bit)                                 { return setBit (bit, true); }

    ChannelMask& setBit (int bit, bool shouldBeSet)
    {
        if (inRange (bit))
            bitsSet.set (static_cast<size_t> (bit), shouldBeSet);

        return *this;
    }

    /** Sets or clears `numBits` bits from `startBit`; a count of zero or
        less does nothing, as BigInteger's did. */
    ChannelMask& setRange (int startBit, int numBits, bool shouldBeSet)
    {
        for (int i = startBit; i < startBit + numBits; ++i)
            setBit (i, shouldBeSet);

        return *this;
    }

    bool operator[] (int bit) const noexcept                      { return inRange (bit) && bitsSet.test (static_cast<size_t> (bit)); }

    int countNumberOfSetBits() const noexcept                     { return static_cast<int> (bitsSet.count()); }

    /** The highest set bit, or -1 when none is. */
    int getHighestBit() const noexcept
    {
        for (int i = maxChannels; --i >= 0;)
            if (bitsSet.test (static_cast<size_t> (i)))
                return i;

        return -1;
    }

    /** The first set bit at or after `startIndex`, or -1 when none is. */
    int findNextSetBit (int startIndex) const noexcept
    {
        for (int i = std::max (0, startIndex); i < maxChannels; ++i)
            if (bitsSet.test (static_cast<size_t> (i)))
                return i;

        return -1;
    }

    /** The low 32 bits, as an int. */
    int toInteger() const noexcept
    {
        uint32 v = 0;

        for (int i = 0; i < 32; ++i)
            if (bitsSet.test (static_cast<size_t> (i)))
                v |= (1u << i);

        return static_cast<int> (v);
    }

    /** The bits as text, highest first, for a log line. Base 2 only. */
    String toString (int base, int minimumNumCharacters = 1) const
    {
        jassert (base == 2);
        (void) base;
        String s;

        for (int i = getHighestBit(); i >= 0; --i)
            s << (bitsSet.test (static_cast<size_t> (i)) ? '1' : '0');

        while (s.length() < minimumNumCharacters)
            s = "0" + s;

        return s;
    }

    //==============================================================================
    bool operator== (const ChannelMask& other) const noexcept     { return bitsSet == other.bitsSet; }
    bool operator!= (const ChannelMask& other) const noexcept     { return bitsSet != other.bitsSet; }

    /** Orders as the integers the bits spell. */
    bool operator< (const ChannelMask& other) const noexcept
    {
        for (int i = maxChannels; --i >= 0;)
        {
            const bool a = bitsSet.test (static_cast<size_t> (i)), b = other.bitsSet.test (static_cast<size_t> (i));

            if (a != b)
                return b;
        }

        return false;
    }

    ChannelMask& operator&= (const ChannelMask& other) noexcept   { bitsSet &= other.bitsSet; return *this; }
    ChannelMask& operator|= (const ChannelMask& other) noexcept   { bitsSet |= other.bitsSet; return *this; }

private:
    static bool inRange (int bit) noexcept                        { return bit >= 0 && bit < maxChannels; }

    std::bitset<maxChannels> bitsSet;
};

} // namespace juce
