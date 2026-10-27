// SPDX-License-Identifier: ISC
// Copyright (c) 2026 Sam Aaron
/*
 * juce_String.h: juce::String as a thin wrapper over std::string.
 *
 * JUCE's String was its own UTF-8 implementation with a character-pointer
 * abstraction underneath, 7,900 lines across the text directory. Smoothie's
 * drivers use about thirty of its methods, all of them things std::string
 * does, so this keeps those names and signatures over a std::string and the
 * drivers read unchanged. Text is UTF-8 throughout; length() and indexing
 * count bytes, which suits every caller here (names and messages from the
 * operating system, logged or compared whole).
 *
 * CharPointer_UTF8 survives only as a named `const char*`, so a String can
 * still be made from one, and StringRef as the by-reference parameter type.
 */

#if JUCE_MAC || JUCE_IOS
 // A CFStringRef can only be forward-declared by forward-declaring the
 // aliased type.
 #if __has_attribute(objc_bridge)
  #define JUCE_CF_BRIDGED_TYPE(T) __attribute__ ((objc_bridge (T)))
 #else
  #define JUCE_CF_BRIDGED_TYPE(T)
 #endif

 typedef const struct JUCE_CF_BRIDGED_TYPE(NSString) __CFString * CFStringRef;

 #undef JUCE_CF_BRIDGED_TYPE
#endif

namespace juce
{

#if JUCE_WINDOWS
 using juce_wchar = wchar_t;
#else
 using juce_wchar = uint32;
#endif

class String;

//==============================================================================
/** A UTF-8 character pointer, as the name JUCE gave it: a const char* that
    a String can be made from. */
struct CharPointer_UTF8
{
    using CharType = char;

    explicit CharPointer_UTF8 (const char* text) noexcept : data (text) {}

    const char* getAddress() const noexcept     { return data; }
    operator const char*() const noexcept       { return data; }

    /** True when `text` holds valid UTF-8 for at most `maxBytes` bytes (stops at a null). */
    static bool isValidString (const char* text, int maxBytes) noexcept
    {
        if (text == nullptr)
            return false;

        const auto* p = reinterpret_cast<const unsigned char*> (text);

        for (int i = 0; i < maxBytes && p[i] != 0;)
        {
            const auto c = p[i];
            const int extra = c < 0x80 ? 0 : (c >> 5) == 0x6 ? 1 : (c >> 4) == 0xe ? 2 : (c >> 3) == 0x1e ? 3 : -1;

            if (extra < 0)
                return false;

            for (int k = 1; k <= extra; ++k)
                if (i + k >= maxBytes || (p[i + k] & 0xc0) != 0x80)
                    return false;

            i += extra + 1;
        }

        return true;
    }

    const char* data;
};

//==============================================================================
/** A string: UTF-8 in a std::string, with the JUCE method names Smoothie uses. */
class JUCE_API  String  final
{
public:
    //==============================================================================
    String() = default;
    String (const String&) = default;
    String (String&&) noexcept = default;
    String& operator= (const String&) = default;
    String& operator= (String&&) noexcept = default;

    String (const char* text) : text (text != nullptr ? text : "") {}
    String (const char* text, size_t maxChars) : text (text != nullptr ? std::string (text, std::min (maxChars, std::strlen (text))) : std::string()) {}
    String (const std::string& text) : text (text) {}
    String (std::string&& text) noexcept : text (std::move (text)) {}
    String (CharPointer_UTF8 text) : String (text.data) {}
    String (const wchar_t* text) : text (fromWide (text, text != nullptr ? std::wcslen (text) : 0)) {}
    String (const wchar_t* text, size_t numChars) : text (fromWide (text, numChars)) {}

    explicit String (int number)                 : text (std::to_string (number)) {}
    explicit String (unsigned int number)        : text (std::to_string (number)) {}
    explicit String (long number)                : text (std::to_string (number)) {}
    explicit String (unsigned long number)       : text (std::to_string (number)) {}
    explicit String (long long number)           : text (std::to_string (number)) {}
    explicit String (unsigned long long number)  : text (std::to_string (number)) {}
    explicit String (float number)               : text (fromDouble (number)) {}
    explicit String (double number)              : text (fromDouble (number)) {}

    //==============================================================================
    bool isEmpty() const noexcept                       { return text.empty(); }
    bool isNotEmpty() const noexcept                    { return ! text.empty(); }
    int length() const noexcept                         { return static_cast<int> (text.size()); }
    void clear() noexcept                               { text.clear(); }
    void swapWith (String& other) noexcept              { text.swap (other.text); }
    void preallocateBytes (size_t numBytes)             { text.reserve (numBytes); }

