#include "parsers.hpp"
#include "cicrypt.hpp"
#include "gdr_parse.hpp"

#include <Geode/Geode.hpp>
#include <matjson.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <exception>

using namespace geode::prelude;
namespace fs = std::filesystem;

bool g_slcTrimmed = false;
std::vector<MacroPos> g_macroPath;
std::string g_macroPathSrc;
std::vector<unsigned char> g_vaultKey;
bool g_vaultStrict = false;

struct RawInput { uint64_t frame = 0; bool down = false; bool player2 = false; float x = -1.f; };

struct MsgReader {
    const uint8_t* p; const uint8_t* end; bool ok = true;
    uint8_t byte() { if (p >= end) { ok = false; return 0; } return *p++; }
    void adv(size_t n) { if (p + n > end) { ok = false; p = end; } else p += n; }
    uint64_t be(int n) { uint64_t v = 0; for (int i = 0; i < n; i++) v = (v << 8) | byte(); return v; }
    int64_t readInt() {
        uint8_t c = byte();
        if (c < 0x80) return c;
        if (c >= 0xe0) return (int8_t)c;
        switch (c) {
            case 0xcc: return (uint8_t)be(1);  case 0xcd: return (uint16_t)be(2);
            case 0xce: return (uint32_t)be(4); case 0xcf: return (int64_t)be(8);
            case 0xd0: return (int8_t)be(1);   case 0xd1: return (int16_t)be(2);
            case 0xd2: return (int32_t)be(4);  case 0xd3: return (int64_t)be(8);
            default: ok = false; return 0;
        }
    }
    double readFloat() {
        uint8_t c = byte();
        if (c == 0xca) { uint32_t b = (uint32_t)be(4); float f; std::memcpy(&f, &b, 4); return f; }
        if (c == 0xcb) { uint64_t b = be(8); double d; std::memcpy(&d, &b, 8); return d; }
        if (!ok) return 240.0;   // byte() hit the end and never advanced p, so do not walk back
        p--;
        // A writer that encodes the framerate as nil, a bool or a string would otherwise trip
        // readInt's default case, clear ok, and abandon the entire macro over one byte that is
        // not even needed - 240 is the right assumption and the inputs are still there.
        if (c == 0xc0 || c == 0xc2 || c == 0xc3 || (c >= 0xa0 && c <= 0xbf)
            || c == 0xd9 || c == 0xda || c == 0xdb) { skip(); return 240.0; }
        return (double)readInt();
    }
    bool readBool() { return byte() == 0xc3; }
    std::string readStr() {
        uint8_t c = byte(); size_t len = 0;
        if (c >= 0xa0 && c <= 0xbf) len = c & 0x1f;
        else if (c == 0xd9) len = (size_t)be(1);
        else if (c == 0xda) len = (size_t)be(2);
        else if (c == 0xdb) len = (size_t)be(4);
        else { ok = false; return ""; }
        // The length comes out of the file. reserve() on it throws bad_alloc straight through a GD
        // hook and nothing on this path catches, so it is bounded by the bytes that really exist.
        if (len > (size_t)(end - p)) { ok = false; return ""; }
        std::string s; s.reserve(len);
        for (size_t i = 0; i < len && ok; i++) s += (char)byte();
        return s;
    }
    size_t readMap() {
        uint8_t c = byte();
        if (c >= 0x80 && c <= 0x8f) return c & 0x0f;
        if (c == 0xde) return (size_t)be(2);
        if (c == 0xdf) return (size_t)be(4);
        ok = false; return 0;
    }
    size_t readArray() {
        uint8_t c = byte();
        if (c >= 0x90 && c <= 0x9f) return c & 0x0f;
        if (c == 0xdc) return (size_t)be(2);
        if (c == 0xdd) return (size_t)be(4);
        ok = false; return 0;
    }
    // Depth is explicit because this runs on downloaded files: a file that is nothing but nested
    // arrays recurses once per level, and a stack overflow on Windows cannot be caught by any
    // try/catch. Real replay files are two levels deep.
    void skip(int depth = 0) {
        if (!ok) return;
        if (depth > 32) { ok = false; return; }
        uint8_t c = byte();
        if (c < 0x80 || c >= 0xe0) return;
        if (c >= 0xa0 && c <= 0xbf) { adv(c & 0x1f); return; }
        if (c >= 0x80 && c <= 0x8f) { size_t n = c & 0x0f; for (size_t i = 0; i < 2 * n && ok; i++) skip(depth + 1); return; }
        if (c >= 0x90 && c <= 0x9f) { size_t n = c & 0x0f; for (size_t i = 0; i < n && ok; i++) skip(depth + 1); return; }
        switch (c) {
            case 0xc0: case 0xc2: case 0xc3: return;
            case 0xcc: case 0xd0: adv(1); return;
            case 0xcd: case 0xd1: adv(2); return;
            case 0xce: case 0xd2: case 0xca: adv(4); return;
            case 0xcf: case 0xd3: case 0xcb: adv(8); return;
            case 0xd9: adv((size_t)be(1)); return;  case 0xda: adv((size_t)be(2)); return;  case 0xdb: adv((size_t)be(4)); return;
            case 0xc4: adv((size_t)be(1)); return;  case 0xc5: adv((size_t)be(2)); return;  case 0xc6: adv((size_t)be(4)); return;
            case 0xdc: { size_t n = (size_t)be(2); for (size_t i = 0; i < n && ok; i++) skip(depth + 1); return; }
            case 0xdd: { size_t n = (size_t)be(4); for (size_t i = 0; i < n && ok; i++) skip(depth + 1); return; }
            case 0xde: { size_t n = (size_t)be(2); for (size_t i = 0; i < 2 * n && ok; i++) skip(depth + 1); return; }
            case 0xdf: { size_t n = (size_t)be(4); for (size_t i = 0; i < 2 * n && ok; i++) skip(depth + 1); return; }
            case 0xd4: adv(2); return; case 0xd5: adv(3); return; case 0xd6: adv(5); return;
            case 0xd7: adv(9); return; case 0xd8: adv(17); return;
            case 0xc7: adv(1 + (size_t)be(1)); return;
            case 0xc8: adv(1 + (size_t)be(2)); return;
            case 0xc9: adv(1 + (size_t)be(4)); return;
            default: ok = false; return;
        }
    }
};

