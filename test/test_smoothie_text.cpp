// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_smoothie_text.cpp — juce::String and juce::StringArray, Smoothie's
 * wrappers over std::string and std::vector (smoothie/modules/juce_core/text).
 *
 * The device drivers read device and channel names through these and
 * compare, trim, number and join them. Each case pins a behaviour a driver
 * relies on, so a driver that only compiles on another platform is covered
 * here for the text it handles.
 */
#include <catch2/catch_test_macros.hpp>
#include "smoothie_juce_drivers.h"
#include <string>
#include <vector>

using juce::String;
using juce::StringArray;

TEST_CASE("String: numbers print as JUCE printed them", "[smoothie][text]") {
    CHECK(String(48000) == "48000");
    CHECK(String(-7) == "-7");
    CHECK(String(1.0) == "1");
    CHECK(String(3.5) == "3.5");
    CHECK(String(44100.0) == "44100");
    CHECK(String(0.1f) == "0.1");                          // a float prints as a double does
    CHECK(String(1.0 / 3.0) == "0.333333");                // six significant digits, as JUCE streamed it
    CHECK(String(48000.5) == "48000.5");
    CHECK(String(1e-7) == "1e-07");
    CHECK(String::toHexString(255) == "ff");
    CHECK(String::toHexString(static_cast<long long>(0x1234abcd)) == "1234abcd");
    CHECK(String::toHexString(-1) == "ffffffff");
}

TEST_CASE("String: building a line with <<", "[smoothie][text]") {
    String s;
    s << "rate=" << 48000 << " buf=" << 128 << ' ' << 1.5 << " " << std::string("x") << String("y");
    CHECK(s == "rate=48000 buf=128 1.5 xy");
    CHECK(("a" + String("b") + 'c' + 1 + std::string("d")) == "abc1d");
}

TEST_CASE("String: searching and comparing", "[smoothie][text]") {
    const String s("Built-in Output");
    CHECK(s.length() == 15);
    CHECK(s.indexOf("in") == 6);
    CHECK(s.indexOf(7, "in") == -1);
    CHECK(s.lastIndexOf("t") == 14);
    CHECK(s.indexOfChar('-') == 5);
    CHECK(s.contains("Output"));
    CHECK_FALSE(s.contains("output"));
    CHECK(s.containsIgnoreCase("output"));
    CHECK(s.containsChar('O'));
    CHECK(s.containsAnyOf("xyz-"));
    CHECK(s.startsWith("Built"));
    CHECK(s.endsWith("Output"));
    CHECK(s.equalsIgnoreCase("built-in output"));
    CHECK(s.equalsIgnoreCase(L"BUILT-IN OUTPUT"));
    CHECK(s.compareIgnoreCase(String("built-in output")) == 0);
    CHECK(String("abc") < String("abd"));
    CHECK(String("x") == std::string("x"));
    CHECK(String("x") != "y");
    CHECK(String().isEmpty());
    CHECK(String("x").isNotEmpty());
    CHECK(String(static_cast<const char*>(nullptr)).isEmpty());
}