    int hashCode() const noexcept                       { return static_cast<int> (std::hash<std::string>() (text)); }
    int64 hashCode64() const noexcept                   { return static_cast<int64> (std::hash<std::string>() (text)); }

    //==============================================================================
    const std::string& toStdString() const noexcept     { return text; }
    const char* toRawUTF8() const noexcept              { return text.c_str(); }
    CharPointer_UTF8 toUTF8() const noexcept            { return CharPointer_UTF8 (text.c_str()); }
    CharPointer_UTF8 getCharPointer() const noexcept    { return CharPointer_UTF8 (text.c_str()); }

    /** The text as the platform's wide characters: UTF-16 on Windows, UTF-32
        elsewhere. The pointer is this String's own and lives as long as it is
        not changed. */
    const wchar_t* toWideCharPointer() const
    {
        wide = toWide (text);
        return wide.c_str();
    }

    static String fromUTF8 (const char* utf8, int numBytes = -1)
    {
        if (utf8 == nullptr)
            return {};

        return numBytes < 0 ? String (utf8) : String (std::string (utf8, static_cast<size_t> (numBytes)));
    }

   #if JUCE_MAC || JUCE_IOS
    static String fromCFString (CFStringRef cfString);
    CFStringRef toCFString() const;
   #endif

    //==============================================================================
    juce_wchar operator[] (int index) const noexcept    { return index >= 0 && index < length() ? static_cast<juce_wchar> (static_cast<unsigned char> (text[static_cast<size_t> (index)])) : 0; }
    juce_wchar getLastCharacter() const noexcept        { return text.empty() ? 0 : static_cast<juce_wchar> (static_cast<unsigned char> (text.back())); }

    int compare (const String& other) const noexcept    { return text.compare (other.text); }
    int compare (const char* other) const noexcept      { return text.compare (other != nullptr ? other : ""); }
    int compareIgnoreCase (const String& other) const noexcept { return lower (text).compare (lower (other.text)); }

    bool equalsIgnoreCase (const String& other) const noexcept  { return lower (text) == lower (other.text); }
    bool equalsIgnoreCase (const char* other) const noexcept    { return lower (text) == lower (other != nullptr ? other : ""); }
    bool equalsIgnoreCase (const wchar_t* other) const          { return equalsIgnoreCase (String (other)); }

    bool startsWith (const String& other) const noexcept        { return text.compare (0, other.text.size(), other.text) == 0; }
    bool startsWith (const char* other) const noexcept          { return startsWith (String (other)); }
    bool endsWith (const String& other) const noexcept          { return text.size() >= other.text.size() && text.compare (text.size() - other.text.size(), other.text.size(), other.text) == 0; }
    bool endsWith (const char* other) const noexcept            { return endsWith (String (other)); }

    bool contains (const String& other) const noexcept          { return text.find (other.text) != std::string::npos; }
    bool contains (const char* other) const noexcept            { return other != nullptr && text.find (other) != std::string::npos; }
    bool containsChar (juce_wchar c) const noexcept             { return c < 0x80 && text.find (static_cast<char> (c)) != std::string::npos; }
    bool containsIgnoreCase (const String& other) const noexcept { return lower (text).find (lower (other.text)) != std::string::npos; }
    bool containsIgnoreCase (const char* other) const noexcept  { return containsIgnoreCase (String (other)); }
    bool containsAnyOf (const String& chars) const noexcept     { return text.find_first_of (chars.text) != std::string::npos; }
    bool containsAnyOf (const char* chars) const noexcept       { return chars != nullptr && text.find_first_of (chars) != std::string::npos; }

    int indexOf (const String& other) const noexcept            { return pos (text.find (other.text)); }
    int indexOf (const char* other) const noexcept              { return other == nullptr ? -1 : pos (text.find (other)); }
    int indexOf (int startIndex, const String& other) const noexcept { return startIndex < 0 || startIndex > length() ? -1 : pos (text.find (other.text, static_cast<size_t> (startIndex))); }
    int indexOf (int startIndex, const char* other) const noexcept   { return indexOf (startIndex, String (other)); }
    int indexOfChar (juce_wchar c) const noexcept               { return c < 0x80 ? pos (text.find (static_cast<char> (c))) : -1; }
    int indexOfChar (int startIndex, juce_wchar c) const noexcept { return c < 0x80 && startIndex >= 0 ? pos (text.find (static_cast<char> (c), static_cast<size_t> (startIndex))) : -1; }
    int lastIndexOf (const String& other) const noexcept        { return pos (text.rfind (other.text)); }
    int lastIndexOf (const char* other) const noexcept          { return other == nullptr ? -1 : pos (text.rfind (other)); }
    int lastIndexOfChar (juce_wchar c) const noexcept           { return c < 0x80 ? pos (text.rfind (static_cast<char> (c))) : -1; }

