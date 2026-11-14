// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//
// Seeds for fuzz_params: runs of frames as the target reads them (a byte for
// the verb and the connection, a length, the arguments), each a thing a client
// really does with the parameter table: declare, set, list, read text,
// redeclare, retire, and all of it again after it has come back on another
// connection.

#include "params/clockwork_params.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>

namespace {

enum : uint8_t { kDefine = 0, kList = 1, kSet = 2, kText = 3, kOther = 8 };   // kOther: the client's other connection

using Bytes = std::vector<uint8_t>;

void pad(Bytes& b) { while (b.size() % 4) b.push_back(0); }
void tags(Bytes& b, const char* t) { b.insert(b.end(), t, t + std::strlen(t) + 1); pad(b); }
void int32(Bytes& b, uint32_t v) { for (int s = 24; s >= 0; s -= 8) b.push_back(static_cast<uint8_t>(v >> s)); }
void float32(Bytes& b, float f) { uint32_t v; std::memcpy(&v, &f, 4); int32(b, v); }
void blob(Bytes& b, const Bytes& data) { int32(b, static_cast<uint32_t>(data.size())); b.insert(b.end(), data.begin(), data.end()); pad(b); }

ClockworkParamEntry entry(uint32_t id, const char* name, uint32_t kind, float min, float max, float def) {
    ClockworkParamEntry e{};
    e.id = id; e.kind = kind; e.min = min; e.max = max; e.def = def;
    std::snprintf(e.name, sizeof e.name, "%s", name);
    return e;
}

Bytes define(const std::vector<ClockworkParamEntry>& entries, const Bytes& text = {}) {
    Bytes records(entries.size() * sizeof(ClockworkParamEntry));
    std::memcpy(records.data(), entries.data(), records.size());
    Bytes b;
    tags(b, text.empty() ? ",b" : ",bb");
    blob(b, records);
    if (!text.empty()) blob(b, text);
    return b;
}
Bytes set(std::initializer_list<std::pair<uint32_t, float>> pairs) {
    Bytes b;
    std::string t = ",";
    for (size_t i = 0; i < pairs.size(); ++i) t += "if";
    tags(b, t.c_str());
    for (const auto& [id, v] : pairs) { int32(b, id); float32(b, v); }
    return b;
}
Bytes one(uint32_t v) { Bytes b; tags(b, ",i"); int32(b, v); return b; }
Bytes none() { Bytes b; tags(b, ","); return b; }

struct Run {
    Bytes bytes;
    Run& frame(uint8_t pick, const Bytes& body) {
        bytes.push_back(pick);
        bytes.push_back(static_cast<uint8_t>(body.size()));
        bytes.push_back(static_cast<uint8_t>(body.size() >> 8));
        bytes.insert(bytes.end(), body.begin(), body.end());
        return *this;
    }
    void save(const std::filesystem::path& dir, const char* name) const {
        std::ofstream(dir / name, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
};

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) { std::fprintf(stderr, "usage: params_seeds <directory>\n"); return 2; }   // stdio-ok: a tool's usage
    const std::filesystem::path dir = argv[1];
    std::filesystem::create_directories(dir);

    ClockworkParamEntry level = entry(1, "level", CLOCKWORK_PARAM_CONTINUOUS, 0.f, 1.f, 0.5f);
    level.flags = CLOCKWORK_PARAM_AUTOMATABLE;
    ClockworkParamEntry cutoff = entry(2, "cutoff", CLOCKWORK_PARAM_CONTINUOUS, 20.f, 20000.f, 1000.f);
    cutoff.unit = CLOCKWORK_PARAM_UNIT_HERTZ; cutoff.taper = CLOCKWORK_PARAM_TAPER_LOG; cutoff.group = 9;
    ClockworkParamEntry group = entry(9, "filter", CLOCKWORK_PARAM_GROUP, 0.f, 0.f, 0.f);
    ClockworkParamEntry held = entry(3, "held", CLOCKWORK_PARAM_SWITCH, 0.f, 1.f, 0.f);
    held.role = CLOCKWORK_PARAM_ROLE_KEY_HELD;

