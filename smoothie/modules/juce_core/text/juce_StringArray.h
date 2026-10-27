// SPDX-License-Identifier: ISC
// Copyright (c) 2026 Sam Aaron
/*
 * juce_StringArray.h: juce::StringArray as a std::vector<String> with the
 * JUCE method names the device drivers use (device and channel name lists).
 */

namespace juce
{

class JUCE_API  StringArray  final
{
public:
    //==============================================================================
    StringArray() = default;
    StringArray (const StringArray&) = default;
    StringArray (StringArray&&) noexcept = default;
    StringArray& operator= (const StringArray&) = default;
    StringArray& operator= (StringArray&&) noexcept = default;

    StringArray (const String& firstValue) : strings { firstValue } {}
    StringArray (std::initializer_list<const char*> values) { for (auto* v : values) strings.emplace_back (v); }

    template <typename Type>
    StringArray (const Array<Type>& values)                 { addArray (values.begin(), values.end()); }

    //==============================================================================
    int size() const noexcept                               { return static_cast<int> (strings.size()); }
    bool isEmpty() const noexcept                           { return strings.empty(); }
    void clear() noexcept                                   { strings.clear(); }
    void swapWith (StringArray& other) noexcept             { strings.swap (other.strings); }
    void ensureStorageAllocated (int minNumElements)        { strings.reserve (static_cast<size_t> (std::max (0, minNumElements))); }

    const String& operator[] (int index) const noexcept     { return inRange (index) ? strings[static_cast<size_t> (index)] : empty(); }
    String& getReference (int index) noexcept               { jassert (inRange (index)); return strings[static_cast<size_t> (index)]; }
    const String& getReference (int index) const noexcept   { jassert (inRange (index)); return strings[static_cast<size_t> (index)]; }
    const String& getUnchecked (int index) const noexcept   { return getReference (index); }

    String* begin() noexcept                                { return strings.data(); }
    String* end() noexcept                                  { return strings.data() + strings.size(); }
    const String* begin() const noexcept                    { return strings.data(); }
    const String* end() const noexcept                      { return strings.data() + strings.size(); }

    //==============================================================================
    void add (String stringToAdd)                           { strings.push_back (std::move (stringToAdd)); }

    void insert (int index, String stringToAdd)
    {
        const auto at = std::clamp (index, 0, size());
        strings.insert (strings.begin() + at, std::move (stringToAdd));
    }

    bool addIfNotAlreadyThere (const String& stringToAdd, bool ignoreCase = false)
    {
        if (contains (stringToAdd, ignoreCase))
            return false;

        add (stringToAdd);
        return true;
    }

    void set (int index, String newString)
    {
        if (inRange (index))
            strings[static_cast<size_t> (index)] = std::move (newString);
        else if (index >= 0)
            add (std::move (newString));
    }

    template <typename Iterator>
    void addArray (Iterator start, Iterator finish)
    {
        for (; start != finish; ++start)
            strings.emplace_back (*start);
    }

    void addArray (const StringArray& other)                { strings.insert (strings.end(), other.strings.begin(), other.strings.end()); }

    /** Splits on spaces, tabs and line breaks. With `preserveQuotedStrings`, text
        inside single or double quotes is one token, quotes kept. Consecutive
        separators give empty tokens, as JUCE did. Returns the number added. */
    int addTokens (StringRef text, bool preserveQuotedStrings)
    {
        return addTokens (text, " \t\r\n", preserveQuotedStrings ? "\"'" : "");
    }

    int addTokens (StringRef text, StringRef breakCharacters, StringRef quoteCharacters)
    {
        if (text.isEmpty())
            return 0;

        const std::string s (text.text);
        const std::string breaks (breakCharacters.text);
        const std::string quotes (quoteCharacters.text);
        int num = 0;
        size_t tokenStart = 0;
        char openQuote = 0;

        for (size_t i = 0; i <= s.size(); ++i)
        {
            const char c = i < s.size() ? s[i] : 0;

            if (openQuote != 0)
            {
                if (c == openQuote)
                    openQuote = 0;
                else if (c == 0)
                    break;

                continue;
            }

            if (c != 0 && quotes.find (c) != std::string::npos)
            {
                openQuote = c;
                continue;
            }

            if (c == 0 || breaks.find (c) != std::string::npos)
            {
                strings.emplace_back (s.substr (tokenStart, i - tokenStart));
                ++num;
                tokenStart = i + 1;
            }
        }

        if (openQuote != 0)   // an unclosed quote runs to the end
        {
            strings.emplace_back (s.substr (tokenStart));
            ++num;
        }

        return num;
    }

    static StringArray fromTokens (StringRef stringToTokenise, bool preserveQuotedStrings)
    {
        StringArray s;
        s.addTokens (stringToTokenise, preserveQuotedStrings);
        return s;
    }

    //==============================================================================
    void remove (int index)
    {
        if (inRange (index))
            strings.erase (strings.begin() + index);
    }