TEST_CASE("String: taking pieces", "[smoothie][text]") {
    const String s("  Scarlett 2i2 USB  ");
    CHECK(s.trim() == "Scarlett 2i2 USB");
    CHECK(s.trimStart() == "Scarlett 2i2 USB  ");
    CHECK(s.trimEnd() == "  Scarlett 2i2 USB");
    CHECK(String("   ").trim().isEmpty());
    CHECK(String("Mix").toUpperCase() == "MIX");
    CHECK(String("Mix").toLowerCase() == "mix");
    CHECK(String("abcdef").substring(2, 4) == "cd");
    CHECK(String("abcdef").substring(4) == "ef");
    CHECK(String("abcdef").substring(4, 100) == "ef");
    CHECK(String("abcdef").substring(-3, 2) == "ab");
    CHECK(String("abcdef").substring(5, 2).isEmpty());
    CHECK(String("abcdef").dropLastCharacters(2) == "abcd");
    CHECK(String("abcdef").getLastCharacters(2) == "ef");
    CHECK(String("hw:0,0").fromFirstOccurrenceOf(":", false, false) == "0,0");
    CHECK(String("hw:0,0").fromFirstOccurrenceOf(":", true, false) == ":0,0");
    CHECK(String("hw:0,0").fromFirstOccurrenceOf("#", false, false).isEmpty());
    CHECK(String("hw:0,0").upToFirstOccurrenceOf(",", false, false) == "hw:0");
    CHECK(String("hw:0,0").upToFirstOccurrenceOf(",", true, false) == "hw:0,");
    CHECK(String("hw:0,0").upToFirstOccurrenceOf("#", false, false) == "hw:0,0");
    CHECK(String("a/b/c").fromLastOccurrenceOf("/", false, false) == "c");
    CHECK(String("a/b/c").upToLastOccurrenceOf("/", false, false) == "a/b");
    CHECK(String("A:B").fromFirstOccurrenceOf("a:", false, true) == "B");
    CHECK(String("a-b-c").replace("-", "+") == "a+b+c");
    CHECK(String("a-b-c").replace("", "+") == "a-b-c");
    CHECK(String("Ab").replace("A", "x", true) == "xb");
    CHECK(String("a-b_c").removeCharacters("-_") == "abc");
    CHECK(String("a-b").replaceCharacter('-', ' ') == "a b");
    CHECK(String("\"quoted\"").unquoted() == "quoted");
    CHECK(String("'q'").unquoted() == "q");
    CHECK(String("plain").unquoted() == "plain");
    CHECK(String("q").quoted() == "\"q\"");
    CHECK(String("7").paddedLeft('0', 3) == "007");
    CHECK(String("1234").paddedLeft('0', 3) == "1234");
    CHECK(String("Built-in Output")[0] == 'B');
    CHECK(String("x")[5] == 0);
    CHECK(String("abc").getLastCharacter() == 'c');
}

TEST_CASE("String: numbers read from text", "[smoothie][text]") {
    CHECK(String("48000").getIntValue() == 48000);
    CHECK(String("-12 Hz").getIntValue() == -12);
    CHECK(String("x").getIntValue() == 0);
    CHECK(String("3.25").getDoubleValue() == 3.25);
    CHECK(String("3.25").getFloatValue() == 3.25f);
    CHECK(String("Out 12").getTrailingIntValue() == 12);
    CHECK(String("Out").getTrailingIntValue() == 0);
    CHECK(String("ff").getHexValue32() == 255);
    CHECK(String("9000000000").getLargeIntValue() == 9000000000LL);
}

TEST_CASE("String: UTF-8 in and out, wide characters on the way to the OS", "[smoothie][text]") {
    const String s("Lautsprecher \xc3\x9c\xc3\xb1 \xf0\x9f\x94\x8a");   // Ü ñ and a speaker emoji
    CHECK(std::string(s.toRawUTF8()) == s.toStdString());
    CHECK(std::string(s.toUTF8()) == s.toStdString());
    CHECK(String(s.toWideCharPointer()) == s);              // round trip through UTF-16 or UTF-32
    CHECK(String(L"Ü") == "\xc3\x9c");
    CHECK(String::fromUTF8("abc", 2) == "ab");
    CHECK(String::fromUTF8("abc") == "abc");
    CHECK(String::fromUTF8(nullptr).isEmpty());
    CHECK(String("abcdef", 3) == "abc");
    CHECK(String("ab", 3) == "ab");
    CHECK(String(juce::CharPointer_UTF8("via pointer")) == "via pointer");
    CHECK(juce::CharPointer_UTF8::isValidString("plain", 5));
    CHECK(juce::CharPointer_UTF8::isValidString("\xc3\x9c", 2));
    CHECK_FALSE(juce::CharPointer_UTF8::isValidString("\xc3", 1));         // a lead byte with no continuation
    CHECK_FALSE(juce::CharPointer_UTF8::isValidString("\xff", 1));         // never a UTF-8 byte
    CHECK(juce::CharPointer_UTF8::isValidString("ok\xff", 2));             // not looked at past the limit
    CHECK(s.hashCode() == String(s).hashCode());
}