    //==============================================================================
    String substring (int startIndex, int endIndex) const
    {
        const auto len = length();
        startIndex = std::clamp (startIndex, 0, len);
        endIndex   = std::clamp (endIndex, startIndex, len);
        return String (text.substr (static_cast<size_t> (startIndex), static_cast<size_t> (endIndex - startIndex)));
    }

    String substring (int startIndex) const                     { return substring (startIndex, length()); }
    String dropLastCharacters (int numberToDrop) const          { return substring (0, length() - std::max (0, numberToDrop)); }
    String getLastCharacters (int numCharacters) const          { return substring (length() - std::max (0, numCharacters)); }

    String fromFirstOccurrenceOf (const String& sub, bool includeSub, bool ignoreCase) const
    {
        const auto i = find (sub, ignoreCase, false);
        return i < 0 ? String() : substring (includeSub ? i : i + sub.length());
    }

    String fromLastOccurrenceOf (const String& sub, bool includeSub, bool ignoreCase) const
    {
        const auto i = find (sub, ignoreCase, true);
        return i < 0 ? *this : substring (includeSub ? i : i + sub.length());
    }

    String upToFirstOccurrenceOf (const String& sub, bool includeSub, bool ignoreCase) const
    {
        const auto i = find (sub, ignoreCase, false);
        return i < 0 ? *this : substring (0, includeSub ? i + sub.length() : i);
    }

    String upToLastOccurrenceOf (const String& sub, bool includeSub, bool ignoreCase) const
    {
        const auto i = find (sub, ignoreCase, true);
        return i < 0 ? *this : substring (0, includeSub ? i + sub.length() : i);
    }

    String trim() const                                         { return trimStart().trimEnd(); }
    String trimStart() const                                    { const auto i = text.find_first_not_of (" \t\r\n"); return i == std::string::npos ? String() : String (text.substr (i)); }
    String trimEnd() const                                      { const auto i = text.find_last_not_of (" \t\r\n"); return i == std::string::npos ? String() : String (text.substr (0, i + 1)); }
    String toUpperCase() const                                  { return String (upper (text)); }
    String toLowerCase() const                                  { return String (lower (text)); }

    String replace (const String& from, const String& to, bool ignoreCase = false) const
    {
        if (from.isEmpty())
            return *this;

        std::string out;
        const auto hay = ignoreCase ? lower (text) : text;
        const auto needle = ignoreCase ? lower (from.text) : from.text;
        size_t last = 0;

        for (auto i = hay.find (needle); i != std::string::npos; i = hay.find (needle, last))
        {
            out.append (text, last, i - last);
            out += to.text;
            last = i + needle.size();
        }

        out.append (text, last, std::string::npos);
        return String (std::move (out));
    }

    String replaceCharacter (juce_wchar from, juce_wchar to) const
    {
        std::string out = text;

        for (auto& c : out)
            if (static_cast<unsigned char> (c) == from)
                c = static_cast<char> (to);

        return String (std::move (out));
    }

    String removeCharacters (const String& chars) const
    {
        std::string out;

        for (const char c : text)
            if (chars.text.find (c) == std::string::npos)
                out += c;

        return String (std::move (out));
    }

    String unquoted() const
    {
        if (text.size() >= 2 && (text.front() == '"' || text.front() == '\'') && text.back() == text.front())
            return substring (1, length() - 1);

        return *this;
    }

    String quoted (juce_wchar quoteCharacter = '"') const
    {
        const auto q = static_cast<char> (quoteCharacter);
        return String (q + text + q);
    }

    String paddedLeft (juce_wchar padCharacter, int minimumLength) const
    {
        const auto missing = minimumLength - length();
        return missing > 0 ? String (std::string (static_cast<size_t> (missing), static_cast<char> (padCharacter)) + text) : *this;
    }

    //==============================================================================
    int getIntValue() const noexcept                            { return static_cast<int> (std::strtol (text.c_str(), nullptr, 10)); }
    int64 getLargeIntValue() const noexcept                     { return static_cast<int64> (std::strtoll (text.c_str(), nullptr, 10)); }
    float getFloatValue() const noexcept                        { return std::strtof (text.c_str(), nullptr); }
    double getDoubleValue() const noexcept                      { return std::strtod (text.c_str(), nullptr); }
    int getHexValue32() const noexcept                          { return static_cast<int> (std::strtoul (text.c_str(), nullptr, 16)); }

