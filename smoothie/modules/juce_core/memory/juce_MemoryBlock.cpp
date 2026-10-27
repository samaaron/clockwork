/*
  ==============================================================================

   This file is part of the JUCE library.
   Copyright (c) 2022 - Raw Material Software Limited

   JUCE is an open source library subject to commercial or open-source
   licensing.

   The code included in this file is provided under the terms of the ISC license
   http://www.isc.org/downloads/software-support-policy/isc-license. Permission
   To use, copy, modify, and/or distribute this software for any purpose with or
   without fee is hereby granted provided that the above copyright notice and
   this permission notice appear in all copies.

   JUCE IS PROVIDED "AS IS" WITHOUT ANY WARRANTY, AND ALL WARRANTIES, WHETHER
   EXPRESSED OR IMPLIED, INCLUDING MERCHANTABILITY AND FITNESS FOR PURPOSE, ARE
   DISCLAIMED.

  ==============================================================================
*/

namespace juce
{

MemoryBlock::MemoryBlock() noexcept {}

MemoryBlock::MemoryBlock (size_t initialSize, bool initialiseToZero)
{
    if (initialSize > 0)
    {
        size = initialSize;
        data.allocate (initialSize, initialiseToZero);
    }
    else
    {
        size = 0;
    }
}

MemoryBlock::MemoryBlock (const MemoryBlock& other)
    : size (other.size)
{
    if (size > 0)
    {
        jassert (other.data != nullptr);
        data.malloc (size);
        memcpy (data, other.data, size);
    }
}

MemoryBlock::MemoryBlock (const void* const dataToInitialiseFrom, const size_t sizeInBytes)
    : size (sizeInBytes)
{
    jassert (((ssize_t) sizeInBytes) >= 0);

    if (size > 0)
    {
        jassert (dataToInitialiseFrom != nullptr); // non-zero size, but a zero pointer passed-in?

        data.malloc (size);

        if (dataToInitialiseFrom != nullptr)
            memcpy (data, dataToInitialiseFrom, size);
    }
}

MemoryBlock::~MemoryBlock() noexcept
{
}

MemoryBlock& MemoryBlock::operator= (const MemoryBlock& other)
{
    if (this != &other)
    {
        setSize (other.size, false);
        memcpy (data, other.data, size);
    }

    return *this;
}

MemoryBlock::MemoryBlock (MemoryBlock&& other) noexcept
    : data (std::move (other.data)),
      size (other.size)
{
}

MemoryBlock& MemoryBlock::operator= (MemoryBlock&& other) noexcept
{
    data = std::move (other.data);
    size = other.size;
    return *this;
}

//==============================================================================
bool MemoryBlock::operator== (const MemoryBlock& other) const noexcept
{
    return matches (other.data, other.size);
}

bool MemoryBlock::operator!= (const MemoryBlock& other) const noexcept
{
    return ! operator== (other);
}

bool MemoryBlock::matches (const void* dataToCompare, size_t dataSize) const noexcept
{
    return size == dataSize
            && memcmp (data, dataToCompare, size) == 0;
}

//==============================================================================
// this will resize the block to this size
void MemoryBlock::setSize (const size_t newSize, const bool initialiseToZero)
{
    if (size != newSize)
    {
        if (newSize <= 0)
        {
            reset();
        }
        else
        {
            if (data != nullptr)
            {
                data.realloc (newSize);

                if (initialiseToZero && (newSize > size))
                    zeromem (data + size, newSize - size);
            }
            else
            {
                data.allocate (newSize, initialiseToZero);
            }

            size = newSize;
        }
    }
}

void MemoryBlock::reset()
{
    data.free();
    size = 0;
}

void MemoryBlock::ensureSize (size_t minimumSize, bool initialiseToZero)
{
    if (size < minimumSize)
        setSize (minimumSize, initialiseToZero);
}

void MemoryBlock::swapWith (MemoryBlock& other) noexcept
{
    std::swap (size, other.size);
    data.swapWith (other.data);
}

//==============================================================================
void MemoryBlock::fillWith (uint8 value) noexcept
{
    memset (data, (int) value, size);
}

void MemoryBlock::append (const void* srcData, size_t numBytes)
{
    if (numBytes > 0)
    {
        jassert (srcData != nullptr); // this must not be null!
        auto oldSize = size;
        setSize (size + numBytes);
        memcpy (data + oldSize, srcData, numBytes);
    }
}

void MemoryBlock::replaceAll (const void* srcData, size_t numBytes)
{
    if (numBytes <= 0)
    {
        reset();
        return;
    }

    jassert (srcData != nullptr); // this must not be null!
    setSize (numBytes);
    memcpy (data, srcData, numBytes);
}

void MemoryBlock::insert (const void* srcData, size_t numBytes, size_t insertPosition)
{
    if (numBytes > 0)
    {
        jassert (srcData != nullptr); // this must not be null!
        insertPosition = jmin (size, insertPosition);
        auto trailingDataSize = size - insertPosition;
        setSize (size + numBytes, false);

        if (trailingDataSize > 0)
            memmove (data + insertPosition + numBytes,
                     data + insertPosition,
                     trailingDataSize);

        memcpy (data + insertPosition, srcData, numBytes);
    }
}

void MemoryBlock::removeSection (size_t startByte, size_t numBytesToRemove)
{
    if (startByte + numBytesToRemove >= size)
    {
        setSize (startByte);
    }
    else if (numBytesToRemove > 0)
    {
        memmove (data + startByte,
                 data + startByte + numBytesToRemove,
                 size - (startByte + numBytesToRemove));

        setSize (size - numBytesToRemove);
    }
}

void MemoryBlock::copyFrom (const void* const src, int offset, size_t num) noexcept
{
    auto* d = static_cast<const char*> (src);

    if (offset < 0)
    {
        d -= offset;
        num += (size_t) -offset;
        offset = 0;
    }

    if ((size_t) offset + num > size)
        num = size - (size_t) offset;

    if (num > 0)
        memcpy (data + offset, d, num);
}

void MemoryBlock::copyTo (void* const dst, int offset, size_t num) const noexcept
{
    auto* d = static_cast<char*> (dst);

    if (offset < 0)
    {
        zeromem (d, (size_t) -offset);
        d -= offset;
        num -= (size_t) -offset;
        offset = 0;
    }

    if ((size_t) offset + num > size)
    {
        auto newNum = (size_t) size - (size_t) offset;
        zeromem (d + newNum, num - newNum);
        num = newNum;
    }

    if (num > 0)
        memcpy (d, data + offset, num);
}

} // namespace juce
