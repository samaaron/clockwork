// SPDX-License-Identifier: ISC
// Copyright (c) 2026 Sam Aaron
// Conversions between juce::String and Core Foundation strings.

namespace juce
{

String String::fromCFString (CFStringRef cfString)
{
    if (cfString == nullptr)
        return {};

    const CFRange range = { 0, CFStringGetLength (cfString) };
    CFIndex bytesNeeded = 0;
    CFStringGetBytes (cfString, range, kCFStringEncodingUTF8, 0, false, nullptr, 0, &bytesNeeded);

    std::string utf8 (static_cast<size_t> (bytesNeeded), '\0');
    CFStringGetBytes (cfString, range, kCFStringEncodingUTF8, 0, false,
                      reinterpret_cast<UInt8*> (utf8.data()), bytesNeeded, nullptr);

    return String (std::move (utf8));
}

CFStringRef String::toCFString() const
{
    if (CFStringRef result = CFStringCreateWithBytes (kCFAllocatorDefault, reinterpret_cast<const UInt8*> (text.data()),
                                                      static_cast<CFIndex> (text.size()), kCFStringEncodingUTF8, false))
        return result;

    // CFStringCreateWithBytes refuses malformed UTF-8; give an empty string
    // rather than a null pointer.
    return CFStringCreateWithBytes (kCFAllocatorDefault, nullptr, 0, kCFStringEncodingUTF8, false);
}

} // namespace juce