    // a choice, a custom unit and a long name, with their text
    Bytes text;
    const auto say = [&](uint8_t tag, const char* words) { text.push_back(tag); text.insert(text.end(), words, words + std::strlen(words) + 1); };
    ClockworkParamEntry mode = entry(4, "mode", CLOCKWORK_PARAM_CHOICE, 0.f, 2.f, 1.f);
    mode.text = static_cast<uint32_t>(text.size());
    say(CLOCKWORK_PARAM_TEXT_VALUE, "Idle"); say(CLOCKWORK_PARAM_TEXT_VALUE, "Run"); say(CLOCKWORK_PARAM_TEXT_VALUE, "Fault");
    mode.text_bytes = static_cast<uint32_t>(text.size()) - mode.text;
    ClockworkParamEntry temp = entry(5, "temp", CLOCKWORK_PARAM_CONTINUOUS, -40.f, 125.f, 20.f);
    temp.unit = CLOCKWORK_PARAM_UNIT_CUSTOM; temp.flags = CLOCKWORK_PARAM_READ_ONLY;
    temp.text = static_cast<uint32_t>(text.size());
    say(CLOCKWORK_PARAM_TEXT_FULL_NAME, "Coolant temperature"); say(CLOCKWORK_PARAM_TEXT_DOC, "How hot it runs"); say(CLOCKWORK_PARAM_TEXT_UNIT, "\xC2\xB0""C");
    temp.text_bytes = static_cast<uint32_t>(text.size()) - temp.text;

    Run().frame(kDefine, define({ level })).frame(kSet, set({ { 1, 0.25f } })).frame(kList, none()).save(dir, "declare-set-list");
    Run().frame(kDefine, define({ group, cutoff, held })).frame(kList, one(0)).frame(kSet, set({ { 2, 440.f }, { 3, 1.f } })).save(dir, "a-group-and-a-role");
    Run().frame(kDefine, define({ mode, temp }, text)).frame(kText, one(4)).frame(kText, one(5)).frame(kSet, set({ { 4, 2.f } })).save(dir, "text");
    // the same ids again with other facts, then gone, the slot taken again in the same define
    ClockworkParamEntry wider = level; wider.max = 2.f;
    ClockworkParamEntry gone{}; gone.id = 1;
    ClockworkParamEntry fresh = entry(6, "fresh", CLOCKWORK_PARAM_STEPPED, 0.f, 7.f, 3.f);
    Run().frame(kDefine, define({ level })).frame(kDefine, define({ wider })).frame(kDefine, define({ gone, fresh })).frame(kList, none()).save(dir, "redeclare-and-retire");
    // the client, back on another connection: its entries are still its own
    Run().frame(kDefine, define({ level, temp }, text)).frame(kSet | kOther, set({ { 1, 0.75f } }))
         .frame(kDefine | kOther, define({ wider })).frame(kDefine | kOther, define({ gone })).frame(kSet | kOther, set({ { 5, 1.f } })).save(dir, "another-connection");
    // text that fills its room, so a later define has to close the gaps
    Bytes big;
    ClockworkParamEntry wordy = entry(7, "wordy", CLOCKWORK_PARAM_CONTINUOUS, 0.f, 1.f, 0.f);
    big.push_back(CLOCKWORK_PARAM_TEXT_DOC); big.insert(big.end(), 400, 'x'); big.push_back(0);
    wordy.text_bytes = static_cast<uint32_t>(big.size());
    Run run;
    for (uint32_t id = 20; id < 28; ++id) { ClockworkParamEntry e = wordy; e.id = id; run.frame(kDefine, define({ e }, big)); }
    for (uint32_t id = 20; id < 28; id += 2) { ClockworkParamEntry e{}; e.id = id; run.frame(kDefine, define({ e })); }
    run.frame(kDefine, define({ wordy }, big)).frame(kText, one(7)).save(dir, "text-churn");
    return 0;
}