double g_macroFps = 240.0;

static void pairInto(std::vector<RawInput>& raws, double fps, std::vector<Action>& out) {
    // A macro in which EVERY input belongs to player two is not a dual, because a dual has both.
    // It is a single-player recording from a writer that sets the flag the other way round:
    // iCreate Pro 1.0 marks all 1,176 inputs of an ordinary single-player run "2p": true, where
    // xdBot writes the same run on the same level as false throughout. Left alone the whole guide
    // ends up hung off a player that is not the one being watched. Only fires when there is not a
    // single player-one input, so a real dual is untouched.
    bool anyP1 = false;
    for (auto const& in : raws) if (!in.player2) { anyP1 = true; break; }
    if (!anyP1 && !raws.empty()) {
        log::info("[Click Indicators] every input in this macro is flagged player two, which no "
                  "dual can be - reading it as a single-player recording");
        for (auto& in : raws) in.player2 = false;
    }
    std::sort(raws.begin(), raws.end(), [](const RawInput& a, const RawInput& b) { return a.frame < b.frame; });
    out.clear();
    bool down[2] = { false, false }; double press[2] = { 0.0, 0.0 }; float px[2] = { -1.f, -1.f };
    long long pressF[2] = { -1, -1 };
    for (auto const& in : raws) {
        int p = in.player2 ? 1 : 0;
        double tt = static_cast<double>(in.frame) / (fps > 0 ? fps : 240.0);
        if (in.down) {
            if (!down[p]) { down[p] = true; press[p] = tt; px[p] = in.x; pressF[p] = (long long)in.frame; }
        }
        else if (down[p]) {
            down[p] = false;
            Action a{}; a.pressTime = press[p]; a.px = px[p]; a.releaseTime = tt; a.p2 = (p == 1);
            // The frames exactly as recorded, so a replay never has to guess them back.
            a.pressFrame = pressF[p]; a.releaseFrame = (long long)in.frame;
            out.push_back(a);
        }
    }
    for (int p = 0; p < 2; p++) if (down[p]) {
        Action a{}; a.pressTime = press[p]; a.px = px[p]; a.releaseTime = press[p]; a.p2 = (p == 1);
        a.pressFrame = pressF[p]; a.releaseFrame = pressF[p];
        out.push_back(a);
    }
    std::sort(out.begin(), out.end(), [](const Action& a, const Action& b) { return a.pressTime < b.pressTime; });
}

static bool parseMsgpack(const std::vector<uint8_t>& data, std::vector<Action>& out, double& fpsOut) {
    MsgReader r{ data.data(), data.data() + data.size() };
    double fps = 240.0; std::vector<RawInput> raws;
    size_t n = r.readMap();
    for (size_t i = 0; i < n && r.ok; i++) {
        std::string key = r.readStr();
        if (key == "framerate" || key == "fps") fps = r.readFloat();
        else if (key == "frameFixes" || key == "framefixes") {
            // [ { frame, p1:{x,y,...}, p2:{...} }, ... ] - the run's real position, per frame.
            size_t cnt = r.readArray();
            if (cnt > (size_t)(r.end - r.p)) return false;
            for (size_t j = 0; j < cnt && r.ok; j++) {
                size_t fields = r.readMap();
                float fx = -1e9f, fy = -1e9f;
                for (size_t k = 0; k < fields && r.ok; k++) {
                    std::string fk = r.readStr();
                    if (fk == "p1") {
                        size_t pf = r.readMap();
                        for (size_t q = 0; q < pf && r.ok; q++) {
                            std::string pk = r.readStr();
                            if (pk == "x") fx = (float)r.readFloat();
                            else if (pk == "y") fy = (float)r.readFloat();
                            else r.skip();
                        }
                    } else r.skip();
                }
                if (fx > -1e8f && fy > -1e8f) g_macroPath.push_back({ fx, fy });
            }
        }
        else if (key == "inputs") {
            size_t cnt = r.readArray();
            // An input record is at least a byte, so a count bigger than the bytes remaining is a
            // lie - and reserving on it is a multi-gigabyte allocation from a 13-byte file.
            if (cnt > (size_t)(r.end - r.p)) return false;
            raws.reserve(cnt);
            for (size_t j = 0; j < cnt && r.ok; j++) {
                size_t fields = r.readMap();
                uint64_t frame = 0; bool p2 = false, dn = false; int64_t btn = 1;
                for (size_t k = 0; k < fields && r.ok; k++) {
                    std::string fk = r.readStr();
                    if (fk == "frame") frame = (uint64_t)r.readInt();
                    else if (fk == "2p") p2 = r.readBool();
                    else if (fk == "down") dn = r.readBool();
                    else if (fk == "btn") {
                        // readInt is weaker than skip - it fails on anything that is not an
                        // integer - so keep skip as the fallback rather than losing the file.
                        const uint8_t* save = r.p;
                        int64_t b = r.readInt();
                        if (!r.ok) { r.ok = true; r.p = save; r.skip(); }
                        else btn = b;
                    }
                    else r.skip();
                }
                // 1 = jump, 2 = left, 3 = right. Only the jump is a click; a platformer's
                // steering held down while jumping was closing the jump early, because the
                // pairing keeps one hold flag per player and could not tell them apart.
                if (btn <= 1) raws.push_back({ frame, dn, p2 });
            }
        } else r.skip();
    }
    if (raws.empty()) return false;
    // A framerate of exactly 0 makes parseMacroFile return 0, and the caller reads that as
    // "could not parse" and drops a macro whose times were already computed correctly.
    if (!(fps > 1.0 && fps < 2000.0)) fps = 240.0;
    fpsOut = fps; pairInto(raws, fps, out); return true;
}