    int getTrailingIntValue() const noexcept
    {
        auto i = text.size();

        while (i > 0 && std::isdigit (static_cast<unsigned char> (text[i - 1])))
            --i;

        return i == text.size() ? 0 : static_cast<int> (std::strtol (text.c_str() + i, nullptr, 10));
    }

    /** The number in lower-case hex, no padding, as JUCE spelt it. */
    template <typename IntegerType>
    static String toHexString (IntegerType number)
    {
        using U = typename std::make_unsigned<IntegerType>::type;
        const auto value = static_cast<unsigned long long> (static_cast<U> (number));
        char buf[24];
        std::snprintf (buf, sizeof (buf), "%llx", value);
        return String (buf);
    }

    //==============================================================================
    String& operator+= (const String& other)        { text += other.text; return *this; }
    String& operator+= (const char* other)          { if (other != nullptr) text += other; return *this; }
    String& operator+= (const std::string& other)   { text += other; return *this; }
    String& operator+= (char c)                     { text += c; return *this; }

    String& operator<< (const String& other)        { return *this += other; }
    String& operator<< (const char* other)          { return *this += other; }
    String& operator<< (const std::string& other)   { return *this += other; }
    String& operator<< (CharPointer_UTF8 other)     { return *this += other.data; }
    String& operator<< (char c)                     { return *this += c; }
    String& operator<< (int n)                      { return *this += std::to_string (n); }
    String& operator<< (unsigned int n)             { return *this += std::to_string (n); }
    String& operator<< (long n)                     { return *this += std::to_string (n); }
    String& operator<< (unsigned long n)            { return *this += std::to_string (n); }
    String& operator<< (long long n)                { return *this += std::to_string (n); }
    String& operator<< (unsigned long long n)       { return *this += std::to_string (n); }
    String& operator<< (float n)                    { return *this += fromDouble (n); }
    String& operator<< (double n)                   { return *this += fromDouble (n); }

    //==============================================================================
    friend bool operator== (const String& a, const String& b) noexcept  { return a.text == b.text; }
    friend bool operator!= (const String& a, const String& b) noexcept  { return a.text != b.text; }
    friend bool operator<  (const String& a, const String& b) noexcept  { return a.text <  b.text; }
    friend bool operator>  (const String& a, const String& b) noexcept  { return a.text >  b.text; }
    friend bool operator<= (const String& a, const String& b) noexcept  { return a.text <= b.text; }
    friend bool operator>= (const String& a, const String& b) noexcept  { return a.text >= b.text; }
    friend bool operator== (const String& a, const char* b) noexcept    { return a.text == (b != nullptr ? b : ""); }
    friend bool operator!= (const String& a, const char* b) noexcept    { return ! (a == b); }
    friend bool operator== (const char* a, const String& b) noexcept    { return b == a; }
    friend bool operator!= (const char* a, const String& b) noexcept    { return ! (b == a); }
    friend bool operator== (const String& a, const std::string& b) noexcept { return a.text == b; }
    friend bool operator!= (const String& a, const std::string& b) noexcept { return a.text != b; }

    friend String operator+ (String a, const String& b)             { a.text += b.text; return a; }
    friend String operator+ (String a, const char* b)               { if (b != nullptr) a.text += b; return a; }
    friend String operator+ (const char* a, const String& b)        { return String (a) + b; }
    friend String operator+ (String a, const std::string& b)        { a.text += b; return a; }
    friend String operator+ (const std::string& a, const String& b) { return String (a) + b; }
    friend String operator+ (String a, char b)                      { a.text += b; return a; }
    friend String operator+ (char a, const String& b)               { return String (std::string (1, a)) + b; }
    friend String operator+ (String a, int b)                       { return a << b; }
    friend String operator+ (String a, unsigned int b)              { return a << b; }
    friend String operator+ (String a, long b)                      { return a << b; }
    friend String operator+ (String a, unsigned long b)             { return a << b; }
    friend String operator+ (String a, long long b)                 { return a << b; }
    friend String operator+ (String a, unsigned long long b)        { return a << b; }
    friend String operator+ (String a, float b)                     { return a << b; }
    friend String operator+ (String a, double b)                    { return a << b; }