    /** Moves the entry at `currentIndex` to `newIndex`, the others closing up:
        on { a, b, c, d } move (2, 0) gives { c, a, b, d }. An index that is not
        in the array does nothing, so a move of what indexOf did not find is
        harmless. */
    void move (int currentIndex, int newIndex) noexcept
    {
        if (! inRange (currentIndex) || currentIndex == newIndex)
            return;

        newIndex = std::clamp (newIndex, 0, size() - 1);
        auto first = strings.begin() + std::min (currentIndex, newIndex);
        auto last  = strings.begin() + std::max (currentIndex, newIndex) + 1;

        if (currentIndex < newIndex)
            std::rotate (first, first + 1, last);
        else
            std::rotate (first, last - 1, last);
    }

    void removeRange (int startIndex, int numberToRemove)
    {
        const auto from = std::clamp (startIndex, 0, size());
        const auto to = std::clamp (startIndex + numberToRemove, from, size());
        strings.erase (strings.begin() + from, strings.begin() + to);
    }

    void removeString (StringRef stringToRemove, bool ignoreCase = false)
    {
        for (int i = size(); --i >= 0;)
            if (ignoreCase ? strings[static_cast<size_t> (i)].equalsIgnoreCase (stringToRemove.text) : strings[static_cast<size_t> (i)] == stringToRemove)
                remove (i);
    }

    void removeEmptyStrings (bool removeWhitespaceStrings = true)
    {
        for (int i = size(); --i >= 0;)
        {
            const auto& s = strings[static_cast<size_t> (i)];

            if (removeWhitespaceStrings ? s.trim().isEmpty() : s.isEmpty())
                remove (i);
        }
    }

    void removeDuplicates (bool ignoreCase)
    {
        for (int i = 0; i < size() - 1; ++i)
        {
            const auto s = strings[static_cast<size_t> (i)];

            for (int nextIndex = indexOf (s, ignoreCase, i + 1); nextIndex >= 0; nextIndex = indexOf (s, ignoreCase, nextIndex))
                remove (nextIndex);
        }
    }

    /** Where several entries share a name, numbers them " (1)", " (2)", ... so
        each is distinct; with `appendNumberToFirstInstance` false the first
        keeps its bare name. Device drivers use this on channel and device lists. */
    void appendNumbersToDuplicates (bool ignoreCase,
                                    bool appendNumberToFirstInstance,
                                    const char* preNumberString = nullptr,
                                    const char* postNumberString = nullptr)
    {
        const String pre (preNumberString != nullptr ? preNumberString : " (");
        const String post (postNumberString != nullptr ? postNumberString : ")");

        for (int i = 0; i < size() - 1; ++i)
        {
            auto& s = strings[static_cast<size_t> (i)];
            auto nextIndex = indexOf (s, ignoreCase, i + 1);

            if (nextIndex < 0)
                continue;

            const auto original = s;
            int number = 0;

            if (appendNumberToFirstInstance)
                s = original + pre + String (++number) + post;
            else
                ++number;

            while (nextIndex >= 0)
            {
                set (nextIndex, (*this)[nextIndex] + pre + String (++number) + post);
                nextIndex = indexOf (original, ignoreCase, nextIndex + 1);
            }
        }
    }

    void trim()
    {
        for (auto& s : strings)
            s = s.trim();
    }

    void sort (bool ignoreCase)
    {
        std::stable_sort (strings.begin(), strings.end(), [ignoreCase] (const String& a, const String& b)
        {
            return ignoreCase ? a.compareIgnoreCase (b) < 0 : a < b;
        });
    }

    //==============================================================================
    int indexOf (StringRef stringToLookFor, bool ignoreCase = false, int startIndex = 0) const
    {
        for (int i = std::max (0, startIndex); i < size(); ++i)
        {
            const auto& s = strings[static_cast<size_t> (i)];

            if (ignoreCase ? s.equalsIgnoreCase (stringToLookFor.text) : s == stringToLookFor)
                return i;
        }

        return -1;
    }

    bool contains (StringRef stringToLookFor, bool ignoreCase = false) const   { return indexOf (stringToLookFor, ignoreCase) >= 0; }

    String joinIntoString (StringRef separator, int startIndex = 0, int numberOfElements = -1) const
    {
        const auto from = std::clamp (startIndex, 0, size());
        const auto to = numberOfElements < 0 ? size() : std::clamp (from + numberOfElements, from, size());
        String out;

        for (int i = from; i < to; ++i)
        {
            if (i > from)
                out += separator.text;

            out += strings[static_cast<size_t> (i)];
        }

        return out;
    }

    //==============================================================================
    bool operator== (const StringArray& other) const noexcept   { return strings == other.strings; }
    bool operator!= (const StringArray& other) const noexcept   { return strings != other.strings; }

private:
    bool inRange (int index) const noexcept     { return index >= 0 && index < size(); }
    static const String& empty() noexcept       { static const String s; return s; }

    std::vector<String> strings;
};

} // namespace juce