static bool parseGdrJsonValue(matjson::Value const& j, std::vector<Action>& out, double& fpsOut) {
    double fps = 240.0;
    if (j.contains("framerate")) fps = j["framerate"].asDouble().unwrapOr(240.0);
    else if (j.contains("fps")) fps = j["fps"].asDouble().unwrapOr(240.0);
    if (!j.contains("inputs")) return false;
    auto arrRes = j["inputs"].asArray();
    if (arrRes.isErr()) return false;
    auto const& arr = arrRes.unwrap();
    std::vector<RawInput> raws; raws.reserve(arr.size());
    for (auto const& in : arr) {
        uint64_t frame = (uint64_t)in["frame"].asInt().unwrapOr(0);
        bool down = in["down"].asBool().unwrapOr(false);
        bool p2 = in.contains("2p") ? in["2p"].asBool().unwrapOr(false) : false;
        // btn 2/3 are platformer left/right, same as the binary GDR path and parseMsgpack. Without
        // this a platformer macro draws a cue every time the player steps sideways.
        int btn = in.contains("btn") ? (int)in["btn"].asInt().unwrapOr(1) : 1;
        if (btn <= 1) raws.push_back({ frame, down, p2 });
    }
    if (raws.empty()) return false;
    // Same ceiling the .slc reader uses. 2000 was low enough to silently rewrite a legitimate
    // high-tickrate macro to 240, which corrupts every timestamp in the file rather than failing.
    if (!(fps > 1.0 && fps < 100000.0)) fps = 240.0;
    fpsOut = fps; pairInto(raws, fps, out); return true;
}

static bool parseZbf(const std::vector<uint8_t>& data, std::vector<Action>& out, double& fpsOut) {
    if (data.size() < 8) return false;
    float delta; std::memcpy(&delta, data.data(), 4);
    double fps = (delta > 1e-7f) ? std::round(1.0 / delta) : 240.0;
    // Byte 6 is the player. The old test for '2' matched nothing - it is '0' or '1' across all
    // 24,343 records in the three real .zbf files here - so both players were merged into one
    // stream, which on ton618lustre.zbf swallowed 3,400 presses.
    //
    // Which value means player two is NOT guessed, because guessing it wrong is worse than the
    // bug: it would relabel the main stream of every zBot macro, and a version of that mistake
    // has already shipped in this mod once. It is read off the file instead. Player one is
    // present for the whole level; player two only exists inside dual sections, so the value
    // spanning the widest range of frames is player one and anything else is player two. On the
    // three real files that gives 100% coverage against 52%, 90% and 22% - the last being Nine
    // Circles' dual, which is exactly where its second stream lives. This also settles the
    // ASCII-versus-raw-bool question without needing to know which encoding a writer used.
    uint8_t widest = 0; int64_t widestSpan = -1;
    for (int v = 0; v < 256; v++) {
        int64_t lo = -1, hi = -1; size_t seen = 0;
        for (size_t o = 8; o + 6 <= data.size(); o += 6) {
            if (data[o + 5] != (uint8_t)v) continue;
            int32_t fr; std::memcpy(&fr, data.data() + o, 4);
            if (fr < 0) fr = 0;
            if (lo < 0 || fr < lo) lo = fr;
            if (fr > hi) hi = fr;
            seen++;
        }
        if (!seen) continue;
        int64_t span = hi - lo;
        if (span > widestSpan) { widestSpan = span; widest = (uint8_t)v; }
    }
    std::vector<RawInput> raws;
    for (size_t off = 8; off + 6 <= data.size(); off += 6) {
        int32_t frame; std::memcpy(&frame, data.data() + off, 4);
        uint8_t b5 = data[off + 4], b6 = data[off + 5];
        bool down = (b5 != '0' && b5 != 0);
        bool p2 = (b6 != widest);
        raws.push_back({ (uint64_t)(frame < 0 ? 0 : frame), down, p2 });
    }
    if (raws.empty()) return false;
    fpsOut = fps; pairInto(raws, fps, out); return true;
}

// .mhr / "HACKPRO". fps u32@0x0c, dataStart u32@0x18, count u32@0x1c
// records 32b: u16 type (2=input), u8 hold, u8 player2, i32 frame, then floats.
// The hold field is ONE byte and the next one is the player. Read as a u16 it takes four values
// on a dual - 0, 1, 256, 257 - and "hold == 1" is then false for every player-2 press, so every
// one of them was read as a release. Measured on TON 618.gdr: 17,565 of 35,918 records.
static bool parseMhr(const std::vector<uint8_t>& data, std::vector<Action>& out, double& fpsOut) {
    if (data.size() < 0x20 || std::memcmp(data.data(), "HACKPRO", 7) != 0) return false;
    auto u32 = [&](size_t o) { uint32_t v; std::memcpy(&v, data.data() + o, 4); return v; };
    // "TON 618.gdr" is a HACKPRO file wearing a .gdr name and declares 3500 - clamping it to
    // 240 stretched a 70 second run over 1027 seconds.
    uint32_t fps = u32(0x0c);
    if (fps == 0 || fps > 12000) {
        log::warn("[Click Indicators] .mhr declares {} ticks/sec, out of range - assuming 240", fps);
        fps = 240;
    }
    uint32_t dataStart = u32(0x18); uint32_t count = u32(0x1c);
    if (dataStart < 0x20 || dataStart > data.size()) dataStart = 0x20;
    std::vector<RawInput> raws;
    size_t off = dataStart;
    for (uint32_t i = 0; i < count && off + 32 <= data.size(); i++, off += 32) {
        uint16_t typ; int32_t frame;
        std::memcpy(&typ, data.data() + off, 2);
        uint8_t hold = data[off + 2], plr = data[off + 3];   // in bounds: off + 32 <= size above
        std::memcpy(&frame, data.data() + off + 4, 4);
        if (typ != 2) continue; // 2 = input; 3 = position/state snapshot
        raws.push_back({ (uint64_t)(frame < 0 ? 0 : frame), hold == 1, plr == 1 });
    }
    // A .mhr holds the whole recording session, restarts included, and the frame counter goes
    // backwards at each one. Everything before the last restart belongs to an attempt that
    // ended, so merging it in scatters phantom clicks over the opening of the level - which is
    // exactly where a player is most likely to be practising. Keep only the final attempt, the
    // same way the .slc reader does.
    size_t keepFrom = 0;
    for (size_t i = 1; i < raws.size(); i++)
        if (raws[i].frame < raws[i - 1].frame) keepFrom = i;
    if (keepFrom > 0) raws.erase(raws.begin(), raws.begin() + (ptrdiff_t)keepFrom);
    if (raws.empty()) return false;
    fpsOut = (double)fps; pairInto(raws, (double)fps, out); return true;
}