    friend std::ostream& operator<< (std::ostream& stream, const String& s) { return stream << s.text; }

private:
    //==============================================================================
    static int pos (size_t i) noexcept                  { return i == std::string::npos ? -1 : static_cast<int> (i); }
    static std::string lower (std::string s)            { for (auto& c : s) c = static_cast<char> (std::tolower (static_cast<unsigned char> (c))); return s; }
    static std::string upper (std::string s)            { for (auto& c : s) c = static_cast<char> (std::toupper (static_cast<unsigned char> (c))); return s; }

    int find (const String& sub, bool ignoreCase, bool last) const noexcept
    {
        const auto hay = ignoreCase ? lower (text) : text;
        const auto needle = ignoreCase ? lower (sub.text) : sub.text;
        return pos (last ? hay.rfind (needle) : hay.find (needle));
    }

    /** Six significant digits, trailing zeros dropped: JUCE streamed the
        number at the default precision, so a float and a double print alike
        and a rate prints as "48000". */
    static std::string fromDouble (double value)
    {
        char buf[32];
        std::snprintf (buf, sizeof (buf), "%g", value);
        return buf;
    }

    static std::string fromWide (const wchar_t* w, size_t numChars)
    {
        std::string out;

        if (w == nullptr)
            return out;

        for (size_t i = 0; i < numChars; ++i)
        {
            auto c = static_cast<uint32> (w[i]);

            if (sizeof (wchar_t) == 2 && c >= 0xd800 && c < 0xdc00 && i + 1 < numChars)   // a UTF-16 pair
            {
                const auto low = static_cast<uint32> (w[++i]);
                c = 0x10000 + ((c - 0xd800) << 10) + (low - 0xdc00);
            }

            appendUTF8 (out, c);
        }

        return out;
    }

    static void appendUTF8 (std::string& out, uint32 c)
    {
        if (c < 0x80)         { out += static_cast<char> (c); }
        else if (c < 0x800)   { out += static_cast<char> (0xc0 | (c >> 6));  out += static_cast<char> (0x80 | (c & 0x3f)); }
        else if (c < 0x10000) { out += static_cast<char> (0xe0 | (c >> 12)); out += static_cast<char> (0x80 | ((c >> 6) & 0x3f)); out += static_cast<char> (0x80 | (c & 0x3f)); }
        else                  { out += static_cast<char> (0xf0 | (c >> 18)); out += static_cast<char> (0x80 | ((c >> 12) & 0x3f)); out += static_cast<char> (0x80 | ((c >> 6) & 0x3f)); out += static_cast<char> (0x80 | (c & 0x3f)); }
    }

    static std::wstring toWide (const std::string& s)
    {
        std::wstring out;
        const auto* p = reinterpret_cast<const unsigned char*> (s.data());
        const auto n = s.size();

        for (size_t i = 0; i < n;)
        {
            uint32 c = p[i];
            int extra = 0;

            if (c >= 0xf0)      { c &= 0x07; extra = 3; }
            else if (c >= 0xe0) { c &= 0x0f; extra = 2; }
            else if (c >= 0xc0) { c &= 0x1f; extra = 1; }

            ++i;

            for (int k = 0; k < extra && i < n; ++k, ++i)
                c = (c << 6) | (p[i] & 0x3f);

            if (sizeof (wchar_t) == 2 && c >= 0x10000)
            {
                c -= 0x10000;
                out += static_cast<wchar_t> (0xd800 + (c >> 10));
                out += static_cast<wchar_t> (0xdc00 + (c & 0x3ff));
            }
            else
            {
                out += static_cast<wchar_t> (c);
            }
        }

        return out;
    }

    std::string text;
    mutable std::wstring wide;
};

//==============================================================================
/** A string handed to a function by reference without a copy: JUCE's
    parameter type, kept as the name. Holds a pointer into text that outlives
    the call. */
class StringRef
{
public:
    StringRef (const char* stringLiteral) noexcept : text (stringLiteral != nullptr ? stringLiteral : "") {}
    StringRef (const String& string) noexcept : text (string.toRawUTF8()) {}
    StringRef (const std::string& string) noexcept : text (string.c_str()) {}

    operator const char*() const noexcept       { return text; }
    operator String() const                     { return String (text); }
    bool isEmpty() const noexcept               { return text[0] == 0; }
    bool isNotEmpty() const noexcept            { return text[0] != 0; }
    int length() const noexcept                 { return static_cast<int> (std::strlen (text)); }

    const char* text;
};

inline bool operator== (const String& a, StringRef b) noexcept   { return a == b.text; }
inline bool operator!= (const String& a, StringRef b) noexcept   { return a != b.text; }
inline String operator+ (String a, StringRef b)                  { return a + b.text; }
inline String& operator<< (String& a, StringRef b)               { return a << b.text; }

} // namespace juce