TEST_CASE("StringArray: adding, finding and joining names", "[smoothie][text]") {
    StringArray names;
    CHECK(names.isEmpty());
    names.add("Out 1");
    names.add(String("Out 2"));
    names.insert(0, "Out 0");
    CHECK(names.size() == 3);
    CHECK(names[0] == "Out 0");
    CHECK(names[7].isEmpty());
    CHECK(names.indexOf("Out 2") == 2);
    CHECK(names.indexOf("out 2") == -1);
    CHECK(names.indexOf("out 2", true) == 2);
    CHECK(names.indexOf("Out 0", false, 1) == -1);
    CHECK(names.contains("Out 1"));
    CHECK_FALSE(names.addIfNotAlreadyThere("Out 1"));
    CHECK(names.addIfNotAlreadyThere("Out 3"));
    CHECK(names.joinIntoString(", ") == "Out 0, Out 1, Out 2, Out 3");
    CHECK(names.joinIntoString("/", 1, 2) == "Out 1/Out 2");
    names.set(1, "Out one");
    CHECK(names[1] == "Out one");
    names.getReference(1) = "Out 1";
    names.remove(0);
    CHECK(names.joinIntoString(" ") == "Out 1 Out 2 Out 3");
    names.removeString("Out 2");
    CHECK(names.size() == 2);
    int seen = 0;
    for (const auto& n : names) seen += n.length();
    CHECK(seen == 10);
    const StringArray copy(names);
    CHECK(copy == names);
    names.clear();
    CHECK(names.isEmpty());
    CHECK(copy != names);
    const StringArray one(String("only"));
    CHECK(one.size() == 1);
    const StringArray fromList { "a", "b" };
    CHECK(fromList.joinIntoString("") == "ab");
    juce::Array<String> arr;
    arr.add("x");
    arr.add("y");
    const StringArray fromArray(arr);
    CHECK(fromArray.joinIntoString("") == "xy");
}

TEST_CASE("StringArray: move puts a found entry first and ignores one not found", "[smoothie][text]") {
    StringArray ids { "hw:0", "pulse", "default", "hw:1" };

    // What the ALSA driver does: the default and pulse devices to the front.
    ids.move(ids.indexOf("pulse"), 0);
    CHECK(ids.joinIntoString(",") == "pulse,hw:0,default,hw:1");
    ids.move(ids.indexOf("default"), 0);
    CHECK(ids.joinIntoString(",") == "default,pulse,hw:0,hw:1");

    ids.move(ids.indexOf("not there"), 0);                 // -1: nothing happens
    CHECK(ids.joinIntoString(",") == "default,pulse,hw:0,hw:1");
    ids.move(1, 1);
    CHECK(ids.joinIntoString(",") == "default,pulse,hw:0,hw:1");
    ids.move(0, 3);                                        // forwards, the others close up
    CHECK(ids.joinIntoString(",") == "pulse,hw:0,hw:1,default");
    ids.move(0, 99);                                       // past the end: to the end
    CHECK(ids.joinIntoString(",") == "hw:0,hw:1,default,pulse");
}

TEST_CASE("StringArray: duplicate names are numbered so each is distinct", "[smoothie][text]") {
    StringArray channels { "Out", "Out", "In", "Out" };
    channels.appendNumbersToDuplicates(false, true);
    CHECK(channels.joinIntoString(",") == "Out (1),Out (2),In,Out (3)");

    StringArray devices { "Speakers", "speakers", "Mic" };
    devices.appendNumbersToDuplicates(false, false);       // case differs: not duplicates
    CHECK(devices.joinIntoString(",") == "Speakers,speakers,Mic");
    devices.appendNumbersToDuplicates(true, false);        // first keeps its bare name
    CHECK(devices.joinIntoString(",") == "Speakers,speakers (2),Mic");

    StringArray dup { "A", "A" };
    dup.removeDuplicates(false);
    CHECK(dup.size() == 1);
}

TEST_CASE("StringArray: tokens, trimming and sorting", "[smoothie][text]") {
    const StringArray tokens = StringArray::fromTokens("L R  \"Side Left\" C", true);
    CHECK(tokens.size() == 5);                             // the double space gives an empty token, as JUCE did
    CHECK(tokens[0] == "L");
    CHECK(tokens[2].isEmpty());
    CHECK(tokens[3] == "\"Side Left\"");
    CHECK(tokens[4] == "C");

    const StringArray bare = StringArray::fromTokens("\"Side Left\" C", false);
    CHECK(bare.size() == 3);
    CHECK(bare[0] == "\"Side");

    StringArray lines;
    CHECK(lines.addTokens("a:b::c", ":", "") == 4);
    lines.removeEmptyStrings();
    CHECK(lines.joinIntoString("") == "abc");
    lines.clear();
    lines.addTokens("  x , y ", ",", "");
    lines.trim();
    CHECK(lines.joinIntoString("|") == "x|y");

    StringArray sorted { "b", "A", "c" };
    sorted.sort(false);
    CHECK(sorted.joinIntoString("") == "Abc");
    sorted.sort(true);
    CHECK(sorted.joinIntoString("") == "Abc");
    StringArray mixed { "b", "C", "a" };
    mixed.sort(true);
    CHECK(mixed.joinIntoString("") == "abC");
}