static inline uint16_t rdU16(const std::vector<uint8_t>& d, size_t o) { uint16_t v; std::memcpy(&v, d.data() + o, 2); return v; }
static inline uint32_t rdU32(const std::vector<uint8_t>& d, size_t o) { uint32_t v; std::memcpy(&v, d.data() + o, 4); return v; }
static inline float    rdF32(const std::vector<uint8_t>& d, size_t o) { float    v; std::memcpy(&v, d.data() + o, 4); return v; }

// .replay / "RPLY": u8 version, [v2: u8 frameFlag], f32 fps, then 5-byte records
// { u32 frame, u8 state } - state bit0 = hold, bit1 = p2
static bool parseReplayBot(const std::vector<uint8_t>& data, std::vector<Action>& out, double& fpsOut) {
    if (data.size() < 10 || std::memcmp(data.data(), "RPLY", 4) != 0) return false;
    size_t off = 4;
    uint8_t version = data[off++];
    bool frameBased = true;
    if (version >= 2) { if (off >= data.size()) return false; frameBased = (data[off++] != 0); }
    if (off + 4 > data.size()) return false;
    double fps = (double)rdF32(data, off); off += 4;
    if (!(fps > 1.0 && fps < 2000.0)) fps = 240.0;
    if (!frameBased) return false;   // xpos replays can't be timed
    std::vector<RawInput> raws;
    for (; off + 5 <= data.size(); off += 5) {
        uint8_t st = data[off + 4];
        raws.push_back({ (uint64_t)rdU32(data, off), (st & 1) != 0, (st & 2) != 0 });
    }
    if (raws.empty()) return false;
    fpsOut = fps; pairInto(raws, fps, out); return true;
}

// .ddhor / "DDHR": u16 fps, u32 p1 count, u32 p2 count, then 5-byte { f32 frame, u8 hold }
// hold is inverted (0 = held). first p1-count records are p1, rest are p2
static bool parseDdhor(const std::vector<uint8_t>& data, std::vector<Action>& out, double& fpsOut) {
    if (data.size() < 14 || std::memcmp(data.data(), "DDHR", 4) != 0) return false;
    double fps = (double)rdU16(data, 4);
    if (!(fps > 1.0 && fps < 2000.0)) fps = 240.0;
    uint32_t n1 = rdU32(data, 6), n2 = rdU32(data, 10);
    size_t off = 14;
    uint64_t total = (uint64_t)n1 + (uint64_t)n2;
    if (total == 0 || total > 5000000ull) return false;
    std::vector<RawInput> raws;
    for (uint64_t i = 0; i < total && off + 5 <= data.size(); i++, off += 5) {
        float fr = rdF32(data, off);
        if (!std::isfinite(fr) || fr < 0.f) fr = 0.f;
        raws.push_back({ (uint64_t)llround((double)fr), data[off + 4] == 0, i >= n1 });
    }
    if (raws.empty()) return false;
    fpsOut = fps; pairInto(raws, fps, out); return true;
}

// xBot text - "fps: N", mode line ("frames" or "pro_plus"), then "<state> <frame>" per line
// state: bit0 = hold, bit1 = p2
static bool parseXbotText(const std::vector<uint8_t>& data, std::vector<Action>& out, double& fpsOut) {
    std::string s(reinterpret_cast<const char*>(data.data()), data.size());
    std::istringstream in(s);
    std::string line;
    double fps = 0.0;
    bool frameBased = true, sawMode = false;
    std::vector<RawInput> raws;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty()) continue;
        if (fps <= 0.0) {
            size_t c = line.find(':');
            if (c != std::string::npos && line.compare(0, 3, "fps") == 0) {
                fps = std::atof(line.c_str() + c + 1);
                continue;
            }
            return false;   // xBot always starts with fps
        }
        if (!sawMode && (line == "frames" || line == "pro_plus")) {
            frameBased = (line == "frames"); sawMode = true; continue;
        }
        long st = 0, val = 0; float px = -1.f;
        int got = std::sscanf(line.c_str(), "%ld %ld %f", &st, &val, &px);
        if (got < 2) continue;
        if (!frameBased) return false;   // xpos mode has no usable timing
        raws.push_back({ (uint64_t)(val < 0 ? 0 : val), (st & 1) != 0, (st & 2) != 0,
                         got >= 3 ? px : -1.f });
    }
    if (!(fps > 1.0 && fps < 2000.0) || raws.empty()) return false;
    fpsOut = fps; pairInto(raws, fps, out); return true;
}

static bool parseTasbotJson(matjson::Value const& j, std::vector<Action>& out, double& fpsOut) {
    if (!j.contains("macro") || !j.contains("fps")) return false;
    auto arr = j["macro"].asArray();
    if (arr.isErr()) return false;
    double fps = j["fps"].asDouble().unwrapOr(240.0);
    if (!(fps > 1.0 && fps < 2000.0)) fps = 240.0;
    std::vector<RawInput> raws;
    for (auto const& e : arr.unwrap()) {
        uint64_t frame = (uint64_t)e["frame"].asInt().unwrapOr(0);
        auto click = [&](const char* nested, const char* flat) -> int {
            if (e.contains(nested) && e[nested].contains("click")) return (int)e[nested]["click"].asInt().unwrapOr(-1);
            if (e.contains(flat)) return (int)e[flat].asInt().unwrapOr(-1);
            return -1;
        };
        int c1 = click("player_1", "player_1_click");
        int c2 = click("player_2", "player_2_click");
        if (c1 >= 0) raws.push_back({ frame, c1 != 0, false });
        if (c2 >= 0) raws.push_back({ frame, c2 != 0, true });
    }
    if (raws.empty()) return false;
    fpsOut = fps; pairInto(raws, fps, out); return true;
}

static bool parseEchoJson(matjson::Value const& j, std::vector<Action>& out, double& fpsOut) {
    if (!j.contains("Echo Replay")) return false;
    auto arr = j["Echo Replay"].asArray();
    if (arr.isErr()) return false;
    double fps = j.contains("FPS") ? j["FPS"].asDouble().unwrapOr(240.0) : 240.0;
    if (!(fps > 1.0 && fps < 2000.0)) fps = 240.0;
    int64_t start = j.contains("Starting Frame") ? j["Starting Frame"].asInt().unwrapOr(0) : 0;
    std::vector<RawInput> raws;
    for (auto const& e : arr.unwrap()) {
        int64_t fr = e["Frame"].asInt().unwrapOr(0) + start;
        raws.push_back({ (uint64_t)(fr < 0 ? 0 : fr),
                         e["Hold"].asBool().unwrapOr(false),
                         e.contains("Player 2") ? e["Player 2"].asBool().unwrapOr(false) : false });
    }
    if (raws.empty()) return false;
    fpsOut = fps; pairInto(raws, fps, out); return true;
}

static bool parseMhrJson(matjson::Value const& j, std::vector<Action>& out, double& fpsOut) {
    if (!j.contains("events")) return false;
    auto arr = j["events"].asArray();
    if (arr.isErr()) return false;
    double fps = 240.0;
    if (j.contains("meta") && j["meta"].contains("fps")) fps = j["meta"]["fps"].asDouble().unwrapOr(240.0);
    if (!(fps > 1.0 && fps < 2000.0)) fps = 240.0;
    std::vector<RawInput> raws;
    for (auto const& e : arr.unwrap()) {
        if (!e.contains("down")) continue;   // events file has non-input rows
        raws.push_back({ (uint64_t)e["frame"].asInt().unwrapOr(0),
                         e["down"].asBool().unwrapOr(false),
                         e.contains("p2") ? e["p2"].asBool().unwrapOr(false) : false });
    }
    if (raws.empty()) return false;
    fpsOut = fps; pairInto(raws, fps, out); return true;
}

// matjson recurses once per nesting level and these files come off a public upload site, so the
// depth is checked before the parser ever sees the buffer. A stack overflow on Windows cannot be
// caught by any handler, so this has to be prevented rather than contained.
static bool jsonDepthSane(const std::vector<uint8_t>& data, int limit = 64) {
    int d = 0; bool inStr = false, esc = false;
    for (uint8_t c : data) {
        if (inStr) {
            if (esc) esc = false;
            else if (c == 92) esc = true;   // backslash
            else if (c == '"') inStr = false;
            continue;
        }
        if (c == '"') inStr = true;
        else if (c == '[' || c == '{') { if (++d > limit) return false; }
        else if ((c == ']' || c == '}') && d > 0) d--;
    }
    return true;
}

static bool parseJsonMacro(const std::vector<uint8_t>& data, std::vector<Action>& out, double& fpsOut) {
    if (!jsonDepthSane(data)) {
        log::warn("[Click Indicators] macro rejected: JSON nests far too deep to be a real macro");
        return false;
    }
    std::string s(reinterpret_cast<const char*>(data.data()), data.size());
    auto res = matjson::Value::parse(s);
    if (res.isErr()) return false;
    auto j = std::move(res).unwrap();
    return parseGdrJsonValue(j, out, fpsOut)
        || parseTasbotJson(j, out, fpsOut)
        || parseEchoJson(j, out, fpsOut)
        || parseMhrJson(j, out, fpsOut);
}

// .slc has 2 formats. v2 "SILL" (footer "EOM"), v3 "SLC3RPLY" (footer 0xCC)
// both LE, frame-delta, bit-packed. layouts from silicate-bot/slc + slc_oxide
// frames dont rewind across restarts so a file can hold a failed run then the real one
// we keep only the run after the last restart
struct SlcReader {
    const uint8_t* p = nullptr; size_t n = 0, o = 0; bool bad = false;
    bool need(size_t k) { if (o + k > n) { bad = true; return false; } return true; }
    uint64_t uint(size_t k) {   // le
        if (!need(k)) return 0;
        uint64_t v = 0;
        for (size_t i = 0; i < k; i++) v |= (uint64_t)p[o + i] << (8 * i);
        o += k; return v;
    }
    double f64() { if (!need(8)) return 0.0; double d; std::memcpy(&d, p + o, 8); o += 8; return d; }
    void skip(size_t k) { if (need(k)) o += k; }
};

// kind: 0 = jump press/release, 1 = attempt boundary (restart / full restart / death)
struct SlcAct { uint64_t frame = 0; int kind = 0; bool hold = false; bool p2 = false; };

// Same order as parseDdhor's existing guard. Every count and every expansion in this format comes
// from an exponent of a file-supplied bit field, so all of them are bounded by it.
static constexpr uint64_t kSlcMaxActs = 5000000ull;

static bool slcDecodeV2(const std::vector<uint8_t>& d, std::vector<SlcAct>& acts, double& tps) {
    SlcReader r{ d.data(), d.size() };
    r.skip(4);   // "SILL"
    tps = r.f64();
    uint64_t metaSize = r.uint(8);
    if (r.bad || metaSize > d.size()) return false;
    r.skip((size_t)metaSize);
    r.uint(8);   // declared input count, unused
    uint64_t blobCount = r.uint(8);
    if (r.bad || blobCount > (1u << 20)) return false;

    struct Blob { uint64_t bytes = 0, start = 0, len = 0; };
    std::vector<Blob> blobs; blobs.reserve((size_t)blobCount);
    for (uint64_t i = 0; i < blobCount; i++) {
        Blob b; b.bytes = r.uint(8); b.start = r.uint(8); b.len = r.uint(8);
        // records: 1/2/4/8 bytes only, anything else means we lost sync
        if (r.bad || (b.bytes != 1 && b.bytes != 2 && b.bytes != 4 && b.bytes != 8)) return false;
        blobs.push_back(b);
    }

    uint64_t frame = 0;
    for (auto const& b : blobs) {
        for (uint64_t i = 0; i < b.len; i++) {
            uint64_t st = r.uint((size_t)b.bytes);
            if (r.bad) return false;
            // The v3 decoder caps this; v2 did not. b.len is a 64-bit count straight out of
            // the file, so a 64MB .slc of single-byte records yields tens of millions of
            // actions and a multi-second freeze on the main thread - repeated every time the
            // player enters that level, because the file is re-read each entry.
            if ((uint64_t)acts.size() > kSlcMaxActs) return false;
            frame += st >> 5;
            int typ = (int)((st >> 2) & 0x7);   // 0=Skip 1=Jump 2=L 3=R 4=Rst 5=RstFull 6=Death 7=TPS
            if (typ == 7) r.f64();
            if (typ == 1)                 acts.push_back({ frame, 0, (st & 1) != 0, (st & 2) != 0 });
            else if (typ >= 4 && typ <= 6) acts.push_back({ frame, 1, false, false });
        }
    }
    return !r.bad && !acts.empty();
}

static bool slcDecodeV3(const std::vector<uint8_t>& d, std::vector<SlcAct>& acts, double& tps) {
    SlcReader r{ d.data(), d.size() };
    r.skip(8);   // "SLC3RPLY"
    if (r.uint(2) != 64) return false;
    tps = r.f64();
    r.skip(56);
    if (r.bad) return false;

    const size_t end = d.size() - 1;   // 0xCC footer
    uint64_t frame = 0;
    while (r.o < end) {
        uint32_t atomId = (uint32_t)r.uint(4);
        uint64_t size = r.uint(8) & 0x00FFFFFFFFFFFFFFull;   // top byte = flags
        if (r.bad || r.o + size > d.size()) return false;
        size_t payloadEnd = r.o + (size_t)size;
        if (atomId != 1) { r.o = payloadEnd; continue; }   // atomId 1 = Action

        uint64_t count = r.uint(8), got = 0;
        if (r.bad || count > kSlcMaxActs) return false;
        while (r.o < payloadEnd && got < count) {
            uint16_t hdr = (uint16_t)r.uint(2);
            if (r.bad) return false;
            int sid = hdr >> 14;
            if (sid == 0 || sid == 1) {   // Input / Repeat
                size_t dsz  = (size_t)1 << ((hdr >> 12) & 0x3);
                size_t len  = (size_t)1 << ((hdr >> 8) & 0xF);
                size_t reps = (sid == 1) ? ((size_t)1 << ((hdr >> 3) & 0x1F)) : 1;
                // reps and len are 5- and 4-bit EXPONENTS, so a 98-byte file can ask for 2^31 x
                // 2^15 expansions. Nothing inside the expansion reads the file, so the reader's
                // own bad flag can never stop it - it runs to completion or dies allocating.
                // A file that expands past the count it declared is lying, so it is rejected.
                uint64_t work = (uint64_t)reps * (uint64_t)len;
                if (work > count || (uint64_t)acts.size() + work * 2 > kSlcMaxActs) return false;
                struct Rec { uint64_t delta; int btn; bool p2, hold; };
                std::vector<Rec> recs; recs.reserve(len);
                for (size_t i = 0; i < len; i++) {
                    uint64_t st = r.uint(dsz);
                    if (r.bad) return false;
                    recs.push_back({ st >> 4, (int)((st >> 2) & 0x3), (st & 2) != 0, (st & 1) != 0 });
                }
                bool full = false;
                for (size_t rep = 0; rep < reps && !full; rep++)
                    for (auto const& rc : recs) {
                        if (got >= count) { full = true; break; }
                        frame += rc.delta;
                        // btn 0 "Swift" = same-frame tap, expands to 2 actions
                        if (rc.btn == 0) {
                            acts.push_back({ frame, 0, true,  rc.p2 });
                            acts.push_back({ frame, 0, false, rc.p2 });
                            got += 2;
                        } else {
                            if (rc.btn == 1) acts.push_back({ frame, 0, rc.hold, rc.p2 });
                            got += 1;   // L/R are dropped but still counted
                        }
                    }
            } else if (sid == 2) {   // Special
                size_t dsz = (size_t)1 << ((hdr >> 8) & 0x3);
                int stype  = (hdr >> 10) & 0xF;
                frame += r.uint(dsz);   // raw delta, not bit-packed
                if (stype <= 2)      { r.uint(8); acts.push_back({ frame, 1, false, false }); }
                else if (stype == 3) { r.f64(); }
                else if (stype != 4) return false;   // 4 = Bugpoint
                got += 1;
            } else return false;
            if (r.bad) return false;
        }
        r.o = payloadEnd;
    }
    return !acts.empty();
}

// v1 - the oldest Silicate format, and the one most of the archive is written in. It carries NO
// magic: 8 bytes of f64 tick rate, 4 bytes of u32 record count, then that many fixed 4-byte
// records. Dispatch is therefore structural rather than by signature, and the identity
// `body length == count * 4` is exact enough that a false positive is not a practical concern.
//
// Each record packs (frame << 4) | (player2 << 3) | (button << 1) | down - libGDR's own
// (frame << 3) | (button << 1) | down layout with a player-2 bit inserted.
//
// The frame shift was established against .gdr2 recordings of the SAME levels rather than by
// eye: hold-duration plausibility suggests a shift of 3, and 3 is wrong - it yields runs exactly
// twice as long as they really are. Cross-checking seven levels that the channel published in both
// formats puts the shift at 4, agreeing on run length to under 1% (Nhelv 154s vs 153.1s, CITRA
// 38s vs 38.2s, DEAD OF NIGHT 46s vs 45.6s). Bit statistics over 4,260 records back the rest up:
// bit 0 set on exactly 50.0% (the press/release alternation), bit 1 always set and bit 2 never
// (a button field pinned to jump), bit 3 on 8.2% (player two).
static bool slcDecodeV1(const std::vector<uint8_t>& d, std::vector<SlcAct>& acts, double& tps) {
    if (d.size() < 12) return false;
    SlcReader r{ d.data(), d.size() };
    tps = r.f64();
    uint64_t count = r.uint(4);
    if (r.bad || count == 0 || count > kSlcMaxActs) return false;
    if (d.size() - 12 != count * 4) return false;   // the identity that identifies the format
    acts.reserve((size_t)count);
    for (uint64_t i = 0; i < count; i++) {
        uint32_t v = (uint32_t)r.uint(4);
        if (r.bad) return false;
        // btn 2/3 are platformer left/right, filtered here exactly as in every sibling parser so
        // a sideways step never becomes a click cue.
        if (((v >> 1) & 3u) > 1u) continue;
        acts.push_back({ (uint64_t)(v >> 4), 0, (v & 1u) != 0, ((v >> 3) & 1u) != 0 });
    }
    return !acts.empty();
}

static bool parseSlc(const std::vector<uint8_t>& data, std::vector<Action>& out, double& fpsOut) {
    std::vector<SlcAct> acts;
    double tps = 0.0;
    bool ok = false;
    if (data.size() >= 8 && std::memcmp(data.data(), "SLC3RPLY", 8) == 0) ok = slcDecodeV3(data, acts, tps);
    else if (data.size() >= 4 && std::memcmp(data.data(), "SILL", 4) == 0) ok = slcDecodeV2(data, acts, tps);
    else ok = slcDecodeV1(data, acts, tps);   // no signature to test - v1 is identified by shape
    if (!ok) return false;
    // 2000 was an invented ceiling, not a corruption test, and real files sit above it:
    // "Satans Circles Redux slc2.slc" declares 2229 and was being rewritten to 240, scheduling a
    // 59.5 second run across 552 seconds. That is the whole of "silicate doesn't work" - the
    // decode was landing exactly on the file's own EOM footer, and then this line threw the
    // answer away. Silicate records at the physics tick rate, which routinely exceeds 2000.
    if (!(std::isfinite(tps) && tps > 1.0 && tps <= 100000.0)) {
        log::warn("[Click Indicators] .slc declares {} ticks/sec, out of range - assuming 240, "
                  "so the timing will be wrong", tps);
        tps = 240.0;
    }

    std::vector<RawInput> best, cur;
    uint64_t base = 0;
    bool sawBoundary = false;
    for (auto const& a : acts) {
        if (a.kind == 1) {
            if (!cur.empty()) best.swap(cur);
            cur.clear();
            base = a.frame;
            sawBoundary = true;
            continue;
        }
        cur.push_back({ a.frame >= base ? a.frame - base : 0, a.hold, a.p2 });
    }
    if (!cur.empty()) best.swap(cur);
    if (best.empty()) return false;

    if (sawBoundary) {
        g_slcTrimmed = true;
        log::warn("[Click Indicators] .slc contained a restart; kept the final {} inputs. "
                  "Timing may be slightly offset - the format does not record the respawn frame.",
                  (int)best.size());
    }
    fpsOut = tps;
    pairInto(best, tps, out);
    return !out.empty();
}

static double parseMacroFileInner(const fs::path& path, std::vector<Action>& out) {
    out.clear();
    std::error_code szec;
    auto sz = fs::file_size(path, szec);
    // No real macro is 64 MB, and everything below reads the whole file into memory twice.
    if (!szec && sz > (uintmax_t)(64u << 20)) return 0.0;
    std::ifstream f(path, std::ios::binary);
    if (!f) return 0.0;
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (data.size() < 4) return 0.0;

    // THE VAULT. Everything the mod writes into a level's macro folder is sealed with a key the
    // server only hands to an account, so the 1,510-file library cannot simply be zipped up and
    // passed around. Opening one needs that key; without it this returns nothing and the file is
    // exactly as useful as the random bytes it looks like.
    {
        std::string blob((const char*)data.data(), data.size());
        if (cicrypt::vaultIsSealed(blob)) {
            std::string plain;
            if (g_vaultKey.size() != 32
                || !cicrypt::vaultOpen(blob, g_vaultKey.data(), plain) || plain.size() < 4)
                return 0.0;
            data.assign(plain.begin(), plain.end());
        } else if (g_vaultKey.size() == 32 || g_vaultStrict) {
            // Plain bytes in a folder the mod manages. Refused outright once this install has a
            // vault key, and refused for everybody after the migration date.
            //
            // This is the last thing a patched build had. It cannot download a macro and it cannot
            // open a sealed one, but it would read a PLAINTEXT pack dropped into the folder all day
            // - which is the entire product, handed over. Forcing the licence check true does not
            // help any more: the check is no longer what decides. Reading a macro needs a key, the
            // key comes from an account, and there is no branch to patch that produces one.
            //
            // A real install converts its own cache the first time it has a key (see licResealCache
            // in main.cpp), so a paying customer never meets this - by the time the rule applies to
            // them, everything they own is already sealed.
            //
            // The "imported" exemption that used to be here was worse than useless. No folder of
            // that name exists anywhere in the mod, so it exempted nothing; meanwhile the recorder
            // was still writing plain files into this same folder, which meant every recording a
            // player had made was going to stop loading on the morning of the migration date. The
            // recorder now seals what it writes, so there is nothing left to exempt.
            return 0.0;
        }
    }

    // The per-buyer watermark the server appends. Thirty-two bytes naming which account the file
    // was served to, kept through the seal so that any plaintext which escapes still carries it -
    // and stripped here, because none of the format parsers below would know what to do with it.
    //
    // Checked for a magic first, so a macro that happens to end in something else is untouched.
    if (data.size() > 32 + 4) {
        const uint8_t* tail = data.data() + data.size() - 32;
        if (tail[0] == 'C' && tail[1] == 'I' && tail[2] == 'W' && tail[3] == 'M' && tail[4] == '1')
            data.resize(data.size() - 32);
    }
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    double fps = 0.0;

    if (data.size() >= 7 && std::memcmp(data.data(), "HACKPRO", 7) == 0)
        return parseMhr(data, out, fps) ? fps : 0.0;
    if (std::memcmp(data.data(), "RPLY", 4) == 0)
        return parseReplayBot(data, out, fps) ? fps : 0.0;
    if (std::memcmp(data.data(), "DDHR", 4) == 0)
        return parseDdhor(data, out, fps) ? fps : 0.0;
    if ((data.size() >= 8 && std::memcmp(data.data(), "SLC3RPLY", 8) == 0)
     || std::memcmp(data.data(), "SILL", 4) == 0)
        return parseSlc(data, out, fps) ? fps : 0.0;

    if (data[0] == 'G' && data[1] == 'D' && data[2] == 'R') {
        std::vector<GdrRawInput> graws;
        std::vector<GdrPos> gpos;
        double rfps = parseGdr2Bytes(data.data(), data.size(), graws, &gpos);
        for (auto const& q : gpos) g_macroPath.push_back({ q.x, q.y });
        if (rfps <= 0.0) return 0.0;
        std::vector<RawInput> raws; raws.reserve(graws.size());
        // btn 2/3 are platformer left/right. parseMsgpack filters these; this path did not, so a
        // platformer macro drew a cue every time the player merely stepped sideways.
        for (auto const& g : graws)
            if (g.button <= 1) raws.push_back({ g.frame, g.down, g.player2 });
        pairInto(raws, rfps, out);
        // A structurally valid header with no inputs is a real thing in the wild - EclipseBot
        // writes 60-80 byte .gdr2 stubs with framerate 240 and zero records. Reporting those as a
        // successful parse let an empty file satisfy the cache check and silently kill the guide
        // for that level. Every other parser here already returns false on an empty result.
        return out.empty() ? 0.0 : rfps;
    }

    size_t k = 0;
    while (k < data.size() && (data[k] == ' ' || data[k] == '\n' || data[k] == '\r' || data[k] == '\t')) k++;
    if (k < data.size() && (data[k] == '{' || data[k] == '['))
        return parseJsonMacro(data, out, fps) ? fps : 0.0;

    // Silicate v1 carries no signature, so it can only be reached by extension - the magic-based
    // branch above catches v2/v3 and everything older falls through to here.
    if (ext == ".slc" && parseSlc(data, out, fps)) return fps;
    if (ext == ".zbf")
        return parseZbf(data, out, fps) ? fps : 0.0;
    if (ext == ".replay" && parseReplayBot(data, out, fps)) return fps;
    if (ext == ".ddhor" && parseDdhor(data, out, fps)) return fps;
    if ((ext == ".txt" || ext == ".xbot") && parseXbotText(data, out, fps)) return fps;
    if (data.size() > 4 && std::memcmp(data.data(), "fps", 3) == 0 && parseXbotText(data, out, fps)) return fps;

    bool isMap = (data[0] >= 0x80 && data[0] <= 0x8f) || data[0] == 0xde || data[0] == 0xdf;
    if (isMap && parseMsgpack(data, out, fps)) return fps;
    return 0.0;
}

bool isMacroExt(const std::string& ext) {
    return ext == ".gdr" || ext == ".gdr2" || ext == ".json" || ext == ".zbf" || ext == ".mhr"
        || ext == ".replay" || ext == ".ddhor" || ext == ".txt" || ext == ".xbot"
        || ext == ".slc";
}

// Parsing runs on the main thread inside PlayLayer::setupHasCompleted, so anything thrown here
// unwinds through GD's own frames and terminates the process. A malformed macro must cost the
// guide, never the game. This cannot catch a Windows stack overflow - that is what the depth
// guards above are for.
double parseMacroFile(const fs::path& path, std::vector<Action>& out) {
    // Per file, not per session. This clear was missing and the positions of every macro the mod
    // read piled up end to end: fourteen recordings deep by the sixth level, so the route could be
    // drawn from one run while the clicks came from another.
    g_macroPath.clear();
    g_macroPathSrc.clear();
    try {
        double fps = parseMacroFileInner(path, out);
        if (!g_macroPath.empty()) g_macroPathSrc = path.filename().string();
        // One line, always. A macro that fails to read used to return a silent zero that meant
        // "unknown format", "corrupt", and "parsed but empty" all at once, which is why working
        // out which formats actually worked took two days and a pile of sample files.
        if (fps > 0.0 && !out.empty()) {
            // The rate the macro's frame numbers are in. Playback needs it: the game's own frame
            // counter does not necessarily tick at the same rate - a TPS bypass changes it - so a
            // macro frame can only be matched by converting through seconds.
            g_macroFps = fps;
            log::info("[Click Indicators] {}: read {} clicks at {:.0f} ticks/sec, last at {:.1f}s",
                      path.filename().string(), (int)out.size(), fps, out.back().pressTime);
        }
        else {
            std::error_code ec;   // file_size takes it by reference, so it needs a name
            log::warn("[Click Indicators] {}: not read - no parser recognised it, or it held no "
                      "inputs. {} bytes.", path.filename().string(),
                      (long long)fs::file_size(path, ec));
        }
        return fps;
    } catch (std::exception const& e) {
        log::warn("[Click Indicators] {} could not be read: {}", path.filename().string(), e.what());
    } catch (...) {
        log::warn("[Click Indicators] {} could not be read", path.filename().string());
    }
    out.clear();
    return 0.0;
}
