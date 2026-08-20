#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/LevelInfoLayer.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/binding/FMODAudioEngine.hpp>
#include <Geode/binding/ButtonSprite.hpp>
#include <Geode/binding/GJSearchObject.hpp>
#include <Geode/binding/GJGameLevel.hpp>
#include <Geode/binding/GameLevelManager.hpp>
#include <Geode/binding/GameStatsManager.hpp>
#include <Geode/binding/LevelManagerDelegate.hpp>
#include <Geode/binding/StartPosObject.hpp>
#include <Geode/binding/LevelSettingsObject.hpp>
#include <Geode/binding/LevelTools.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/ui/GeodeUI.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/ColorPickPopup.hpp>
#include <Geode/ui/ScrollLayer.hpp>
#include <Geode/ui/Scrollbar.hpp>
#include <Geode/ui/LoadingSpinner.hpp>
#include <Geode/ui/Notification.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/utils/async.hpp>
#include <Geode/utils/file.hpp>

#ifdef _WIN32
#include <windows.h>
#include <dpapi.h>
#pragma comment(lib, "crypt32.lib")
#endif

#include "cicrypt.hpp"
#include "parsers.hpp"
#include "render.hpp"
#include <matjson.hpp>

#include <functional>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <sstream>
#include <string>
#include <vector>

using namespace geode::prelude;
namespace fs = std::filesystem;

static inline float clmp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static double nowSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// GD's own inverse of the curve that placed the cues. loadUpToPosition asks it with the spawn's
// order/channel, which gate which rotate-gameplay objects the walk can see, so 0/0 answers a
// different question in any level that uses them.
// How wrong GD's own time<->position model is AT ONE START POS: canonical time minus the time the
// level really takes to get there. Measured from the macro, which is the only ground truth there
// is - a run recorded from 0% carries real time by definition, so where its presses land against
// the model IS the model's error.
//
// Stored per spawn and applied nowhere else, because a tester's sweep of fifteen start positions
// on one level showed the error is not proportional to depth and cannot be extrapolated:
//
//     canon  25s .. 122s     within +-6 frames, no trend
//     canon 415s             +76 frames
//     canon 420s             +77 frames
//
// A rate fitted to the deep end predicts +22 frames at 122s where the truth is +6, so a rate
// would have made the middle of that level worse to fix the end of it. Whatever the model is
// missing, it is concentrated in one stretch rather than spread evenly, so the only honest thing
// to do with a measurement is use it where it was taken. That is enough: grinding one start pos
// is what practice IS, and this makes the second visit to a spawn as good as the tenth.
static double g_spawnErr = 0.0;
static std::string g_spawnKey;
static std::string g_lvlKey;
static constexpr double kSpawnErrCap = 0.60;   // same ceiling the estimator itself refuses past

// GD's own answer, uncorrected. Only the rate learner wants this; everything else wants the
// corrected one below.
static double canonRawAtX(PlayLayer* pl, float wx) {
    if (!pl || wx <= 1.f) return -1.0;
    int ord = 0, chn = 0;
    if (auto* sp = pl->m_startPosObject)
        if (auto* ss = sp->m_startSettings) { ord = ss->m_targetOrder; chn = ss->m_targetChannel; }
    double t = (double)pl->timeForPos(ccp(wx, 0.f), ord, chn, false, 0);
    return (std::isfinite(t) && t > 0.0) ? t : -1.0;
}

static double canonTimeAtX(PlayLayer* pl, float wx) {
    double t = canonRawAtX(pl, wx);
    return t > 0.0 ? t - g_spawnErr : -1.0;
}

// The probes added while tracking down the StartPos misalignment. Kept, because the next
// report of "the cues look wrong" is answered in one run instead of a day - but silent
// unless Debug logging is on.
static bool dbgLog() { return Mod::get()->getSettingValue<bool>("debug-logging"); }

// GD's timeForPos models the level's speed portals, but it is not how the level actually plays:
// measured against a real run, the true clock runs 0.16s ahead of it by x=37400 on one level.
// From 0% that never shows - the game clock and the macro were both recorded on real time, so
// they agree. At a StartPos GD seeds the clock from the canonical model, which is low by exactly
// that divergence, and every cue is drawn that much further down the level.
//
// The real mapping cannot be derived, only observed - so it is observed. Any run that starts at
// 0% is on real time by definition, and its position/time pairs are recorded here, cached per
// level, and used to correct the clock when the player later spawns mid-level.
struct XtPt { float x; float t; };
static std::vector<XtPt> g_xt;        // ascending in x, this level
static int g_xtLevel = 0;
static float g_xtNextX = 0.f;
static bool g_xtDirty = false;

static std::string xtKey(int levelID) { return fmt::format("xt-{}", levelID); }

// A level with no id of its own is identified by its name instead, so two different local levels
// do not share one map. Hashed rather than used raw: a name can contain anything at all.
static void xtLoad(int levelID);
static void xtLoadNamed(std::string const& name) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : name) { h ^= c; h *= 1099511628211ull; }
    xtLoad(-(int)(h % 2000000000u) - 1);   // negative space, never collides with a real level id
}



static void xtLoad(int levelID) {
    g_xt.clear(); g_xtLevel = levelID; g_xtNextX = 0.f; g_xtDirty = false;
    auto raw = Mod::get()->getSavedValue<std::string>(xtKey(levelID), std::string());
    size_t i = 0;
    while (i < raw.size()) {
        size_t c = raw.find(':', i), e = raw.find(',', i);
        if (c == std::string::npos) break;
        if (e == std::string::npos) e = raw.size();
        g_xt.push_back({ std::strtof(raw.c_str() + i, nullptr),
                         std::strtof(raw.c_str() + c + 1, nullptr) });
        i = e + 1;
    }
    if (!g_xt.empty()) g_xtNextX = g_xt.back().x + 200.f;
}

static void xtSave() {
    if (!g_xtDirty || g_xt.empty()) return;
    std::string out;
    out.reserve(g_xt.size() * 14);
    for (auto const& p : g_xt) out += fmt::format("{:.0f}:{:.3f},", p.x, p.t);
    Mod::get()->setSavedValue<std::string>(xtKey(g_xtLevel), out);
    g_xtDirty = false;
}

// Real time at a position, from what was actually observed. -1 when this stretch is unmapped.
static double xtTimeAt(float x) {
    if (g_xt.size() < 2 || x < g_xt.front().x || x > g_xt.back().x) return -1.0;
    size_t lo = 0, hi = g_xt.size() - 1;
    while (hi - lo > 1) { size_t m = (lo + hi) / 2; if (g_xt[m].x <= x) lo = m; else hi = m; }
    float dx = g_xt[hi].x - g_xt[lo].x;
    if (dx <= 0.f) return g_xt[lo].t;
    return g_xt[lo].t + (double)(g_xt[hi].t - g_xt[lo].t) * (double)((x - g_xt[lo].x) / dx);
}


static bool g_wxOk = false;   // per-click level positions are built and usable
// Which player's inputs this macro actually carries. A single-player macro can be tagged
// either way - the one in hand tags all 186 of its inputs as player 2 - so replaying "player
// 1" found nothing and the path dived in a straight line forever. Only a genuine dual has
// both, and only then does it matter which one is being drawn.
static bool g_macroP1 = false, g_macroP2 = false;
static inline bool pathPlayerIsP2() { return !g_macroP1 && g_macroP2; }

static double g_startOffset = 0.0;
// How long the run takes to reach the spawn, from the engine itself. A property of the level, kept
// apart from g_startOffset, which is a CORRECTION - and whether any correction is wanted depends on
// what GD already put in its own clock, which is measured rather than assumed. See trackStartOffset.
static double g_spawnTime = 0.0;
static double g_spawnCanonRaw = 0.0;   // the same instant, uncorrected - the rate learner's input
static bool g_offsetLocked = false;   // the correction has been anchored at least once
static bool g_offsetObserved = false; // ...and it came from measured data, not the model
static bool g_anchored = false;            // the spawn clock was fixed from presses, not the model
static std::vector<double> g_spawnVotes;   // raw press times while the spawn clock is unverified
static bool g_spawnFixed = false;          // the presses have settled it; stop steering on the model
static double g_spawnPrevOffset = 0.0;     // what to go back to if that turns out to be wrong
static double g_offsetStep = 0.0;          // a deliberate clock jump, for postUpdate to discount
static double g_spawnHoldErr = 0.0;        // last frame's model error, to tell drift from a jump
static bool g_spawnHoldSeeded = false;
static int  g_spawnRollbacks = 0;          // fixes undone by the drop streak
static bool g_spawnGiveUp = false;         // twice wrong: leave the clock alone for this spawn
static double g_trackPrevLT = -1.0e9;

// Exact  = the engine and our own walk over the speed portals agree, or there is no StartPos.
// Approx = only one of them produced a number, or the spawn's own speed contradicts the walk.
// Failed = no number at all, so the guide goes dark rather than confidently cue the opening of
//          the level at someone standing half way through it.
// Geode's "enable-if" grammar has no value comparison and its setting: component only accepts a
// bool, so the highway-only options cannot key off a string setting directly. They key off this
// saved bool instead, which tracks the style. Only relevant to Geode's own settings page - the
// mod's settings screen just leaves the rows out.
static void hwModeSync(std::string const& mode) {
    auto* m = Mod::get();
    bool hw = (mode == "highway");
    if (m->getSavedValue<bool>("hw-mode") != hw) m->setSavedValue<bool>("hw-mode", hw);
}

enum class StartAlign { Exact, Approx, Failed };
static StartAlign g_startAlign = StartAlign::Exact;
static StartAlign g_startAlignResolved = StartAlign::Exact;   // the drop-streak trip is per attempt
static StartPosObject* g_startKeyObj = nullptr;
static float g_startKeyX = 0.f;
static int g_dropStreak = 0;

// Half-width of the visual "press around here" band. This is a HINT, not a tolerance - nothing
// here knows what was survivable, and grading measures frames from the macro's press instead.
// Sized to be visible: at +-12ms the green arming lasted 1.4 render frames at 60fps and could
// fall between two, so it often never appeared.
static constexpr double kBandSec = 0.035;

// GD 2.2 steps physics at 240Hz, and level time is game time, so level-seconds x 240 is the
// physics frame count regardless of speedhack. Frames were being counted in the MACRO's
// recording rate, which meant the same 8ms error read as 1.9, 1.0 or 0.5 frames depending on
// whether the file happened to be 240, 120 or 60fps. The number the player sees has to mean
// the same thing every time.
static constexpr double kPhysFps = 240.0;
// handleButton runs while the engine is stepping the frame that the input belongs to, and
// m_levelTime has not been advanced for that step yet - so a press on frame N is timestamped
// (N-1)/240. Replaying a macro against its own guide graded a dead-perfect bot at -1 frame every
// time, which is how this was caught. One step, added where the timestamp is taken, so grading
// and recording both agree with the convention every other tool writes macros in.
static constexpr double kPressLag = 1.0 / kPhysFps;

struct AudioEvent { double time = 0.0; int type = 0; int idx = -1; }; // type 0=press, 1=release
struct ClickStat { int count = 0; double sumFrames = 0.0; double sumAbs = 0.0; };

struct Calib {
    float L = 0.f, Sbar = 0.0025f; int n = 0;
    float bC[16] = { 0 }; int nC[16] = { 0 };
    float h = 1.0f; double lastWall = -1.0;
    float frozenL = 0.f;       // real seconds
    float leadShown = 0.f;     // frozenL * h -> level seconds
    int rejected = 0, clipped = 0;
    int hFrames = 0;
    int dropped = 0;           // presses with no match
};
static Calib g_cal;
static int g_attempt = 0;     // bumped every spawn/respawn

// THE CLOCK A MACRO'S FRAMES ARE COUNTED IN - AND IT IS NOT m_currentProgress.
//
// 2.2074 declared `void processCommands(float dt)` and called it once per 1/240 physics tick, so
// m_gameState.m_currentProgress WAS the frame number. That is exactly why Eclipse polls straight
// against it and why xdBot writes `frame = m_currentProgress` for imported macros - both are true
// statements about that version.
//
// 2.2081 declares `void processCommands(float dt, bool isHalfTick, bool isLastTick)` and runs each
// tick as a half-tick plus the tick, inserting a further substep whenever
// shouldUseSubstepForButton decides a queued press must land mid-step. m_currentProgress counts
// every one of those calls, so it is a substep counter now.
//
// The measurements say so exactly, and they are not "about twice" - they are twice plus a small
// integer: macro frame 104 -> 210, 161 -> 324, 183 -> 368, 248 -> 500. That is 2n+2, 2n+2, 2n+2,
// 2n+4. Two per tick with extras where the clicks are. No divisor can undo that, which is why
// every attempt at a rate failed, and why the ratio appeared to drift from 2.019 to 2.005.
//
// So count real ticks. One increment per full tick, which is the quantum a macro's frames are in.
static long long g_tick = 0;

struct ErrSample { float ms = 0.f; unsigned char ctx = 0; bool hit = false; };
static constexpr int ERR_KEEP = 240;
static ErrSample g_err[ERR_KEEP];
static int g_errN = 0, g_errHead = 0;

static void errPush(float ms, int ctx, bool hit) {
    if (!std::isfinite(ms)) return;
    g_err[g_errHead] = ErrSample{ ms, (unsigned char)(ctx & 15), hit };
    g_errHead = (g_errHead + 1) % ERR_KEEP;
    if (g_errN < ERR_KEEP) g_errN++;
}
static void errClear() { g_errN = 0; g_errHead = 0; }

struct ErrStats {
    int n = 0;
    int hits = 0;       // inside window
    float mean = 0.f;   // + = late
    float sd = 0.f;
    float p90 = 0.f;
};
static ErrStats errStats(int gmFilter = -1) {
    ErrStats st;
    double sum = 0.0;
    static float tmp[ERR_KEEP];
    int m = 0;
    for (int i = 0; i < g_errN; i++) {
        auto const& e = g_err[i];
        if (gmFilter >= 0 && (e.ctx >> 1) != gmFilter) continue;
        tmp[m++] = e.ms;
        sum += e.ms;
        if (e.hit) st.hits++;
    }
    st.n = m;
    if (!m) return st;
    st.mean = (float)(sum / m);
    if (m > 1) {
        double v = 0.0;
        for (int i = 0; i < m; i++) { double d = tmp[i] - st.mean; v += d * d; }
        st.sd = (float)std::sqrt(v / (m - 1));
    }
    static float abs_[ERR_KEEP];
    for (int i = 0; i < m; i++) abs_[i] = std::fabs(tmp[i]);
    std::sort(abs_, abs_ + m);
    st.p90 = abs_[(int)std::min(m - 1, (int)(m * 0.9f))];
    return st;
}


static bool g_calLoaded = false;
static void calibLoad() {
    if (g_calLoaded) return;
    g_calLoaded = true;
    auto* m = Mod::get();
    g_cal.L    = (float)m->getSavedValue<double>("cal-L", 0.0);
    g_cal.Sbar = (float)m->getSavedValue<double>("cal-S", 0.0025);
    g_cal.n    = (int)m->getSavedValue<int64_t>("cal-n", (int64_t)0);
    for (int i = 0; i < 16; i++) {
        g_cal.bC[i] = (float)m->getSavedValue<double>(fmt::format("cal-b{}", i), 0.0);
        g_cal.nC[i] = (int)m->getSavedValue<int64_t>(fmt::format("cal-c{}", i), (int64_t)0);
    }
    if (!std::isfinite(g_cal.L)) g_cal.L = 0.f;
    if (!std::isfinite(g_cal.Sbar) || g_cal.Sbar <= 0.f) g_cal.Sbar = 0.0025f;
}
static void calibSave() {
    auto* m = Mod::get();
    m->setSavedValue<double>("cal-L", (double)g_cal.L);
    m->setSavedValue<double>("cal-S", (double)g_cal.Sbar);
    m->setSavedValue<int64_t>("cal-n", (int64_t)g_cal.n);
    for (int i = 0; i < 16; i++) {
        m->setSavedValue<double>(fmt::format("cal-b{}", i), (double)g_cal.bC[i]);
        m->setSavedValue<int64_t>(fmt::format("cal-c{}", i), (int64_t)g_cal.nC[i]);
    }
}
static void calibReset() {
    g_cal.L = 0.f; g_cal.Sbar = 0.0025f; g_cal.n = 0;
    for (int i = 0; i < 16; i++) { g_cal.bC[i] = 0.f; g_cal.nC[i] = 0; }
    errClear();
    g_cal.rejected = g_cal.clipped = 0; g_cal.frozenL = 0.f;
    calibSave();
}

static std::vector<Action> g_actions;
static std::vector<AudioEvent> g_events;
static double g_fps = 240.0;

// Clicks the player has marked unnecessary. A macro often taps more than a given player needs, so
// they can strike those out and the guide stops cueing, sounding and grading them. Kept per level
// AND per macro, since click indices only mean anything within one macro.
static int g_activeLevel = 0;
static std::string g_activeMacro;

static std::string muteKey(int level, std::string const& macro) {
    return fmt::format("mute-{}-{}", level, macro);
}

static void muteSave() {
    if (g_activeLevel <= 0 || g_activeMacro.empty()) return;
    std::string csv;
    for (size_t i = 0; i < g_actions.size(); i++)
        if (g_actions[i].muted) csv += (csv.empty() ? "" : ",") + std::to_string(i);
    Mod::get()->setSavedValue<std::string>(muteKey(g_activeLevel, g_activeMacro), csv);
}

static void muteLoadFor(int level, std::string const& macro) {
    for (auto& a : g_actions) a.muted = false;
    if (level <= 0 || macro.empty()) return;
    std::string csv = Mod::get()->getSavedValue<std::string>(muteKey(level, macro), std::string());
    size_t pos = 0;
    while (pos < csv.size()) {
        size_t comma = csv.find(',', pos);
        std::string tok = csv.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        if (!tok.empty()) {
            char* end = nullptr;
            long i = std::strtol(tok.c_str(), &end, 10);   // strtol does not throw, unlike stoi
            if (end != tok.c_str() && i >= 0 && i < (long)g_actions.size()) g_actions[(size_t)i].muted = true;
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
}

static int muteCount() {
    int n = 0;
    for (auto const& a : g_actions) if (a.muted) n++;
    return n;
}
static std::vector<double> g_off;
static float g_volume = 2.0f;
static std::string g_soundPack = "click";

static std::vector<ClickStat> g_clickStats;
static int g_total = 0, g_good = 0, g_streak = 0, g_bestStreak = 0;
static double g_lastFrames = 0.0; static bool g_lastIn = true;
static bool g_fbActive = false; static double g_fbFrames = 0.0; static bool g_fbIn = true;
// Which macro click is currently being held, so its RELEASE is graded against the same click the
// press was graded against instead of being searched for all over again.
static int  g_heldIdx = -1;
static int  g_heldAtt = -1;
static bool g_relActive = false; static double g_relFrames = 0.0;
static int g_tracerTTL = 0; static bool g_tracerGood = true;

static void resetStats() {
    g_clickStats.assign(g_actions.size(), ClickStat{});
    g_total = g_good = g_streak = g_bestStreak = 0;
    g_lastFrames = 0.0; g_lastIn = true; g_fbActive = false;
    g_heldIdx = -1; g_heldAtt = -1; g_relActive = false;
}

// ---- drift ---------------------------------------------------------------------------------
// A macro records press TIMES, and those times only put you where the macro was if you match them.
// In cube that barely matters: x is a function of time and a landing resets the arc, so a slightly
// late jump still leaves you in nearly the right place. In the modes where position is the integral
// of your input - ship, ufo, wave, swing - a timing error is never corrected. Release 10ms late in
// wave and you travelled the wrong way for 10ms, so you are permanently off by 10ms x speed and
// every later cue is aimed at a corridor you are no longer in.
//
// So the SIGNED errors are summed. That sum, times the mode's vertical speed, is a first-order
// estimate of how far off the macro's line you are. It is an estimate, not a measurement - no macro
// format the mod can read carries positions to check against - and it understates ship, where a
// duration error becomes a velocity error and compounds. Which is why it is reported as drift with a
// number, not as a verdict.
static bool g_btnDown = false;
static double g_driftSec = 0.0;
static bool g_driftShown = false;

static bool gmIntegrates(int gm) { return gm == 1 || gm == 3 || gm == 4 || gm == 7; }

static int liveGamemode(PlayerObject* p) {
    if (!p) return 0;
    if (p->m_isShip) return 1;
    if (p->m_isBall) return 2;
    if (p->m_isBird) return 3;   // ufo
    if (p->m_isDart) return 4;   // wave
    if (p->m_isRobot) return 5;
    if (p->m_isSpider) return 6;
    if (p->m_isSwing) return 7;
    return 0;
}

static const char* gmKey(int gm) {
    switch (gm) {
        case 1: return "gm-ship";  case 2: return "gm-ball";   case 3: return "gm-ufo";
        case 4: return "gm-wave";  case 5: return "gm-robot";  case 6: return "gm-spider";
        case 7: return "gm-swing"; default: return "gm-cube";
    }
}
static float appliedLeadRealMs() {
    float h = (std::isfinite(g_cal.h) && g_cal.h > 0.05f) ? g_cal.h : 1.f;
    return (g_cal.leadShown / h) * 1000.f;
}

// Still called on every attempt boundary, because g_attempt is what re-opens clicks for grading
// and re-locks where their marks are drawn. The learned lead it used to compute is gone; see the
// manual "lead" setting.
static void calibFreeze(PlayerObject* p) {
    (void)p;
    g_attempt++;
}

static bool gamemodeGuideOn(PlayerObject* p) {
    return Mod::get()->getSettingValue<bool>(gmKey(liveGamemode(p)));
}

static void buildSchedule() {
    g_events.clear();
    for (size_t i = 0; i < g_actions.size(); i++) {
        auto const& a = g_actions[i];
        if (a.muted) continue;
        g_events.push_back({ a.sweet, 0, (int)i });
        if ((a.releaseTime - a.pressTime) > 0.04) g_events.push_back({ a.releaseTime, 1, (int)i });
    }
    std::sort(g_events.begin(), g_events.end(),
        [](const AudioEvent& a, const AudioEvent& b) { return a.time < b.time; });
}

// One macro at a time. Aggregating several used to widen each click into the spread of where
// different players pressed, which is not a survivable window and is meaningless when they took
// different routes - on wave it produced windows no single run could satisfy. A single macro is one
// player's actual run, so the guide follows a route that is known to work.
static void loadActions(const fs::path& path) {
    g_wxOk = false;   // a new macro means new positions
    std::vector<Action> loaded;
    double fps = parseMacroFile(path, loaded);
    if (loaded.empty()) { g_actions.clear(); g_fps = 240.0; g_events.clear(); resetStats(); return; }
    g_fps = fps > 0 ? fps : 240.0;

    g_actions = std::move(loaded);
    for (auto& a : g_actions) {
        a.sweet = a.pressTime;
        a.winStart = a.pressTime - kBandSec;
        a.winEnd = a.pressTime + kBandSec;
        a.support = 1;
    }
    g_macroP1 = g_macroP2 = false;
    for (auto const& a : g_actions) { if (a.p2) g_macroP2 = true; else g_macroP1 = true; }

    g_activeMacro = path.filename().string();
    muteLoadFor(g_activeLevel, g_activeMacro);

    constexpr double kClusterGap = 0.16;
    for (size_t i = 0; i < g_actions.size(); i++) {
        double best = 1e9;
        if (i > 0) best = std::min(best, g_actions[i].sweet - g_actions[i - 1].sweet);
        if (i + 1 < g_actions.size()) best = std::min(best, g_actions[i + 1].sweet - g_actions[i].sweet);
        g_actions[i].cluster = (best < kClusterGap);
    }

    buildSchedule();
    resetStats();
}

static std::string cuePath(const char* base) {
    std::string sfx = (g_soundPack == "click" || g_soundPack.empty()) ? "" : ("_" + g_soundPack);
    return (Mod::get()->getResourcesDir() / (std::string(base) + sfx + ".wav")).string();
}
static void playPress(int tier) {
    float speed = tier == 2 ? 1.45f : (tier == 1 ? 1.2f : 1.0f); // tight / medium / loose window
    if (auto* e = FMODAudioEngine::sharedEngine()) e->playEffect(cuePath("press"), speed, 1.0f, g_volume);
}
static void playRelease() {
    if (auto* e = FMODAudioEngine::sharedEngine()) e->playEffect(cuePath("release"), 1.0f, 1.0f, g_volume * 0.9f);
}

namespace cgweb = geode::utils::web;

struct HyperMacro {
    std::string id, format, fps, author, fileUrl, filename;
    int64_t bytes = 0;
    std::string localPath;   // set when this entry IS a local file (upload etc)
    // Which library this came from, as the server labelled it. Shown in the row so a macro's
    // origin is visible before it runs in someone's game, and so the channel gets its credit.
    std::string source;
};
using MacroListResult = geode::Result<std::vector<HyperMacro>, std::string>;

static bool g_webActive = false;
static int g_webLevelID = -1;

static std::string jstr(matjson::Value const& v) {
    auto s = v.asString(); if (s.isOk()) return s.unwrap();
    auto i = v.asInt();    if (i.isOk()) return std::to_string(i.unwrap());
    auto d = v.asDouble(); if (d.isOk()) return std::to_string(d.unwrap());
    return "";
}

static std::string formatExt(std::string const& format, std::string const& filename) {
    if (!filename.empty()) {
        auto e = fs::path(filename).extension().string();
        if (!e.empty()) return e;
    }
    std::string f = format;
    std::transform(f.begin(), f.end(), f.begin(), ::tolower);
    if (f == "mhr") return ".mhr";
    if (f == "gdr" || f == "gdr2") return ".gdr";
    if (f == "zbf" || f == "zbot_frame" || f == "zbot") return ".zbf";
    if (f == "slc" || f == "silicate") return ".slc";
    if (f == "xdbot" || f == "json" || f == "gdr_json") return ".gdr.json";
    return ".gdr";
}

// Defined with the licence code below. Everything this mod writes into a macro folder goes
// through it, including the player's own recordings.
static std::string licSealForDisk(std::string const& body, std::string const& name);

static fs::path levelCacheFolder(int levelID) {
    return Mod::get()->getConfigDir(true) / "macros" / std::to_string(levelID);
}

// Working the macro's path out - defined with the rest of the replay, far below, but the macro
// popup is the thing that drives it.
// Why the last attempt produced nothing, in words the person who pressed download can act on. It
// said "Ready" whatever happened, so a macro that cannot be flown looked identical to one that had
// been - and the level then opened with no line and no explanation for it.
static std::string g_procWhy;
// Set when a macro was declined only because its path is already worked out. Without it the popup
// says "Added to your library" and the person is left wondering why it never processed - when in
// fact there was nothing to process and the level is ready.
static bool g_procReady = false;

static bool   ghostProcessStart(GJGameLevel* lvl);
static bool   ghostProcessTick();
static double ghostProcessFrac();
static bool   ghostProcessBusy();
static void   ghostProcessStop();

// The level currently open on the level page. The macro popup knows only a level ID, and flying a
// level needs the object with its data on it - so the page leaves it here while it is up.
// RETAINED, BECAUSE NOTHING CLEARS IT.
//
// This was a raw pointer set when a level page opened and cleared in that page's onExit - except
// LevelInfoLayer does not declare onExit, so Geode never hooked it and the clear has never run. The
// pointer therefore outlived the page and the popup dereferenced freed memory the moment a macro
// was picked: "crashed when i chose another macro". A Ref keeps the level alive for as long as this
// points at it, which costs one object and cannot dangle.
static Ref<GJGameLevel> g_pageLevel = nullptr;

static bool levelCacheExists(int levelID) {
    std::error_code ec;
    auto folder = levelCacheFolder(levelID);
    if (!fs::exists(folder, ec)) return false;
    // Any file at all used to answer yes, so a stray note or a half-written download lit the
    // badge and sent setup down the load branch for something no parser can read.
    for (auto const& e : fs::directory_iterator(folder, ec)) {
        if (!e.is_regular_file()) continue;
        std::string ext = e.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (isMacroExt(ext)) return true;
    }
    return false;
}

static void markWebLoaded(int levelID) {
    g_webActive = true; g_webLevelID = levelID;
}

static void ghostForget();   // defined with the replay; clears the drawn path

static void clearGuide() {
    g_actions.clear(); g_events.clear(); resetStats();
    g_webActive = false; g_webLevelID = -1;
    // The drawn path belongs to the macro that produced it. Removing the macro and leaving the path
    // on screen is how a level ended up showing the previous macro's line, in the wrong place, with
    // nothing on the page admitting anything had happened.
    ghostForget();
}

static bool loadActionsFromCacheFolder(int levelID) {
    std::error_code ec;
    auto folder = levelCacheFolder(levelID);
    // Nothing below may leave ANOTHER level's macro live. A stale one is cued, sounded and graded,
    // and because guideActive() only asks whether g_actions is non-empty it also makes safe mode
    // engage - silently voiding every attempt on a level that has no guide at all.
    g_actions.clear(); g_events.clear(); resetStats(); g_wxOk = false;
    if (!fs::exists(folder, ec)) return false;
    g_slcTrimmed = false;
    g_activeLevel = levelID;

    // Only one macro is ever loaded. Honour the player's pick; otherwise choose the best one
    // rather than whichever the filesystem happened to hand back first. That was not stable
    // between sessions - the same level loaded a 322-click macro one day and a 249-click one the
    // next, so a click the player needed simply was not in the file and the guide looked broken
    // through no fault of its own. Furthest through the level wins, then most clicks, then name,
    // so the choice is deterministic and the same every launch.
    std::string want = Mod::get()->getSavedValue<std::string>(fmt::format("pick-{}", levelID), std::string());
    fs::path primary, chosen;
    double bestEnd = -1.0; size_t bestCount = 0; std::string bestName;
    for (auto const& e : fs::directory_iterator(folder, ec)) {
        if (!e.is_regular_file()) continue;
        std::string ext = e.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (!isMacroExt(ext)) continue;
        std::vector<Action> probe;
        if (parseMacroFile(e.path(), probe) <= 0 || probe.empty()) continue;
        double end = probe.back().releaseTime;
        std::string nm = e.path().filename().string();
        bool better = end > bestEnd + 0.5
                   || (std::fabs(end - bestEnd) <= 0.5
                       && (probe.size() > bestCount
                           || (probe.size() == bestCount && nm < bestName)));
        if (primary.empty() || better) {
            primary = e.path(); bestEnd = end; bestCount = probe.size(); bestName = nm;
        }
        if (!want.empty() && nm == want) { chosen = e.path(); break; }
    }
    if (!chosen.empty()) primary = chosen;
    else if (!primary.empty())
        log::info("[Click Indicators] {} chosen: {} clicks, covers {:.1f}s",
                  primary.filename().string(), (int)bestCount, bestEnd);
    if (primary.empty()) return false;
    loadActions(primary);
    return !g_actions.empty();
}

struct RecInput { uint32_t frame; bool down; bool p2; float x; };
static std::vector<RecInput> g_rec;
static std::vector<RecInput> g_recBest;
static bool g_recActive = false;
static bool g_recOn = false;
static constexpr double REC_FPS = 240.0;   // GD 2.2 physics rate
static constexpr size_t REC_CAP = 200000;

static inline uint32_t recFrameOf(double levelTime) {
    return levelTime <= 0.0 ? 0u : (uint32_t)std::llround(levelTime * REC_FPS);
}
static inline uint32_t recEndOf(std::vector<RecInput> const& v) {
    return v.empty() ? 0u : v.back().frame;
}
static std::vector<RecInput> const& recBuffer() {
    return recEndOf(g_rec) >= recEndOf(g_recBest) ? g_rec : g_recBest;
}
static int recClicks()  { return (int)(recBuffer().size() / 2); }
static float recLength(){ return (float)(recEndOf(recBuffer()) / REC_FPS); }

static void recReset() { g_rec.clear(); g_recBest.clear(); }
static void recStart() { recReset(); g_recActive = true; }
static void recStop()  { g_recActive = false; }

static void recBank() {
    if (recEndOf(g_rec) > recEndOf(g_recBest)) g_recBest = g_rec;
    g_rec.clear();
}

// Everything here is on the macro's timebase, including "are we back at the start of the attempt" -
// which from a StartPos means back at the spawn's time, not back at zero. Testing it against zero
// meant recBank never fired from a StartPos and best-run banking quietly died.
static void recRewind(double macroTime) {
    if (macroTime - g_spawnTime <= 4.0 / REC_FPS) { recBank(); return; }
    uint32_t f = recFrameOf(macroTime);
    while (!g_rec.empty() && g_rec.back().frame > f) g_rec.pop_back();
}

static void recCapture(double levelTime, bool down, bool player1, float x) {
    if (g_rec.size() >= REC_CAP) return;
    g_rec.push_back({ recFrameOf(levelTime), down, !player1, x });
}

static bool recSave(int levelID, std::string& msg) {
    auto const& buf = recBuffer();
    if (buf.size() < 2) { msg = "Nothing recorded yet - play some of the level first"; return false; }
    // An approximate offset baked into a saved file is wrong permanently, and these get shared.
    if (g_startAlign != StartAlign::Exact) {
        msg = "Can't save from here - the guide isn't aligned to the level start";
        return false;
    }
    std::error_code ec;
    auto folder = levelCacheFolder(levelID);
    fs::create_directories(folder, ec);
    int secs = (int)std::llround(recEndOf(buf) / REC_FPS);
    std::string name = fmt::format("my-run-{}s.txt", secs);
    std::string out = fmt::format("fps: {}\nframes\n", (int)REC_FPS);
    // The X the recorder already captures, which used to be thrown away. parseXbotText reads an
    // optional third column, so this stays readable by anything that read the old files.
    for (auto const& r : buf)
        out += fmt::format("{} {} {:.2f}\n", (r.down ? 1 : 0) | (r.p2 ? 2 : 0), r.frame, r.x);
    // Sealed like everything else in this folder. The player's own recording is not what the vault
    // is FOR, but the parser refuses unsealed files here once the migration date passes - and this
    // is the one path that was still writing them, so without this every recording anyone had made
    // would stop loading on the same morning, looking exactly like the mod breaking.
    std::string sealed = licSealForDisk(out, name);
    std::ofstream f(folder / name, std::ios::binary | std::ios::trunc);
    if (!f) { msg = "Couldn't write the recording"; return false; }
    f.write(sealed.data(), (std::streamsize)sealed.size());
    f.close();
    loadActionsFromCacheFolder(levelID);
    msg = fmt::format("Saved {} clicks over {:.1f}s", (int)buf.size() / 2, recEndOf(buf) / REC_FPS);
    return true;
}


// What m_gameState.m_levelTime holds at a StartPos spawn is stated nowhere - not in the bindings,
// not in a doc comment - and the whole overlay hangs off the answer. If GD's clock already counts
// from the level start, adding the spawn time on top puts every cue in the level twice. So it is
// not assumed in either direction: resolveStartOffset works out what the true time at the spawn IS,
// and trackStartOffset holds the clock closed against GD's own inverse of that curve, so the
// attempt. The offset is whatever reconciles the two, which is 0 if GD already did the work.

// GD's x-velocity per speed tier, units/sec.
static constexpr double kSpdUps[5] = { 251.16, 311.58, 387.42, 468.00, 576.00 };

// Speed::Normal is 0 and Speed::Slow is 1, so the enum does not ascend with speed - indexing the
// table with the raw enum silently swaps 0.5x and 1x.
static double spdOfEnum(Speed s) {
    switch (s) {
        case Speed::Slow:    return kSpdUps[0];
        case Speed::Normal:  return kSpdUps[1];
        case Speed::Fast:    return kSpdUps[2];
        case Speed::Faster:  return kSpdUps[3];
        case Speed::Fastest: return kSpdUps[4];
    }
    return kSpdUps[1];
}
static double spdOfPortal(int objectID) {
    switch (objectID) {
        case 200:  return kSpdUps[0];
        case 201:  return kSpdUps[1];
        case 202:  return kSpdUps[2];
        case 203:  return kSpdUps[3];
        case 1334: return kSpdUps[4];
        default:   return 0.0;
    }
}

// x-velocity in a classic level is a step function of x: whatever the last speed portal said, and
// nothing else changes it. So the profile is read off the level once and answered by binary search.
// That is exact on the frame a portal is crossed, and - the part no measurement of past motion can
// ever manage - it can see a portal that is still ahead, which is where the cue is being drawn.
struct SpdSeg { double x, v; };
static std::vector<SpdSeg> g_segs;   // sorted by x, [0] is x=0 at the level's start speed

// Per level, because it is a property of this level's model and this level's macro rather than of
// the machine - and because keying it to the level means it can never be applied to a level it
// was not measured on.
static void lvlKeyLoad(PlayLayer* pl) {
    g_lvlKey.clear();
    if (!pl || !pl->m_level) return;
    int id = (int)pl->m_level->m_levelID;
    if (id > 0) { g_lvlKey = std::to_string(id); return; }
    // A local or editor copy of a level has no online id. The first version of this keyed on the
    // id alone and gave up when it was zero - so on the tester's machine, which had exactly that,
    // nothing was ever stored and nothing said why. The name always exists.
    std::string nm = pl->m_level->m_levelName;
    if (nm.empty()) return;
    unsigned long long h = 1469598103934665603ULL;
    for (unsigned char c : nm) { h ^= (unsigned long long)c; h *= 1099511628211ULL; }
    g_lvlKey = "n" + std::to_string(h);
}

// Every spawn in this level whose error has actually been measured, as x:canonTime:error. Stored
// as one CSV string per level, the same way the muted-click list is - a saved value per spawn
// cannot be enumerated, and enumerating is the whole point.
//
// A start pos that has never been visited is the case still costing the player: a tester's log
// shows a fresh spawn 71 frames out with all seven of his presses too far off to grade, because
// the estimator needs ten before it will speak. Neighbouring spawns already know the answer, so
// they are asked. Strictly between two measured spawns this is interpolation, which is sound.
// Beyond the outermost one it is extrapolation, which on this data is not: the same level
// measured about +6 frames at two minutes in and +77 at seven, so the error is not proportional
// to depth and a long reach would invent a number. Hence the two distance limits below - past
// them it declines to guess and the estimator does what it did before.
struct SpawnFix { double x, t, e; };
static std::vector<SpawnFix> g_spawnFixes;
static std::string g_spawnListKey;
static constexpr double kSeedBracket = 120.0;  // widest gap between two measured spawns worth
                                               // interpolating across, in canon seconds
static constexpr double kSeedReach   = 45.0;   // and how far past the end of them to reach at all
static constexpr size_t kSeedMax     = 48;

static void spawnFixLoad(PlayLayer* pl) {
    g_spawnFixes.clear();
    g_spawnListKey.clear();
    lvlKeyLoad(pl);
    if (g_lvlKey.empty()) return;
    g_spawnListKey = "sperrs-" + g_lvlKey;
    std::string csv = Mod::get()->getSavedValue<std::string>(g_spawnListKey, std::string());
    size_t pos = 0;
    while (pos < csv.size() && g_spawnFixes.size() < kSeedMax) {
        size_t comma = csv.find(',', pos);
        std::string tok = csv.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        double v[3] = { 0, 0, 0 };
        int got = 0;
        size_t fp = 0;
        while (got < 3 && fp <= tok.size()) {
            size_t colon = tok.find(':', fp);
            std::string f = tok.substr(fp, colon == std::string::npos ? std::string::npos : colon - fp);
            char* end = nullptr;
            v[got] = std::strtod(f.c_str(), &end);   // strtod does not throw, unlike stod
            if (end == f.c_str()) break;
            got++;
            if (colon == std::string::npos) break;
            fp = colon + 1;
        }
        if (got == 3 && std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2])
            && v[1] > 0.0 && std::fabs(v[2]) <= kSpawnErrCap)
            g_spawnFixes.push_back({ v[0], v[1], v[2] });
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    std::sort(g_spawnFixes.begin(), g_spawnFixes.end(),
              [](SpawnFix const& a, SpawnFix const& b) { return a.t < b.t; });
}

static void spawnFixSave() {
    if (g_spawnListKey.empty()) return;
    std::string csv;
    for (auto const& f : g_spawnFixes)
        csv += fmt::format("{}{:.0f}:{:.4f}:{:.5f}", csv.empty() ? "" : ",", f.x, f.t, f.e);
    Mod::get()->setSavedValue<std::string>(g_spawnListKey, csv);
}

static void spawnFixStore(double x, double t, double e) {
    if (g_spawnListKey.empty() || !(t > 0.0) || !std::isfinite(e)) return;
    for (auto& f : g_spawnFixes)
        if (std::fabs(f.x - x) < 1.0) { f.t = t; f.e = e; spawnFixSave(); return; }
    if (g_spawnFixes.size() >= kSeedMax) g_spawnFixes.erase(g_spawnFixes.begin());
    g_spawnFixes.push_back({ x, t, e });
    std::sort(g_spawnFixes.begin(), g_spawnFixes.end(),
              [](SpawnFix const& a, SpawnFix const& b) { return a.t < b.t; });
    spawnFixSave();
}

// "measured" tells the caller whether this spawn's own error is known or whether the number is
// borrowed from its neighbours - the difference between a seed to be trusted and one to be
// checked. Returns 0 when it has no business guessing.
static double spawnFixFor(double x, double t, char const** how) {
    *how = "none";
    if (g_spawnFixes.empty() || !(t > 0.0)) return 0.0;
    for (auto const& f : g_spawnFixes)
        if (std::fabs(f.x - x) < 1.0) { *how = "measured here"; return f.e; }
    for (size_t i = 0; i + 1 < g_spawnFixes.size(); i++) {
        auto const& a = g_spawnFixes[i];
        auto const& b = g_spawnFixes[i + 1];
        if (t > a.t && t < b.t && (b.t - a.t) <= kSeedBracket) {
            double w = (t - a.t) / (b.t - a.t);
            *how = "interpolated";
            return a.e + w * (b.e - a.e);
        }
    }
    auto const& lo = g_spawnFixes.front();
    auto const& hi = g_spawnFixes.back();
    if (t <= lo.t && lo.t - t <= kSeedReach) { *how = "nearest"; return lo.e; }
    if (t >= hi.t && t - hi.t <= kSeedReach) { *how = "nearest"; return hi.e; }
    return 0.0;
}
static bool g_segsOk = false;
static PlayLayer* g_segsFor = nullptr;
static int g_segsCount = -1;         // m_speedObjects size the table was built from
static int g_segsLevel = -1;         // and which level, so a recycled address cannot pass
static void* g_segsSpawn = nullptr;  // the StartPos the table was built for - it sets its own speed
static float g_segsSpawnX = 0.f;

// addToSpeedObjects is inlined on Windows so there is no hook to rebuild on. The array's size is
// the next best invalidation key and costs one compare a frame.
static void ensureSpeedTable(PlayLayer* pl) {
    // Level identity, not just the pointer: GD frees and reallocates PlayLayer between levels and
    // the allocator reuses the address, so a bare pointer compare can serve one level's speed
    // portals to another.
    int lid = (pl && pl->m_level) ? (int)pl->m_level->m_levelID : 0;
    if (lid != g_segsLevel) { g_segsFor = nullptr; g_segsLevel = lid; }
    int n = (pl && pl->m_speedObjects) ? pl->m_speedObjects->count() : -1;
    void* sp = pl ? (void*)pl->m_startPosObject : nullptr;
    float spx = pl && pl->m_startPosObject ? pl->m_startPosObject->getPositionX() : 0.f;
    if (pl == g_segsFor && n == g_segsCount && sp == g_segsSpawn && spx == g_segsSpawnX
        ) return;

    g_segs.clear();
    g_segsOk = false;
    g_segsFor = pl;
    g_segsCount = n;
    g_segsSpawn = sp;
    g_segsSpawnX = spx;
    if (!pl || !pl->m_levelSettings || pl->m_isPlatformer) return;

    // Every velocity carries the measured correction, so xAfterDt, xtSimulate and every
    // projection inherit it without knowing about it.
    g_segs.push_back({ 0.0, spdOfEnum(pl->m_levelSettings->m_startSpeed) });
    for (int i = 0; i < n; i++) {
        auto* o = static_cast<GameObject*>(pl->m_speedObjects->objectAtIndex(i));
        if (!o) continue;
        double v = spdOfPortal(o->m_objectID);
        if (v <= 0.0) continue;   // the array is not only speed portals
        g_segs.push_back({ (double)o->getPositionX(), v });
    }
    // Stable, so the x=0 seed stays ahead of a portal sitting on x=0 and the collapse below lets
    // the portal win - which is what GD does when one is stacked on the level start.
    std::stable_sort(g_segs.begin(), g_segs.end(),
                     [](SpdSeg const& a, SpdSeg const& b) { return a.x < b.x; });
    size_t w = 1;
    for (size_t r = 1; r < g_segs.size(); r++) {
        if (g_segs[r].x - g_segs[w - 1].x < 1e-6) g_segs[w - 1].v = g_segs[r].v;
        else g_segs[w++] = g_segs[r];
    }
    g_segs.resize(w);

    // No StartPos speed override here. One was added on the strength of a log line showing the
    // table at 251 while vx read 387 at a spawn - but that vx was stale from before the teleport,
    // so it was never evidence of anything. Overriding the table's speed at the spawn makes every
    // mark latch short if the declared speed is lower than the real one.
    g_segsOk = true;
}

static size_t segAtX(double x) {
    size_t lo = 0, hi = g_segs.size();
    while (hi - lo > 1) {
        size_t mid = lo + (hi - lo) / 2;
        if (g_segs[mid].x <= x) lo = mid; else hi = mid;
    }
    return lo;
}

// Where the player will be dtLevel LEVEL-seconds from x, crossing every portal in between. i is
// segAtX(x), passed in so the search happens once a frame rather than once per cue. Negative walks
// back, which the window bars need once a cue is armed.
static double xAfterDt(double x, double dtLevel, size_t i) {
    if (g_segs.empty()) return x;
    if (i >= g_segs.size()) i = g_segs.size() - 1;
    if (dtLevel >= 0.0) {
        for (; i + 1 < g_segs.size(); i++) {
            double span = (g_segs[i + 1].x - x) / g_segs[i].v;
            if (span >= dtLevel) break;
            dtLevel -= span; x = g_segs[i + 1].x;
        }
    } else {
        for (; i > 0; i--) {
            double span = (g_segs[i].x - x) / g_segs[i].v;   // <= 0
            if (span <= dtLevel) break;
            dtLevel -= span; x = g_segs[i].x;
        }
    }
    return x + g_segs[i].v * dtLevel;
}

// Build the map by stepping the level the way the ENGINE does, instead of integrating it the way
// GD's own timeForPos does. That is the whole difference: timeForPos assumes a speed portal takes
// effect at the exact x it sits at, but the game runs at a fixed physics rate and can only change
// speed on a frame boundary, so the player spends a fraction of a frame at the wrong speed on
// every portal. Fractions of a frame, but they accumulate over a level - and that residual is
// what put a cue inside a spike at a spawn. Every model GD exposes integrates continuously, which
// is why none of them could see it and all three agreed with each other.
//
// Costs one pass at the physics rate on level entry - about 60k steps for a four minute level,
// which is nothing - and it needs no run to learn, so it works the first time on any level.
static void xtSimulate(PlayLayer* pl) {
    if (!pl || g_segs.empty() || pl->m_isPlatformer) return;
    double endX = (double)pl->m_endXPosition;
    if (!(endX > 1.0)) return;

    const double dt = 1.0 / 240.0;   // GD 2.2 physics rate
    double x = 0.0, t = 0.0, nextRec = 0.0;
    size_t seg = 0;
    std::vector<XtPt> out;
    while (x <= endX && t < 1200.0) {
        // The speed for this step is whichever portal the player has already reached at the START
        // of it - the engine cannot apply one part way through a frame.
        while (seg + 1 < g_segs.size() && x >= g_segs[seg + 1].x) seg++;
        if (x >= nextRec) { out.push_back({ (float)x, (float)t }); nextRec = x + 200.0; }
        x += g_segs[seg].v * dt;
        t += dt;
    }
    if (out.size() < 2) return;
    g_xt = std::move(out);
    g_xtNextX = g_xt.back().x + 200.f;
    g_xtDirty = false;   // derived, not observed - no need to persist it
}

// Sums dx/v from the level start to targetX across the speed portals.
static double integrateToX(PlayLayer* pl, float targetX, double* endSpeed) {
    if (!pl || !pl->m_levelSettings) return -1.0;
    ensureSpeedTable(pl);
    if (!g_segsOk) return -1.0;

    double x = 0.0, t = 0.0, v = g_segs[0].v;
    for (size_t i = 1; i < g_segs.size(); i++) {
        if (g_segs[i].x >= (double)targetX) break;
        if (g_segs[i].x > x) { t += (g_segs[i].x - x) / v; x = g_segs[i].x; }
        v = g_segs[i].v;
    }
    if ((double)targetX > x) t += ((double)targetX - x) / v;
    if (endSpeed) *endSpeed = v;
    return t;
}

static void resolveStartOffsetInner(PlayLayer* pl) {
    g_startOffset   = 0.0;
    g_spawnTime     = 0.0;
    g_spawnCanonRaw = 0.0;
    g_spawnErr      = 0.0;   // no start pos means no correction: a run from 0% is already right
    g_spawnKey.clear();
    g_offsetLocked  = false;
    g_offsetObserved = false;
    g_trackPrevLT   = -1.0e9;
    g_anchored = false;
    g_spawnVotes.clear();
    g_spawnFixed = false;
    g_spawnPrevOffset = 0.0;
    g_offsetStep = 0.0;
    g_spawnHoldSeeded = false;
    g_spawnRollbacks = 0;
    g_spawnGiveUp = false;
    g_startAlign    = StartAlign::Exact;
    g_dropStreak    = 0;
    g_startKeyObj = pl ? pl->m_startPosObject : nullptr;
    g_startKeyX   = g_startKeyObj ? g_startKeyObj->getPositionX() : 0.f;
    if (!pl || !g_startKeyObj) return;   // no StartPos: level time already is macro time

    if (pl->m_isPlatformer) {
        // x is not a function of time there - you can walk back over the same x all day
        g_startAlign = StartAlign::Failed;
        Notification::create("Start pos: can't align the guide in platformer levels",
                             NotificationIcon::Error)->show();
        return;
    }

    float spawnX = g_startKeyX;
    if (spawnX <= 1.f) return;

    double endV = 0.0;
    double calc = integrateToX(pl, spawnX, &endV);
    // GD's own x->time integrator, the one it uses to line the song up with a StartPos. It also
    // accounts for time warp and rotate gameplay, which the sum above does not. But the meaning of
    // its order/channel/id arguments is documented nowhere and a wrong call returns 0, which is
    // indistinguishable from "already aligned" - so it is only trusted when the walk agrees.
    double engine = (double)pl->timeForPos(ccp(spawnX, g_startKeyObj->getPositionY()), 0, 0, false, 0);
    bool engineOK = std::isfinite(engine) && engine > 0.0 && calc > 0.0
                 && engine > calc * 0.25 && engine < calc * 4.0;

    // Here rather than only at level entry: changing start pos does not re-enter the level, and
    // each spawn carries its own correction.
    spawnFixLoad(pl);
    char const* how = "none";
    if (!g_lvlKey.empty()) {
        g_spawnKey = g_lvlKey;
        double e = spawnFixFor((double)spawnX, engine > 0.0 ? engine : calc, &how);
        if (std::isfinite(e) && std::fabs(e) <= kSpawnErrCap) g_spawnErr = e;
    }
    if (engineOK) {
        g_spawnCanonRaw = engine;
        g_spawnTime = engine - g_spawnErr;
    } else if (calc > 0.0) {
        g_spawnCanonRaw = calc;
        g_spawnTime = calc - g_spawnErr;
        g_startAlign = StartAlign::Approx;
    } else {
        g_startAlign = StartAlign::Failed;
        log::warn("[Click Indicators] StartPos x={:.0f}: no level geometry (engine {:.3f})", spawnX, engine);
        Notification::create("Start pos: couldn't work out where you came in - guide off",
                             NotificationIcon::Error)->show();
        return;
    }

    // The StartPos carries the speed the player actually spawns at. If the walk ended on a
    // different one it missed something, so the offset is a guess even when both sources agreed.
    if (auto* ss = g_startKeyObj->m_startSettings) {
        double want = spdOfEnum(ss->m_startSpeed);
        if (endV > 0.0 && std::fabs(want - endV) > 1.0) g_startAlign = StartAlign::Approx;
    }

    // No popup for Approx. It fires on entry to a level you have not even started playing, says
    // nothing you can act on, and the HUD already carries "[start pos: approx]" for the whole run.
    // The offset stays 0 until the first frame of the attempt measures what GD actually put in its
    // clock. Nothing draws in between - this runs during setup, the sample runs at the top of the
    // first postUpdate, before anything reads g_startOffset.
    g_offsetLocked = false;
    log::info("[Click Indicators] StartPos x={:.0f} -> spawn at {:.3f}s (engine {:.3f}, portals {:.3f}, {})",
              spawnX, g_spawnTime, engine, calc,
              g_startAlign == StartAlign::Exact ? "exact" : "approx");
    if (dbgLog())
        log::info("[CI-SPAWNERR] x={:.0f} lvl={} seed={:+.3f}s ({:+.1f} frames) [{}] "
                  "from {} measured spawn(s)",
                  spawnX, g_spawnKey.empty() ? "(level has no usable identity)" : g_spawnKey,
                  g_spawnErr, g_spawnErr * kPhysFps, how, (int)g_spawnFixes.size());
}

static void resolveStartOffset(PlayLayer* pl) {
    resolveStartOffsetInner(pl);
    g_startAlignResolved = g_startAlign;
}

// A click happens at a fixed place in the level. The macro's time maps to a world x through the
// level's own speed profile, and that mapping does not care where the player is, how fast they are
// moving, or what rate the game is being played at. Working it out once per click and drawing
// there is exact, and immune to every one of those - which is what projecting from the player's
// live velocity was not. posForTime is GD's own time->position, so it agrees with the level by
// construction; the window and release edges come off the local speed, which is cheap.
static void buildActionPositions(PlayLayer* pl) {
    g_wxOk = false;
    if (!pl || g_actions.empty()) return;
    if (pl->m_isPlatformer) return;
    ensureSpeedTable(pl);
    if (!g_segsOk) return;
    // Only the press comes from posForTime. The three edges are tiny offsets from it, walked with
    // the local speed - taking them from posForTime too meant any time it could not place returned
    // 0, collapsing a band's left edge to the start of the level and drawing it across everything.
    for (auto& a : g_actions) {
        // The macro's times are real; posForTime's are the model's. Real time T is where the
        // model reads T plus its own error, so that is where to ask. Zero without a start pos,
        // which leaves a run from 0% drawing exactly where it always did.
        double sx = (double)pl->posForTime((float)(a.sweet + g_spawnErr)).x;
        if (!(sx > 0.0)) return;   // engine could not place it: leave the projection fallback on
        size_t i = segAtX(sx);
        a.wxSweet = (float)sx;
        a.wxStart = (float)xAfterDt(sx, a.winStart - a.sweet, i);
        a.wxEnd   = (float)xAfterDt(sx, a.winEnd   - a.sweet, i);
        a.wxRel   = (float)xAfterDt(sx, a.releaseTime - a.sweet, i);
    }
    g_wxOk = true;
    if (dbgLog()) log::info("[CI-WX] placed {} clicks: first x={:.0f} last x={:.0f}", (int)g_actions.size(),
              g_actions.front().wxSweet, g_actions.back().wxSweet);
}

// The offset belongs to the spawn point, so it is recomputed when - and only when - the spawn
// point moves. Recomputing on every resetLevel is what double-counted it in practice mode.
// The X is part of the key because a freed StartPos can be replaced at the same address.
static void syncStartOffset(PlayLayer* pl) {
    if (!pl) return;
    auto* sp = pl->m_startPosObject;
    float x = sp ? sp->getPositionX() : 0.f;
    if (sp == g_startKeyObj && x == g_startKeyX) return;
    resolveStartOffset(pl);
    buildActionPositions(pl);   // the new spawn may carry a different correction
}

// The cue positions come off GD's canonical time->x curve, so the clock that decides WHICH cue is
// live has to sit on that same curve. m_levelTime does not: GD seeds it with the spawn's canonical
// time and then advances it with the PLAYER, and after a StartPos the player travels at the spawn's
// own declared speed (loadStartPosObject -> setupLevelStart), which the canonical curve knows
// nothing about. The gap opens from zero AT the spawn and holds for the rest of the run - which is
// exactly why sampling it at the spawn, the one place it is provably zero, measured nothing. So it
// is not sampled once; it is held closed against GD's own inverse map, every frame.
static void trackStartOffset(PlayLayer* pl) {
    if (!pl || !pl->m_player1) return;
    double lt = pl->m_gameState.m_levelTime;
    double dtl = lt - g_trackPrevLT;
    g_trackPrevLT = lt;

    // x is only a clock where it is monotone in time. Platformer and reverse gameplay both break
    // that, and there the seeded clock is the best answer available - hold rather than steer on a
    // reading that cannot be trusted.
    // Only ever a StartPos correction. Playing from 0% is confirmed correct, so that path stays
    // bit-exact: nothing steers it.
    if (!g_startKeyObj) return;
    if (pl->m_isPlatformer || pl->m_player1->m_isGoingLeft || !pl->isGameplayActive()) return;
    double canon = canonTimeAtX(pl, pl->m_player1->getPositionX());
    if (canon < 0.0) return;

    // What the level actually took to reach here, if a 0% run has ever mapped this stretch.
    // That is the number the macro was recorded against; GD's canonical model is not.
    double obs = xtTimeAt(pl->m_player1->getPositionX());
    double truth = obs > 0.0 ? obs : canon;
    g_offsetObserved = obs > 0.0;
    double err = truth - (lt + g_startOffset);

    // Once the player's own presses have settled the offset, stop steering on the model.
    //
    // This is the defect that made the old anchor useless rather than merely imprecise. The anchor
    // would correct g_startOffset and this function would slew it straight back toward canon
    // within kTau - a tenth of a second - because canon is what it treats as truth when there is
    // no measured map. A real log shows the result: the offset swinging +0.092, +0.009, -0.000,
    // +0.109, -0.007 across six presses while the true error sat at -0.144 the whole time. No
    // estimator, however good, converges while something else is pulling the other way.
    //
    // Where a measured map exists it is still better than presses and still wins - this only
    // holds when obs came back empty. A jump too large to be model drift is a teleport or a
    // checkpoint, not drift, so that re-opens the question.
    constexpr double kDead = 0.020;   // the +-0.035 band hides this much; don't chase quantisation
    constexpr double kSnap = 0.250;   // spawn, checkpoint respawn, teleport: take it whole
    constexpr double kTau  = 0.080;   // level seconds to close the rest of the gap

    if (g_spawnFixed && !g_offsetObserved) {
        if (!g_spawnHoldSeeded) { g_spawnHoldErr = err; g_spawnHoldSeeded = true; return; }
        double jump = std::fabs(err - g_spawnHoldErr);
        g_spawnHoldErr = err;
        // A STEP between two frames, not the size of err itself. Testing |err| was wrong twice
        // over: it swallowed the 0.25-0.60s discontinuities the kSnap branch below still handles
        // today - making them permanent instead of snapped - and because a legal correction can
        // approach its own cap, it could push |err| past the threshold and make the fix undo
        // itself on the very next frame. Standing error is exactly what we are choosing to
        // ignore here; only a sudden change means something really moved.
        // g_trackPrevLT is reset to -1e9 each attempt, so the first frame back fails the dtl test
        // and the fix survives a respawn rather than being thrown away by the seam.
        if (!(dtl > 0.0 && dtl <= 0.5) || jump < kSnap) return;
        g_spawnFixed = false;
        g_anchored = false;          // the HUD must stop claiming "anchored" once it is discarded
        g_spawnVotes.clear();
    }

    if (std::fabs(err) < kDead) { g_offsetLocked = true; return; }

    if (!g_offsetLocked || std::fabs(err) > kSnap) {
        g_startOffset += err;
        g_offsetLocked = true;
        if (dbgLog())
            log::info("[CI-LT] snap levelTime={:.3f} canon={:.3f} observed={:.3f} playerX={:.0f} -> offset={:+.3f} ({})",
                      lt, canon, obs, pl->m_player1->getPositionX(), g_startOffset,
                      obs > 0.0 ? "observed" : "canonical-fallback");
        return;
    }
    if (!(dtl > 0.0) || dtl > 0.5) return;

    // Dash orbs and slopes move the player at a speed no portal explains, so the inverse map wobbles
    // for a few frames - hence a slew, not an assignment. Floored at -0.9 of a frame of level time
    // so raw stays strictly increasing: the audio cursor and the jump detector both rely on that.
    double step = (1.0 - std::exp(-dtl / kTau)) * err;
    if (step < -0.9 * dtl) step = -0.9 * dtl;
    g_startOffset += step;
}

// Peak-agreement estimate of the spawn clock offset, taken from the player's own presses.
//
// At a StartPos deep in a level there is no measured x/t map and there never will be: the map only
// grows on a run from 0%, and nobody reaches x=130000 from 0% on the level they are practising -
// needing a StartPos is the whole reason they are there. Every model the game exposes agrees with
// every other one, and a real run measured all of them 0.07s to 0.14s away from the truth, so
// there is nothing better to fall back on. The presses are the only ground truth that deep.
//
// The old estimator took the median error of presses matched to their NEAREST click. That cannot
// work once the clock is already a tenth of a second out: at that offset a press lands nearer a
// neighbouring click, or outside the 0.20s match radius entirely - six dropped against seven
// graded in the log this was written from - so the survivors are precisely the ones whose apparent
// error is small, and their median reads about zero while the truth is -0.144.
//
// So no press is assumed to belong to any particular click. Every press votes for every click near
// it and the offset the most presses agree on wins. Wrong pairings scatter, because they depend on
// the spacing between clicks and that varies; the true offset is the single value they all line up
// on at once.
//
// Applies once, only on a clear result, and never moves the clock further than kCap. A wrong
// correction here would be worse than none - confidently wrong instead of visibly wrong.
static void spawnVoteFix() {
    // Every constant here was measured, not guessed. A replica of this function was run against
    // simulated runs at known offsets before it was written: at ten presses it recovers the true
    // offset to a median of 1.4 physics frames, and moves an already-correct clock by more than
    // 3.6 frames in 0.5% of runs. An earlier version binned votes at one physics frame and
    // refused 197 times out of 200 - safe, and useless. Human press jitter is about 20ms, so the
    // agreement window has to be sized for the player, not for the physics rate.
    constexpr size_t kMinPresses = 10;    // ten halves the median's standard error against five
    constexpr double kWin     = 0.50;     // how far a press may sit from a click it votes for
    constexpr double kTol     = 0.035;    // two presses "agree" within this - roughly the jitter
    constexpr double kSpread  = 0.080;    // a winning cluster looser than this is a coincidence
    constexpr double kMinOff  = 0.025;    // below this the estimate is inside its own noise
    constexpr double kCap     = 0.60;     // larger than this is not a clock error, so refuse it
    constexpr size_t kMargin  = 3;        // and it must beat the runner-up by this much
    if (g_spawnVotes.size() < kMinPresses || g_actions.empty()) return;

    // Candidate offsets per press. Bounded and small, so the clustering below stays cheap enough
    // to run on the input path - a naive pass over every action for every candidate would be
    // millions of operations per press on a long macro.
    std::vector<std::vector<double>> per;
    std::vector<double> flat;
    per.reserve(g_spawnVotes.size());
    for (double p : g_spawnVotes) {
        std::vector<double> row;
        for (auto const& a : g_actions) {
            if (a.muted) continue;
            double d = p - a.sweet;
            if (d >= -kWin && d <= kWin) { row.push_back(d); flat.push_back(d); }
        }
        per.push_back(std::move(row));
    }
    if (flat.empty()) return;

    // The offset the most presses agree on. Each press contributes at most one vote, so a burst
    // of clicks in one place cannot outvote the rest of the run.
    std::vector<double> best;
    for (double d0 : flat) {
        std::vector<double> grp;
        for (auto const& row : per) {
            bool got = false; double pick = 0.0;
            for (double d : row) {
                double gap = std::fabs(d - d0);
                if (gap <= kTol && (!got || gap < std::fabs(pick - d0))) { pick = d; got = true; }
            }
            if (got) grp.push_back(pick);
        }
        if (grp.size() > best.size()) best = std::move(grp);
    }
    if (best.size() < kMinPresses) return;

    std::sort(best.begin(), best.end());
    double off = (best.size() % 2) ? best[best.size() / 2]
                                   : 0.5 * (best[best.size() / 2 - 1] + best[best.size() / 2]);
    if (best.size() >= 3 && (best.back() - best.front()) > kSpread) return;

    // The largest group of presses that do NOT agree with the winner, found the same way the
    // winner was. An earlier version picked, per press, the candidate nearest ZERO rather than the
    // one nearest the cluster being tested - a different rule from the winner's - so a single
    // near-zero stray hid whatever real rival existed and this test waved through evidence that
    // was actually ambiguous. That matters most on an evenly spaced click track, where the whole
    // run aliases cleanly onto the offset one click spacing away: waves, straight flies and spam,
    // which is exactly what people open a StartPos to practise. This is the only guard against
    // locking onto that alias, so it has to measure the thing it claims to.
    size_t second = 0;
    for (double d0 : flat) {
        if (std::fabs(d0 - off) <= 2.0 * kTol) continue;
        size_t n = 0;
        for (auto const& row : per) {
            for (double d : row) {
                if (std::fabs(d - d0) <= kTol && std::fabs(d - off) > 2.0 * kTol) { n++; break; }
            }
        }
        if (n > second) second = n;
    }

    size_t need = (g_spawnVotes.size() * 7) / 10;
    if (need < 5) need = 5;
    if (best.size() < need || best.size() < second + kMargin) return;
    if (std::fabs(off) > kCap || std::fabs(off) < kMinOff) return;

    // Remembered so twelve straight misses can put it back - see the drop streak in handleButton.
    // Without that, a wrong fix is permanent: nothing resets g_spawnFixed on a respawn, because
    // syncStartOffset returns early when the StartPos has not moved.
    g_spawnPrevOffset = g_startOffset;
    g_startOffset -= off;
    // postUpdate reads (m_levelTime + g_startOffset) as a clock and takes the difference between
    // frames as elapsed time. Moving the offset therefore looks to it like the level jumped, which
    // spikes the g_cal.h rate estimate or trips the respawn branch and re-opens every click for
    // grading. Declare the step so it can be subtracted out; the old anchor got away without this
    // only because its corrections were down in the noise.
    g_offsetStep -= off;

    // The presses have just measured how wrong GD's model is at this spawn. Keep it, so coming
    // back here - next attempt, next session - starts right instead of paying another ten presses
    // to learn the same number again. A real log shows what that costs: ten presses at seventy
    // frames out, eight of the ten too far off to even be graded, every single time the start pos
    // changed. It found +0.32s eleven times across two sessions and threw it away eleven times.
    //
    // Stored, not applied live: the offset above has already corrected this attempt, and doing
    // both would count it twice.
    if (!g_spawnListKey.empty()) {
        double e = g_spawnErr + off;
        if (std::isfinite(e) && std::fabs(e) <= kSpawnErrCap) {
            spawnFixStore((double)g_startKeyX, g_spawnCanonRaw, e);
            if (dbgLog())
                log::info("[CI-SPAWNERR] learned x={:.0f} -> {:+.4f}s ({:+.1f} frames); model says "
                          "{:.3f}s here, the macro says {:.3f}s; {} spawn(s) known on this level",
                          g_startKeyX, e, e * kPhysFps, g_spawnCanonRaw, g_spawnCanonRaw - e,
                          (int)g_spawnFixes.size());
        }
    }
    g_spawnHoldSeeded = false;
    g_spawnFixed = true;
    g_anchored = true;
    if (dbgLog())
        log::info("[CI-ANCHOR] vote fix: {} presses, offset {:+.3f}s ({:+.1f} frames), "
                  "agreement {} vs runner-up {} -> startOffset {:+.3f}",
                  (int)g_spawnVotes.size(), off, off * kPhysFps,
                  (int)best.size(), (int)second, g_startOffset);
}

static bool webLoadStillRelevant(int levelID) {
    auto* pl = PlayLayer::get();
    if (!pl) return true;
    return pl->m_level && (int)pl->m_level->m_levelID == levelID;
}


// SECRETS AT REST, BOUND TO THE MACHINE THEY WERE ISSUED TO.
//
// Everything built so far assumed the attacker has to produce something: a grant they cannot sign, a
// vault key they cannot derive, a build they cannot get unmarked. None of it addresses the simplest
// attack left - copy a real buyer's save file. lic-k, lic-g and lic-v sat there in plain text, and
// together they are a working licence and the key to the macro cache. Pasted onto another PC they
// worked, and no amount of signing changes that, because everything in them is genuine.
//
// On Windows they are now wrapped with DPAPI, which encrypts to the current user on the current
// machine using a key the OS holds and will not hand over. The file copies fine; it just does not
// open anywhere else. The extra entropy means another program on the same account cannot unwrap
// them either by simply calling CryptUnprotectData.
//
// Elsewhere this is a no-op, and honestly so: the mod runs on four platforms and this is the one the
// cracking happens on. A portable fingerprint scheme would look like protection while being defeated
// by reading the algorithm out of the very binary it is meant to protect.
static const char kProtPfx[] = "dp1:";

#ifdef _WIN32
static bool licWrapRaw(std::string const& in, std::string& out, bool unwrap) {
    static const BYTE ent[] = { 'c','i','-','a','t','-','r','e','s','t','-','v','1' };
    DATA_BLOB inB{ (DWORD)in.size(), (BYTE*)in.data() };
    DATA_BLOB entB{ (DWORD)sizeof(ent), (BYTE*)ent };
    DATA_BLOB outB{ 0, nullptr };
    BOOL ok = unwrap ? CryptUnprotectData(&inB, nullptr, &entB, nullptr, nullptr, 0, &outB)
                     : CryptProtectData(&inB, L"ci", &entB, nullptr, nullptr, 0, &outB);
    if (!ok) return false;
    out.assign((const char*)outB.pbData, outB.cbData);
    if (outB.pbData) LocalFree(outB.pbData);
    return true;
}
#endif

static std::string licProtect(std::string const& plain) {
    if (plain.empty()) return plain;
#ifdef _WIN32
    std::string raw;
    if (licWrapRaw(plain, raw, false))
        return std::string(kProtPfx)
             + cicrypt::b64uEncode((const unsigned char*)raw.data(), raw.size());
#endif
    return plain;
}

static std::string licUnprotect(std::string const& blob) {
    // Anything without the marker is a value written before this existed. Read it, and it gets
    // wrapped the next time it is written - so nobody has to sign in again for the change.
    if (blob.size() < 4 || blob.compare(0, 4, kProtPfx) != 0) return blob;
#ifdef _WIN32
    std::string raw, out;
    if (cicrypt::b64uDecode(blob.substr(4), raw) && licWrapRaw(raw, out, true)) return out;
#endif
    // Wrapped, and this machine cannot open it: a save file from somewhere else, or a Windows
    // profile that no longer exists. Empty reads as signed out, which is recoverable in one click.
    return std::string();
}

// One reader for all three, so no call site can forget to unwrap. Adding a fourth secret later
// means adding it here and nowhere else.
static std::string licGet(const char* name) {
    return licUnprotect(Mod::get()->getSavedValue<std::string>(name, std::string("")));
}

// The device token, or empty when signed out. Macro requests carry it and the server
// refuses them without it. This is deliberately the only thing standing between a build
// and the macros: the licence check in this binary gates the UI, and a UI gate is one
// instruction away from being flipped - a patched build was doing exactly that. It still
// cannot mint one of these, so it gets an empty list instead of a working product.
// Defined with the rest of the licence code, where licNow() and the migration date live. Declared
// here because the download path below needs to seal before it writes, and that runs first.
static void licVaultSync();
static std::string licSealForDisk(std::string const& body, std::string const& name);

static std::string licToken() {
    return licGet("lic-k");
}
// The mod's own version, on every request it makes.
//
// It used to say "ClickGuide/1.0" and had done since the first build, which meant the server could
// not tell a current install from one three dozen releases old. That is fine until the server wants
// to send something only a NEW build understands - and the macro watermark is exactly that: 32
// bytes on the end of every file, which a build that predates the stripping code would hand
// straight to a format parser. A JSON macro would simply fail to load, for everyone still on the
// published build.
//
// So the server is told who it is talking to, and decides.
static std::string ciUserAgent() {
    return "ClickGuide/" + Mod::get()->getVersion().toNonVString();
}

// WHERE THE SERVER LIVES - and why it is a list rather than one name.
//
// For two days in August 2026 the mod could not reach its own server, and the cause was never
// the server: every hostname in clickindicatorsmod.com was being killed on the way out. Some of
// that was Cloudflare's edge refusing to serve the zone; some of it is security software that
// decides a paid game mod is unwanted and drops any connection naming that host. Norton does
// exactly this today - on a machine where clickindicatorsmod.com cannot be reached at all,
// clickindicators-api.msmithbh9.workers.dev answers 200 from the same process a second later.
//
// A single hostname is therefore a single point of failure that nothing on the server side can
// repair: the block follows the NAME, so redeploying, moving hosts and re-pointing DNS all leave
// the customer exactly as stuck. The only fix that reaches everybody is for the mod to know more
// than one way home.
//
// All three of these serve the same API - the apex and the Railway proxy both end at the same
// Worker the workers.dev name belongs to - so any one of them working is enough. They are
// deliberately on different domains and different providers, because the failure being defended
// against is per-name and, with reputation blocklists, often per-provider too.
// Every name here must be one we cannot lose control of, because this list is compiled into a
// binary that customers keep for years and the mod posts an email and password to whichever of
// them answers. A hostname handed out by a platform - the Railway address the apex currently
// resolves to, for instance - is only ours while that project exists: delete it and the name goes
// back in the pool, and whoever takes it next is handed the credentials of every install that
// still falls through to it. So platform-assigned names do not go in this list.
//
// Railway is reached anyway without being named: clickindicatorsmod.com resolves to it, so the
// entry for that domain already goes there. Naming the platform address a second time bought
// nothing and risked exactly the above.
//
// What is left are two domains jackz owns and a workers.dev name bound to his Cloudflare account.
// None can be claimed by a stranger while the domains are registered and the account exists.
// Order matters: this is the order they are tried, and the first one that answers is remembered.
//
// clickindicators.com is first because it is served by a healthy Cloudflare zone, direct, with no
// proxy in front of it. clickindicatorsmod.com is second and is currently only reachable through
// a failover proxy, because its zone has been refusing every request for three days and deleting
// and re-adding it returned the same broken zone; it stays in the list because every existing
// link and roughly 1,300 older installs still use that name.
//
// The workers.dev name is last and is the one that matters most when things are bad. Both of the
// domains contain the string "clickindicators", and the networks that filter this product appear
// to match on exactly that - the new domain is blocked from the same connection the old one is,
// while workers.dev answers 200 from that same machine a second later. A list of names that all
// share the trigger is not a fallback list at all; this last entry is what makes it one.
static const char* CI_HOSTS[] = {
    "clickindicators.com",
    "clickindicatorsmod.com",
    "clickindicators-api.msmithbh9.workers.dev",
};
static constexpr int CI_HOST_COUNT = (int)(sizeof(CI_HOSTS) / sizeof(CI_HOSTS[0]));

// Which one is currently believed good. Remembered across launches: a player behind antivirus
// that blocks the apex should pay the discovery cost once, not on every start.
static int g_ciHost = -1;

static const char* ciHostKey = "api-host";

static int ciHostIndex() {
    if (g_ciHost < 0) {
        // Stored as the host string rather than an index, so reordering or removing an entry in
        // CI_HOSTS can never silently point an existing install at the wrong server.
        std::string name = Mod::get()->getSavedValue<std::string>(ciHostKey, std::string());
        g_ciHost = 0;
        for (int i = 0; i < CI_HOST_COUNT; i++)
            if (name == CI_HOSTS[i]) { g_ciHost = i; break; }
    }
    return g_ciHost;
}

static void ciSetHost(int i) {
    if (i < 0 || i >= CI_HOST_COUNT) return;
    if (g_ciHost == i) return;
    g_ciHost = i;
    Mod::get()->setSavedValue<std::string>(ciHostKey, std::string(CI_HOSTS[i]));
    geode::log::info("[CI-NET] api host is now {}", CI_HOSTS[i]);
}

static std::string ciApiOn(int hostIndex, std::string const& path) {
    if (hostIndex < 0 || hostIndex >= CI_HOST_COUNT) hostIndex = 0;
    return std::string("https://") + CI_HOSTS[hostIndex] + "/api/" + path;
}

static std::string ciApi(std::string const& path) {
    return ciApiOn(ciHostIndex(), path);
}

// The macro list hands back absolute download URLs that the server built from its own hostname.
// If we are only talking to this server because the usual name is blocked, those URLs name the
// blocked host and every download fails while the list itself looked fine. So any URL pointing
// at one of OUR hosts is moved onto the one that is currently working.
//
// Only our own hosts are touched. A URL to anywhere else is left exactly as it came, because
// rewriting a third party's host would send a request - carrying the Authorization header - to a
// server chosen by whoever controls that response.
static std::string ciRehost(std::string const& url) {
    static const std::string pre = "https://";
    if (url.size() <= pre.size() || url.compare(0, pre.size(), pre) != 0) return url;
    size_t slash = url.find('/', pre.size());
    std::string host = (slash == std::string::npos)
        ? url.substr(pre.size()) : url.substr(pre.size(), slash - pre.size());
    for (int i = 0; i < CI_HOST_COUNT; i++) {
        if (host != CI_HOSTS[i]) continue;
        const char* want = CI_HOSTS[ciHostIndex()];
        if (host == want) return url;
        return pre + want + (slash == std::string::npos ? std::string("/") : url.substr(slash));
    }
    return url;
}

// Is this URL one of ours? The licence token is a bearer credential - whoever holds it is the
// account - so it must only ever be attached to a host on the list compiled into this binary.
// The download URLs it travels with are read out of a server response, and today the server
// always builds them from its own origin; this makes that a rule the mod enforces rather than a
// habit the server happens to keep, so a compromised or confused response cannot turn into
// "post the customer's credential to an address of my choosing".
static bool ciIsOurHost(std::string const& url) {
    static const std::string pre = "https://";
    if (url.size() <= pre.size() || url.compare(0, pre.size(), pre) != 0) return false;
    size_t slash = url.find('/', pre.size());
    std::string host = (slash == std::string::npos)
        ? url.substr(pre.size()) : url.substr(pre.size(), slash - pre.size());
    for (int i = 0; i < CI_HOST_COUNT; i++) if (host == CI_HOSTS[i]) return true;
    return false;
}

// True when the request never reached a server at all - DNS, TLS or the connection being cut.
// An HTTP status, even 500, means the name resolved and something answered, so it says nothing
// about whether this host is reachable and must NOT trigger a switch.
static bool ciTransportFailed(cgweb::WebResponse const& res) {
    return res.code() <= 0;
}

static void fetchMacroPageFrom(int hostIndex, int triesLeft, int levelID, int page,
        std::shared_ptr<std::vector<HyperMacro>> acc,
        std::function<void(MacroListResult)> done) {
    // Through the site, not hyperbolus directly. The upstream address never reaches this
    // binary now, so removing the licence check does not reveal where else to look.
    std::string url = ciApiOn(hostIndex, fmt::format("macros?level_id={}&page={}", levelID, page));
    (void)geode::async::spawn(
        cgweb::WebRequest().userAgent(ciUserAgent())
            .header("Authorization", "Bearer " + licToken())
            .timeout(std::chrono::seconds(15)).get(url),
        [=](cgweb::WebResponse res) {
            // Same rule as sign-in: only a request that never arrived is worth asking a
            // different host about. Anything with a status came from the real server.
            if (ciTransportFailed(res) && triesLeft > 1) {
                int next = (hostIndex + 1) % CI_HOST_COUNT;
                geode::log::info("[CI-NET] {} unreachable for macros, trying {}",
                                 CI_HOSTS[hostIndex], CI_HOSTS[next]);
                fetchMacroPageFrom(next, triesLeft - 1, levelID, page, acc, done);
                return;
            }
            if (!ciTransportFailed(res)) ciSetHost(hostIndex);
            if (!res.ok()) {
                // Our own server answers a dead upstream with 502 and a sentence saying so. Pass
                // that sentence on: "the macro server did not answer" and "we could not be reached
                // at all" are different problems with different things to try, and showing the
                // same "Offline" for both is what makes this look broken rather than empty.
                std::string why = fmt::format("HTTP {}", res.code());
                if (auto j2 = res.json(); j2.isOk()) {
                    auto const& v = j2.unwrap();
                    if (v.contains("error")) {
                        auto e2 = v["error"].asString();
                        if (e2.isOk() && !e2.unwrap().empty()) why = e2.unwrap();
                    }
                }
                done(geode::Err(why));
                return;
            }
            auto jr = res.json();
            if (jr.isErr()) { done(geode::Err("bad JSON")); return; }
            matjson::Value data = std::move(jr).unwrap();
            if (!data.contains("data")) { done(geode::Err("unexpected response")); return; }
            auto arr = data["data"].asArray();
            if (arr.isErr()) { done(geode::Err("'data' not an array")); return; }
            for (auto const& m : arr.unwrap()) {
                HyperMacro hm;
                hm.id = m.contains("id") ? jstr(m["id"]) : "";
                hm.format = m.contains("format") ? jstr(m["format"]) : "";
                hm.fps = m.contains("fps") ? jstr(m["fps"]) : "";
                hm.source = m.contains("source") ? jstr(m["source"]) : "";
                if (m.contains("author") && m["author"].contains("name")) hm.author = jstr(m["author"]["name"]);
                if (m.contains("files")) {
                    auto fr = m["files"].asArray();
                    if (fr.isOk() && !fr.unwrap().empty()) {
                        auto const& f0 = fr.unwrap()[0];
                        hm.fileUrl = f0.contains("url") ? jstr(f0["url"]) : "";
                        hm.filename = f0.contains("filename") ? jstr(f0["filename"]) : "";
                        if (f0.contains("bytes")) hm.bytes = f0["bytes"].asInt().unwrapOr(0);
                    }
                }
                if (!hm.id.empty() && !hm.fileUrl.empty()) acc->push_back(std::move(hm));
            }
            bool hasNext = false;
            if (data.contains("next_page_url")) {
                auto u = data["next_page_url"].asString();
                if (u.isOk()) hasNext = !u.unwrap().empty();
            }
            // Later pages stay on the host that just answered rather than starting the search
            // again, and get a full set of tries of their own in case it dies mid-walk.
            if (hasNext && page < 50)
                fetchMacroPageFrom(hostIndex, CI_HOST_COUNT, levelID, page + 1, acc, done);
            else done(geode::Ok(std::move(*acc)));
        });
}

static void fetchMacroPage(int levelID, int page,
        std::shared_ptr<std::vector<HyperMacro>> acc,
        std::function<void(MacroListResult)> done) {
    fetchMacroPageFrom(ciHostIndex(), CI_HOST_COUNT, levelID, page, std::move(acc), std::move(done));
}

static void fetchMacroList(int levelID, std::function<void(MacroListResult)> done) {
    fetchMacroPage(levelID, 1, std::make_shared<std::vector<HyperMacro>>(), std::move(done));
}

// The id and the filename both come from a third-party community upload site, and
// fs::path::operator/ REPLACES the left side when the right one is absolute or carries a root
// name - so an id of "C:/.../Startup/x", or one containing "../..", writes downloaded bytes
// wherever it likes. Nothing the server sends is used as a path component any more; only its own
// characters survive, and only from a set that cannot traverse or re-root.
static std::string safeCacheName(std::string const& id) {
    std::string o;
    for (char c : id) {
        bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
               || c == '-' || c == '_';
        if (ok) o += c;
        if (o.size() >= 48) break;
    }
    return o.empty() ? std::string("macro") : o;
}

// formatExt echoes the uploader's own filename, so the extension can be anything at all. Only the
// ones a parser actually claims get through; everything else lands as .gdr, which parseMacroFile
// sniffs by content anyway.
static std::string safeCacheExt(std::string const& format, std::string const& filename) {
    std::string ext = formatExt(format, filename);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext == ".json") return ext;
    return isMacroExt(ext) ? ext : std::string(".gdr");
}

static fs::path macroCachePath(int levelID, HyperMacro const& it) {
    if (!it.localPath.empty()) return fs::path(it.localPath);
    return levelCacheFolder(levelID) / (safeCacheName(it.id) + safeCacheExt(it.format, it.filename));
}
static bool isMacroCached(int levelID, HyperMacro const& it) {
    std::error_code ec;
    auto p = macroCachePath(levelID, it);
    return fs::exists(p, ec) && fs::file_size(p, ec) > 0;
}
static int countCached(int levelID, std::vector<HyperMacro> const& items) {
    int c = 0; for (auto const& it : items) if (isMacroCached(levelID, it)) c++; return c;
}

static void afterCacheChanged(int levelID) {
    if (g_webLevelID != levelID) return; // not the active guide -> nothing loaded to update
    if (levelCacheExists(levelID)) { loadActionsFromCacheFolder(levelID); markWebLoaded(levelID); }
    else clearGuide();
}
static bool removeMacroFile(int levelID, HyperMacro const& it) {
    std::error_code ec;
    bool ok = fs::remove(macroCachePath(levelID, it), ec);
    if (ok) afterCacheChanged(levelID);
    return ok;
}
static int removeLevelCache(int levelID) {
    std::error_code ec;
    int n = (int)fs::remove_all(levelCacheFolder(levelID), ec);
    afterCacheChanged(levelID);
    return n;
}

// Geode negates curl's error codes so they cannot collide with HTTP statuses, which is why
// these arrive negative. "file server error -28" told a player nothing and told us nothing
// either; each of these has a different cause and a different fix, so each gets its own words.
//
// Worth knowing when reading these: both the list and the files come through the site, which
// checks the licence and then fetches from the macro source itself. So a 401 here means signed
// out, a 502 means the site reached us but the source did not answer, and anything else is the
// player's own connection. The mod never talks to the source directly.
static std::string dlErrorText(int code) {
    switch (code) {
        case -6:  return "couldn't find the download server - DNS problem on your connection";
        case -7:  return "couldn't connect to the download server - firewall or blocked?";
        // Seen in the wild as antivirus TLS inspection, not a slow server: Norton's filter proxy
        // makes curl - which is what the game downloads through - fail or stall on connections
        // that Windows' own TLS stack accepts, so the site is reachable in a browser while this
        // times out. Worth naming, because "try again" alone sends the player in a circle.
        case -28: return "the download timed out. Antivirus HTTPS scanning is the usual cause - "
                         "try excluding Geometry Dash from it. The mod already tried its backup "
                         "servers, so this is between this PC and the internet";
        case -35:
        case -60: return "secure connection to the download server failed - antivirus or a proxy "
                         "is probably interfering";
        case -56: return "the connection dropped part way through the download";
        // Macros come through the site now and need the licence, so a signed-out mod gets
        // a 401 here rather than a working download. Say so plainly - "server refused the
        // request" would send a paying customer to support for something they can fix.
        case 401: return "you're signed out - sign in to Click Indicators again to load macros";
        case 403: return "the download server refused the request";
        case 404: return "that macro is not on the server any more";
        case 413: return "that macro is too large to load";
        case 429: return "too many requests just now - wait a minute and retry";
        case 502: return "the macro server didn't answer - try again in a minute";
        default: break;
    }
    if (code >= 500) return fmt::format("the download server had a problem ({})", code);
    if (code < 0)    return fmt::format("network error {} while downloading", -code);
    return fmt::format("file server error {}", code);
}

static void downloadAndLoad(int levelID, std::vector<HyperMacro> items,
        std::function<void(int, int, int)> onProgress, std::function<void(int, std::string)> onDone) {
    std::error_code ec;
    fs::create_directories(levelCacheFolder(levelID), ec);
    int total = (int)items.size();
    if (total == 0) { if (onDone) onDone(0, ""); return; }

    auto rem  = std::make_shared<int>(total);
    auto okc  = std::make_shared<int>(0);
    auto dn   = std::make_shared<int>(0);
    auto err  = std::make_shared<std::string>();   // last err, shown in UI
    auto prog = std::make_shared<std::function<void(int, int, int)>>(std::move(onProgress));
    auto fin  = std::make_shared<std::function<void(int, std::string)>>(std::move(onDone));
    auto step = std::make_shared<std::function<void(bool)>>();
    *step = [=](bool ok) {
        (*dn)++; if (ok) (*okc)++;
        if (*prog) (*prog)(*dn, total, *okc);
        if (--(*rem) == 0) {
            int n = *okc;
            if (n > 0 && webLoadStillRelevant(levelID)) {
                loadActionsFromCacheFolder(levelID);
                markWebLoaded(levelID);
            }
            if (*fin) (*fin)(n, *err);
        }
    };

    for (auto const& it : items) {
        fs::path outPath = macroCachePath(levelID, it);
        std::error_code e2;
        if (fs::exists(outPath, e2) && fs::file_size(outPath, e2) > 0) { (*step)(true); continue; } // already have it
        std::string url = ciRehost(it.fileUrl);
        cgweb::WebRequest rq;
        rq.userAgent(ciUserAgent()).followRedirects(true).timeout(std::chrono::seconds(60));
        // Only our own hosts are told who is asking. A URL that came back pointing anywhere else
        // is still fetched - it may be a legitimate third-party mirror - but it is fetched
        // anonymously, because the token identifies the paying account.
        if (ciIsOurHost(url)) rq.header("Authorization", "Bearer " + licToken());
        (void)geode::async::spawn(
            rq.get(url),   // the site proxies and streams it back
            [=](cgweb::WebResponse res) {
                bool good = false;
                if (res.ok()) {
                    auto const& body = res.data();
                    // Uncapped, this buffers an arbitrary body in RAM, writes it to disk, and then
                    // reads it back into a second vector to parse. No real macro is 64 MB.
                    if (body.empty()) { *err = "file server returned no data"; }
                    else if (body.size() > (size_t)(64u << 20)) { *err = "that file is too large"; }
                    else {
                        // Sealed before it touches the disk. This is the file the whole scheme is
                        // about: a downloaded macro, one of 1,510, and the thing a leaked pack is
                        // made of.
                        std::string sealed = licSealForDisk(
                            std::string((const char*)body.data(), body.size()),
                            outPath.filename().string());
                        auto wr = geode::utils::file::writeBinary(
                            outPath, geode::ByteVector(sealed.begin(), sealed.end()));
                        good = wr.isOk();
                        if (!good) { *err = "couldn't save the file"; log::warn("[Click Indicators] write failed: {}", wr.unwrapErr()); }
                        else {
                            // Prove it parses before keeping it, the way onUpload already does.
                            // Without this, a body that yields no actions - a truncated download, or
                            // one of the 60-80 byte zero-input .gdr2 stubs that exist in the wild -
                            // stays on disk, and levelCacheExists only tests name and existence. The
                            // level then latches onto the cache branch forever: auto-fetch is never
                            // reached again, Clear re-downloads the same dead file, and the player
                            // gets a "macro loaded" toast with no guide and no way back.
                            std::vector<Action> probe;
                            if (parseMacroFile(outPath, probe) <= 0 || probe.empty()) {
                                std::error_code rmec;
                                fs::remove(outPath, rmec);
                                good = false;
                                *err = "that macro had no inputs";
                                log::warn("[Click Indicators] discarded unusable macro: {}",
                                          outPath.filename().string());
                            }
                        }
                    }
                } else {
                    *err = dlErrorText(res.code());
                    log::warn("[Click Indicators] download code {} url={}", res.code(), url);
                }
                (*step)(good);
            });
    }
}

// Fetches one macro, not a batch: only one is ever used, and the rest are a click away in the list.
static void fetchAndLoadForLevel(int levelID) {
    fetchMacroList(levelID, [levelID](MacroListResult r) {
        if (r.isErr()) { log::warn("[Click Indicators] auto-fetch: {}", r.unwrapErr()); return; }
        auto items = std::move(r).unwrap();
        if (items.empty()) return;
        items.resize(1);
        downloadAndLoad(levelID, std::move(items), nullptr, [](int n, std::string) {
            if (n > 0) Notification::create("Click Indicators: macro loaded", NotificationIcon::Success)->show();
        });
    });
}

class MacroListPopup : public geode::Popup, public LevelManagerDelegate {
protected:
    int m_levelID = 0;             // level we cache FOR
    int m_sourceID = 0;            // level we FETCH FROM (defaults to levelID)
    int m_originalID = 0;          // startpos copies expose their origin here
    bool m_alive = true;
    bool m_busy = false;
    bool m_processing = false;   // the path for the macro just downloaded is being worked out
    CCMenu* m_tabBar = nullptr;
    CCNode* m_macroTab = nullptr;
    CCNode* m_searchTab = nullptr; // "Find a level" tab content
    geode::ScrollLayer* m_scroll = nullptr;
    CCLabelBMFont* m_status = nullptr;
    CCMenu* m_upload = nullptr;
    CCMenu* m_footer = nullptr;
    geode::TextInput* m_searchInput = nullptr;
    CCLabelBMFont* m_searchStatus = nullptr;
    geode::ScrollLayer* m_resultScroll = nullptr;
    std::vector<HyperMacro> m_items;
    std::vector<CCNode*> m_badges; // parallel to m_items: the "saved" tag per row
    std::vector<CCNode*> m_trash;   // parallel to m_items
    std::vector<CCNode*> m_dl;      // parallel to m_items
    std::vector<CCNode*> m_spin;    // parallel to m_items
    CCLayerColor* m_progBg = nullptr;
    CCLayerColor* m_progFill = nullptr;
    // GameLevelManager has ONE delegate slot for the whole game. Borrowing it means putting
    // back what was there, and only answering to searches we started.
    LevelManagerDelegate* m_prevGlmDelegate = nullptr;
    bool m_searchPending = false;

    bool initContent() {
        if (!Popup::init(380.f, 320.f)) return false;
        this->setTitle("Macros");
        this->scheduleUpdate();   // so a downloaded macro can be worked out while this is open
        // The X is placed by Geode with addChildAtPosition(Anchor::TopLeft), which writes
        // AnchorLayoutOptions on the button. setPosition does not touch those options, so a
        // nudge here is a transient that the next layout pass over m_buttonMenu silently
        // reverts - and while it holds it floats the X inside the panel instead of straddling
        // the corner, which is exactly the button 'sitting weirdly'. Let the anchor place it.

        m_tabBar = CCMenu::create();
        m_tabBar->setPosition({ 190.f, 264.f }); m_tabBar->setContentSize({ 340.f, 28.f });
        m_mainLayer->addChild(m_tabBar);
        if (auto* b = makeToolBtn("This level", menu_selector(MacroListPopup::onTabMacros))) m_tabBar->addChild(b);
        if (auto* b = makeToolBtn("Find a level", menu_selector(MacroListPopup::onTabSearch))) m_tabBar->addChild(b);
        m_tabBar->setLayout(geode::RowLayout::create()->setGap(10.f));

        m_macroTab = CCNode::create();
        m_macroTab->setContentSize({ 380.f, 320.f });
        m_mainLayer->addChild(m_macroTab);

        m_upload = CCMenu::create();
        m_upload->setPosition({ 336.f, 232.f }); m_upload->setContentSize({ 40.f, 26.f });
        m_macroTab->addChild(m_upload);
        if (auto* usp = CCSprite::createWithSpriteFrameName("GJ_plusBtn_001.png")) {
            usp->setScale(0.7f);
            auto ub = CCMenuItemSpriteExtra::create(usp, this, menu_selector(MacroListPopup::onUpload));
            m_upload->addChild(ub);
        } else if (auto* b = makeToolBtn("Upload", menu_selector(MacroListPopup::onUpload))) m_upload->addChild(b);
        m_upload->setLayout(geode::RowLayout::create());

        m_status = CCLabelBMFont::create("Searching for macros...", "bigFont.fnt");
        m_status->setScale(0.34f); m_status->setAnchorPoint({ 0.5f, 0.5f });
        m_status->setPosition({ 156.f, 232.f });
        m_macroTab->addChild(m_status);

        m_progBg = CCLayerColor::create({ 0, 0, 0, 130 }, 240.f, 10.f);
        m_progBg->setPosition({ 70.f, 210.f }); m_progBg->setVisible(false);
        m_macroTab->addChild(m_progBg);
        m_progFill = CCLayerColor::create({ 90, 220, 120, 255 }, 0.f, 10.f);
        m_progFill->setPosition({ 70.f, 210.f }); m_progFill->setVisible(false);
        m_macroTab->addChild(m_progFill);

        m_scroll = geode::ScrollLayer::create({ 336.f, 158.f });
        m_scroll->setPosition({ 22.f, 44.f });
        m_macroTab->addChild(m_scroll);
        m_scroll->m_contentLayer->setLayout(
            geode::ColumnLayout::create()->setGap(4.f)->setAxisReverse(true)->setAutoGrowAxis(158.f));

        m_footer = CCMenu::create();
        m_footer->setPosition({ 190.f, 22.f }); m_footer->setContentSize({ 360.f, 36.f });
        m_macroTab->addChild(m_footer);

        m_searchTab = CCNode::create();
        m_searchTab->setContentSize({ 380.f, 320.f });
        m_mainLayer->addChild(m_searchTab);

        m_searchInput = geode::TextInput::create(230.f, "Level name", "bigFont.fnt");
        m_searchInput->setPosition({ 136.f, 230.f });
        m_searchTab->addChild(m_searchInput);
        auto smenu = CCMenu::create();
        smenu->setPosition({ 312.f, 230.f }); smenu->setContentSize({ 72.f, 30.f });
        m_searchTab->addChild(smenu);
        if (auto* b = makeToolBtn("Search", menu_selector(MacroListPopup::onSearch))) smenu->addChild(b);
        smenu->setLayout(geode::RowLayout::create());

        m_searchStatus = CCLabelBMFont::create("Type a level name and search.", "bigFont.fnt");
        m_searchStatus->setScale(0.34f); m_searchStatus->setAnchorPoint({ 0.5f, 0.5f });
        m_searchStatus->setPosition({ 190.f, 206.f });
        m_searchTab->addChild(m_searchStatus);

        m_resultScroll = geode::ScrollLayer::create({ 336.f, 160.f });
        m_resultScroll->setPosition({ 22.f, 28.f });
        m_searchTab->addChild(m_resultScroll);
        m_resultScroll->m_contentLayer->setLayout(
            geode::ColumnLayout::create()->setGap(3.f)->setAxisReverse(true)->setAutoGrowAxis(160.f));

        switchTab(0);
        refetch();
        return true;
    }

    void switchTab(int t) {
        if (m_macroTab) m_macroTab->setVisible(t == 0);
        if (m_searchTab) m_searchTab->setVisible(t == 1);
    }
    void onTabMacros(CCObject*) { switchTab(0); }
    void onTabSearch(CCObject*) { switchTab(1); }

    static void fitLabel(CCLabelBMFont* l, float maxW, float baseScale) {
        if (!l) return;
        l->setScale(baseScale);
        float w = l->getScaledContentSize().width;
        if (w > maxW) l->setScale(baseScale * maxW / w);
    }
    void setStatus(std::string const& msg, ccColor3B col) {
        m_status->setVisible(true);
        m_status->setString(msg.c_str());
        m_status->setColor(col);
        fitLabel(m_status, 250.f, 0.34f);
    }

    CCMenuItemSpriteExtra* makeToolBtn(std::string const& label, cocos2d::SEL_MenuHandler sel) {
        auto spr = ButtonSprite::create(label.c_str(), "bigFont.fnt", "GJ_button_04.png", 0.8f);
        if (!spr) return nullptr;
        spr->setScale(0.5f);
        return CCMenuItemSpriteExtra::create(spr, this, sel);
    }

    void refetch() {
        if (m_busy) return;
        m_items.clear(); m_badges.clear(); m_trash.clear(); m_dl.clear();
        m_scroll->m_contentLayer->removeAllChildren();
        m_scroll->m_contentLayer->updateLayout();
        m_footer->removeAllChildren();
        bool other = (m_sourceID != m_levelID);
        setStatus(other ? fmt::format("Searching level {}...", m_sourceID) : std::string("Searching for macros..."),
            { 235, 235, 235 });
        Ref<MacroListPopup> self = this;
        int src = m_sourceID;
        fetchMacroList(src, [self, src](MacroListResult r) {
            if (!self->m_alive || self->m_sourceID != src) return; // ignore a stale fetch
            bool err = r.isErr();
            std::string why = err ? r.unwrapErr() : std::string();
            size_t online = 0;
            if (!err) { self->m_items = std::move(r).unwrap(); online = self->m_items.size(); }
            self->populate();   // always - local files should still show even on network error
            if (err) {
                // "Offline" was said for every failure, including the common one where the mod
                // reached our server perfectly well and the third-party macro source behind it was
                // down. That reads as "your internet is broken", sends people to check a connection
                // that is fine, and tells them nothing about whether waiting would help.
                bool upstream = why.find("macro server") != std::string::npos;
                self->setStatus(upstream ? "Macro server is down - showing your saved macros"
                                         : "Offline - showing your saved macros",
                                { 255, 170, 90 });
                log::info("[CI-MACROS] list failed: {}", why);
            }
            else if (online == 0 && self->m_items.empty())
                self->setStatus("No macros for this level yet", { 235, 235, 235 });
            else if (online == 0)
                self->setStatus("No online macros - showing saved", { 235, 235, 235 });
        });
    }

    void setSearchStatus(std::string const& msg, ccColor3B col) {
        m_searchStatus->setVisible(true);
        m_searchStatus->setString(msg.c_str()); fitLabel(m_searchStatus, 320.f, 0.34f);
        m_searchStatus->setColor(col);
    }

    void onSearch(CCObject*) {
        std::string q = m_searchInput->getString();
        size_t a = q.find_first_not_of(" \t"), b = q.find_last_not_of(" \t");
        q = (a == std::string::npos) ? "" : q.substr(a, b - a + 1);
        if (q.empty()) { setSearchStatus("Enter a level name first.", { 255, 200, 90 }); return; }
        m_resultScroll->m_contentLayer->removeAllChildren();
        m_resultScroll->m_contentLayer->updateLayout();
        setSearchStatus(fmt::format("Searching GD for \"{}\"...", q), { 235, 235, 235 });
        auto glm = GameLevelManager::sharedState();
        // Save and restore rather than assign and null. This is a single global slot: if a
        // level list had a search in flight and owned it, taking the slot and later nulling it
        // means that layer never receives its callback and sits on a loading circle forever,
        // and every later search in the game has no delegate at all. Nothing to do with other
        // mods - done the old way this breaks the base game on its own.
        if (glm->m_levelManagerDelegate != this) m_prevGlmDelegate = glm->m_levelManagerDelegate;
        glm->m_levelManagerDelegate = this;
        m_searchPending = true;
        glm->getOnlineLevels(GJSearchObject::create(SearchType::Search, q));
    }

    void onLevels(cocos2d::CCArray* levels) {
        if (!m_alive) return;
        auto* content = m_resultScroll->m_contentLayer;
        content->removeAllChildren();
        int n = levels ? levels->count() : 0;
        for (int i = 0; i < n; i++) {
            auto* lvl = typeinfo_cast<GJGameLevel*>(levels->objectAtIndex(i));
            if (!lvl) continue;
            content->addChild(makeResultRow((int)lvl->m_levelID, std::string(lvl->m_levelName.c_str())));
        }
        content->updateLayout();
        m_resultScroll->scrollToTop();
        setSearchStatus(n > 0 ? fmt::format("{} result(s) - tap one to load its macros", n)
                              : std::string("No levels found."), { 235, 235, 235 });
    }
    void onSearchFailed() { if (m_alive) setSearchStatus("Search failed - check your connection.", { 255, 100, 100 }); }

    // Only answer for a search we started. While we hold the slot, a result belonging to
    // whoever owned it before can still land here; showing it as our own would be wrong, and
    // consuming it would strand them.
    bool mine() { if (!m_searchPending) return false; m_searchPending = false; return true; }

    void loadLevelsFinished(cocos2d::CCArray* levels, char const*) override { if (mine()) onLevels(levels); }
    void loadLevelsFinished(cocos2d::CCArray* levels, char const*, int) override { if (mine()) onLevels(levels); }
    void loadLevelsFailed(char const*) override { if (mine()) onSearchFailed(); }
    void loadLevelsFailed(char const*, int) override { if (mine()) onSearchFailed(); }

    CCNode* makeResultRow(int id, std::string const& name) {
        std::string cap = name.size() > 30 ? (name.substr(0, 29) + "...") : name;
        auto row = CCMenu::create();
        row->setContentSize({ 340.f, 30.f });
        auto spr = ButtonSprite::create(cap.c_str(), "bigFont.fnt", "GJ_button_05.png", 0.8f);
        if (spr) {
            spr->setScale(0.55f);
            auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(MacroListPopup::onPickLevel));
            btn->setTag(id); btn->setPosition({ 170.f, 15.f });
            row->addChild(btn);
        }
        return row;
    }
    void onPickLevel(CCObject* sender) {
        int id = static_cast<CCNode*>(sender)->getTag();
        if (id <= 0) return;
        auto glm = GameLevelManager::sharedState();
        if (glm && glm->m_levelManagerDelegate == this)
            glm->m_levelManagerDelegate = m_prevGlmDelegate;   // hand it back, don't drop it
        m_sourceID = id;
        switchTab(0);
        refetch();
    }

    void onUpload(CCObject*) {
        if (m_busy) return;
        namespace gfile = geode::utils::file;
        gfile::FilePickOptions opts;
        // keep in sync with isMacroExt
        opts.filters.push_back({ "GD macros", { "*.gdr2", "*.gdr", "*.json", "*.zbf", "*.mhr",
                                                "*.slc", "*.replay", "*.ddhor", "*.xbot", "*.txt" } });
        Ref<MacroListPopup> self = this;
        int levelID = m_levelID;
        (void)geode::async::spawn(gfile::pick(gfile::PickMode::OpenFile, opts),
            [self, levelID](gfile::PickResult res) {
                if (!self->m_alive) return;
                if (res.isErr()) { self->setStatus("Upload cancelled", { 235, 235, 235 }); return; }
                auto opt = std::move(res).unwrap();
                if (!opt.has_value()) { self->setStatus("Upload cancelled", { 235, 235, 235 }); return; }
                fs::path src = *opt; std::error_code ec;
                fs::create_directories(levelCacheFolder(levelID), ec);
                fs::path dst = levelCacheFolder(levelID) / src.filename();
                // A file the player chose is sealed on the way in like any other. It costs them
                // nothing - the mod reads it back either way - and it means a macro folder is
                // uniformly unreadable to anyone without the account, rather than uniformly
                // readable to anyone who knows to use the import button.
                {
                    // Without a key this would write a plain file that the parser then refuses -
                    // "added", followed by nothing working. Say the real reason instead.
                    if (g_vaultKey.size() != 32) {
                        self->setStatus("Sign in to add your own macros", { 255, 170, 90 });
                        return;
                    }
                    auto rd = geode::utils::file::readBinary(src);
                    if (rd.isErr()) { self->setStatus("Couldn't read that file", { 255, 100, 100 }); return; }
                    auto raw = rd.unwrap();
                    std::string sealed = licSealForDisk(
                        std::string((const char*)raw.data(), raw.size()), dst.filename().string());
                    auto wr = geode::utils::file::writeBinary(
                        dst, geode::ByteVector(sealed.begin(), sealed.end()));
                    if (wr.isErr()) { self->setStatus("Couldn't save that file", { 255, 100, 100 }); return; }
                }
                std::vector<Action> probe;
                if (parseMacroFile(dst, probe) <= 0 || probe.empty()) {
                    fs::remove(dst, ec);
                    self->setStatus("Not a supported macro file", { 255, 100, 100 });
                    return;
                }
                if (webLoadStillRelevant(levelID)) { loadActionsFromCacheFolder(levelID); markWebLoaded(levelID); }
                self->populate();
                // Uploading is the same event as downloading, from the mod's point of view: a macro
                // this level did not have a moment ago now exists and its path is unknown. It gets
                // the same treatment - worked out here, behind the same progress bar, so the level
                // opens with the line already drawn.
                if (false) {   // path work does not happen in menus - see ghostProcessStart
                    self->m_processing = true;
                    self->setStatus("Processing macro...", { 200, 220, 255 });
                    self->showProgress(0, 100);
                } else {
                    self->setStatus(fmt::format("Added '{}' - guide ready!", src.filename().string()),
                                    { 120, 230, 130 });
                }
            });
    }

    CCNode* makeRow(HyperMacro const& it, int idx) {
        auto row = CCNode::create();
        row->setContentSize({ 330.f, 30.f });
        auto bg = CCScale9Sprite::create("square02b_001.png");
        bg->setContentSize({ 330.f, 30.f }); bg->setOpacity(45); bg->setPosition({ 165.f, 15.f });
        row->addChild(bg);
        bool local = !it.localPath.empty();
        bool cached = isMacroCached(m_levelID, it);

        std::string main = local ? it.author : (it.author.empty() ? ("Macro " + it.id) : it.author);
        auto t1 = CCLabelBMFont::create(main.c_str(), "bigFont.fnt");
        t1->setAnchorPoint({ 0.f, 0.5f }); t1->setScale(0.42f); t1->setPosition({ 9.f, 20.f });
        if (t1->getScaledContentSize().width > 230.f)
            t1->setScale(0.42f * 230.f / t1->getScaledContentSize().width);
        row->addChild(t1);

        std::string sub = it.format;
        if (!local && !it.fps.empty()) sub += "  " + it.fps + "fps";
        if (!local && !it.source.empty()) sub += "  -  " + it.source;
        auto t2 = CCLabelBMFont::create(sub.c_str(), "bigFont.fnt");
        t2->setAnchorPoint({ 0.f, 0.5f }); t2->setScale(0.30f); t2->setColor({ 175, 180, 195 });
        // Same clamp the title above has always had. The subtitle never needed one while it was
        // just "gdr2 240.00fps"; it does now that it carries a source label.
        if (t2->getScaledContentSize().width > 230.f)
            t2->setScale(0.30f * 230.f / t2->getScaledContentSize().width);
        t2->setPosition({ 9.f, 8.f }); row->addChild(t2);

        // Only one macro is active at a time, so the downloaded badge doubles as the picker: lit on
        // the one in use, dim on the others, tap to switch.
        bool active = cached && !g_activeMacro.empty() && m_levelID == g_activeLevel
                   && macroCachePath(m_levelID, it).filename().string() == g_activeMacro;
        CCNode* badge = nullptr;
        auto bMenu = CCMenu::create();
        bMenu->setPosition({ 0.f, 0.f }); bMenu->setContentSize({ 330.f, 30.f });
        if (auto* bspr = CCSprite::createWithSpriteFrameName("GJ_completesIcon_001.png")) {
            bspr->setScale(0.62f);
            if (!active) { bspr->setColor({ 120, 128, 145 }); bspr->setOpacity(150); }
            auto bbtn = CCMenuItemSpriteExtra::create(bspr, this, menu_selector(MacroListPopup::onUseRow));
            bbtn->setPosition({ 292.f, 15.f }); bbtn->setTag(idx);
            bMenu->addChild(bbtn);
            badge = bMenu;
        } else {
            auto b = CCLabelBMFont::create(active ? "in use" : "ready", "bigFont.fnt");
            b->setScale(0.30f); b->setColor(active ? ccColor3B{ 90, 230, 110 } : ccColor3B{ 150, 155, 170 });
            b->setPosition({ 292.f, 15.f });
            badge = b;
        }
        bMenu->setVisible(cached);
        if (badge != bMenu) { badge->setVisible(cached); row->addChild(badge); }
        row->addChild(bMenu);
        if (idx >= 0 && idx < (int)m_badges.size()) m_badges[idx] = badge;

        auto rmMenu = CCMenu::create();
        rmMenu->setPosition({ 0.f, 0.f }); rmMenu->setContentSize({ 330.f, 30.f });
        if (auto* tspr = CCSprite::createWithSpriteFrameName("GJ_deleteIcon_001.png")) {
            tspr->setScale(0.5f);
            auto tbtn = CCMenuItemSpriteExtra::create(tspr, this, menu_selector(MacroListPopup::onRemoveRow));
            tbtn->setPosition({ 316.f, 15.f }); tbtn->setTag(idx);
            rmMenu->addChild(tbtn);
        }
        rmMenu->setVisible(cached);
        row->addChild(rmMenu);
        if (idx >= 0 && idx < (int)m_trash.size()) m_trash[idx] = rmMenu;

        auto dlMenu = CCMenu::create();
        dlMenu->setPosition({ 0.f, 0.f }); dlMenu->setContentSize({ 330.f, 30.f });
        CCNode* dlspr = CCSprite::createWithSpriteFrameName("GJ_downloadBtn_001.png");
        float dscale = 0.5f;
        if (!dlspr) { dlspr = ButtonSprite::create("Get", "bigFont.fnt", "GJ_button_05.png", 0.8f); dscale = 0.55f; }
        if (dlspr) {
            dlspr->setScale(dscale);
            auto dbtn = CCMenuItemSpriteExtra::create(dlspr, this, menu_selector(MacroListPopup::onDownloadRow));
            dbtn->setPosition({ 310.f, 15.f }); dbtn->setTag(idx);
            dlMenu->addChild(dbtn);
        }
        dlMenu->setVisible(!cached);
        row->addChild(dlMenu);
        if (idx >= 0 && idx < (int)m_dl.size()) m_dl[idx] = dlMenu;

        auto spin = geode::LoadingSpinner::create(18.f);
        spin->setPosition({ 310.f, 15.f });
        spin->setVisible(false);
        row->addChild(spin);
        if (idx >= 0 && idx < (int)m_spin.size()) m_spin[idx] = spin;
        return row;
    }

    void mergeLocalMacros() {
        m_items.erase(std::remove_if(m_items.begin(), m_items.end(),
            [](HyperMacro const& h) { return !h.localPath.empty(); }), m_items.end());
        std::error_code ec;
        auto folder = levelCacheFolder(m_levelID);
        if (!fs::is_directory(folder, ec)) return;
        std::vector<std::string> known;
        known.reserve(m_items.size());
        for (auto const& it : m_items) known.push_back(macroCachePath(m_levelID, it).filename().string());
        for (auto const& e : fs::directory_iterator(folder, ec)) {
            if (!e.is_regular_file(ec)) continue;
            auto name = e.path().filename().string();
            if (std::find(known.begin(), known.end(), name) != known.end()) continue;
            HyperMacro h;
            h.localPath = e.path().string();
            h.filename  = name;
            h.author    = e.path().stem().string();
            h.format    = e.path().extension().string();
            if (!h.format.empty() && h.format[0] == '.') h.format.erase(0, 1);
            h.bytes     = (int64_t)fs::file_size(e.path(), ec);
            m_items.push_back(std::move(h));
        }
    }

    void populate() {
        mergeLocalMacros();
        auto* content = m_scroll->m_contentLayer;
        content->removeAllChildren();
        m_badges.assign(m_items.size(), nullptr);
        m_trash.assign(m_items.size(), nullptr);
        m_dl.assign(m_items.size(), nullptr);
        m_spin.assign(m_items.size(), nullptr);
        for (int i = 0; i < (int)m_items.size(); i++) content->addChild(makeRow(m_items[i], i));
        content->updateLayout();
        m_scroll->scrollToTop();
        updateHeader();
        showListFooter();
    }

    void updateHeader() {
        int cached = countCached(m_levelID, m_items);
        setStatus(fmt::format("{} found  -  {} saved", (int)m_items.size(), cached),
            cached > 0 ? ccColor3B{ 150, 230, 150 } : ccColor3B{ 235, 235, 235 });
    }

    void refreshRows() {
        for (int i = 0; i < (int)m_items.size(); i++) {
            bool c = isMacroCached(m_levelID, m_items[i]);
            if (i < (int)m_badges.size() && m_badges[i]) m_badges[i]->setVisible(c);
            if (i < (int)m_trash.size() && m_trash[i]) m_trash[i]->setVisible(c);
            if (i < (int)m_dl.size() && m_dl[i]) m_dl[i]->setVisible(!c);
        }
    }

    // Driven every frame while a downloaded macro is being worked out. The bar is the one the
    // download was already using, so from the outside this is simply the download taking a little
    // longer - which is exactly what it should feel like.
    void update(float dt) override {
        CCLayer::update(dt);
        if (!m_processing) return;
        const double f = ghostProcessFrac();
        showProgress((int)(f * 100.0), 100);
        setStatus(fmt::format("Processing macro... {:.0f}%", f * 100.0), { 200, 220, 255 });
        if (ghostProcessTick()) {
            m_processing = false;
            hideProgress();
            if (!g_procWhy.empty()) setStatus(g_procWhy, { 255, 170, 90 });
            else setStatus("Ready - this level will open instantly", { 120, 230, 130 });
        }
    }

    void showProgress(int done, int total) {
        float frac = total > 0 ? std::min(1.f, (float)done / (float)total) : 0.f;
        if (m_progBg) m_progBg->setVisible(true);
        if (m_progFill) { m_progFill->setVisible(true); m_progFill->setContentSize({ 240.f * frac, 10.f }); }
    }
    void hideProgress() {
        if (m_progBg) m_progBg->setVisible(false);
        if (m_progFill) m_progFill->setVisible(false);
    }

    CCMenuItemSpriteExtra* makeBtn(std::string const& label, cocos2d::SEL_MenuHandler sel) {
        auto spr = ButtonSprite::create(label.c_str(), "goldFont.fnt", "GJ_button_05.png", 0.8f);
        if (!spr) return nullptr;
        spr->setScale(0.62f);
        return CCMenuItemSpriteExtra::create(spr, this, sel);
    }

    void showListFooter() {
        m_footer->removeAllChildren();
        int total = (int)m_items.size();
        int cached = countCached(m_levelID, m_items);
        if (total == 0) return;
        // No bulk download: only one macro is ever used, so they are fetched one at a time from the
        // row buttons and picked with the badge.
        if (cached > 0)
            if (auto* b = makeBtn("Clear", menu_selector(MacroListPopup::onClearAll))) m_footer->addChild(b);
        m_footer->setLayout(geode::RowLayout::create()->setGap(10.f));
    }

    void showCloseFooter() {
        m_footer->removeAllChildren();
        if (auto* b = makeBtn("Close", menu_selector(MacroListPopup::onCloseBtn))) m_footer->addChild(b);
        m_footer->setLayout(geode::RowLayout::create());
    }

    void onCloseBtn(CCObject* s) { this->onClose(s); }

    void onDownloadRow(CCObject* sender) {
        if (m_busy) return;
        int idx = static_cast<CCNode*>(sender)->getTag();
        if (idx < 0 || idx >= (int)m_items.size()) return;
        if (isMacroCached(m_levelID, m_items[idx])) return;
        m_busy = true;
        if (idx < (int)m_dl.size()   && m_dl[idx])   m_dl[idx]->setVisible(false);
        if (idx < (int)m_spin.size() && m_spin[idx]) m_spin[idx]->setVisible(true);
        std::vector<HyperMacro> one{ m_items[idx] };
        Ref<MacroListPopup> self = this;
        downloadAndLoad(m_levelID, std::move(one), nullptr,
            [self, idx](int ok, std::string err) {
                if (!self->m_alive) return;
                self->m_busy = false;
                if (idx < (int)self->m_spin.size() && self->m_spin[idx])
                    self->m_spin[idx]->setVisible(false);
                self->refreshRows(); self->updateHeader();
                if (ok > 0) {
                    // The download is the natural home for this. Working the path out here means
                    // the level opens instantly later, and the few seconds it takes are spent
                    // where the player is already watching a progress bar rather than where they
                    // are trying to play.
                    if (false) {   // path work does not happen in menus - see ghostProcessStart
                        self->m_processing = true;
                        self->setStatus("Processing macro...", { 200, 220, 255 });
                        self->showProgress(0, 100);
                    } else if (g_procReady) {   // unreachable today; kept for when a menu can
                        self->setStatus("Added - the path is already worked out", { 120, 230, 130 });
                    } else {
                        self->setStatus("Added - the path is worked out when you open the level",
                                        { 120, 230, 130 });
                    }
                } else {
                    self->setStatus(err.empty() ? "Download failed" : ("Failed: " + err), { 255, 100, 100 });
                }
            });
    }

    void onRemoveRow(CCObject* sender) {
        if (m_busy) return;
        int idx = static_cast<CCNode*>(sender)->getTag();
        if (idx < 0 || idx >= (int)m_items.size()) return;
        bool wasLocal = !m_items[idx].localPath.empty();
        removeMacroFile(m_levelID, m_items[idx]);
        if (wasLocal) { populate(); return; }   // local rows disappear, online rows just re-toggle
        refreshRows(); updateHeader(); showListFooter();
    }
    void onUseRow(CCObject* sender) {
        if (m_busy) return;
        int idx = static_cast<CCNode*>(sender)->getTag();
        if (idx < 0 || idx >= (int)m_items.size()) return;
        auto path = macroCachePath(m_levelID, m_items[idx]);
        std::error_code ec;
        if (!fs::exists(path, ec)) { setStatus("Download it first", { 255, 200, 120 }); return; }
        Mod::get()->setSavedValue<std::string>(fmt::format("pick-{}", m_levelID), path.filename().string());
        loadActionsFromCacheFolder(m_levelID);
        markWebLoaded(m_levelID);
        populate();
        // Picking a different macro is picking a different path - the cache is keyed on the macro's
        // own inputs, so the one worked out for the last choice does not apply. Work this one out
        // now, for the same reason the download does: better a few seconds here than a level that
        // opens with no line or, worse, the previous macro's.
        if (false) {   // path work does not happen in menus - see ghostProcessStart
            m_processing = true;
            setStatus("Processing macro...", { 200, 220, 255 });
            showProgress(0, 100);
        } else {
            setStatus(fmt::format("Using this macro - {} clicks. The path is worked out when the "
                                  "level opens", (int)g_actions.size()), { 120, 230, 130 });
        }
    }
    void onClearAll(CCObject*) {
        if (m_busy) return;
        removeLevelCache(m_levelID);
        populate();
        Notification::create("Click Indicators: removed this level's macros", NotificationIcon::None)->show();
    }

    void onClose(CCObject* s) override {
        // Ask before throwing the work away. It is minutes of somebody's time and there is nothing
        // on screen saying that closing costs them - so say it, and default to staying.
        if (m_processing) {
            Ref<MacroListPopup> self = this;
            geode::createQuickPopup(
                "Still working",
                "This macro is still being set up. <cr>Leaving now will stop it</c> and the "
                "path will have to be worked out again next time. Leave anyway?",
                "Stay", "Leave",
                [self, s](auto*, bool leave) {
                    if (!leave || !self->m_alive) return;
                    self->m_processing = false;
                    ghostProcessStop();
                    self->reallyClose(s);
                });
            return;
        }
        reallyClose(s);
    }

    void reallyClose(CCObject* s) {
        m_alive = false;
        if (m_processing) { ghostProcessStop(); m_processing = false; }
        auto glm = GameLevelManager::sharedState();
        if (glm && glm->m_levelManagerDelegate == this)
            glm->m_levelManagerDelegate = m_prevGlmDelegate;   // hand it back, don't drop it
        geode::Popup::onClose(s);
    }

public:
    static MacroListPopup* create(int levelID, int originalID) {
        auto ret = new MacroListPopup();
        ret->m_levelID = levelID;
        ret->m_sourceID = levelID;
        ret->m_originalID = originalID;
        if (ret->initContent()) { ret->autorelease(); return ret; }
        delete ret;
        return nullptr;
    }
};

static constexpr uint32_t LIC_OK = 0x5A3C91E7u;
static uint32_t g_licTok = 0;
static std::string g_licMsg = "Not signed in.";
static bool g_licBusy = false;
static constexpr int64_t LIC_GRACE = 14LL * 86400;

// THE LICENCE IS NO LONGER SOMETHING THIS BINARY CAN COMPUTE FOR ITSELF.
//
// licHash() below is FNV-1a over the saved values, mixed with a seed sitting in this file. Anything
// the binary can compute a cracker can compute, so licGate() accepted any key at all as long as
// lic-s matched - which means the crack in circulation never needed to patch a single instruction.
// Four values in the mod's save file opened both gates, and a data crack costs nothing to re-apply
// to a new build. That is why they reappeared the day each release shipped, and why none of the
// per-build work touched them.
//
// A grant is signed by the server with a private key that exists only in the Worker's secret store.
// This is the matching public key: it can verify a grant and cannot produce one. Embedding it is
// safe in a way that embedding any symmetric secret never is.
static const unsigned char kLicPub[32] = {
    0xad, 0x85, 0x72, 0xfd, 0x01, 0x33, 0xe2, 0xf2,
    0x3f, 0xa5, 0xed, 0x42, 0x84, 0x38, 0x0a, 0x5f,
    0x01, 0x9c, 0xc0, 0xfc, 0x7e, 0x78, 0x3f, 0x8f,
    0x7b, 0xef, 0x36, 0x24, 0xc3, 0x69, 0xd9, 0x19,
};

// Nobody who bought this can be locked out by the change, and the old path cannot become a permanent
// bypass either. Until this moment a copy holding a valid legacy record still works, so existing
// installs keep running until each of them happens to be online once - after it, a grant is required,
// and there is no way to extend that from the save file because the deadline is compiled in rather
// than stored.
//
// SET BY bump.py ON EVERY BUILD, to forty-five days after that build was made. It used to be a fixed
// calendar date, chosen on the assumption that a build would ship within a few days of it being
// written. That assumption was wrong - the release is waiting on unrelated work - and a fixed date
// has a nasty property when it is missed: it passes while every customer is still on a build that
// has never heard of any of this, so the FIRST build they ever receive with this code has no grace
// left at all, and anyone not online at first launch is locked out of something they paid for.
//
// Measured from the build, the window is always the thing it was meant to be: however long it takes
// to ship, whoever installs it gets six weeks to be online once.
static constexpr int64_t LIC_LEGACY_UNTIL = 1790579922LL;

static int64_t licNow();   // defined a few lines below, with the rest of the clock handling

static void licResealCache();   // defined below, next to the sealer it uses

// The vault key in the form the parser wants it, refreshed whenever the licence is. Held here
// rather than derived per file because parseMacroFile runs on the game thread during a level load.
static void licVaultSync() {
    const std::string secret = licGet("lic-v");
    if (secret.size() < 16) { g_vaultKey.clear(); }
    else {
        g_vaultKey.assign(32, 0);
        cicrypt::vaultKeyFrom(secret, g_vaultKey.data());
    }
    // Before the migration date a plain macro file is still read: an install that has not been
    // online since the update has a folder full of them, and refusing those would take the product
    // away from someone who paid for it. After it, only sealed files count.
    g_vaultStrict = licNow() >= LIC_LEGACY_UNTIL;
    licResealCache();
}

// Convert a cache written before any of this existed. Once, per install, the first time there is
// a key to do it with.
//
// Without this the rule in the parser - no key, no macro - would take away the library of every
// customer who already had one, which is the opposite of the point. With it, a real install seals
// its own files on the first launch after signing in, and the rule only ever bites somebody who has
// no account at all.
// NOT WHILE THE GAME IS TRYING TO OPEN.
//
// This walks every cached macro folder, reads each file, encrypts the ones not yet sealed and
// writes them back - and it ran inside MenuLayer::init, on the main thread, before anything could
// be drawn. With a library of any size that is the black second at startup: the window exists, the
// menu does not, and the game is busy doing filesystem work nobody asked for yet.
//
// It is a one-time job per key and nothing needs it before the menu is up, so it waits a frame. The
// menu draws, then this happens, and the only difference to anyone is that the game starts.
static void licResealCacheNow();

static void licResealCache() {
    Loader::get()->queueInMainThread([] { licResealCacheNow(); });
}

static void licResealCacheNow() {
    auto* m = Mod::get();
    if (g_vaultKey.size() != 32) return;
    const auto t0 = std::chrono::steady_clock::now();
    // Keyed on the vault key itself, so an account change re-runs it rather than leaving files
    // sealed to a key this install no longer has.
    unsigned char h[64];
    cicrypt::sha512(g_vaultKey.data(), g_vaultKey.size(), h);
    const std::string mark = cicrypt::b64uEncode(h, 8);
    if (m->getSavedValue<std::string>("lic-resealed", std::string("")) == mark) return;

    std::error_code ec;
    const fs::path root = m->getConfigDir(true) / "macros";
    int done = 0, failed = 0;
    if (fs::exists(root, ec)) {
        for (auto const& lvl : fs::directory_iterator(root, ec)) {
            if (ec) break;
            if (!lvl.is_directory(ec)) continue;
            for (auto const& e : fs::directory_iterator(lvl.path(), ec)) {
                if (ec) break;
                if (!e.is_regular_file(ec)) continue;
                auto rd = geode::utils::file::readBinary(e.path());
                if (rd.isErr()) { failed++; continue; }
                auto raw = rd.unwrap();
                std::string body((const char*)raw.data(), raw.size());
                if (cicrypt::vaultIsSealed(body)) continue;      // already done
                std::string sealed;
                if (!cicrypt::vaultSeal(body, e.path().filename().string(),
                                        g_vaultKey.data(), sealed)) { failed++; continue; }
                // writeBinarySafe, so a crash half way through leaves the original rather than a
                // truncated file that neither this build nor the next one can read.
                auto wr = geode::utils::file::writeBinarySafe(
                    e.path(), geode::ByteVector(sealed.begin(), sealed.end()));
                if (wr.isErr()) failed++; else done++;
            }
        }
    }
    if (done || failed)
        log::info("[Click Indicators] sealed {} cached macro(s) in {:.0f} ms{}", done,
                  std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - t0).count(),
                  failed ? fmt::format(", {} could not be read", failed) : "");
    if (!failed) m->setSavedValue<std::string>("lic-resealed", mark);
}

// Seal a file about to be written into a level's macro folder. With no key - an account that has
// not been issued one yet, or a server that has not been deployed with the column - this returns
// the bytes untouched, so a half-applied rollout writes plain files exactly as it did before
// rather than writing something nothing can read.
static std::string licSealForDisk(std::string const& body, std::string const& name) {
    if (g_vaultKey.size() != 32) return body;
    std::string out;
    if (!cicrypt::vaultSeal(body, name, g_vaultKey.data(), out)) return body;
    return out;
}
// How stale a licence may get before the next level entry revalidates it. This is what
// gives "one device at a time" any teeth: the account only holds one device, but the
// machine that lost it keeps playing on its cached token until it next asks. At a day,
// two people sharing a login could both play all day and never collide. Half an hour is
// short enough that the second player notices, and still only a couple of requests an
// hour. Being unable to reach the server does not sign anyone out - that is LIC_GRACE.
static constexpr int64_t LIC_RECHECK = 1800;

static inline bool licOK() { return g_licTok == LIC_OK; }

#ifdef CI_DEV_UNLOCK
static constexpr uint64_t LIC_DEV_HASH = 0xDB0F44783A396956ull;
static bool licIsDev(std::string const& key) {
    std::string norm;   // dashes + case don't matter
    for (char c : key) {
        if (c == '-' || c == '_' || c == ' ') continue;
        norm += (c >= 'a' && c <= 'z') ? char(c - 32) : c;
    }
    uint64_t h = 1469598103934665603ull;
    std::string probe = norm + "ci-devunlock";
    for (unsigned char c : probe) { h ^= c; h *= 1099511628211ull; }
    return h == LIC_DEV_HASH;
}
#else
static bool licIsDev(std::string const&) { return false; }
#endif

static int64_t licNow() {
    return (int64_t)std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

static uint64_t licHash(std::string const& key, std::string const& inst, int64_t ts) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](std::string const& s) {
        for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    };
    mix(key); mix(inst); mix(std::to_string(ts)); mix("ci\x1f" "9f4a" "\x07" "seed");
    return h;
}

static std::string licSanitise(std::string const& s) {
    std::string o;
    for (char c : s) {
        bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                  || c == '-' || c == '_';
        if (ok) o += c;
    }
    return o;
}

static void licVaultSync();
static bool licGate();   // defined below - the real gate, and licRefresh must not
                         // report a licence that it is going to refuse

// Bumped whenever the licence state is touched, so the cached gate below re-derives on the very
// next frame instead of waiting out its interval.
static int g_licGen = 0;

static void licRefresh() {
    g_licGen++;
    // Kept in step with the licence rather than set once at load: an install that signs in
    // mid-session gets its vault key the moment it arrives, without a restart.
    licVaultSync();
    auto* m = Mod::get();
    std::string key  = licGet("lic-k");
    std::string inst = m->getSavedValue<std::string>("lic-i", "");
    int64_t ts       = (int64_t)m->getSavedValue<int64_t>("lic-t", (int64_t)0);
    uint64_t sig     = (uint64_t)m->getSavedValue<int64_t>("lic-s", (int64_t)0);

    g_licTok = 0;
    if (key.empty()) {
        // A reason survives being signed out, because every route to the licence UI calls
        // licRefresh() first - so a plain "Not signed in." here would erase the one sentence
        // that explains what happened before the player ever reads it.
        std::string why = m->getSavedValue<std::string>("lic-why", std::string(""));
        g_licMsg = why.empty() ? "Not signed in." : why;
        return;
    }
    if (sig != licHash(key, inst, ts)) {   // cache tampered w/ or copied from elsewhere
        g_licMsg = "Saved sign-in is invalid. Please sign in again.";
        return;
    }
    // A timestamp in the FUTURE means the clock moved - a dual boot, a dead CMOS battery, a VM
    // snapshot - not that the cache is bad. Treating it as invalid signs a paying customer out
    // with no way back while offline. licTick revalidates on a large jump in either direction.
    int64_t age = licNow() - ts;
    if (age < 0) age = 0;
    if (ts <= 0) { g_licMsg = "Sign-in needs checking online."; return; }
#ifdef CI_DEV_UNLOCK
    if (licIsDev(key)) { g_licTok = LIC_OK; g_licMsg = "DEVELOPER UNLOCK - not a real license."; return; }
#endif
    if (age > LIC_GRACE)    { g_licMsg = "Please sign in again."; return; }
    // The UI must not claim a licence the data path is about to refuse. Without this, an install
    // that has not been online since the update reads "Signed in as you@example.com" while nothing
    // draws - which sends the buyer to support instead of to the sign-in button.
    if (!licGate()) {
        const int64_t bad = (int64_t)m->getSavedValue<int64_t>("lic-bad", (int64_t)0);
        g_licMsg = (bad > 0 && licNow() - bad < 3 * 86400)
            ? "Couldn't reach any Click Indicators server - the mod tried its backups too. That "
              "points at this PC or this network rather than the site: antivirus HTTPS scanning, "
              "a VPN or a proxy will all do it. Excluding Geometry Dash from your antivirus is "
              "the usual fix."
            : "Signed in - connect once to finish updating.";
        return;
    }
    g_licTok = LIC_OK;
    g_licMsg = fmt::format("Signed in as {}.", inst.empty() ? std::string("(unknown)") : inst);
}

// The posted decompile of v1.0.6 found the licence branch by the message text sitting in plain
// ASCII right beside the compare. These are assembled at runtime instead, so the binary carries
// no such label. This does not make the branch unpatchable - nothing purely local does - it
// only stops the file from advertising where the branch is.
static std::string licDeob(const unsigned char* d, size_t n) {
    std::string s; s.reserve(n);
    for (size_t i = 0; i < n; i++) s.push_back((char)(d[i] ^ 0x5Cu));
    return s;
}

// A second, independent answer to "is this licensed".
//
// licOK() reads a flag that licRefresh() sets, so the entire gate is one compare against one
// constant: patch it once and all fifteen call sites open together, which is exactly what the
// posted listing was doing. This one is recomputed from the stored credential on every call,
// and it is wired into the data path rather than the UI - so a build with the flag forced true
// still parses no macros and draws no cues, and has to defeat this separately.
//
// It re-derives rather than re-reads: the saved signature is checked here too, so a hand-edited
// or copied save file fails this even where it satisfied licRefresh earlier in the session.
// Deliberately shares no state with g_licTok.
// ANSWER THIS ONCE, NOT SIXTY TIMES A SECOND.
//
// guideActive() and postUpdate both ask this every frame, and every ask ran two DPAPI decryptions,
// three saved-value lookups and a full Ed25519 signature verification. That is a syscall and a
// scalar multiplication per frame to re-derive an answer that changes when somebody types in a
// licence key - which is to say, almost never. It is why the mod costs frames on a level with no
// macro loaded at all: none of this work has anything to do with drawing anything.
//
// The check is not weakened by being cached. It still runs, it still has to pass, and it re-runs
// every ten seconds and immediately whenever the licence state is touched - licRefresh bumps the
// generation, so entering a key takes effect on the next frame rather than in ten seconds.
static bool licGateReal();

static bool licGate() {
    static bool  val = false;
    static int64_t at = -1;
    static int   gen = -1;
    const int64_t now = licNow();
    if (gen != g_licGen || at < 0 || now - at >= 10 || now < at) {
        val = licGateReal();
        at = now; gen = g_licGen;
    }
    return val;
}

static bool licGateReal() {
    auto* m = Mod::get();
    std::string key = licGet("lic-k");
    if (key.size() < 8) return false;
#ifdef CI_DEV_UNLOCK
    // The dev key never carries a server signature, so without this a local test build would
    // pass licOK() and fail here. Compiled out of every shipped binary - CI hard-fails if this
    // is ever on - so it cannot become a bypass in a customer's copy.
    if (licIsDev(key)) return true;
#endif
    std::string inst = m->getSavedValue<std::string>("lic-i", std::string(""));
    int64_t ts       = (int64_t)m->getSavedValue<int64_t>("lic-t", (int64_t)0);
    uint64_t sig     = (uint64_t)m->getSavedValue<int64_t>("lic-s", (int64_t)0);
    if (ts <= 0 || sig != licHash(key, inst, ts)) return false;

    // The part no save file can satisfy. A grant is 56 bytes the SERVER signed, binding an expiry
    // to this exact token; verifying it needs only the public key above, and producing one needs a
    // private key that is not in this binary, on this machine, or anywhere a customer can reach.
    // Everything above this line is still checked, because it costs nothing and catches a
    // half-edited save - but on its own it never proved anything, and it is not what decides here.
    {
        const std::string grant = licGet("lic-g");
        cicrypt::Grant g;
        if (cicrypt::grantVerify(grant, key, kLicPub, g)) {
            const int64_t nowT = licNow();
            // Same clock tolerance as everywhere else: a future timestamp is a dual boot or a dead
            // CMOS battery, not evidence of anything.
            if (g.expires > nowT) return true;
            // An expired grant still counts inside the offline grace, so a fortnight away from the
            // internet does not cost someone the thing they paid for.
            if (nowT - g.expires <= LIC_GRACE) return true;
            return false;
        }
        // No grant, or one that does not verify. Before the deadline this is an install that has
        // simply not been online since the update; after it, it is the answer.
        if (licNow() >= LIC_LEGACY_UNTIL) return false;
    }
    // A future timestamp means the clock moved (dual boot, dead CMOS, VM snapshot), not that the
    // cache is bad - same reading licRefresh takes, so the two never disagree about a live licence.
    int64_t age = licNow() - ts;
    if (age < 0) age = 0;
    return age <= LIC_GRACE;
}

/* ---- in-mod updating ----------------------------------------------------------------------
 *
 * Click Indicators is not on the Geode index, so the loader's own updater never sees it. Without
 * this a buyer learns about a release from a Discord post, or never - which is how most of them
 * ended up still running an old build. One unauthenticated GET of /api/release on the first menu
 * of a launch, and it says nothing at all unless there is genuinely something newer: an updater
 * that nags on every launch gets turned off, and then it is not an updater.
 */
static bool g_updChecked = false;
static bool g_updBusy    = false;
static std::string g_updVer;              // newer version on the server, empty when up to date
static std::string g_updNotes;
static size_t g_updSize = 0;              // announced by /api/release, checked after download

// "v1.0.7" / "1.0.7" -> {1,0,7}. Anything unparseable returns false and is treated as "no
// update": telling a paying customer to install something we cannot even name is worse than
// staying quiet, and a tagged version like 1.0.7-beta lands here too.
static bool verParse(std::string s, int out[3]) {
    if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s.erase(0, 1);
    out[0] = out[1] = out[2] = 0;
    int n = 0; size_t p = 0;
    while (n < 3) {
        size_t q = s.find('.', p);
        std::string part = (q == std::string::npos) ? s.substr(p) : s.substr(p, q - p);
        if (part.empty() || part.size() > 6) return false;
        for (char c : part) if (c < '0' || c > '9') return false;
        out[n++] = std::atoi(part.c_str());
        if (q == std::string::npos) break;
        p = q + 1;
    }
    return n > 0;
}
static bool verNewer(std::string const& server, std::string const& mine) {
    int a[3], b[3];
    if (!verParse(server, a) || !verParse(mine, b)) return false;
    for (int i = 0; i < 3; i++) if (a[i] != b[i]) return a[i] > b[i];
    return false;
}

static void updInstall() {
    if (g_updBusy) return;
    g_updBusy = true;
    Notification::create("Click Indicators: downloading update...", NotificationIcon::Loading)->show();
    size_t want = g_updSize;
    std::string wantVer = g_updVer;
    (void)geode::async::spawn(
        cgweb::WebRequest().userAgent(ciUserAgent())
            .header("Authorization", "Bearer " + licToken())
            .timeout(std::chrono::seconds(180)).get(ciApi("mod/download")),
        [want, wantVer](cgweb::WebResponse res) {
            g_updBusy = false;
            if (!res.ok()) {
                Notification::create(fmt::format("Update failed: {}", dlErrorText(res.code())),
                                     NotificationIcon::Error)->show();
                return;
            }
            auto const& body = res.data();
            // Overwriting a working install with something the loader cannot open takes the mod
            // out entirely and leaves the customer no way back except a manual reinstall - so
            // check before writing, not after. A .geode is a zip: PK\x03\x04. An HTML error page
            // or a truncated body fails here rather than on next launch.
            if (body.size() < 4 || body[0] != 'P' || body[1] != 'K' || body[2] != 0x03 || body[3] != 0x04) {
                Notification::create("Update failed - the server didn't send a mod file",
                                     NotificationIcon::Error)->show();
                return;
            }
            // A truncation guard, and only that. An earlier version of this comment called it a
            // cross-check against the size /api/release announced, as though the two vouched for
            // each other - they do not. Both come from the same Worker behind the same TLS, so
            // anything able to lie about one can lie about the other just as easily. What it does
            // catch is a download that stopped early, which is the failure that actually happens.
            if (want > 0 && body.size() != want) {
                Notification::create("Update failed - the download was incomplete",
                                     NotificationIcon::Error)->show();
                return;
            }
            // Refuse bytes that are not the version we were told about. Not a security control for
            // the reason above; it catches release.js and the bundled blob drifting apart, which
            // would otherwise install silently and report the wrong version forever.
            auto got = res.header("x-ci-version");
            std::string gotVer = got ? std::string(*got) : std::string("");
            if (!wantVer.empty() && !gotVer.empty() && gotVer != wantVer) {
                log::warn("[Click Indicators] update version mismatch: asked {}, served {}",
                          wantVer, gotVer);
                Notification::create("Update failed - the server sent a different version",
                                     NotificationIcon::Error)->show();
                return;
            }
            auto path = Mod::get()->getPackagePath();
            // writeBinarySafe, NOT writeBinary. writeBinary opens with CREATE_ALWAYS, which
            // truncates the live .geode to zero the moment the handle opens - so a 1.7MB write
            // that fails part way (disk full, an antivirus filter rejecting the write, the game
            // crashing) leaves a partial package that Geode cannot load. That takes the mod out
            // entirely, including the sign-in and the macro browser, and the message below would
            // then be telling the customer to reinstall using a path that no longer exists. The
            // safe variant writes alongside and renames, so a failure leaves the working copy
            // untouched.
            auto wr = geode::utils::file::writeBinarySafe(path, body);
            if (!wr.isOk()) {
                log::warn("[Click Indicators] update write failed: {}", wr.unwrapErr());
                Notification::create("Update couldn't be saved - your current version is fine, "
                                     "get the new one from clickindicators.com",
                                     NotificationIcon::Error)->show();
                return;
            }
            Notification::create("Update installed - restart GD to use it",
                                 NotificationIcon::Success)->show();
        });
}

static void updPrompt() {
    if (g_updVer.empty()) return;
    geode::createQuickPopup(
        "Click Indicators",
        fmt::format("<cg>{}</c> is available.\n\n{}", g_updVer,
                    g_updNotes.empty() ? std::string("Install it now?") : g_updNotes),
        "Later", "Update",
        [](FLAlertLayer*, bool update) { if (update) updInstall(); });
}

static void updCheck() {
    if (g_updChecked) return;
    g_updChecked = true;
    // Only for people who can actually use it. An unlicensed copy being told to update is
    // noise, and the download would refuse it anyway.
    if (!licOK() || !licGate()) return;
#ifdef GEODE_IS_IOS
    // The in-app updater cannot work on iOS. Geode installs an updated .geode by unzipping it and
    // then renaming getModRuntimeDir()/binaries/<id>.ios.dylib over the freshly-unzipped binary -
    // and that binaries/ directory is only ever populated by extractBinary(), which the app itself
    // runs at launch. On a JIT-less ("patchless") install, which is how most of the iOS audience
    // runs, that path is not taken, so the update would rename a file that does not exist over the
    // one that does. Offering the update at all would be offering to break the install.
    return;
#endif
    (void)geode::async::spawn(
        cgweb::WebRequest().userAgent(ciUserAgent()).timeout(std::chrono::seconds(15))
            .get(ciApi("release")),
        [](cgweb::WebResponse res) {
            if (!res.ok()) return;              // offline is not an error worth reporting here
            auto jr = res.json();
            if (jr.isErr()) return;
            auto j = std::move(jr).unwrap();
            if (!j.contains("latest")) return;
            auto const& L = j["latest"];
            std::string ver = L.contains("version") ? jstr(L["version"]) : std::string("");
            if (ver.empty()) return;
            std::string mine = Mod::get()->getVersion().toNonVString(false);
            if (!verNewer(ver, mine)) return;
            g_updVer  = ver;
            g_updSize = L.contains("size") ? (size_t)L["size"].asInt().unwrapOr(0) : 0;
            if (L.contains("notes")) {
                auto arr = L["notes"].asArray();
                if (arr.isOk()) {
                    int shown = 0;
                    for (auto const& n : arr.unwrap()) {
                        if (shown++ >= 3) break;
                        // FLAlertLayer parses markup, so server text goes in filtered and capped.
                        // These notes are ours, but "it is our own server" is how a popup ends up
                        // rendering whatever the server says - and this one has a button on it.
                        std::string line = jstr(n).substr(0, 160);
                        for (char& c : line) if (c == '<' || c == '>') c = ' ';
                        g_updNotes += "- " + line + "\n";
                    }
                }
            }
            updPrompt();
        });
}

// A grant that arrives but does not verify is not an outage.
//
// The people cracking this attacked the network layer - the screenshot they posted was DNS traffic
// to their own router while the mod sat on "TIMED OUT". Signed grants already make that pointless,
// because whatever answers cannot produce one. What was missing is that the mod could not TELL the
// two apart: a redirected API and a dead connection both ended up in the same silent retry, and a
// forged reply would have been stored and then quietly rejected later, leaving someone "signed in"
// with nothing working.
//
// So a reply carrying a grant this build cannot verify is dropped rather than stored - the good one
// already held stays - and it is recorded, because it is the only signal that says someone is
// standing in the middle rather than the internet being slow.
static bool licGrantOK(std::string const& grant, std::string const& token) {
    if (grant.empty()) return true;                 // an older deployment; nothing to check
    cicrypt::Grant g;
    if (cicrypt::grantVerify(grant, token, kLicPub, g)) return true;
    Mod::get()->setSavedValue<int64_t>("lic-bad", (int64_t)licNow());
    log::warn("[Click Indicators] a sign-in reply arrived that this build cannot verify. Either "
              "something is standing between this PC and the server, or the signing key was "
              "rotated and this build predates it.");
    return false;
}

static void licStore(std::string const& key, std::string const& inst,
                     std::string const& grant = std::string(),
                     std::string const& vault = std::string()) {
    auto* m = Mod::get();
    int64_t ts = licNow();
    // An empty grant means the server did not send one - an older deployment, or the signing key
    // not configured yet. Keep whatever is already stored rather than throwing a good grant away
    // over a response that simply did not mention it.
    if (!grant.empty()) m->setSavedValue<std::string>("lic-g", licProtect(grant));
    if (!vault.empty()) m->setSavedValue<std::string>("lic-v", licProtect(vault));
    m->setSavedValue<std::string>("lic-k", licProtect(key));
    m->setSavedValue<std::string>("lic-i", inst);
    m->setSavedValue<int64_t>("lic-t", ts);
    m->setSavedValue<int64_t>("lic-s", (int64_t)licHash(key, inst, ts));
    m->setSavedValue<std::string>("lic-why", std::string(""));   // signed in again, old reason is spent
    licVaultSync();
    licRefresh();
}

static void licClear() {
    auto* m = Mod::get();
    m->setSavedValue<std::string>("lic-k", std::string(""));
    m->setSavedValue<std::string>("lic-i", std::string(""));
    m->setSavedValue<int64_t>("lic-t", (int64_t)0);
    m->setSavedValue<int64_t>("lic-s", (int64_t)0);
    m->setSavedValue<std::string>("lic-g", std::string(""));
    // The vault key is NOT cleared. Signing out and back in on the same account gives the same key,
    // and wiping it here would make every macro already downloaded unreadable for the sake of a
    // sign-out the customer may have done by accident.
    // Cleared here and set again by the caller that knows why, so pressing Sign out
    // yourself never shows a stale "signed in on another device".
    m->setSavedValue<std::string>("lic-why", std::string(""));
    licRefresh();
}

// Why the last sign-in request failed, so the player can be told something they can act on.
// "Couldn't reach the server" is true and useless: three separate support reports today turned
// out to be antivirus HTTPS inspection or a blocked host, and none of them is guessable from
// that sentence. The macro downloader already learned this lesson; sign-in had not.
static int g_licNetCode = 0;

static std::string licNetMsg(int code) {
    switch (code) {
        case -6:  return "Can't find the server - DNS problem on your connection.";
        case -7:  return "Can't connect - firewall blocking Geometry Dash?";
        case -28: return "Timed out. Antivirus HTTPS scanning is the usual cause.";
        // DO NOT BLAME THE CUSTOMER'S MACHINE FOR A TLS FAILURE.
        //
        // These two are curl's SSL_CONNECT_ERROR and PEER_FAILED_VERIFICATION. Antivirus HTTPS
        // inspection does cause them - but so does the server's own certificate being absent,
        // expired or mid-renewal, and that fails for EVERY customer at once while this sentence
        // sends all of them to turn off their antivirus. It happened: the site served plain HTTP
        // and could not complete a handshake, and the support load was people disabling security
        // software that was working correctly.
        //
        // So say both, in the order that respects the customer's time: check whether it is us
        // first, because that is the one they cannot see and the one that is broken for everybody.
        case -35:
        case -60: return "Secure connection failed. If other people can sign in, antivirus HTTPS "
                         "scanning or a proxy on this PC is the usual cause - otherwise the "
                         "server's certificate is at fault and it is being fixed.";
        case -56: return "Connection dropped part way through.";
        default: break;
    }
    if (code >= 500) return fmt::format("Server problem ({}), try again shortly.", code);
    if (code < 0)    return fmt::format("Network error {} - check firewall and antivirus.", -code);
    return "Couldn't reach the server.";
}

// Sign-in and the licence recheck. This is the request that strands people: if it cannot get
// out, the mod is inert no matter how healthy the server is, and "blocked by antivirus" and
// "server is down" look identical from here. So a transport failure is not the end - it walks
// the rest of CI_HOSTS before giving up, and the first host that answers is remembered.
//
// Only transport failures rotate. A 401 is a real answer from a real server and must be shown to
// the player as-is; retrying it elsewhere would just ask three servers the same question and
// slow the wrong password down to a minute.
// triesLeft counts hosts, not retries of one host, and is what makes the recursion terminate:
// each transport failure spends one and moves to the next name in CI_HOSTS, wrapping, so at
// worst every host is attempted exactly once before the player is told it failed.
static void licPostFrom(int hostIndex, int triesLeft, const char* endpoint, std::string jsonBody,
                        std::function<void(bool, bool, matjson::Value)> cb) {
    std::string url = ciApiOn(hostIndex, std::string("mod/") + endpoint);
    (void)geode::async::spawn(
        cgweb::WebRequest()
            .header("Accept", "application/json")
            .header("Content-Type", "application/json")
            .bodyString(jsonBody)
            .timeout(std::chrono::seconds(20))
            .post(url),
        [cb, hostIndex, triesLeft, endpoint, jsonBody](cgweb::WebResponse res) {
            if (ciTransportFailed(res) && triesLeft > 1) {
                int next = (hostIndex + 1) % CI_HOST_COUNT;
                geode::log::info("[CI-NET] {} unreachable ({}), trying {}",
                                 CI_HOSTS[hostIndex], res.code(), CI_HOSTS[next]);
                licPostFrom(next, triesLeft - 1, endpoint, jsonBody, cb);
                return;
            }
            if (ciTransportFailed(res)) {
                geode::log::warn("[CI-NET] every host unreachable for mod/{}", endpoint);
                g_licNetCode = res.code();
                cb(false, false, matjson::Value());
                return;
            }
            ciSetHost(hostIndex);   // this one answers - remember it for everything else
            g_licNetCode = res.code();   // kept for licNetMsg; a bare failure tells nobody anything
            bool httpOk = res.ok();   // a 500 / 404 / rate limit is an outage, not a verdict
            auto j = res.json();
            if (j.isErr()) { cb(false, httpOk, matjson::Value()); return; }
            cb(true, httpOk, std::move(j).unwrap());
        });
}

static void licPost(const char* endpoint, std::string jsonBody,
                    std::function<void(bool, bool, matjson::Value)> cb) {
    licPostFrom(ciHostIndex(), CI_HOST_COUNT, endpoint, std::move(jsonBody), std::move(cb));
}

static std::string licErrOf(matjson::Value const& j, const char* fallback) {
    if (j.contains("error")) {
        auto e = j["error"].asString();
        if (e.isOk() && !e.unwrap().empty()) return e.unwrap();
    }
    return fallback;
}

static std::string jsonEscape(std::string const& in) {
    std::string o;
    for (char c : in) {
        if (c == '\"' || c == '\\') { o += '\\'; o += c; }
        else if (c == '\n' || c == '\r' || c == '\t') o += ' ';
        else o += c;
    }
    return o;
}

static void licLogin(std::string email, std::string password,
                     std::function<void(bool, std::string)> done) {
    if (email.find('@') == std::string::npos) { done(false, "Enter your email address."); return; }
    if (password.empty()) { done(false, "Enter your password."); return; }
    if (g_licBusy) { done(false, "Already signing in, hold on."); return; }
    g_licBusy = true;
    // per install id so re-signin doesnt burn another slot
    auto* mm = Mod::get();
    std::string dev = mm->getSavedValue<std::string>("dev-id", "");
    if (dev.empty()) {
        static const char* HEX = "0123456789abcdef";
        dev = "GD-";
        for (int i = 0; i < 12; i++) dev += HEX[(int)(rand() % 16)];
        mm->setSavedValue<std::string>("dev-id", dev);
    }
    std::string body = fmt::format("{{\"email\":\"{}\",\"password\":\"{}\",\"device\":\"{}\"}}",
                                   jsonEscape(email), jsonEscape(password), jsonEscape(dev));
    licPost("login", body, [email, done](bool reached, bool httpOk, matjson::Value j) {
        (void)httpOk;   // login must still surface a 401's error text to the user
        g_licBusy = false;
        if (!reached) { g_licMsg = licNetMsg(g_licNetCode); done(false, g_licMsg); return; }
        bool ok = j.contains("ok") && j["ok"].asBool().unwrapOr(false);
        if (!ok) {
            g_licMsg = licErrOf(j, "Sign in failed.");
            done(false, g_licMsg);
            return;
        }
        std::string tok = j.contains("token") ? j["token"].asString().unwrapOr("") : "";
        if (tok.empty()) { g_licMsg = "Server sent no token."; done(false, g_licMsg); return; }
        std::string grant = j.contains("grant") ? j["grant"].asString().unwrapOr("") : "";
        std::string vault = j.contains("vault") ? j["vault"].asString().unwrapOr("") : "";
        if (!licGrantOK(grant, tok)) {
            g_licMsg = "Couldn't verify the reply from the server. If you are on a VPN, a proxy, "
                       "or antivirus with HTTPS scanning, turn it off and try again.";
            done(false, g_licMsg);
            return;
        }
        licStore(tok, email, grant, vault);
        done(true, "Signed in.");
    });
}

// Backoff after a check that could not reach a verdict, held in memory only. lic-t is
// deliberately untouched: it carries the signature and drives the offline grace, so writing it
// here would either extend that grace on a failure or cut it short. Without this, an outage
// means every death fires another request - a player grinding one level is a request every few
// seconds, from everyone at once, for as long as the outage lasts. One good response clears it.
static int64_t g_licNextTry = 0;
static int     g_licFails   = 0;

static void licValidate(std::function<void(bool, std::string)> done) {
    auto* m = Mod::get();
    std::string tok = licGet("lic-k");
    if (tok.empty()) { if (done) done(false, "Not signed in."); return; }
    if (licIsDev(tok)) { licRefresh(); if (done) done(licOK(), g_licMsg); return; }
    if (g_licBusy) return;
    g_licBusy = true;
    std::string email = m->getSavedValue<std::string>("lic-i", "");
    licPost("check", fmt::format("{{\"token\":\"{}\"}}", jsonEscape(tok)),
        [tok, email, done](bool reached, bool httpOk, matjson::Value j) {
            g_licBusy = false;
            // Sign out, or a sign-in as someone else, can land while this is in flight - and
            // now does far more often, since this fires at every launch and every half hour
            // rather than once a day. The reply is about a token this install no longer holds,
            // so writing it back would resurrect the licence the user just cleared.
            if (licGet("lic-k") != tok) {
                licRefresh();
                if (done) done(licOK(), g_licMsg);
                return;
            }
            // A licence is only ever revoked by a server that authored a revocation. A 500, a 404,
            // a rate limit or a body we could not parse is an outage - and licClear() destroys the
            // token, so failing closed here signs out every paying customer for the length of the
            // outage AND leaves the offline grace period nothing to stand on. The API answers
            // ok:true for both genuine cases (device signed out, subscription lapsed), so gating
            // on ok is exact.
            bool authoritative = reached && httpOk && j.contains("ok")
                              && j["ok"].asBool().unwrapOr(false);
            if (!authoritative) {
                // 1, 2, 4 ... capped at 30 min, so an outage costs each install a handful of
                // requests rather than one per death.
                if (g_licFails < 16) g_licFails++;
                int64_t back = (int64_t)60 << (g_licFails > 5 ? 5 : g_licFails - 1);
                g_licNextTry = licNow() + (back > 1800 ? 1800 : back);
                licRefresh();   // stay signed in on the cached token until the grace runs out
                if (done) done(licOK(), licNetMsg(g_licNetCode));
                return;
            }
            g_licFails = 0; g_licNextTry = 0;
            bool valid = j.contains("valid") && j["valid"].asBool().unwrapOr(false);
            if (valid) {
                // Every check renews the grant, so an install that is online at all never comes
                // near the 21-day expiry - and one whose account was refunded or signed out stops
                // being renewed and expires on its own.
                std::string grant = j.contains("grant") ? j["grant"].asString().unwrapOr("") : "";
                std::string vault = j.contains("vault") ? j["vault"].asString().unwrapOr("") : "";
                // Dropped, not stored. Keeping the grant already held means an interception costs
                // the player nothing until their real one runs out, and costs whoever is doing the
                // intercepting everything.
                if (!licGrantOK(grant, tok)) grant.clear();
                licStore(tok, email, grant, vault);   // bump ts, extends grace
                if (done) done(true, "Signed in.");
            } else {
                // Keep the server's sentence: it is the difference between "the mod broke"
                // and "your licence moved to another PC, sign in again to bring it back".
                // Stored because licClear() wipes the live copy, and announced now because
                // otherwise it waits to be discovered in a menu the player may never open.
                std::string why = licErrOf(j, "You've been signed out.");
                licClear();
                Mod::get()->setSavedValue<std::string>("lic-why", why);
                g_licMsg = why;
                Notification::create(g_licMsg, NotificationIcon::Warning)->show();
                if (done) done(false, g_licMsg);
            }
        });
}

static void licTick() {
    auto* m = Mod::get();
    std::string k = licGet("lic-k");
    if (k.empty() || licIsDev(k)) return;
    int64_t nowS = licNow();
    if (g_licNextTry > 0 && nowS < g_licNextTry) return;
    int64_t ts = (int64_t)m->getSavedValue<int64_t>("lic-t", (int64_t)0);
    int64_t d = nowS - ts;
    if (d < 0) d = -d;   // a backwards clock jump must TRIGGER a recheck, not suppress one forever
    if (d < LIC_RECHECK) return;
    licValidate(nullptr);
}

class LicensePopup : public geode::Popup {
protected:
    geode::TextInput* m_input = nullptr;
    geode::TextInput* m_pass = nullptr;
    CCLabelBMFont* m_status = nullptr;
    CCLabelBMFont* m_showLbl = nullptr;
    bool m_showPass = false;
    bool m_alive = true;

    bool initContent() {
        if (!Popup::init(380.f, 250.f)) return false;
        this->setTitle("Click Indicators");
        // The X is placed by Geode with addChildAtPosition(Anchor::TopLeft), which writes
        // AnchorLayoutOptions on the button. setPosition does not touch those options, so a
        // nudge here is a transient that the next layout pass over m_buttonMenu silently
        // reverts - and while it holds it floats the X inside the panel instead of straddling
        // the corner, which is exactly the button 'sitting weirdly'. Let the anchor place it.
        licRefresh();

        // Built at runtime - see licDeob. In v1.0.6 these two sat in the binary as plain ASCII
        // immediately beside the compare, and that is how the branch was found and posted.
        static const unsigned char kYes[] = {
            0x05,0x33,0x29,0x7b,0x2e,0x39,0x7c,0x2f,0x35,0x3b,0x32,0x39,0x38,0x7c,0x35,0x32,0x72,0x5c };
        static const unsigned char kNo[]  = {
            0x0f,0x35,0x3b,0x32,0x7c,0x35,0x32,0x7c,0x2b,0x35,0x28,0x34,0x7c,0x25,0x33,0x29,0x2e,0x7c,
            0x1f,0x30,0x35,0x3f,0x37,0x7c,0x15,0x32,0x38,0x35,0x3f,0x3d,0x28,0x33,0x2e,0x2f,0x7c,0x3d,
            0x3f,0x3f,0x33,0x29,0x32,0x28,0x72,0x5c };
        std::string subTxt = licOK() ? licDeob(kYes, sizeof(kYes) - 1)
                                     : licDeob(kNo,  sizeof(kNo)  - 1);
        auto sub = CCLabelBMFont::create(subTxt.c_str(), "bigFont.fnt");
        sub->setScale(0.38f); sub->setPosition({ 190.f, 198.f });
        sub->setColor({ 200, 206, 218 });
        m_mainLayer->addChild(sub);

        m_input = geode::TextInput::create(290.f, "Email", "bigFont.fnt");
        m_input->setMaxCharCount(120);
        m_input->setFilter("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789@._+-");
        m_input->setPosition({ 190.f, 168.f });
        auto lastEmail = Mod::get()->getSavedValue<std::string>("lic-i", "");
        if (!lastEmail.empty()) m_input->setString(lastEmail);
        m_mainLayer->addChild(m_input);

        m_pass = geode::TextInput::create(230.f, "Password", "bigFont.fnt");
        m_pass->setMaxCharCount(120);
        // Every printable ASCII character, including space, backslash, quote and backtick.
        // The website's password box filters nothing, so a password set there containing any
        // character missing from this list could never be typed here - and here is the only
        // place it actually matters. Customers hit exactly that: signed in fine on the site,
        // failed in the game over and over, with no way to tell why.
        m_pass->setFilter(" !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`"
                          "abcdefghijklmnopqrstuvwxyz{|}~");
        m_pass->setPosition({ 160.f, 132.f });
        m_pass->setPasswordMode(true);
        m_mainLayer->addChild(m_pass);

        // Masking draws every character as a dot, and a dot is a glyph the font has to actually
        // have. Several players running GUI texture packs reported the password box refusing
        // input while the email box directly above it took text fine - and password mode is the
        // only difference between the two, so a replaced bigFont.fnt missing that glyph fits
        // exactly. This is the control that gets them in, and it earns its place regardless:
        // nobody can proofread a password they cannot see.
        auto pmenu = CCMenu::create();
        pmenu->setPosition({ 318.f, 132.f });
        pmenu->setContentSize({ 70.f, 30.f });
        m_mainLayer->addChild(pmenu);
        m_showLbl = CCLabelBMFont::create("Show", "bigFont.fnt");
        m_showLbl->setScale(0.42f);
        pmenu->addChild(geode::cocos::CCMenuItemExt::createSpriteExtra(m_showLbl,
            [this](CCMenuItemSpriteExtra*) {
                m_showPass = !m_showPass;
                if (m_pass) {
                    // Put the text back if switching modes drops it. Cheap insurance - losing a
                    // half-typed password to the button meant to help you type it would be worse
                    // than the problem.
                    std::string cur = m_pass->getString();
                    m_pass->setPasswordMode(!m_showPass);
                    if (m_pass->getString() != cur) m_pass->setString(cur);
                }
                if (m_showLbl) m_showLbl->setString(m_showPass ? "Hide" : "Show");
            }));
        pmenu->setLayout(geode::RowLayout::create());

        m_status = CCLabelBMFont::create(g_licMsg.c_str(), "bigFont.fnt");
        m_status->setScale(0.32f); m_status->setPosition({ 190.f, 104.f });
        m_status->setColor(licOK() ? ccColor3B{ 120, 230, 130 } : ccColor3B{ 170, 176, 190 });
        m_mainLayer->addChild(m_status);

        auto menu = CCMenu::create();
        menu->setPosition({ 190.f, 76.f });
        menu->setContentSize({ 340.f, 40.f });
        m_mainLayer->addChild(menu);
        auto mk = [](const char* label, const char* tex, std::function<void()> cb) {
            auto spr = ButtonSprite::create(label, "bigFont.fnt", tex, 0.8f);
            spr->setScale(0.6f);
            return geode::cocos::CCMenuItemExt::createSpriteExtra(spr,
                [cb](CCMenuItemSpriteExtra*) { cb(); });
        };
        menu->addChild(mk("Sign in", "GJ_button_01.png", [this] { onActivate(); }));
        menu->addChild(mk("Buy", "GJ_button_05.png", [] {
            geode::utils::web::openLinkInBrowser("https://clickindicators.com");
        }));
        menu->addChild(mk("Discord", "GJ_button_04.png", [] {
            geode::utils::web::openLinkInBrowser("https://discord.gg/FncjJJNcES");
        }));
        menu->setLayout(geode::RowLayout::create()->setGap(9.f));

        auto note = CCLabelBMFont::create("Buy at clickindicators.com, then sign in here.", "bigFont.fnt");
        note->setScale(0.26f); note->setColor({ 110, 118, 134 });
        note->setPosition({ 190.f, 40.f });
        m_mainLayer->addChild(note);
        return true;
    }

    void say(std::string const& msg, bool good) {
        if (!m_alive || !m_status) return;
        m_status->setString(msg.c_str());
        m_status->setColor(good ? ccColor3B{ 120, 230, 130 } : ccColor3B{ 255, 120, 120 });
    }

    void onActivate() {
        if (!m_input || !m_pass) return;
        say("Signing in...", true);
        Ref<LicensePopup> self = this;
        licLogin(m_input->getString(), m_pass->getString(), [self](bool ok, std::string msg) {
            self->say(msg, ok);
            if (ok) {
                Notification::create("Click Indicators unlocked", NotificationIcon::Success)->show();
            }
        });
    }

    void onClose(CCObject* s) override { m_alive = false; Popup::onClose(s); }

public:
    static LicensePopup* create() {
        auto r = new LicensePopup();
        if (r->initContent()) { r->autorelease(); return r; }
        delete r;
        return nullptr;
    }
};

class ClickGuidePopup : public geode::Popup {
protected:
    // 430x300 was 94% of GD's 320-tall design canvas, and 430 wide is more than a 4:3 canvas has
    // (426.7) - so on a 4:3 phone the panel was wider than the screen. 400x280 leaves a margin of
    // real backdrop on every aspect ratio GD runs at.
    static constexpr float PW = 400.f, PH = 280.f;
    static constexpr float ROW_W = 340.f;
    static constexpr float PREV_PY = 6.f;
    static constexpr float PREV_PH = 132.f;
    static constexpr float PREV_H  = PREV_PY + PREV_PH;
    static constexpr float PREV_SCALE = 0.42f;
    int m_levelID = 0;
    // One list, groups that open and close, instead of six tabs. The tab axis was the defect: the
    // first tab held 25 of the 43 settings and had grown its own sub-headers, which is the list
    // saying out loud that the tab and the group were the same idea used twice. The eight gamemode
    // toggles were rendered on two different tabs because a tab model makes that possible; one
    // list with headers makes it impossible.
    std::string m_openGroup;   // at most one group open at a time - the header list stays an index
    geode::ScrollLayer* m_scroll = nullptr;
    CCDrawNode* m_preview = nullptr;
    CCLabelBMFont* m_prevName = nullptr;
    CCLabelBMFont* m_licLbl = nullptr;
    geode::TextInput* m_licInput = nullptr;
    float m_prevT = 0.f;

    static CCLabelBMFont* mkLabel(const char* txt, const char* font, float scale, ccColor3B col) {
        auto l = CCLabelBMFont::create(txt, font);
        l->setScale(scale); l->setColor(col); l->setAnchorPoint({ 0.f, 0.5f });
        return l;
    }
    CCNode* newRow(float h = 32.f) {
        auto row = CCNode::create();
        row->setContentSize({ ROW_W, h });
        return row;
    }
    void rowText(CCNode* row, const char* title, const char* desc) {
        float h = row->getContentSize().height;
        bool hasDesc = desc && *desc;
        auto t = mkLabel(title, "bigFont.fnt", 0.40f, { 255, 255, 255 });
        t->setPosition({ 6.f, hasDesc ? h * 0.5f + 7.f : h * 0.5f });
        row->addChild(t);
        if (hasDesc) {
            auto d = mkLabel(desc, "bigFont.fnt", 0.29f, { 165, 168, 180 });
            d->setPosition({ 6.f, h * 0.5f - 8.f });
            row->addChild(d);
        }
    }
    CCMenu* rowMenu(CCNode* row) {
        auto m = CCMenu::create();
        m->setPosition({ 0.f, 0.f });
        m->setContentSize(row->getContentSize());
        row->addChild(m);
        return m;
    }
    void push(CCNode* row) { m_scroll->m_contentLayer->addChild(row); }

    void addHeader(const char* txt) {
        auto row = newRow(22.f);
        auto l = mkLabel(txt, "bigFont.fnt", 0.34f, { 120, 200, 255 });
        l->setPosition({ 4.f, 8.f });
        row->addChild(l);
        push(row);
    }

    void addToggle(const char* key, const char* title, const char* desc) {
        auto row = newRow(); rowText(row, title, desc);
        std::string k = key;
        auto tgl = geode::cocos::CCMenuItemExt::createTogglerWithStandardSprites(0.5f,
            [k](CCMenuItemToggler*) {
                auto* m = Mod::get();
                m->setSettingValue<bool>(k, !m->getSettingValue<bool>(k));
            });
        tgl->toggle(Mod::get()->getSettingValue<bool>(key));
        tgl->setPosition({ ROW_W - 26.f, row->getContentSize().height * 0.5f });
        rowMenu(row)->addChild(tgl);
        push(row);
    }

    // safe mode gets a confirm both ways. too easy to mistap otherwise
    void addSafeModeToggle() {
        auto row = newRow();
        rowText(row, "Safe mode", "Nothing counts while this is on - no best %, completion, orbs or attempts");
        auto tgl = geode::cocos::CCMenuItemExt::createTogglerWithStandardSprites(0.5f,
            [](CCMenuItemToggler* t) {
                auto* m = Mod::get();
                bool cur = m->getSettingValue<bool>("safe-mode");
                bool want = !cur;
                Ref<CCMenuItemToggler> keep = t;   // popup may close before the dialog resolves
                // The toggler flips its own sprite as part of activation and does it AFTER this
                // callback returns, so putting it back here was undone a moment later - the box
                // showed the opposite of the setting until the row was rebuilt. Re-assert it next
                // frame instead, once activation is finished. Only the popup writes the setting.
                geode::queueInMainThread([keep, cur] {
                    if (keep->getParent()) keep->toggle(cur);
                });
                if (want) {
                    geode::createQuickPopup("Safe mode",
                        "Turn <cg>on</c> safe mode?\n\nNothing you do will count - no best percentage, "
                        "no completion, no stars, orbs or coins, and no attempts.",
                        "Cancel", "Turn on",
                        [keep](FLAlertLayer*, bool yes) {
                            if (!yes) return;
                            Mod::get()->setSettingValue<bool>("safe-mode", true);
                            keep->toggle(true);
                        });
                } else {
                    geode::createQuickPopup("Safe mode",
                        "Turn <cr>off</c> safe mode?\n\nYour runs will count again, <cy>including runs "
                        "played with the guide on screen</c>.\n\nThe attempt you are in now still will "
                        "not count - restart the level for one that does.",
                        "Cancel", "Turn off",
                        [keep](FLAlertLayer*, bool yes) {
                            if (!yes) return;
                            Mod::get()->setSettingValue<bool>("safe-mode", false);
                            keep->toggle(false);
                        });
                }
            });
        tgl->toggle(Mod::get()->getSettingValue<bool>("safe-mode"));
        tgl->setPosition({ ROW_W - 26.f, row->getContentSize().height * 0.5f });
        rowMenu(row)->addChild(tgl);
        push(row);
    }

    void addNumber(const char* key, const char* title, const char* desc,
                   double lo, double hi, double step, bool isInt, const char* suffix = "") {
        auto row = newRow(); rowText(row, title, desc);
        auto menu = rowMenu(row);
        float cy = row->getContentSize().height * 0.5f;
        std::string k = key, sfx = suffix;
        auto fmtVal = [k, isInt, sfx]() -> std::string {
            auto* m = Mod::get();
            double v = isInt ? (double)m->getSettingValue<int64_t>(k) : m->getSettingValue<double>(k);
            return isInt ? fmt::format("{}{}", (int)v, sfx) : fmt::format("{:.2f}{}", v, sfx);
        };
        auto val = CCLabelBMFont::create(fmtVal().c_str(), "bigFont.fnt");
        val->setScale(0.4f); val->setPosition({ ROW_W - 52.f, cy });
        menu->addChild(val);
        auto arrow = [&](bool right) {
            auto s = CCSprite::createWithSpriteFrameName("GJ_arrow_01_001.png");
            s->setScale(0.42f);
            if (right) s->setFlipX(true);
            auto b = geode::cocos::CCMenuItemExt::createSpriteExtra(s,
                [k, lo, hi, step, isInt, right, val, fmtVal](CCMenuItemSpriteExtra*) {
                    auto* m = Mod::get();
                    double v = isInt ? (double)m->getSettingValue<int64_t>(k) : m->getSettingValue<double>(k);
                    v += right ? step : -step;
                    v = std::max(lo, std::min(hi, v));
                    if (isInt) m->setSettingValue<int64_t>(k, (int64_t)llround(v));
                    else       m->setSettingValue<double>(k, v);
                    val->setString(fmtVal().c_str());
                });
            b->setPosition({ ROW_W - (right ? 20.f : 84.f), cy });
            return b;
        };
        menu->addChild(arrow(false));
        menu->addChild(arrow(true));
        push(row);
    }

    // rebuild = the choice changes which rows belong on the tab. The rebuild is deferred a frame
    // because it deletes the very button being clicked, and cocos still touches that button after
    // the callback returns.
    void addChoice(const char* key, const char* title, const char* desc, std::vector<std::string> opts,
                   bool rebuild = false) {
        auto row = newRow(); rowText(row, title, desc);
        auto menu = rowMenu(row);
        float cy = row->getContentSize().height * 0.5f;
        std::string k = key;
        auto val = CCLabelBMFont::create(Mod::get()->getSettingValue<std::string>(key).c_str(), "bigFont.fnt");
        val->setScale(0.4f); val->setPosition({ ROW_W - 54.f, cy });
        menu->addChild(val);
        auto arrow = [&](bool right) {
            auto s = CCSprite::createWithSpriteFrameName("GJ_arrow_01_001.png");
            s->setScale(0.42f);
            if (right) s->setFlipX(true);
            auto b = geode::cocos::CCMenuItemExt::createSpriteExtra(s,
                [this, k, opts, right, val, rebuild](CCMenuItemSpriteExtra*) {
                    auto* m = Mod::get();
                    auto cur = m->getSettingValue<std::string>(k);
                    int n = (int)opts.size(), i = 0;
                    for (int j = 0; j < n; j++) if (opts[j] == cur) { i = j; break; }
                    i = right ? (i + 1) % n : (i - 1 + n) % n;
                    m->setSettingValue<std::string>(k, opts[i]);
                    val->setString(opts[i].c_str());
                    if (rebuild) {
                        this->retain();
                        geode::queueInMainThread([this] {
                            if (this->getParent()) this->rebuildList();
                            this->release();
                        });
                    }
                });
            b->setPosition({ ROW_W - (right ? 20.f : 88.f), cy });
            return b;
        };
        menu->addChild(arrow(false));
        menu->addChild(arrow(true));
        push(row);
    }

    void addButton(const char* title, const char* desc, const char* btnText,
                   std::function<void()> cb, const char* tex = "GJ_button_05.png") {
        auto row = newRow(34.f); rowText(row, title, desc);
        auto spr = ButtonSprite::create(btnText, "bigFont.fnt", tex, 0.8f);
        spr->setScale(0.46f);
        auto b = geode::cocos::CCMenuItemExt::createSpriteExtra(spr,
            [cb](CCMenuItemSpriteExtra*) { cb(); });
        b->setPosition({ ROW_W - 48.f, row->getContentSize().height * 0.5f });
        rowMenu(row)->addChild(b);
        push(row);
    }

    // same scene as the site. block wall, ground, spike, icon jumping. GD units (U)
    void addPreviewRow() {
        auto row = newRow(PREV_H + 22.f);
        const float U = PREV_PH / 7.2f, GND_H = U * 2.6f, GY = PREV_PY + GND_H;
        auto sky = CCLayerGradient::create(ccc4(0x3E, 0x62, 0xFF, 255), ccc4(0x1B, 0x2F, 0xB8, 255));
        sky->setContentSize({ ROW_W, PREV_PY + PREV_PH - GY });
        sky->setPosition({ 0.f, GY });
        row->addChild(sky);
        auto gnd = CCLayerGradient::create(ccc4(0x16, 0x24, 0x9E, 255), ccc4(0x05, 0x0A, 0x2B, 255));
        gnd->setContentSize({ ROW_W, GND_H });
        gnd->setPosition({ 0.f, PREV_PY });
        row->addChild(gnd);

        m_prevName = mkLabel("", "bigFont.fnt", 0.36f, { 255, 255, 255 });
        m_prevName->setPosition({ 8.f, PREV_PY + PREV_PH + 11.f });
        row->addChild(m_prevName);
        auto hint = mkLabel("click the moment it lands / turns green", "bigFont.fnt", 0.25f, { 125, 132, 150 });
        hint->setAnchorPoint({ 1.f, 0.5f });
        hint->setPosition({ ROW_W - 8.f, PREV_PY + PREV_PH + 11.f });
        row->addChild(hint);

        m_preview = CCDrawNode::create();
        applyAA(m_preview);
        row->addChild(m_preview, 5);
        push(row);
    }

    static void pvRect(CCDrawNode* n, float x, float y, float w, float h, ccColor4F col) {
        if (w <= 0.f || h <= 0.f) return;
        CCPoint p[4] = { ccp(x, y), ccp(x + w, y), ccp(x + w, y + h), ccp(x, y + h) };
        n->drawPolygon(p, 4, col, 0.f, ccColor4F{ 0, 0, 0, 0 });
    }
    static void pvRotRect(CCDrawNode* n, CCPoint c, float hw, float hh, float ang,
                          ccColor4F fill, float bw, ccColor4F border) {
        float ca = cosf(ang), sa = sinf(ang);
        const float sx[4] = { -1, 1, 1, -1 }, sy[4] = { -1, -1, 1, 1 };
        CCPoint p[4];
        for (int i = 0; i < 4; i++) {
            float x = sx[i] * hw, y = sy[i] * hh;
            p[i] = ccp(c.x + x * ca - y * sa, c.y + x * sa + y * ca);
        }
        n->drawPolygon(p, 4, fill, bw, border);
    }

    void drawPreview() {
        if (!m_preview) return;
        m_preview->clear();
        auto* mod = Mod::get();
        ccColor3B cc = mod->getSettingValue<ccColor3B>("line-color");
        float op = (float)mod->getSettingValue<double>("indicator-opacity");
        g_cueContrast = (float)mod->getSettingValue<double>("cue-contrast");
        g_cueOpacity = op;
        float cr = cc.r / 255.f, cg = cc.g / 255.f, cb = cc.b / 255.f;

        const float U = PREV_PH / 7.2f;
        const float GND_H = U * 2.6f, GY = PREV_PY + GND_H;
        const float TOPY = PREV_PY + PREV_PH;
        const float SPEED = U * 10.4f;   // GD 1x scroll rate
        const float cubeX = ROW_W * 0.30f;

        const float CYCLE = 2.6f, TP = 1.65f, JUMP = 0.62f;
        float cyc  = fmodf(m_prevT, CYCLE);
        bool armed = cyc >= TP - 0.05f && cyc <= TP + 0.13f;
        float frac = cyc >= TP ? 0.f : clmp((TP - cyc) / 1.30f, 0.f, 1.f);
        float ease = powf(frac, 0.6f);
        float hit  = cyc < TP ? 0.f : clmp((cyc - TP) / 0.32f, 0.f, 1.f);

        float jt = cyc - TP, lift = 0.f, rot = 0.f;
        if (jt > 0.f && jt < JUMP) {
            float u = jt / JUMP;
            lift = 4.f * (2.7f * U) * u * (1.f - u);
            rot  = -u * 3.14159265f;
        }
        float cubeCY = GY + U * 0.5f + lift;

        std::string m = mod->getSettingValue<std::string>("indicator-mode");
        if (m_prevName) m_prevName->setString(m.c_str());

        float cell = U * 1.5f, bandW = cell * 5.f;
        float par = fmodf(m_prevT * SPEED * 0.5f, bandW);
        static const float WALL[7][4] = {   // x,y,w,h in cells
            {0.f,0.f,2.f,2.f}, {2.f,0.f,3.f,1.f}, {2.f,1.f,1.f,2.f},
            {3.f,1.f,2.f,2.f}, {0.f,2.f,2.f,1.f}, {0.f,3.f,3.f,1.f}, {3.f,3.f,2.f,1.f},
        };
        float skyH = TOPY - GY, seam = std::max(1.6f, U * 0.09f);
        for (float ox = -par - bandW; ox < ROW_W + bandW; ox += bandW) {
            for (int i = 0; i < 7; i++) {
                float bx = ox + WALL[i][0] * cell, bw = WALL[i][2] * cell - seam;
                float bh = WALL[i][3] * (skyH / 4.f) - seam;
                float by = GY + WALL[i][1] * (skyH / 4.f);
                if (bx > ROW_W || bx + bw < 0.f) continue;
                float x0 = std::max(0.f, bx), x1 = std::min(ROW_W, bx + bw);
                if (x1 <= x0) continue;
                pvRect(m_preview, x0, by, x1 - x0, bh, ccColor4F{ 0.20f, 0.33f, 0.95f, 0.42f });
                pvRect(m_preview, x0, by + bh - std::max(1.f, U * 0.05f), x1 - x0,
                       std::max(1.f, U * 0.05f), ccColor4F{ 1.f, 1.f, 1.f, 0.06f });
            }
        }

        float sx0 = cubeX + SPEED * (TP + 0.31f - cyc);
        for (int k = -1; k <= 1; k++) {
            float sx = sx0 + k * SPEED * CYCLE;
            if (sx < -U * 2.f || sx > ROW_W + U * 2.f) continue;
            CCPoint t[3] = { ccp(sx - U * 0.5f, GY), ccp(sx, GY + U), ccp(sx + U * 0.5f, GY) };
            m_preview->drawPolygon(t, 3, ccColor4F{ 0.03f, 0.03f, 0.07f, 1.f },
                                   std::max(1.2f, U * 0.08f), ccColor4F{ 1.f, 1.f, 1.f, 0.95f });
        }

        pvRect(m_preview, 0.f, GY - U * 0.16f, ROW_W, U * 0.32f, ccColor4F{ 0.7f, 0.85f, 1.f, 0.18f });
        pvRect(m_preview, 0.f, GY - std::max(1.f, U * 0.055f), ROW_W,
               std::max(2.f, U * 0.11f), ccColor4F{ 1.f, 1.f, 1.f, 1.f });
        float step = U * 3.f, gsp = fmodf(m_prevT * SPEED, step);
        for (float gx = -gsp; gx < ROW_W; gx += step)
            pvRect(m_preview, gx, PREV_PY, 1.4f, GND_H, ccColor4F{ 1.f, 1.f, 1.f, 0.07f });

        for (int p = 0; p < 10; p++) {
            float age = fmodf(m_prevT * 2.1f + p * 0.1f, 1.f);
            float px = cubeX - U * 0.55f - age * U * 4.2f;
            float py = (lift > 0.f ? cubeCY - U * 0.42f : GY + U * 0.12f) + (p % 3) * U * 0.09f;
            float sz = (1.f - age) * U * 0.15f;
            if (sz < 0.6f || px < 0.f) continue;
            pvRect(m_preview, px, py, sz, sz, ccColor4F{ 0.75f, 0.94f, 1.f, (1.f - age) * 0.7f });
        }

        float hw = U * 0.5f, lw = std::max(1.f, U * 0.055f);
        ccColor4F ink{ 0.04f, 0.04f, 0.04f, 1.f }, face{ 0.25f, 0.82f, 0.96f, 1.f };
        pvRotRect(m_preview, ccp(cubeX, cubeCY), hw - lw * 0.5f, hw - lw * 0.5f, rot,
                  ccColor4F{ 0.95f, 0.66f, 0.11f, 1.f }, lw, ink);
        float ca = cosf(rot), sa = sinf(rot);
        auto local = [&](float lx, float ly) {
            return ccp(cubeX + lx * ca - ly * sa, cubeCY + lx * sa + ly * ca);
        };
        float ew = U * 0.150f * 0.5f;
        for (int sg = -1; sg <= 1; sg += 2)
            pvRotRect(m_preview, local(sg * U * 0.188f, U * 0.227f), ew, ew, rot, face, lw * 0.7f, ink);
        pvRotRect(m_preview, local(0.f, -U * 0.099f), U * 0.639f * 0.5f, U * 0.129f * 0.5f,
                  rot, face, lw * 0.7f, ink);

        int pm = (m == "ring") ? 1 : (m == "converge") ? 2 : (m == "pulse") ? 3 : (m == "highway") ? 4 : 0;
        auto fade = [op](ccColor4F c) { c.a *= op; return c; };
        auto fadeDark = [op](ccColor4F c) { c.a *= op * op * op; return c; };
        float cy = GY + U * 0.5f;
        if (pm == 4) {
            const float span = 1.6f;
            const float at[4] = { 0.f, 0.75f, 1.15f, 1.95f };
            const float hl[4] = { 0.f, 0.f, 0.30f, 0.f };
            float phase = cyc - TP;
            bool m_prevHold = false;
            HwNote nn[4];
            int nc = 0;
            for (int i = 0; i < 4; i++) {
                float ld = at[i] - phase;
                if (ld + hl[i] < -0.05f || ld > span) continue;
                nn[nc].lead = ld;
                nn[nc].hold = hl[i];
                nn[nc].armed = ld <= 0.05f;
                nn[nc].done = ld < -0.02f;
                if (hl[i] > 0.02f && ld <= 0.f && ld + hl[i] > 0.f) m_prevHold = true;
                nc++;
            }
            // The preview box is fixed, so position and scale do not apply here - but the guide
            // lines do, since that is the one you want to see before committing to it.
            drawHighway(m_preview, ccp(U * 1.7f, PREV_PY + U * 0.45f), U * 1.55f, PREV_PH - U * 0.95f,
                        span, nn, nc, m_prevT, m_prevHold, cr, cg, cb, op,
                        Mod::get()->getSettingValue<bool>("hw-guides"));
        } else if (pm) {
            drawCue(m_preview, pm, ccp(cubeX, cy), PREV_PY + U * 0.4f, TOPY - U * 0.4f,
                    ease, armed, PREV_SCALE, cr, cg, cb, op);
        } else {
            float lx = cubeX + (ROW_W - cubeX - U) * frac;
            pvRect(m_preview, lx - U * 0.34f, PREV_PY + U * 0.4f, U * 0.68f,
                   TOPY - PREV_PY - U * 0.8f,
                   armed ? fade({ 0.2f, 0.94f, 0.54f, 0.5f }) : fade({ cr, cg, cb, 0.17f }));
            drawSegOL(m_preview, ccp(lx, PREV_PY + U * 0.4f), ccp(lx, TOPY - U * 0.4f),
                      armed ? 3.2f : 2.0f, fade({ 0.92f, 1.f, 0.96f, 0.95f }));
            m_preview->drawDot(ccp(lx, cy), U * 0.30f, fadeDark({ 0, 0, 0, 0.55f }));
            m_preview->drawDot(ccp(lx, cy), U * 0.24f, fade({ 0.92f, 1.f, 0.96f, 0.95f }));
            m_preview->drawDot(ccp(lx, cy), U * 0.11f, fade({ 0.13f, 0.9f, 0.45f, 1.f }));
        }
        if (armed && hit > 0.f && hit < 1.f)
            m_preview->drawDot(ccp(cubeX, cy), U * (0.35f + 1.6f * hit),
                               fade({ 1.f, 1.f, 1.f, 0.5f * (1.f - hit) }));
    }


    void addLicenseRow() {
        auto row = newRow(54.f);
        auto bg = CCLayerColor::create({ 0, 0, 0, 95 }, ROW_W, 50.f);
        bg->setPosition({ 0.f, 2.f }); row->addChild(bg);
        m_licLbl = mkLabel("", "bigFont.fnt", 0.35f, { 210, 214, 224 });
        m_licLbl->setPosition({ 9.f, 27.f });
        row->addChild(m_licLbl);
        refreshLicense();
        push(row);

        // No token box. It rendered the 43-character bearer credential in cleartext under a
        // label reading "you@email.com", so nobody had any reason to redact it before posting a
        // settings screenshot - and nothing ever read the field back.
    }
    void refreshLicense() {
        if (m_licLbl) m_licLbl->setString(g_licMsg.c_str());
    }


    static constexpr float HIST_MS = 120.f;   // +/- ms
    static constexpr int   HIST_BINS = 41;    // odd so a bin is centred on 0




    static const char* gmName(int i) {
        static const char* N[8] = { "Cube", "Ship", "Ball", "UFO", "Wave", "Robot", "Spider", "Swing" };
        return (i >= 0 && i < 8) ? N[i] : "?";
    }



    void addReachRow() {
        ErrStats st = errStats();
        auto row = newRow(38.f);
        auto l1 = mkLabel("", "bigFont.fnt", 0.34f, { 255, 205, 90 });
        l1->setPosition({ 10.f, 25.f });
        row->addChild(l1);
        auto l2 = mkLabel("", "bigFont.fnt", 0.25f, { 140, 148, 165 });
        l2->setPosition({ 10.f, 10.f });
        row->addChild(l2);

        if (st.n < 10) {
            l1->setString("How tight a window can you hit?");
            l2->setString("Play a bit more and this will tell you");
        } else {
            int win = (int)(lround(2.0 * st.p90 / 5.0) * 5);   // 5ms rounding
            l1->setString(fmt::format("You can reliably hit a {} ms window", win).c_str());
            if (g_actions.empty()) {
                l2->setString("Open a level with macros to see how it compares");
            } else {
                float hUse = (std::isfinite(g_cal.h) && g_cal.h > 0.05f) ? g_cal.h : 1.f;
                int tight = 0;
                for (auto const& a : g_actions)
                    if ((float)((a.winEnd - a.winStart) / hUse) * 1000.f < (float)win) tight++;
                l2->setString(fmt::format("This level has {} clicks - {} are tighter than that",
                                          (int)g_actions.size(), tight).c_str());
            }
        }
        push(row);
    }


    // A group header that opens and closes. At most one group is open, so the closed list is
    // seven headers plus the pinned rows - short enough to read at a glance, which is the job the
    // tab strip used to do badly while costing a quarter of the panel.
    void addGroup(const char* id, const char* title) {
        bool open = (m_openGroup == id);
        auto row = newRow(26.f);
        auto bg = CCLayerColor::create({ 0, 0, 0, open ? (GLubyte)70 : (GLubyte)40 }, ROW_W, 26.f);
        row->addChild(bg, -1);
        auto l = mkLabel(title, "bigFont.fnt", 0.36f, open ? ccColor3B{ 255, 255, 255 }
                                                          : ccColor3B{ 150, 205, 255 });
        l->setPosition({ 10.f, 13.f });
        row->addChild(l);
        auto arrow = mkLabel(open ? "-" : "+", "bigFont.fnt", 0.42f,
                             open ? ccColor3B{ 255, 255, 255 } : ccColor3B{ 150, 205, 255 });
        arrow->setAnchorPoint({ 0.5f, 0.5f });
        arrow->setPosition({ ROW_W - 16.f, 13.f });
        row->addChild(arrow);
        auto m = rowMenu(row);
        std::string key = id;
        auto hit = geode::cocos::CCMenuItemExt::createSpriteExtra(
            CCLayerColor::create({ 0, 0, 0, 0 }, ROW_W, 26.f),
            [this, key](CCMenuItemSpriteExtra*) {
                m_openGroup = (m_openGroup == key) ? std::string() : key;
                Mod::get()->setSavedValue<std::string>("ui-group", m_openGroup);
                rebuildList();
            });
        hit->setAnchorPoint({ 0.f, 0.f });
        hit->setPosition({ 0.f, 0.f });
        m->addChild(hit);
        push(row);
    }

    bool groupOpen(const char* id) const { return m_openGroup == id; }

    // The state that decides whether the mod does anything at all, and it was the one thing the
    // popup never said. Without a macro the overlay draws nothing, so "Show guide: on" was a
    // promise the mod could not keep and there was no way to tell from here which case you were in.
    void addMacroRow() {
        auto row = newRow(34.f);
        bool have = !g_actions.empty();
        const char* head = have ? "Macro loaded" : (m_levelID > 0 ? "No macro for this level"
                                                                  : "Open a level to load a macro");
        auto l1 = mkLabel(head, "bigFont.fnt", 0.38f,
                          have ? ccColor3B{ 120, 240, 140 } : ccColor3B{ 255, 170, 60 });
        l1->setPosition({ 10.f, 22.f });
        row->addChild(l1);
        std::string sub = have
            ? fmt::format("{} clicks - {}", (int)g_actions.size(),
                          g_activeMacro.empty() ? std::string("tap to change") : g_activeMacro)
            : (m_levelID > 0 ? std::string("The guide can't show anything until one is loaded")
                             : std::string("Macros are picked per level"));
        if (sub.size() > 46) sub = sub.substr(0, 43) + "...";
        auto l2 = mkLabel(sub.c_str(), "bigFont.fnt", 0.26f, { 150, 156, 172 });
        l2->setPosition({ 10.f, 9.f });
        row->addChild(l2);
        if (m_levelID > 0) {
            auto m = rowMenu(row);
            int lid = m_levelID;
            auto b = geode::cocos::CCMenuItemExt::createSpriteExtra(
                ButtonSprite::create(have ? "Change" : "Get", "bigFont.fnt", "GJ_button_01.png", 0.6f),
                [lid](CCMenuItemSpriteExtra*) {
                    if (auto* p = MacroListPopup::create(lid, 0)) p->show();
                });
            b->setScale(0.62f);
            b->setPosition({ ROW_W - 40.f, 17.f });
            m->addChild(b);
        }
        push(row);
    }

    void update(float dt) override {
        m_prevT += dt;
        if (m_preview) drawPreview();
        if (m_licLbl) refreshLicense();
    }

    // Replaces showTab(). One pass builds the whole list: the rows people always want, then the
    // group headers, with only the open group's contents expanded.
    //
    // keepScroll because a rebuild happens on every group you open, every style you cycle and
    // every All on / All off - and jumping back to the top each time means the list throws away
    // your place every time you touch it. The offset is measured from the top rather than as a
    // raw position, because the content height changes across the rebuild that is the whole point.
    void rebuildList(bool keepScroll = true) {
        auto* mod = Mod::get();
        float keepOff = 0.f;
        if (keepScroll && m_scroll) {
            float vh = m_scroll->getContentHeight();
            float ch = m_scroll->m_contentLayer->getContentHeight();
            keepOff = m_scroll->m_contentLayer->getPositionY() - (-ch + vh);
            if (keepOff < 0.f) keepOff = 0.f;
        }
        // Kept from showTab: the highway settings are only meaningful on the highway style, and
        // this is the only place that syncs them - PlayLayer's call sits below an early-return that
        // never runs when there is no macro.
        hwModeSync(mod->getSettingValue<std::string>("indicator-mode"));
        // Null EVERY member that points into the list BEFORE the children go, or update() will
        // run against freed nodes on the next frame.
        m_preview = nullptr; m_prevName = nullptr;
        m_licLbl = nullptr; m_licInput = nullptr;
        m_scroll->m_contentLayer->removeAllChildren();

        // ---- pinned: the three things you open this panel for -------------------------------
        addMacroRow();
        addToggle("enabled", "Show guide", "Turns the whole overlay off");
        addChoice("indicator-mode", "Style", "Which visual counts you into the click",
                  { "ring", "classic", "converge", "pulse", "highway" }, true);
        addPreviewRow();

        // ---- groups -------------------------------------------------------------------------
        addGroup("look", "LOOK");
        if (groupOpen("look")) {
            addToggle("ring", "Show timing cue", "Off = reticle only");
            addToggle("trajectory", "Wave route", "Draws the macro's whole path onto the level");
            addToggle("auto-path", "Work out the path automatically",
                      "Plays the macro at speed on first load. Experimental - not reliable yet");
            addToggle("playback-raw-frames", "Playback: match frames exactly",
                      "What xdBot and Eclipse do for imported macros - the macro's frames straight "
                      "against the game's own counter");
            addToggle("record-path", "Learn the path as you play",
                      "Builds the wave route from your own runs - every attempt adds to it");
#ifdef CI_DEV_UNLOCK
            // In the panel, not only in mod.json. This mod has its own settings list, so a setting
            // that exists solely as a mod.json entry appears in Geode's generic page and nowhere a
            // user would look - which has already cost one round of "I can't find the setting".
            addToggle("showcase-play", "Play the macro (showcase)",
                      "Flies the macro on your icon so a run can be recorded. Developer builds "
                      "only; the run is forced into test mode and cannot be submitted");
#endif
            addNumber("indicator-opacity", "Opacity", "Fainter over gameplay", 0.1, 1.0, 0.1, false);
            addNumber("cue-contrast", "Contrast", "Dark halo so it reads on bright levels", 0.0, 1.0, 0.1, false);
            addButton("Guide color", "Color of the lines and cue", "Pick", [this] {
                auto cur = Mod::get()->getSettingValue<ccColor3B>("line-color");
                if (auto* p = geode::ColorPickPopup::create(cur)) {
                    p->setCallback([](ccColor4B const& c) {
                        Mod::get()->setSettingValue<ccColor3B>("line-color", { c.r, c.g, c.b });
                    });
                    p->show();
                }
            });
            addToggle("smooth-lines", "Smooth lines", "Antialiased edges; off if it looks wrong on your GPU");
            addToggle("lines", "Guide lines + blocks", "Scrolling window bars (Classic style)");
            addToggle("hold-bar", "Hold bar", "The bar showing how long to keep holding");
            addToggle("lines-classic-only", "Lines in Classic only", "Ring/Converge/Pulse: cue only");
            addToggle("notches", "Countdown notches", "Ticks 150/100/50 ms before (Classic)");
            addToggle("reduce-flashing", "Reduce flashing", "Dims the press flashes");
            // Only on the style they belong to - they mean nothing anywhere else.
            if (mod->getSettingValue<std::string>("indicator-mode") == "highway") {
                addHeader("HIGHWAY LANE");
                addNumber("hw-x", "Lane position", "Left to right, across the screen", 0.02, 0.98, 0.02, false);
                addNumber("hw-y", "Lane height", "Where the bottom of the lane sits", 0.0, 0.7, 0.02, false);
                addNumber("hw-scale", "Lane size", "Width and height together", 0.4, 2.0, 0.1, false, "x");
                addNumber("hw-opacity", "Lane opacity", "How solid the lane and its notes are", 0.1, 1.0, 0.1, false);
                addToggle("hw-guides", "Lane lines", "Side rails and the half-second lines");
            }
        }

        addGroup("macros", "MACROS");
        if (groupOpen("macros")) {
            addToggle("auto-fetch", "Download macros automatically", "Grab one when you enter a level");
            if (m_levelID > 0) {
                int lid = m_levelID;
                addButton("This level's macros", "Browse, download and remove them", "Open", [lid] {
                    if (auto* p = MacroListPopup::create(lid, 0)) p->show();
                }, "GJ_button_01.png");
            }
            addToggle("record-button", "Record button in pause menu",
                      "Adds a Record / Stop button to the pause screen");
        }

        addGroup("sound", "SOUND");
        if (groupOpen("sound")) {
            addChoice("sound-pack", "Cue sounds", "Timbre of the click / release cues",
                      { "click", "beep", "wood", "snap" });
            addToggle("ticks", "Press sound", "A click at the exact moment to press");
            addToggle("release-sound", "Release sound", "Softer tone when to let go of a hold");
            addToggle("tight-pitch", "Pitch by tightness", "Higher pitch = tighter window");
            addNumber("volume", "Volume", "", 0.0, 5.0, 0.25, false);
        }

        addGroup("timing", "TIMING");
        if (groupOpen("timing")) {
            addReachRow();
            addNumber("lead", "Cue lead", "How far ahead of the click the cue lands", 0.0, 0.5, 0.01, false, "s");
            addNumber("lookahead", "Lookahead", "How early the cue appears", 0.5, 4.0, 0.1, false, "s");
            addNumber("hold-threshold", "Hold cutoff", "Shorter than this counts as a tap", 0.01, 0.3, 0.01, false, "s");
            addToggle("feedback", "Trainer feedback", "PERFECT / MISSED / frame counts after each press");
            addToggle("release-grading", "Grade releases too",
                      "Always on in ship, wave, UFO and swing, where the hold is the input");
        }

        addGroup("modes", "GAMEMODES");
        if (groupOpen("modes")) {
            static const char* K[8] = { "gm-cube","gm-ship","gm-ball","gm-ufo","gm-wave","gm-robot","gm-spider","gm-swing" };
            static const char* N[8] = { "Cube","Ship","Ball","UFO","Wave","Robot","Spider","Swing" };
            // "Toggle" flipped to the opposite of whatever the first one happened to be, so what it
            // did depended on state you could not see. Two buttons that each do one thing.
            addButton("All gamemodes", "Turn the guide on or off everywhere", "All on", [this] {
                for (auto* k : K) Mod::get()->setSettingValue<bool>(k, true);
                rebuildList();
            }, "GJ_button_01.png");
            addButton("", "", "All off", [this] {
                for (auto* k : K) Mod::get()->setSettingValue<bool>(k, false);
                rebuildList();
            });
            for (int i = 0; i < 8; i++) addToggle(K[i], N[i], "");
        }

        addGroup("practice", "PRACTICE");
        if (groupOpen("practice")) {
            addSafeModeToggle();
            addNumber("hide-from", "Hide from", "Practise a stretch unaided - guide off from here", 0.0, 100.0, 1.0, false, "%");
            addNumber("hide-to", "Hide to", "...back on past here. Same value in both = off", 0.0, 100.0, 1.0, false, "%");
            addToggle("free-camera", "Free camera when paused",
                      "Pause, then arrows / WASD. Needs a keyboard");
            addToggle("stats-hud", "Stats readout", "Accuracy and streak text in the corner");
        }

        addGroup("account", "ACCOUNT");
        if (groupOpen("account")) {
            addLicenseRow();
            addButton("Sign in", "Opens the sign-in screen", "Sign in", [] {
                if (auto* p = LicensePopup::create()) p->show();
            }, "GJ_button_01.png");
            addButton("Buy Click Indicators", "Opens the store in your browser", "Buy", [] {
                geode::utils::web::openLinkInBrowser("https://clickindicators.com");
            });
            addButton("Sign out", "Signs this install out", "Sign out", [this] {
                // Tell the server too, or the website's "Where your licence is" card goes on
                // naming this PC as the holder forever. Fire-and-forget and deliberately not
                // inside licClear(): signing out locally must not wait on, or be blocked by,
                // a network call, and licClear's other caller runs when the row is already gone.
                std::string tok = licGet("lic-k");
                if (!tok.empty() && !licIsDev(tok))
                    licPost("logout", fmt::format("{{\"token\":\"{}\"}}", jsonEscape(tok)),
                            [](bool, bool, matjson::Value) {});
                licClear(); refreshLicense();
                if (m_licInput) m_licInput->setString("");
            });
            addButton("Support", "Problems with a key? Open a ticket", "Discord", [] {
                geode::utils::web::openLinkInBrowser("https://discord.gg/FncjJJNcES");
            });
            addToggle("debug-logging", "Debug logging", "Write detailed diagnostics to the Geode log");
        }

        m_scroll->m_contentLayer->updateLayout();

        float vh = m_scroll->getContentHeight();
        float ch = m_scroll->m_contentLayer->getContentHeight();
        if (!keepScroll || ch <= vh) {
            m_scroll->scrollToTop();
        } else {
            float top = -ch + vh;               // what scrollToTop would set
            float y = top + keepOff;
            if (y > 0.f) y = 0.f;               // 0 is the bottom of the content
            if (y < top) y = top;
            m_scroll->m_contentLayer->setPositionY(y);
        }
    }

    bool initContent() {
        if (!Popup::init(PW, PH)) return false;
        this->setTitle("Click Indicators");
        // The X is placed by Geode with addChildAtPosition(Anchor::TopLeft), which writes
        // AnchorLayoutOptions on the button. setPosition does not touch those options, so a
        // nudge here is a transient that the next layout pass over m_buttonMenu silently
        // reverts - and while it holds it floats the X inside the panel instead of straddling
        // the corner, which is exactly the button 'sitting weirdly'. Let the anchor place it.
        licRefresh();

        // The tab strip and its name label used to eat 76px of a 300px panel to navigate 43
        // settings. That space is the list now.
        m_openGroup = Mod::get()->getSavedValue<std::string>("ui-group", std::string());

        constexpr float LIST_H = PH - 60.f;
        // The dark inset GD and Geode both put behind a list, so the rows read as a list rather
        // than as text floating on the panel.
        auto well = CCLayerColor::create({ 0, 0, 0, 70 }, ROW_W + 4.f, LIST_H);
        well->setPosition({ (PW - ROW_W - 4.f) * 0.5f - 6.f, 20.f });
        m_mainLayer->addChild(well);

        m_scroll = geode::ScrollLayer::create({ ROW_W + 4.f, LIST_H });
        m_scroll->setPosition({ (PW - ROW_W - 4.f) * 0.5f - 6.f, 20.f });
        m_mainLayer->addChild(m_scroll);
        // Deliberately an AxisLayout, not createDefaultListLayout(): ScrollLayer's default returns
        // a SimpleColumnLayout, and setAutoGrowAxis does not exist on that type.
        m_scroll->m_contentLayer->setLayout(
            geode::ColumnLayout::create()->setGap(3.f)->setAxisReverse(true)->setAutoGrowAxis(LIST_H));

        auto bar = geode::Scrollbar::create(m_scroll);
        bar->setPosition({ (PW + ROW_W) * 0.5f + 4.f, 20.f + LIST_H * 0.5f });
        m_mainLayer->addChild(bar);

        rebuildList(false);   // first build: start at the top
        this->scheduleUpdate();
        return true;
    }

public:
    static ClickGuidePopup* create(int levelID) {
        auto ret = new ClickGuidePopup();
        ret->m_levelID = levelID;
        if (ret->initContent()) { ret->autorelease(); return ret; }
        delete ret;
        return nullptr;
    }
};

// steal testmode flag, flip around super so GD refuses to save
// attempts + jumps arent gated on it, handled seperately in resetLevel/commitJumps

// Safe mode exists so a guided run cannot count. With no macro loaded there is no guide, so there
// is nothing to guard against and blocking progress would just be punishing normal play - a level
// the mod has no macro for should behave exactly as if the mod were not installed.
static bool guideActive() {
    // licGate() as well as licOK(): two independent derivations, so forcing one open does not
    // open the other. See licGate.
    return !g_actions.empty() && licOK() && licGate() && Mod::get()->getSettingValue<bool>("enabled");
}

static bool safeModeOn() {
    return Mod::get()->getSettingValue<bool>("safe-mode") && guideActive();
}

// Free camera. While the pause menu is up, PlayLayer's update is stopped, so nudging the object
// layer sticks and lets you scout the rest of the level. The original position is saved on the
// first nudge and put back by postUpdate on the frame play resumes, which covers every way out of
// the pause menu without having to hook each button.
// postUpdate stops running while the pause menu is up, so the overlay freezes. The click editor
// therefore cannot reuse it - it draws its own markers from the last live frame's geometry.
static size_t g_snapSeg = 0;     // segAtX(g_snapX) from the last live frame
static bool g_snapTab = false;   // was the speed profile usable that frame
static float g_snapX = 0.f, g_snapVx = 0.f, g_snapPy = 0.f;
static double g_snapT = 0.0;
static bool g_snapOk = false;

static bool g_fcActive = false;
static CCPoint g_fcSaved = CCPointZero;
static CCPoint g_fcSavedCam = CCPointZero;
static CCPoint g_fcPan = CCPointZero;   // how far the view has been panned from the real camera

// The pulse node hangs off the icon's parent, not PlayLayer, so it needs its own lookup.
// The overlay hangs off the UI layer so level effects cannot touch it, which means it is no longer
// a direct child of PlayLayer. Look in both places rather than assuming either - the fallback
// matters because the overlay falls back to PlayLayer when m_uiLayer is not there yet.
static CCNode* ciOverlayNode(PlayLayer* pl) {
    if (!pl) return nullptr;
    if (pl->m_uiLayer)
        if (auto* ov = pl->m_uiLayer->getChildByID("ci-overlay"_spr)) return ov;
    return pl->getChildByID("ci-overlay"_spr);
}

// =================================================================================================
// WAVE ROUTE
//
// The line a wave macro flies is a property of the LEVEL, not of the player. It is a fixed set of
// world coordinates: solved once, drawn from then on, never moved. The version this replaces
// re-integrated from wherever the player happened to be, every time it decided its answer had gone
// stale - and it decided that on nearly every frame, because it compared the live gravity and size
// against the state at the END of a 4000 unit solve, which differs the moment any portal is coming
// up. So the line was re-anchored to the icon each frame and slid along underneath it: a projection
// of "where you would go from here" rather than a route you can steer back onto. Being re-seeded at
// the player also meant there was never anything drawn BEHIND the player, which is the other half of
// why it read as a moving line instead of a path.
//
// What makes the route solvable at all: GD sets the wave's y velocity absolutely each physics step
// rather than accumulating it - no gravity, no acceleration - so |dy/dx| is exactly 1, and exactly 2
// when mini, at every speed. Measured over 71,236 samples of real recorded per-frame positions the
// median was 1.000000000 with nothing in between. So between two input transitions the path is a
// straight line, and the whole route is a polyline whose vertices are the macro's clicks projected
// into level positions (Action::wxSweet / wxRel, which come off GD's own time->position curve).
//
// That fixes the SHAPE completely. What it does not fix is the HEIGHT: one unknown scalar per wave
// section. Three things answer it, in order of how much they are worth:
//
//   1. A position the run actually passed through inside that section. Exact, but only available
//      once you have been there.
//   2. The level's own geometry. The shape is rigid, so every solid block converts into a closed
//      interval of vertical offsets that would put the route inside it - in closed form, no search.
//      Union them and the feasible offsets are what is left. In a wave corridor that leaves one
//      narrow band, which IS the route the macro flew, and its centre is the answer. See rtFitShift.
//   3. The mouth of the wave portal, as a last resort.
//
// Gravity and size portals make the shape depend on the height and the height depend on the shape,
// since a portal at y=300 does nothing to a route passing at y=100. That is solved as a fixed point:
// build, fit, rebuild, refit, until the offset stops moving. It converges in two passes in practice.
// =================================================================================================

struct RtObst { float x0, x1, y0, y1; int id = 0; };

// WHICH BLOCKS SLIDE A WAVE, AND WHICH KILL IT.
//
// The mod treated every solid as something to rest on. On a level built out of blocks that kill on
// contact, every slide it produced was fiction and the route was being bent by surfaces that would
// have ended the run.
//
// This set is measured, not remembered. On the level whose macro carries 64,980 recorded positions,
// every flat stretch in the drawn route is a slide by definition - 349 of them - and looking up the
// block each one rests on gives:
//
//     467 x122     170 x38     171 x11     175 x11     173 x1
//
// Those are the surfaces a wave comes to rest on. Everything else a route meets is fatal, which
// makes a line touching one a PLACEMENT error to report rather than a surface to bend around - and
// that distinction is why pushing the line about at the point of contact never helped.
static bool rtSlides(int id) {
    switch (id) {
        case 467: case 170: case 171: case 173: case 175: return true;
        // Unidentified. Half the geometry arrived with no id before the capture below was fixed, and
        // an unknown block is treated as sliding: that is the behaviour this has always had, so a
        // gap in the data cannot silently change what a level looks like.
        case 0: return true;
        default: return false;
    }
}
// A gravity or size portal, with the height band it occupies - a portal at y=300 does not affect an
// icon at y=100, so the height is what decides whether the route goes through it.
// 0 grav normal, 1 grav inverse, 2 big, 3 mini, 4 teleport (ty = where it puts you)
struct RtPortal { int kind; float x, y0, y1; float ty; };
struct RtSection {
    float x0 = 0.f, x1 = 0.f;      // the wave portal, and the gamemode portal that ends the section
    float priorY = 0.f;            // the mouth of the portal it enters by
    float exitY = NAN;             // and the mouth of the one it leaves by, where there is one
    float obsX = 0.f, obsY = 0.f;  // somewhere the run was actually seen inside this section
    bool  obs = false;
    float flip = 1.f, size = 1.f;  // gravity and size at the mouth
    float solvedY = 0.f;
    bool  solved = false;
    // Did this section survive the support gate? An undrawn section still has to be observable -
    // see rtNoteEntry - or the one thing that could rescue it is refused because it needs rescuing.
    bool  drawn = false;
    // The height came from the game itself - the y the player was at the instant they became a
    // wave - rather than from a portal mouth. It is not something to fit away from.
    bool  exact = false;
    // This section opens on a pair of opposite-mode gamemode portals at ONE x - a dual fork. Which
    // of them this macro's icon went through is not decidable from anything available at load, so
    // the section is built, solved and graded exactly like any other, and never drawn.
    bool  fork = false;
};

static std::vector<RtObst>    g_rtObst;      // solid + hazard, inside wave sections only, x0 sorted
// A slope is a straight surface, not a box. Stored as its span plus the two ends of the
// hypotenuse, so the constraint against it can be solved exactly instead of approximated by
// the bounding box - which on a standard 60-unit wave corridor forbids every offset at once.
// A slope is a TRIANGLE, and this used to record only its hypotenuse.
//
// With no thickness and no side, there is no arrangement in which the route can be "inside" a slab -
// so every measurement said the line was clear while the player watched it cut straight through one.
// The check and their eyes were looking at different things, and theirs was right.
//
// `solidBelow` says which side is rock: a slope that rises to the right and is filled underneath is
// a floor, and the region under its face is as solid as any block. That region is emitted alongside
// the surface, so the route can be pushed out of it and CI-THROUGH can finally see it.
struct RtSlope { float x0, x1, ya, yb; int id = 0; bool solidBelow = true; };
static std::vector<RtSlope>   g_rtSlopes;
static float                  g_rtObstMaxW = 0.f;
// The player's half height, read from GD rather than assumed. PlayerObject::getObjectRect is the
// very rect GD collides with - touchedObject does this->getObjectRect().intersectsRect(objectRect)
// - so it is in the same space as the obstacle rects and needs no conversion. Assuming it from the
// icon put the route 14 units above every surface it slid along.
static double                 g_rtHalfBig = 15.0;
static int                    g_rtPortLogs = 0;
// Moving platforms. The scan reads every rect once, so anything a move trigger relocates is
// remembered where it STARTED - and a D block platform that has since risen leaves the route down
// at the floor it was built over. Only objects with a group can be moved by a trigger, and that is
// a small fraction of a level, so those are kept aside and re-read as they move while the static
// geometry is copied back untouched.
struct RtMover {
    GameObject* o; bool slope;
    float x, y;              // where it was last seen, for the has-it-moved check
    CCRect base;             // its rect before any trigger touched it
    double arriveT;          // when the RUN reaches this object, in level seconds
    int trig[12]; int nTrig; // which move triggers act on it, by index into g_rtTrigs
    bool predicted;          // its whole movement is accounted for, so it needs no watching
};
// A move trigger, reduced to what predicting it needs. fireT is precomputed because timeForPos is
// a call into GD and this has to be pure arithmetic afterwards.
struct RtMoveTrig { double fireT; int group; float dx, dy; float dur; int easing; float rate;
                    bool dead; };   // dead: the level was watched and it does not actually fire
static std::vector<RtMoveTrig> g_rtTrigs;
// The groups a move trigger actually aims at. Being IN a group is not the same as moving: on a real
// level 19,086 of 20,190 objects carry a group, for colour triggers and toggles and everything
// else, and treating all of them as movable put the whole level's geometry through a prediction
// where one modelling error displaces thousands of static blocks at once. Only the groups something
// really moves are movable.
// Which objects each move trigger actually moves, taken from GD's own registry rather than worked
// out by hand. GJBaseGameLayer::m_groups is indexed by group id and holds the objects in that
// group, so the question "what does this trigger move" has a direct answer - where asking every
// object "are you in a moved group?" found nothing at all on a level with 101 move triggers.
struct RtMoverLink { GameObject* o; int trig; };
static std::vector<RtMoverLink> g_rtLinks;
static std::vector<RtMover> g_rtMovers;
static std::vector<RtObst>  g_rtObstFixed;
static std::vector<RtSlope> g_rtSlopeFixed;
static double               g_rtMoveAt = -1e9;
// Properties 28/29 of a move trigger, in whatever units GD stores them. Checked against reality at
// run time rather than assumed: the mod predicts where a platform should be NOW and compares that
// with where it actually is, so a wrong scale shows up as a constant ratio instead of a mystery.
static double               g_rtMoveScale = 1.0;
// The level clock when the geometry was read. Everything already moved by then is baked into the
// rects the scan collected, so only what happens BETWEEN the scan and the run's arrival may be
// applied on top - measured, one trigger fires at t=0.2 and drops 358 objects by 90, and adding
// that to rects already carrying it put the route 90 units under the platform.
static double               g_rtScanT = 0.0;
static bool                 g_rtTrigAudited = false;
// A diagnostic-only census of every collidable object and where it started. Nothing here feeds the
// route - it exists to answer the one question hours of modelling could not: WHICH objects on this
// level actually move, so the thing that moves them can be found instead of guessed at.
struct RtWatch { GameObject* o; float x, y; };
static std::vector<RtWatch> g_rtWatch;
static std::vector<RtPortal>  g_rtPortals;   // gravity + size, x sorted
static std::vector<RtSection> g_rtSecs;
// What the SCAN said, kept apart from what each solve does to it.
//
// Both the corridor subdivision and the observed-wave merge rewrite g_rtSecs, and g_rtSecs was the
// only copy - so every solve started from the previous solve's output. Subdividing an already
// subdivided list is harmless; replacing a section with an observation is not, because the guess it
// replaced is then gone for the rest of the session even if the observation later proves too small
// to be the whole story. One stretch seen turned thirteen sections into one.
static std::vector<RtSection> g_rtSecsBase;
// Hazards, kept APART from the solids rather than thrown away.
//
// They used to be in the same list and fed to the same collision, so the route came to rest on
// spikes and skated along them. Taking them out fixed that and created something worse: nothing was
// looking at them at all, so the line ran straight through saws - and CI-THROUGH, which only ever
// checked the solids, reported "nowhere" while the player was watching it happen.
//
// A wave never RESTS on a spike and never PASSES THROUGH one either. Two different jobs, and only
// the first one wanted them out of slideTo.
static std::vector<RtObst> g_rtHaz;
// How often the moving-platform prediction has been caught out on this level. The audit already
// measures it; this is it being kept rather than only logged.
static int g_rtTrigStruck = 0;
      // wave sections, x sorted
// The gamemode portals, kept so a fork can be examined after the fact. In a dual there are two at
// one x - one per player - and everything hangs on which of them the run went through.
struct RtGm { float x, y0, y1; bool wave; };
static std::vector<RtGm> g_rtGm;
// Where the level is in DUAL. Inside these stretches two players exist and therefore two of every
// portal. Every structural test tried against the phantom sections has failed for one reason: a
// phantom IS a real wave corridor, only the other player's, so nothing about the level's shape
// distinguishes it. Where the level is dual is the one piece of structure that is about the
// players rather than the geometry.
struct RtDual { float x; bool on; float y; };
// The level's ground, kept so a mirrored dual line can be told apart from an asymmetric one:
// a second path that runs UNDER the floor is a wrong axis, not a level built out of balance.
static float g_rtFloor = -1e9f;
static std::vector<RtDual> g_rtDual;
// Geometry that LOCKS TO THE PLAYER. A move trigger with lock-to-player-X keeps its objects at a
// fixed offset from the run, so the platform never passes you - you carry it. In level coordinates
// that means it is under you at EVERY x from the moment the trigger fires, which is nothing like
// where the scan found it sitting. Forty solid blocks on the level in hand are driven this way, and
// discarding those triggers as "follows the run, not a clock" left the route landing on air.
struct RtLocked { float fromX, toX, y0, y1; };
static std::vector<RtLocked> g_rtLocked;
static std::vector<CCPoint>   g_rtPts;       // the route: world coordinates, ascending x
static std::vector<uint8_t>   g_rtHold;      // per segment: 0 released, 1 held, 2 gap between sections
// THE SECOND ICON. A dual portal puts another player on the level, and in wave the two are flown by
// the same button with opposite gravity - so the second is the first reflected about the height they
// entered the dual at. That reflection is what a dual wave looks like from the outside: one line and
// its mirror, crossing wherever the run is level with where it came in.
static std::vector<CCPoint>   g_rtPts2;
static std::vector<uint8_t>   g_rtHold2;
static bool g_rtGeoOk = false, g_rtOk = false;
static PlayLayer* g_rtGeoFor = nullptr;
static int  g_rtGeoLevel = -1, g_rtGeoCount = -1;
static int  g_rtObsGen = 0, g_rtSolvedGen = -1;
static std::string g_rtMacro = "\x01";
static size_t g_rtActs = (size_t)-1;
static void* g_rtSpawn = (void*)-1;
static float g_rtSpawnX = -1e9f;
static bool  g_rtWx = false;

// Height of a polyline at a position, or NaN where it does not reach.
static double rtInterp(std::vector<CCPoint> const& p, double qx) {
    if (p.size() < 2) return NAN;
    if (qx < (double)p.front().x - 1.0 || qx > (double)p.back().x + 1.0) return NAN;
    size_t lo = 0, hi = p.size() - 1;
    while (hi - lo > 1) { size_t m = (lo + hi) / 2; if ((double)p[m].x <= qx) lo = m; else hi = m; }
    double x0 = (double)p[lo].x, x1 = (double)p[lo + 1].x;
    if (x1 - x0 < 1e-6) return (double)p[lo].y;
    double k = (qx - x0) / (x1 - x0);
    return (double)p[lo].y + ((double)p[lo + 1].y - (double)p[lo].y) * k;
}

// The same, on the assembled route, which has holes in it: between two wave sections there is no
// answer rather than a straight line joining them.
static double rtRouteYAt(double qx) {
    if (g_rtPts.size() < 2) return NAN;
    size_t lo = 0, hi = g_rtPts.size() - 1;
    while (hi - lo > 1) {
        size_t m = (lo + hi) / 2;
        if ((double)g_rtPts[m].x <= qx) lo = m; else hi = m;
    }
    if (lo + 1 >= g_rtPts.size() || g_rtHold[lo] == 2) return NAN;
    double x0 = (double)g_rtPts[lo].x, x1 = (double)g_rtPts[lo + 1].x;
    if (qx < x0 - 1.0 || qx > x1 + 1.0) return NAN;
    if (x1 - x0 < 1e-6) return (double)g_rtPts[lo].y;
    double k = (qx - x0) / (x1 - x0);
    return (double)g_rtPts[lo].y + ((double)g_rtPts[lo + 1].y - (double)g_rtPts[lo].y) * k;
}

// GD's easings, to the accuracy this needs. What matters most is the ENDPOINT - a platform that
// finished moving before the run arrived is simply at its destination, and every easing agrees
// about that - so the shape in between only has to be close.
static double rtEase(double p, int type, double rate) {
    if (p <= 0.0) return 0.0;
    if (p >= 1.0) return 1.0;
    double r = (rate > 0.1 && rate < 20.0) ? rate : 2.0;
    switch (type) {
        case 1: case 4: case 7: case 10: case 13: case 16:   // the InOut family
            return p < 0.5 ? 0.5 * std::pow(2.0 * p, r)
                           : 1.0 - 0.5 * std::pow(2.0 * (1.0 - p), r);
        case 2: case 5: case 8: case 11: case 14: case 17:   // In
            return std::pow(p, r);
        case 3: case 6: case 9: case 12: case 15: case 18:   // Out
            return 1.0 - std::pow(1.0 - p, r);
        default: return p;                                   // None, and anything unrecognised
    }
}

// Where a movable object will be WHEN THE RUN REACHES IT. Every move trigger aimed at one of its
// groups that will have fired by then contributes its offset, eased by how far through its travel
// it is at that moment.
//
// The result does not change as the level plays, which is what makes this worth doing: the answer
// for each object is one fixed position, so the route is still solved once rather than chased.
static bool rtPredictOffsetAt(RtMover const& m, double at, double& ox, double& oy) {
    ox = 0.0; oy = 0.0;
    if (!(at > 0.0) || m.nTrig <= 0) return false;
    for (int i = 0; i < m.nTrig; i++) {
        RtMoveTrig const& tr = g_rtTrigs[m.trig[i]];
        if (tr.dead) continue;
        double p = tr.dur > 0.001 ? (at - tr.fireT) / (double)tr.dur
                                  : (at >= tr.fireT ? 1.0 : 0.0);
        p = rtEase(p, tr.easing, (double)tr.rate);
        ox += (double)tr.dx * g_rtMoveScale * p;
        oy += (double)tr.dy * g_rtMoveScale * p;
    }
    return true;
}
// How much further a thing will have moved by the time the run reaches it, counting from where the
// scan already found it.
static bool rtPredictOffset(RtMover const& m, double& ox, double& oy) {
    double ax = 0.0, ay = 0.0, sx = 0.0, sy = 0.0;
    if (!rtPredictOffsetAt(m, m.arriveT, ax, ay)) return false;
    rtPredictOffsetAt(m, g_rtScanT, sx, sy);      // what the collected rect already includes
    ox = ax - sx; oy = ay - sy;
    return true;
}

// The obstacle lists are the fixed geometry plus wherever the movable objects are NOW. Called once
// after the scan, and again whenever a trigger has actually moved one of them.
static void rtComposeGeometry() {
    g_rtObst = g_rtObstFixed;
    // Saws stay OUT of the collision, and that is now measured rather than argued.
    //
    // Putting them back was a reasonable idea - the line was passing through them - and the
    // dump says it failed on both counts: the share of impossible gradients went from 10.1%
    // to 15.9%, and the route still sat inside something 21 times. Worse shape, same
    // penetration. A wave does not rest on a spike, and making the solver believe it does
    // buys nothing.
    //
    // Which means the passing-through is not a collision failure at all. The entry height is
    // exact and the turns come from the macro, so if the drawn shape still meets a saw, the
    // shape between two clicks is wrong - and no amount of pushing it around at the point of
    // contact will fix that.
    g_rtSlopes = g_rtSlopeFixed;

    // GEOMETRY THE MOD IS GUESSING AT MUST NOT BEND THE LINE.
    //
    // Between two clicks a wave is a straight line at exactly one or two - it has no other shape,
    // because its vertical speed is set outright each step. So the only things allowed to put a
    // corner in the drawn path are a click and a real surface. Anything else is the mod inventing
    // gameplay: the player put it as "it's making all sorts of curves when there are no d blocks,
    // a wave would just stay 45 degrees".
    //
    // Where those corners came from is measurable. On Tidal Wave 106 of 747 move triggers were
    // predicted to move things that never moved - the very first one was predicted to carry its
    // group 150 units up and it had not shifted at all. Every one of those puts a block somewhere
    // it is not, the route slides along it, and the line bends for no reason a player can see.
    //
    // So once the audit has shown the predictions are unreliable on this level, the predicted
    // positions stop being collided against at all. What is left is the static geometry, which is
    // exactly as true as it ever was. Fewer surfaces means a straighter line, and a straight line
    // is the correct answer where there is nothing to slide on.
    //
    // Under the threshold - the ordinary level, where one trigger in a hundred is mispredicted -
    // nothing changes, and the moving platforms that took days to get right still work.
    // Counted HERE, not cached at scan time. The cached copy was taken 144 lines before the
    // triggers were collected, so it was always zero - which read as "too few triggers to judge"
    // and the gate never once fired on a level with 747 of them and 106 of them wrong.
    const int trigTotal = (int)g_rtTrigs.size();
    // PREDICT. The audit strikes out the individual triggers it catches out, and that is the
    // right granularity - 106 of 747 were wrong, which means 641 were RIGHT, and among those
    // are the ones that open the corridor.
    //
    // Switching prediction off wholesale was aimed at the curving, and the curving turned out
    // to be hazards used as slide surfaces and a hitbox with no width - both since fixed. What
    // the blanket gate actually did was solve the level against where the blocks are at load,
    // and on this one the corridor STARTS CLOSED and is opened by a move trigger. Solving
    // against the closed state squeezes the route through a wall that will not be there.
    const bool trustMovers = true;
    (void)trigTotal;
    if (!trustMovers) {
        log::info("[CI-UNTRUSTED] {} of {} move triggers were predicted wrong, so the {} moving "
                  "objects are used where they ARE rather than where a trigger was expected to "
                  "carry them - a wave does not bend without a surface, and a predicted surface "
                  "on this level is not one",
                  g_rtTrigStruck, trigTotal, (int)g_rtMovers.size());
    }
    for (auto& m : g_rtMovers) {
        if (!m.o) continue;
        double ox = 0.0, oy = 0.0;
        // Untrusted means "do not PREDICT", not "pretend it is not there". Skipping the movers
        // outright removed 4,056 real objects from the level, and the line then ran through blocks
        // that were plainly on screen. What the audit actually established is that the offsets are
        // wrong - and a mispredicted trigger usually means the thing never moved, so where it is
        // right now is the truth rather than a guess. Falling through to the live rect keeps the
        // geometry and drops only the invented part of it.
        m.predicted = trustMovers ? rtPredictOffset(m, ox, oy) : false;
        // Predicted: its base position plus wherever the triggers will have carried it by the time
        // the run arrives. Otherwise fall back to where it is right now, which is at least closer
        // than where it started.
        CCRect r;
        if (m.predicted) {
            r = CCRect(m.base.origin.x + (float)ox, m.base.origin.y + (float)oy,
                       m.base.size.width, m.base.size.height);
        } else {
            r = m.o->getObjectRect();
            // The base rect carries the hazard inset; a live one has to be given it again.
            if (!m.slope && m.o->m_objectType == GameObjectType::Hazard) {
                float ix = r.size.width * 0.25f, iy = r.size.height * 0.25f;
                r = CCRect(r.origin.x + ix, r.origin.y + iy,
                           r.size.width - ix * 2.f, r.size.height - iy * 2.f);
            }
        }
        m.x = m.o->getPositionX(); m.y = m.o->getPositionY();
        if (!(r.size.width > 0.f) || !(r.size.height > 0.f)) continue;
        if (r.size.width > 420.f || r.size.height > 420.f) continue;
        if (m.slope) {
            // slopeYPos reads the live object, so the surface is taken relative to its own rect
            // and then carried to the predicted one.
            CCRect lr = m.o->getObjectRect();
            double ya = m.o->slopeYPos(lr.origin.x);
            double yb = m.o->slopeYPos(lr.origin.x + lr.size.width);
            if (!std::isfinite(ya) || !std::isfinite(yb)) continue;
            double loR = (double)lr.origin.y - 2.0, hiR = (double)lr.getMaxY() + 2.0;
            if (ya < loR || ya > hiR || yb < loR || yb > hiR) continue;
            double shift = (double)r.origin.y - (double)lr.origin.y;
            g_rtSlopes.push_back({ r.origin.x, r.origin.x + r.size.width,
                                   (float)(ya + shift), (float)(yb + shift) });
        } else {
            g_rtObst.push_back({ r.origin.x, r.origin.x + r.size.width,
                                 r.origin.y, r.origin.y + r.size.height, m.o ? (int)m.o->m_objectID : 0 });
        }
    }
    std::sort(g_rtObst.begin(), g_rtObst.end(),
              [](RtObst const& a, RtObst const& b) { return a.x0 < b.x0; });
    std::sort(g_rtSlopes.begin(), g_rtSlopes.end(),
              [](RtSlope const& a, RtSlope const& b) { return a.x0 < b.x0; });
    g_rtObstMaxW = 0.f;
    for (auto const& ob : g_rtObst) g_rtObstMaxW = std::max(g_rtObstMaxW, ob.x1 - ob.x0);
}

// Everything about the level the route needs, read once. m_objects is large and getObjectRect is a
// virtual with work behind it, so this is cached exactly the way the speed table is.
static void rtScanLevel(PlayLayer* pl) {
    g_rtGeoOk = false;
    g_rtSecs.clear(); g_rtPortals.clear(); g_rtObst.clear(); g_rtHaz.clear(); g_rtSlopes.clear(); g_rtObstMaxW = 0.f;
    g_rtMovers.clear(); g_rtObstFixed.clear(); g_rtSlopeFixed.clear(); g_rtMoveAt = -1e9;
    g_rtDual.clear(); g_rtLocked.clear();
    g_rtScanT = pl ? (double)pl->m_gameState.m_levelTime : 0.0;
    g_rtTrigAudited = false;
    g_rtWatch.clear();
    g_rtTrigs.clear(); g_rtLinks.clear();
    // "scanned", not "usable": a platformer or a level with no wave in it has no route and never
    // will, and saying so once stops the scan being retried on every frame for the whole level.
    g_rtTrigStruck = 0;         // a fresh level has no strikes against it yet
    g_rtSecsBase = g_rtSecs;   // the base every solve restarts from
    g_rtGeoOk = true;
    if (!pl || !pl->m_objects || !pl->m_levelSettings || pl->m_isPlatformer) return;

    struct GmP { float x, y0, y1; bool wave; };
    std::vector<GmP> gm;
    int trSeen = 0, trTouch = 0, trSpawn = 0, trNoGroup = 0, trLock = 0, trNoOffset = 0, trNoTime = 0;
    // Groups belonging to move triggers the scan THREW AWAY. "None of the moved objects are solid"
    // is only true of the triggers that survived the filter - a platform driven by a lock-to-player
    // move, or by one whose own x gives no usable time, is invisible to that statement, and 14 of
    // them were discarded on this level.
    struct SkipG { int group; int why; float x; bool lockX, lockY; float dur; };
    std::vector<SkipG> skipG;
    struct SpawnT { float x; int group; float delay; };
    std::vector<SpawnT> spawns;
    struct PendMove { GameObject* o; double ownT; int group; float dx, dy, dur; int easing;
                      float rate; bool firedElsewhere; };
    std::vector<PendMove> pend;
    const int n = pl->m_objects->count();
    for (int i = 0; i < n; i++) {
        auto* o = static_cast<GameObject*>(pl->m_objects->objectAtIndex(i));
        if (!o) continue;
        const float ox = o->getPositionX(), oy = o->getPositionY();
        switch (o->m_objectID) {
            // Gamemode portals. One block either side of centre is what decides whether a route
            // at that height goes through the portal at all.
            case 12: case 13: case 47: case 111: case 745: case 1331: case 1933:
                gm.push_back({ ox, oy - 45.f, oy + 45.f, false }); break;
            case 660:
                gm.push_back({ ox, oy - 45.f, oy + 45.f, true }); break;
            // 10 leaves the run in NORMAL gravity, 11 inverts it. These were the wrong way round,
            // which inverted the slope of every segment in any section whose last gravity portal
            // was one of them - measured on Nine Circles: the section solved to flip=-1 while the
            // run was demonstrably right side up, and the anchor swung to -6132.
            case 10:  g_rtPortals.push_back({ 0, ox, oy - 45.f, oy + 45.f, 0.f }); break;
            case 11:  g_rtPortals.push_back({ 1, ox, oy - 45.f, oy + 45.f, 0.f }); break;
            case 99:  g_rtPortals.push_back({ 2, ox, oy - 45.f, oy + 45.f, 0.f }); break;
            case 101: g_rtPortals.push_back({ 3, ox, oy - 45.f, oy + 45.f, 0.f }); break;
            // Dual and solo portals: 286 splits the run into two players, 287 merges them back.
            case 286: g_rtDual.push_back({ ox, true,  oy }); break;
            case 287: g_rtDual.push_back({ ox, false, oy }); break;
            // Move triggers. Only the ones that fire because the run passes them: touch triggers
            // wait for an input we cannot know about, and spawn triggers are fired by other
            // triggers rather than by position, so neither can be placed on a timeline here.
            // Spawn triggers. A level that drives a platform parks its move trigger off to the
            // side and fires it from here, so the move's own x says nothing about WHEN it happens -
            // which is why four of them were thrown out as having no time, and why the platform
            // never moved as far as the route was concerned.
            case 1268: {
                auto* e = static_cast<EffectGameObject*>(o);
                if (e->m_targetGroupID > 0)
                    spawns.push_back({ ox, e->m_targetGroupID, e->m_spawnTriggerDelay });
                break;
            }
            case 901: {
                auto* e = static_cast<EffectGameObject*>(o);
                trSeen++;
                if (e->m_isTouchTriggered) trTouch++;      // counted, not skipped: a touch move
                if (e->m_isSpawnTriggered) trSpawn++;      // still moves the platform, and a level
                                                           // places it near where it acts
                if (e->m_targetGroupID <= 0) { trNoGroup++; break; }
                if (e->m_lockToPlayerX || e->m_lockToPlayerY) {
                    trLock++;
                    skipG.push_back({ e->m_targetGroupID, 1, ox,
                                      e->m_lockToPlayerX, e->m_lockToPlayerY,
                                      (float)e->m_duration });
                    break;
                }
                if (!(std::fabs(e->m_moveOffset.x) > 0.01f
                   || std::fabs(e->m_moveOffset.y) > 0.01f)) { trNoOffset++; break; }
                // Its own position is only a fallback. If a spawn trigger fires it, that is when it
                // happens, and it is resolved once every spawn trigger has been seen.
                pend.push_back({ o, canonTimeAtX(pl, ox), e->m_targetGroupID,
                                 e->m_moveOffset.x, e->m_moveOffset.y,
                                 e->m_duration, (int)e->m_easingType, e->m_easingRate,
                                 e->m_isSpawnTriggered || e->m_isTouchTriggered });
                break;
            }
            default: break;
        }
    }
    // Which trigger types does this level actually use? Move triggers here move nothing but
    // decoration, so whatever carries the platform's collision is a different object entirely -
    // 2.2 added Advanced Follow, Keyframe and Area Move, any of which can translate real geometry.
    // Listed by id so the next step is a lookup rather than a guess.
    {
        std::vector<std::pair<int,int>> ids;
        for (int i = 0; i < n; i++) {
            auto* q = static_cast<GameObject*>(pl->m_objects->objectAtIndex(i));
            if (!q || q->m_objectID < 900) continue;
            bool found = false;
            for (auto& e : ids) if (e.first == q->m_objectID) { e.second++; found = true; break; }
            if (!found) ids.push_back({ q->m_objectID, 1 });
        }
        std::sort(ids.begin(), ids.end(),
                  [](std::pair<int,int> const& a, std::pair<int,int> const& b) {
                      return a.second > b.second; });
        std::string t;
        for (size_t i = 0; i < ids.size() && i < 24; i++)
            t += fmt::format("{}x{} ", ids[i].first, ids[i].second);
        log::info("[CI-TRIGIDS] trigger-range object ids on this level: {}", t);
    }
    // When does each move trigger actually fire? A spawn trigger at x fires everything in its
    // target group after its delay, so any move trigger sitting in that group fires then. Chains
    // are followed a few links deep, because levels do spawn a spawn.
    {
        std::vector<double> fireOf(pend.size(), -1.0);
        for (int pass = 0; pass < 3; pass++) {
            for (auto const& sp : spawns) {
                if (sp.group <= 0 || (size_t)sp.group >= pl->m_groups.size()) continue;
                CCArray* arr = pl->m_groups[sp.group];
                if (!arr) continue;
                double st = canonTimeAtX(pl, sp.x);
                if (!(st > 0.0)) continue;
                st += (double)sp.delay;
                for (unsigned int i = 0; i < arr->count(); i++) {
                    auto* go = static_cast<GameObject*>(arr->objectAtIndex(i));
                    if (!go) continue;
                    for (size_t k = 0; k < pend.size(); k++) {
                        if (pend[k].o != go) continue;
                        // Earliest firing wins: that is the one the run meets.
                        if (fireOf[k] < 0.0 || st < fireOf[k]) fireOf[k] = st;
                    }
                }
            }
        }
        int fromSpawn = 0, fromOwnX = 0, unresolved = 0;
        for (size_t k = 0; k < pend.size(); k++) {
            double ft = fireOf[k];
            if (ft > 0.0) fromSpawn++;
            else if (pend[k].firedElsewhere) {
                // Fired by a spawn or by a touch, and the chain was not followed. Its own position
                // says NOTHING about when it happens - this one sits at the level start and would
                // read as firing at t=0.2 while the run reaches its geometry at t=120.8, so the
                // route was drawn 90 units under 358 blocks that never moved. Measured: the model
                // predicted -90 for all of them and not one ever shifted. Predicting nothing is
                // right where the alternative is predicting the wrong thing confidently.
                unresolved++;
                continue;
            }
            else { ft = pend[k].ownT; if (ft > 0.0) fromOwnX++; }
            if (!(ft > 0.0)) { trNoTime++; skipG.push_back({ pend[k].group, 2, 0.f, false, false, 0.f }); continue; }
            g_rtTrigs.push_back({ ft, pend[k].group, pend[k].dx, pend[k].dy,
                                  pend[k].dur, pend[k].easing, pend[k].rate, false });
        }
        log::info("[CI-SPAWN] {} spawn triggers | move trigger timing: {} from a spawn, {} from "
                  "their own position, {} dropped as fired-elsewhere-but-unresolved, {} with no "
                  "time at all",
                  (int)spawns.size(), fromSpawn, fromOwnX, unresolved, trNoTime);
    }
    const double lockEndX = pl->m_endXPosition > 1.f ? (double)pl->m_endXPosition : 100000.0;
    for (auto const& sg : skipG) {
        if (sg.group <= 0 || (size_t)sg.group >= pl->m_groups.size()) continue;
        CCArray* arr = pl->m_groups[sg.group];
        if (!arr || arr->count() == 0) continue;
        int nS = 0, nH = 0, nSl = 0, nD = 0;
        for (unsigned int i = 0; i < arr->count(); i++) {
            auto* go = static_cast<GameObject*>(arr->objectAtIndex(i));
            if (!go) continue;
            switch (go->m_objectType) {
                case GameObjectType::Solid:      nS++;  break;
                case GameObjectType::Hazard:     nH++;  break;
                case GameObjectType::Slope:      nSl++; break;
                case GameObjectType::Decoration: nD++;  break;
                default: break;
            }
        }
        log::info("[CI-SKIPPED] a move trigger on group {} was discarded ({}) - that group holds "
                  "solid={} hazard={} slope={} decoration={}",
                  sg.group, sg.why == 1 ? "locks to the player" : "no time at its own x",
                  nS, nH, nSl, nD);
        // A platform that rides along with the run is a floor at its own height for the whole
        // stretch after its trigger, not a block sitting where the scan happened to find it.
        if (sg.why == 1 && sg.lockX && !sg.lockY && (nS || nH)) {
            // The ride is not forever. A move trigger locked to the player carries its group along
            // for its own DURATION and then lets go, so the floor exists over exactly the stretch
            // of level the run covers in that time - found by asking the level's own clock where
            // the run is when the trigger expires, rather than guessing at a speed.
            double t0 = canonTimeAtX(pl, sg.x), rideTo = lockEndX + 600.0;
            if (t0 > 0.0 && sg.dur > 0.01f) {
                double lo = sg.x, hi = lockEndX + 600.0;
                for (int it = 0; it < 40; it++) {
                    double mid = (lo + hi) * 0.5;
                    double tm = canonTimeAtX(pl, (float)mid);
                    if (tm > 0.0 && tm < t0 + (double)sg.dur) lo = mid; else hi = mid;
                }
                rideTo = lo;
            }
            for (unsigned int i = 0; i < arr->count(); i++) {
                auto* go = static_cast<GameObject*>(arr->objectAtIndex(i));
                if (!go) continue;
                if (go->m_objectType != GameObjectType::Solid
                 && go->m_objectType != GameObjectType::Hazard) continue;
                CCRect r = go->getObjectRect();
                if (!(r.size.width > 0.f) || !(r.size.height > 0.f)) continue;
                if (r.size.width > 420.f || r.size.height > 420.f) continue;
                const float sh = (go->m_objectType == GameObjectType::Hazard) ? 0.25f : 0.f;
                const float iy = r.size.height * sh;
                g_rtLocked.push_back({ sg.x, (float)rideTo, r.origin.y + iy,
                                       r.origin.y + r.size.height - iy });
                // Where it is LEFT once the lock releases. It travelled with the run, so it ends
                // that far down the level and stands there as ordinary geometry.
                if (rideTo < lockEndX + 500.0) {
                    float shift = (float)(rideTo - sg.x);
                    g_rtObstFixed.push_back({ r.origin.x + shift,
                                              r.origin.x + shift + r.size.width,
                                              r.origin.y + iy,
                                              r.origin.y + r.size.height - iy });
                }
            }
            log::info("[CI-LOCKED] group {} rides with the run from x={:.0f} to x={:.0f} "
                      "(duration {:.2f}s) - {} solid/hazard pieces are a floor at their own "
                      "height over that stretch, and ordinary geometry once it lets go",
                      sg.group, sg.x, rideTo, sg.dur, nS + nH);
        }
    }
    log::info("[CI-TRIG] {} move triggers on the level: {} usable | skipped: touch={} spawn={} "
              "noTargetGroup={} locksToPlayer={} zeroOffset={} noTimeAtX={}",
              trSeen, (int)g_rtTrigs.size(), trTouch, trSpawn, trNoGroup, trLock,
              trNoOffset, trNoTime);
    // Ask each trigger what it moves.
    int grpMissing = 0, grpEmpty = 0;
    for (size_t ti = 0; ti < g_rtTrigs.size(); ti++) {
        int g = g_rtTrigs[ti].group;
        if (g <= 0 || (size_t)g >= pl->m_groups.size()) { grpMissing++; continue; }
        CCArray* arr = pl->m_groups[g];
        if (!arr || arr->count() == 0) { grpEmpty++; continue; }
        for (unsigned int i = 0; i < arr->count(); i++) {
            auto* go = static_cast<GameObject*>(arr->objectAtIndex(i));
            if (go) g_rtLinks.push_back({ go, (int)ti });
        }
    }
    std::sort(g_rtLinks.begin(), g_rtLinks.end(),
              [](RtMoverLink const& a, RtMoverLink const& b) { return a.o < b.o; });
    // What ARE the objects these triggers move? If they are all decoration then nothing collidable
    // moves on this level and the whole question is moot; if solids are in there, the fault is
    // further down. Counted by type because that is the filter the second pass applies.
    {
        int byType[40] = {0}, solidish = 0;
        for (auto const& lk : g_rtLinks) {
            if (!lk.o) continue;
            int t = (int)lk.o->m_objectType;
            if (t >= 0 && t < 40) byType[t]++;
            if (lk.o->m_objectType == GameObjectType::Solid
             || lk.o->m_objectType == GameObjectType::Hazard
             || lk.o->m_objectType == GameObjectType::Slope) solidish++;
        }
        std::string th;
        for (int t = 0; t < 40; t++) if (byType[t]) th += fmt::format("{}:{} ", t, byType[t]);
        log::info("[CI-GRP] moved objects by type -> {} | {} of them are Solid/Hazard/Slope",
                  th, solidish);
        // Per target group, because "none of them are solid" across the whole level hides the case
        // that matters: one group holding the platform you actually land on. A move trigger's group
        // number is visible in the editor, so this can be checked against the level directly.
        {
            std::vector<int> seenG;
            for (auto const& tr : g_rtTrigs) {
                if (std::find(seenG.begin(), seenG.end(), tr.group) != seenG.end()) continue;
                seenG.push_back(tr.group);
                if ((size_t)tr.group >= pl->m_groups.size()) continue;
                CCArray* arr = pl->m_groups[tr.group];
                if (!arr || arr->count() == 0) continue;
                int nSolid = 0, nHaz = 0, nSlope = 0, nDec = 0, nOther = 0;
                for (unsigned int i = 0; i < arr->count(); i++) {
                    auto* go = static_cast<GameObject*>(arr->objectAtIndex(i));
                    if (!go) continue;
                    switch (go->m_objectType) {
                        case GameObjectType::Solid:      nSolid++; break;
                        case GameObjectType::Hazard:     nHaz++;   break;
                        case GameObjectType::Slope:      nSlope++; break;
                        case GameObjectType::Decoration: nDec++;   break;
                        default: nOther++; break;
                    }
                }
                if (nSolid || nHaz || nSlope || arr->count() < 400)
                    log::info("[CI-GROUP] group {} ({} objects): solid={} hazard={} slope={} "
                              "decoration={} other={} | moved ({:+.0f},{:+.0f}) over {:.1f}s",
                              tr.group, (int)arr->count(), nSolid, nHaz, nSlope, nDec, nOther,
                              tr.dx, tr.dy, tr.dur);
            }
        }
    }
    log::info("[CI-GRP] {} move triggers -> {} object links ({} target groups out of range, "
              "{} empty); m_groups has {} entries",
              (int)g_rtTrigs.size(), (int)g_rtLinks.size(), grpMissing, grpEmpty,
              (int)pl->m_groups.size());

    auto rtMoveTrigs = [](GameObject* o, int* out, int cap) {
        int n = 0;
        auto it = std::lower_bound(g_rtLinks.begin(), g_rtLinks.end(), o,
                                   [](RtMoverLink const& a, GameObject* b) { return a.o < b; });
        for (; it != g_rtLinks.end() && it->o == o && n < cap; ++it) out[n++] = it->trig;
        return n;
    };

    std::sort(gm.begin(), gm.end(), [](GmP const& a, GmP const& b) { return a.x < b.x; });
    g_rtGm.clear();
    std::sort(g_rtHaz.begin(), g_rtHaz.end(),
              [](RtObst const& a, RtObst const& b) { return a.x0 < b.x0; });
    std::sort(g_rtDual.begin(), g_rtDual.end(),
              [](RtDual const& a, RtDual const& b) { return a.x < b.x; });
    for (auto const& q : gm) g_rtGm.push_back({ q.x, q.y0, q.y1, q.wave });
    std::sort(g_rtPortals.begin(), g_rtPortals.end(),
              [](RtPortal const& a, RtPortal const& b) { return a.x < b.x; });

    double endX = pl->m_endXPosition > 1.f ? (double)pl->m_endXPosition : 0.0;
    if (!gm.empty() && (double)gm.back().x + 3000.0 > endX) endX = (double)gm.back().x + 3000.0;
    endX += 600.0;

    // Where the level is in wave. A gamemode portal is assumed hit: a level puts one where the
    // route goes, and unlike a gravity portal there is no self-consistent test available before the
    // route exists. The moment the run is seen entering a section, that assumption is replaced by
    // the position it was seen at.
    bool wave = (pl->m_levelSettings->m_startMode == 4);
    float cur = 0.f, curY = 105.f;   // GD spawns on its own ground at y ~ 105
    // A DUAL puts two gamemode portals at the same x, one per player, and taking both makes a
    // nonsense of the level. Measured on a real one: x=10035 carries a wave portal at y=195 and a
    // non-wave portal at y=315, the run went through at y=314, and the mod took the wave - then
    // drew a zigzag through a stretch flown as a ship. All five of the worst sections in the
    // calibration were exactly this, so the diagnosis is not in doubt.
    //
    // Choosing between them by continuity IS the right idea - a run cannot teleport, so it took the
    // portal nearest where it already was. What is missing is a trustworthy "where it already was".
    // curY is only refreshed at gamemode portals, so at a fork it can be thousands of units stale:
    // it read 135 where the run was at 314, chose the portal 60 units away over the one 179 away,
    // and turned five phantom sections into twelve. A 120-unit guard did not help, because a wrong
    // portal is often the nearer one.
    //
    // The height has to come from the solved route, which means scanning, solving, then re-deciding
    // the forks and solving again. That is a change to the order of the whole pass, not a patch
    // here, so until it exists both portals are taken as before - which is wrong in a known way
    // rather than wrong in a confident one.
    // Coincident opposite-mode portal groups. Anchored on gm[i] rather than chained, so a ladder
    // of portals stepping along in x cannot grow into one group; a cluster of three or six at one
    // exact x falls out for free, because what defines a fork is holding both kinds at one x.
    // A fork is a wave portal and a non-wave portal close together in x AND far apart in y.
    //
    // Both halves are needed and both are measured. Across 44 sections graded against recorded
    // runs, mouths carrying both kinds split cleanly: the phantoms sat 120, 131 and 180 units
    // apart vertically, while genuine ship-to-wave transitions sat 2, 22, 44 and 61 apart - you
    // fly straight through a real transition, so its two portals are at the same height. A pair
    // 130 units apart is not one player changing mode, it is two players.
    //
    // The one correct section with a wide pair (486 apart) is a fork as well; the mod guessed it
    // right by luck. Masking it is the same honest answer - at a fork nothing here knows which
    // portal the icon went through, and a line that is right by coin toss is not worth the three
    // wrong ones it comes with.
    const float FK_DX = 80.f, FK_DY = 100.f;
    std::vector<uint8_t> fkGrp(gm.size(), 0);
    std::vector<int>     fkHead(gm.size(), -1);
    int fkGroups = 0;
    for (size_t i = 0; i < gm.size(); ) {
        size_t j = i;
        while (j < gm.size() && gm[j].x - gm[i].x <= FK_DX) j++;
        bool hasW = false, hasN = false;
        float ysep = 0.f;
        for (size_t k = i; k < j; k++) { if (gm[k].wave) hasW = true; else hasN = true; }
        if (hasW && hasN)
            for (size_t k = i; k < j; k++)
                for (size_t m = i; m < j; m++)
                    if (gm[k].wave != gm[m].wave)
                        ysep = std::max(ysep, std::fabs((gm[k].y0 + gm[k].y1) * 0.5f
                                                      - (gm[m].y0 + gm[m].y1) * 0.5f));
        if (hasW && hasN && ysep >= FK_DY) {
            fkGroups++;
            for (size_t k = i; k < j; k++) { fkGrp[k] = 1; fkHead[k] = (int)i; }
        }
        i = j;
    }
    if (fkGroups) log::info("[CI-FORKMASK] {} fork(s) on this level - a wave portal and a non-wave "
                            "portal within {:.0f} units of x and {:.0f}+ apart in y. Which one the "
                            "icon went through is not knowable here, so those sections are solved "
                            "and graded but never drawn", fkGroups, FK_DX, FK_DY);

    bool openFork = false;
    for (size_t gi = 0; gi < gm.size(); gi++) {
        // A DUAL fork - two gamemode portals at one x, one per player - is the confirmed cause
        // of the worst errors this route makes. Measured against recorded runs: all five phantom
        // wave sections had an entry portal the run never passed through, all thirty-nine real ones
        // did, and where the gamemode is right the simulation lands within 0.5 to 2.2 units. It is
        // not a physics problem. It is knowing which portal was taken.
        //
        // Three ways of choosing were tried and all three came out worse than taking both:
        //   - nearest to the running height: that height is a portal's position from thousands of
        //     units earlier, and read 135 where the run was at 314
        //   - the same with a 120-unit guard: the wrong portal is often the nearer one
        //   - the higher one for player 1, which is right for the three forks measured and wrong
        //     often enough elsewhere to lose more than it gained
        //
        // What they share is deciding the fork before there is a route to decide it with. The way
        // that works is to solve once, then re-decide each fork against the SOLVED route's height -
        // which the calibration shows is good to a couple of units inside a wave section - and
        // solve again. That is a change to the order of scan and solve, not a rule, and until it
        // exists both portals are taken: wrong in a known way rather than wrong confidently.
        GmP const& p = gm[gi];
        if (fkGrp[gi]) {
            // The whole group is resolved once, at its head. What happens today depends on which
            // member std::sort happened to place first - non-wave first swallows itself at the
            // same-state test and the wave portal then opens a phantom; wave first opens at cur=x
            // and the non-wave's "p.x > cur" is false at equal x, so the pair cancels and nothing
            // is created. std::sort is not stable, so that is a coin flip, not a rule. Neither face
            // is knowable, so both are replaced by the same honest answer: build it, grade it,
            // never draw it.
            if (fkHead[gi] != (int)gi) continue;
            size_t gEnd = gi;
            while (gEnd < gm.size() && fkHead[gEnd] == (int)gi) gEnd++;
            float yW = 0.f, yN = 0.f; bool gotW = false, gotN = false;
            for (size_t k = gi; k < gEnd; k++) {
                float my = (gm[k].y0 + gm[k].y1) * 0.5f;
                if (gm[k].wave) { if (!gotW) { yW = my; gotW = true; } }
                else            { if (!gotN) { yN = my; gotN = true; } }
            }
            // An open section still has to CLOSE here - it may not run on through a pair it cannot
            // resolve - but the stretch beyond is still made a section, so the geometry pass still
            // collects it and the calibration still grades it. It is only marked.
            if (wave && p.x > cur) {
                RtSection s; s.x0 = cur; s.x1 = p.x; s.priorY = curY;
                s.exitY = yN; s.fork = openFork;
                g_rtSecs.push_back(s);
            }
            cur = p.x; curY = yW; wave = true; openFork = true;
            gi = gEnd - 1;
            continue;
        }
        // A portal that does NOT change the gamemode says nothing about where the run is. It may
        // be the other player's, or simply redundant - this level starts in wave and carries a
        // wave portal anyway - and the run need never pass through it.
        //
        // Overwriting the height with it is how the route ended up starting at y=-471 on a level
        // whose ground is at 91: the spawn height of 105, which is exactly known, was thrown away
        // for a portal mouth at -375 that the run never went near. The line was then drawn far
        // below the level and simply was not on screen for the first few seconds.
        if (p.wave == wave) continue;
        if (p.wave) { cur = p.x; curY = (p.y0 + p.y1) * 0.5f; wave = true; openFork = false; }
        else {
            if (p.x > cur) {
                RtSection s; s.x0 = cur; s.x1 = p.x; s.priorY = curY;
                // The mouth it LEAVES by is known too, and it is a second constraint on the one
                // unknown this section has. Measured over 81 sections against recorded runs, short
                // sections are worse than long ones - median 46.6 units against 23.1 - which is
                // drift the wrong way round and almost exactly the +-45 half height of a portal's
                // mouth. The height is not drifting; it is being guessed from one end.
                s.exitY = (p.y0 + p.y1) * 0.5f;
                s.fork = openFork;
                g_rtSecs.push_back(s);
            }
            curY = (p.y0 + p.y1) * 0.5f;
            wave = false;
        }
    }
    if (wave && endX > (double)cur) {
        RtSection s; s.x0 = cur; s.x1 = (float)endX; s.priorY = curY;
        s.fork = openFork; g_rtSecs.push_back(s);
    }
    {
        int nt = 0;
        for (auto const& q : g_rtPortals) if (q.kind == 4) nt++;
        if (nt) log::info("[CI-TP] {} teleport portals on this level - they place the run at a new "
                          "height outright, and were invisible to the route until now", nt);
    }
    // Which build is actually running. Builds reach the test machine over Discord, which renames
    // a download to "(1)", "(2)" when the name is taken, and Geode will happily load an old file
    // claiming the same mod id. Two rounds tonight were spent reading results from a build that
    // did not contain the change being tested. A compile stamp costs nothing and ends that.
    log::info("[CI-BUILD] compiled " __DATE__ " " __TIME__);
    log::info("[CI-SCAN] startMode={} startMini={} endX={:.0f} gmPortals={} gravSizePortals={} sections={}",
              (int)pl->m_levelSettings->m_startMode, pl->m_levelSettings->m_startMini ? 1 : 0,
              (double)pl->m_endXPosition, (int)gm.size(), (int)g_rtPortals.size(), (int)g_rtSecs.size());
    for (size_t gi = 0; gi < gm.size() && gi < 12; gi++)
        log::info("[CI-SCAN]   gmPortal x={:.0f} wave={} y={:.0f}", gm[gi].x, gm[gi].wave ? 1 : 0,
                  (gm[gi].y0 + gm[gi].y1) * 0.5f);
    for (size_t si = 0; si < g_rtSecs.size() && si < 8; si++)
        log::info("[CI-SCAN]   section x0={:.0f} x1={:.0f} priorY={:.0f}",
                  g_rtSecs[si].x0, g_rtSecs[si].x1, g_rtSecs[si].priorY);

    if (g_rtSecs.empty()) return;   // no wave in this level, nothing to draw

    // Gravity and size at each mouth. Applied unconditionally, which is only a guess - it cannot be
    // height tested without a route to test - but every one of these is overwritten with the live
    // value the first time the run is seen entering the section.
    for (auto& s : g_rtSecs) {
        float fl = 1.f, sz = pl->m_levelSettings->m_startMini ? 2.f : 1.f;
        for (auto const& p : g_rtPortals) {
            if (p.x > s.x0) break;
            if (p.kind == 0) fl = 1.f;
            else if (p.kind == 1) fl = -1.f;
            else if (p.kind == 2) sz = 1.f;
            else if (p.kind == 3) sz = 2.f;
        }
        s.flip = fl; s.size = sz;
    }

    // Only what a wave section could touch, and only what would actually stop a run.
    //
    // Slopes used to be skipped, because their hitbox is a triangle while getObjectRect is its
    // bounding box - and a route running correctly ALONG a 45 degree surface reads as inside that
    // box half the time. That was the right call for a box model, but it is why a slope corridor
    // had nothing to fit against and fell back to the height the run happened to enter at: the
    // wave corridor in almost every level IS a pair of slopes, so the fit was blind exactly where
    // it is needed. Worse than blind - on the standard 60-unit corridor the two bounding boxes
    // forbid every offset between them, so the feasible band is empty rather than merely wide.
    //
    // A first attempt at including them as strips along the real surface (slopeYPos +
    // slopeFloorTop) made the fit visibly WORSE - the route came out under the corridor - so it is
    // reverted until the orientation and coordinate space of those two calls are established
    // against recorded data rather than assumed.
    // A census of what the scan throws away. Every pass downstream works from what survives this
    // loop, so a route drifting through a stretch the probe reports as empty is either a genuinely
    // open level or a filter here quietly eating it, and those need opposite fixes.
    int rjSeen = 0, rjType = 0, rjOutside = 0, rjBig = 0, rjDegenerate = 0;
    int rjSlopeBig = 0, rjSlopeDeg = 0, rjSlopeNaN = 0, rjSlopeCorner = 0;
    int rjByType[24] = {0};
    for (int i = 0; i < n; i++) {
        auto* o = static_cast<GameObject*>(pl->m_objects->objectAtIndex(i));
        if (!o) continue;
        // The D/J/S/H family are markers, not walls. PlayerObject::touchedObject is a plain switch
        // on the object id - 1755 sets m_stateDartSlide (D, the one that lets a wave slide), 1813
        // m_stateNoAutoJump (J), 1829 stops a dash (S), 1859 m_stateHitHead (H), 2866 flips gravity
        // - and each is a counter decremented every frame, so these are regions the player passes
        // THROUGH. Excluded by id rather than trusted to GameObjectType, because a marker sitting in
        // the middle of a corridor that GD ever reports as solid would forbid the one band of
        // offsets the route actually needs, and it would do it silently.
        switch (o->m_objectID) {
            case 1755: case 1813: case 1829: case 1859: case 2866: continue;
            default: break;
        }
        auto ty = o->m_objectType;
        rjSeen++;
        if (ty == GameObjectType::Slope) {
            const float sxp = o->getPositionX();
            bool inSec = false;
            for (auto const& sc : g_rtSecs)
                if (sxp >= sc.x0 - 200.f && sxp <= sc.x1 + 200.f) { inSec = true; break; }
            if (!inSec) continue;
            CCRect sr = o->getObjectRect();
            if (!(sr.size.width > 0.f)) { rjSlopeDeg++; continue; }
            if (sr.size.width > 420.f || sr.size.height > 420.f) { rjSlopeBig++; continue; }
            double ya = o->slopeYPos(sr.origin.x);
            double yb = o->slopeYPos(sr.origin.x + sr.size.width);
            if (!std::isfinite(ya) || !std::isfinite(yb)) { rjSlopeNaN++; continue; }
            // Only trust it if the surface really does run corner to corner of the box - that is
            // what a slope is, and anything else means the call did not mean what we think.
            double loR = (double)sr.origin.y - 2.0, hiR = (double)sr.getMaxY() + 2.0;
            if (ya < loR || ya > hiR || yb < loR || yb > hiR) { rjSlopeCorner++; continue; }
            // The route has to be STATIC. A player reads their next few clicks off a line that is
            // already drawn, so a route that re-solves as platforms travel crawls about underneath
            // them and is worse than useless. Which means movement has to be PREDICTED, never
            // tracked - and only objects something is known to move are treated as moving at all.
            {
                int mt[12]; int nmt = rtMoveTrigs(o, mt, 12);
                if (nmt > 0) {
                    RtMover mv{};
                    mv.o = o; mv.slope = true;
                    mv.x = o->getPositionX(); mv.y = o->getPositionY();
                    mv.base = sr;
                    mv.nTrig = nmt;
                    for (int gi = 0; gi < nmt; gi++) mv.trig[gi] = mt[gi];
                    mv.arriveT = canonTimeAtX(pl, sr.getMidX());
                    g_rtMovers.push_back(mv);
                } else {
                    // The surface, and the body under it. A slope object fills the space between
                    // its face and the bottom of its own rect, and that body is what the route was
                    // sailing through while every check reported it clear.
                    g_rtSlopeFixed.push_back({ sr.origin.x, sr.origin.x + sr.size.width,
                                               (float)ya, (float)yb, (int)o->m_objectID, true });
                    // The body, as slices. A slope is a TRIANGLE: under its face the rock
                    // reaches down to the bottom of its own rect, and how far down depends on
                    // x. The first attempt emitted one rectangle from the rect bottom up to
                    // min(ya,yb) - and for a face that runs corner to corner those are the
                    // same number, so it was always empty. The solid count did not move by a
                    // single block, which is how it was caught.
                    {
                        const int SL = 6;
                        const float w = sr.size.width / (float)SL;
                        for (int k = 0; k < SL; k++) {
                            const float xa = sr.origin.x + w * k;
                            const float xb = xa + w;
                            const float t  = (k + 0.5f) / (float)SL;
                            const float fy = (float)(ya + (yb - ya) * t);
                            if (fy - sr.origin.y > 2.f)
                                g_rtObstFixed.push_back({ xa, xb, sr.origin.y, fy,
                                                          (int)o->m_objectID });
                        }
                    }
                }
                g_rtWatch.push_back({ o, o->getPositionX(), o->getPositionY() });
            }
            continue;
        }
        // A WAVE DOES NOT SLIDE ON A SPIKE.
        //
        // Hazards were collected alongside solids and fed to the same collision, so the route came
        // to rest on them and skated along their tops. In the player's screenshot the line goes
        // shallow exactly as it passes a spiked ball and steepens again after it - a bend with no
        // block anywhere near it, which is what "why is it curving when a wave only goes 45
        // degrees" was pointing at.
        //
        // The reasoning that put them in was that the route should not pass through a spike. But
        // the run being drawn SURVIVED: it never touched one. So a hazard can only ever push the
        // line somewhere the run was not, and leaving them out costs nothing and removes a whole
        // class of invented corners.
        if (ty == GameObjectType::Hazard) {
            CCRect hr = o->getObjectRect();
            if (hr.size.width > 0.f && hr.size.height > 0.f
                && hr.size.width <= 420.f && hr.size.height <= 420.f) {
                // GD grades a hazard on an inner box, not the sprite - a spike kills near its
                // middle, so the drawn line may legitimately clip the corner of one.
                const float hx = hr.size.width * 0.25f, hy = hr.size.height * 0.25f;
                g_rtHaz.push_back({ hr.origin.x + hx, hr.origin.x + hr.size.width - hx,
                                    hr.origin.y + hy, hr.origin.y + hr.size.height - hy });
            }
            continue;
        }
        if (ty != GameObjectType::Solid && ty != GameObjectType::Hazard) {
            rjType++;
            if ((int)ty >= 0 && (int)ty < 24) rjByType[(int)ty]++;
            continue;
        }
        const float ox = o->getPositionX();
        bool inSection = false;
        for (auto const& s : g_rtSecs)
            if (ox >= s.x0 - 200.f && ox <= s.x1 + 200.f) { inSection = true; break; }
        if (!inSection) { rjOutside++; continue; }
        CCRect r = o->getObjectRect();
        if (!(r.size.width > 0.f) || !(r.size.height > 0.f)) { rjDegenerate++; continue; }
        // A scaled up backdrop is not a wall to thread a route through, and it would forbid every
        // offset at once.
        if (r.size.width > 420.f || r.size.height > 420.f) { rjBig++; continue; }
        // GD grades a hazard on an inner box, not the sprite - a spike kills near its middle.
        const float sh = (ty == GameObjectType::Hazard) ? 0.25f : 0.f;
        const float ix = r.size.width * sh, iy = r.size.height * sh;
        {
            int mt[12]; int nmt = rtMoveTrigs(o, mt, 12);
            if (nmt > 0) {
                RtMover mv{};
                mv.o = o; mv.slope = false;
                mv.x = o->getPositionX(); mv.y = o->getPositionY();
                mv.base = CCRect(r.origin.x + ix, r.origin.y + iy,
                                 r.size.width - ix * 2.f, r.size.height - iy * 2.f);
                mv.nTrig = nmt;
                for (int gi = 0; gi < nmt; gi++) mv.trig[gi] = mt[gi];
                mv.arriveT = canonTimeAtX(pl, r.getMidX());
                g_rtMovers.push_back(mv);
            } else {
                g_rtObstFixed.push_back({ r.origin.x + ix, r.origin.x + r.size.width - ix,
                                          r.origin.y + iy, r.origin.y + r.size.height - iy,
                                          (int)o->m_objectID });
            }
            g_rtWatch.push_back({ o, o->getPositionX(), o->getPositionY() });
        }
        if (g_rtObstFixed.size() >= 80000) break;
    }
    // GD's own floor and ceiling. They are not objects, so nothing above collected them, and a
    // wave rests on them exactly as it rests on a block - which is the whole of "it doesn't count
    // the floor as a D block". Without them a route that drifts low simply leaves the level
    // through a floor that, in the game, it would have slid along.
    //
    // Measured against the recorded runs: no run in 38 files with positions ever goes below about
    // 95, and the commonest minimum is exactly 105.00 across ten of them - a big wave's centre
    // resting 15 above a surface at 90. So 90 is the fallback when the ground layer cannot be read.
    {
        double gy = 90.0;
        auto toObj = [&](CCNode* nd) -> double {
            if (!nd || !nd->getParent() || !pl->m_objectLayer) return NAN;
            CCPoint w = nd->getParent()->convertToWorldSpace(ccp(0.f, nd->getPositionY()));
            return (double)pl->m_objectLayer->convertToNodeSpace(w).y;
        };
        double g1 = toObj(pl->m_groundLayer), g2 = toObj(pl->m_groundLayer2);
        if (std::isfinite(g1) && g1 > -2000.0 && g1 < 2000.0) gy = g1;

        // The ceiling is real and it matters: without it the route escapes UPWARD, which is where
        // it kept going - lost at 675, at 922, and reading "nothing above" at five of eleven probe
        // points on a level whose corridors all sit between 180 and 300.
        //
        // The first attempt at this walled the route in for the whole level, but that was a
        // different bug - a route inside a solid was being confined by it instead of pushed out of
        // it - and the reading itself was right.
        //
        // Checked against the level rather than trusted: a level's own geometry cannot sit above
        // its ceiling, so if much of it does then whatever that number is, it is not a ceiling.
        double cy = NAN;
        int aboveCeil = 0, total = 0;
        if (std::isfinite(g2) && g2 > gy + 200.0) {
            // Against ALL the level's solids, not just the movable ones. Sampling only the movers
            // is sampling a biased handful - trigger-driven platforms sit high by their nature, so
            // 56 of 358 of them were above the ceiling and threw out a reading that 17,585 fixed
            // blocks agree with.
            for (auto const& ob : g_rtObstFixed) {
                total++;
                if ((double)ob.y0 > g2 + 10.0) aboveCeil++;
            }
            for (auto const& m : g_rtMovers) {
                if (m.slope) continue;
                total++;
                if ((double)m.base.origin.y > g2 + 10.0) aboveCeil++;
            }
            if (total == 0 || aboveCeil * 20 < total) cy = g2;
        }
        g_rtFloor = (float)gy;
        log::info("[CI-GROUND] groundLayer={:.1f} groundLayer2={:.1f} -> floor={:.1f} ceiling={} "
                  "({} of {} solids sit above it)",
                  g1, g2, gy, std::isfinite(cy) ? fmt::format("{:.1f}", cy) : "rejected",
                  aboveCeil, total);
        // In slabs rather than one long box, so the rolling obstacle window stays narrow.
        // Player-locked platforms, laid down as a continuous floor from where their trigger fires.
        // In slabs like the ground, so the obstacle window stays narrow.
        for (auto const& lk : g_rtLocked)
            for (double bx = (double)lk.fromX; bx < (double)lk.toX; bx += 480.0)
                g_rtObstFixed.push_back({ (float)bx, (float)std::min(bx + 480.0, (double)lk.toX),
                                          lk.y0, lk.y1 });
        for (double bx = -600.0; bx < endX + 600.0; bx += 480.0) {
            g_rtObstFixed.push_back({ (float)bx, (float)(bx + 480.0),
                                      (float)(gy - 300.0), (float)gy });
            if (std::isfinite(cy))
                g_rtObstFixed.push_back({ (float)bx, (float)(bx + 480.0),
                                          (float)cy, (float)(cy + 300.0) });
        }
    }
    rtComposeGeometry();
    // Where the trigger-driven geometry IS, and what the model does to it. A count of movers says
    // nothing on its own: if every predicted offset comes out zero then the route is drawn exactly
    // where it was before and the whole chain, however well connected, has changed nothing.
    {
        int haveT = 0, nonZero = 0, shown = 0;
        double biggest = 0.0;
        std::string sample;
        for (auto const& m : g_rtMovers) {
            if (m.arriveT > 0.0) haveT++;
            double ox = 0.0, oy = 0.0;
            bool got = rtPredictOffset(m, ox, oy);
            if (got && (std::fabs(ox) > 0.5 || std::fabs(oy) > 0.5)) {
                nonZero++;
                if (std::fabs(oy) > std::fabs(biggest)) biggest = oy;
                if (shown < 10) {
                    shown++;
                    sample += fmt::format("[x{:.0f} y{:.0f} t{:.1f} -> {:+.0f},{:+.0f}] ",
                                          m.base.origin.x, m.base.origin.y, m.arriveT, ox, oy);
                }
            }
        }
        log::info("[CI-PRED] {} movers | {} have an arrival time | {} still to move after the scan "
                  "| biggest remaining move {:+.0f} | scanned at t={:.2f}",
                  (int)g_rtMovers.size(), haveT, nonZero, biggest, g_rtScanT);
        log::info("[CI-PRED] sample: {}", sample.empty() ? "none moved at all" : sample);
        // Where the movable geometry actually is. All of it clustered at one x means one structure
        // is being modelled and every other moving platform on the level is still being drawn where
        // it started.
        {
            float mnx = 1e9f, mxx = -1e9f;
            int bins[10] = {0};
            for (auto const& m : g_rtMovers) {
                mnx = std::min(mnx, m.base.origin.x);
                mxx = std::max(mxx, m.base.origin.x);
            }
            for (auto const& m : g_rtMovers) {
                int b = (int)(m.base.origin.x / 10000.0f);
                if (b >= 0 && b < 10) bins[b]++;
            }
            std::string sp;
            for (int b = 0; b < 10; b++) sp += fmt::format("{}k:{} ", b * 10, bins[b]);
            log::info("[CI-PRED] movers span x {:.0f}..{:.0f} | per 10k of level: {}", mnx, mxx, sp);
        }
        // Every trigger that touches collidable geometry, with when it fires against when the run
        // gets there. A trigger parked early but fired late by a spawn chain we did not follow
        // reads as "already finished" and moves a platform that has not moved.
        {
            std::string tl;
            int shownT = 0;
            for (size_t ti = 0; ti < g_rtTrigs.size() && shownT < 12; ti++) {
                int hits = 0;
                double arr = 0.0;
                for (auto const& m : g_rtMovers)
                    for (int i = 0; i < m.nTrig; i++)
                        if (m.trig[i] == (int)ti) { hits++; arr = m.arriveT; break; }
                if (!hits) continue;
                shownT++;
                tl += fmt::format("[grp{} fires t{:.1f} run arrives t{:.1f} move {:+.0f},{:+.0f} "
                                  "over {:.1f}s x{} objs] ",
                                  g_rtTrigs[ti].group, g_rtTrigs[ti].fireT, arr,
                                  g_rtTrigs[ti].dx, g_rtTrigs[ti].dy, g_rtTrigs[ti].dur, hits);
            }
            log::info("[CI-PRED] triggers that move collidable geometry: {}",
                      tl.empty() ? "none" : tl);
        }
    }
    {
        int pred = 0;
        for (auto const& m : g_rtMovers) if (m.predicted) pred++;
        log::info("[CI-MOVE] {} collidable objects are trigger driven ({} of them predicted "
                  "from {} move triggers); {} fixed",
                  (int)g_rtMovers.size(), pred, (int)g_rtTrigs.size(),
                  (int)g_rtObstFixed.size());
    }

    if (pl->m_player1) {
        cocos2d::CCRect pr = pl->m_player1->getObjectRect();
        double sc = (double)pl->m_player1->m_vehicleSize;
        if (!(sc > 0.05)) sc = 1.0;
        double hb = (double)pr.size.height * 0.5 / sc;
        log::info("[CI-HITBOX] player rect {:.1f}x{:.1f} scale={:.2f} -> half height big={:.2f} "
                  "mini={:.2f} (was assuming 15.0/9.0)",
                  pr.size.width, pr.size.height, sc, hb, hb * 0.6);
        // MEASURE A WAVE, OR DO NOT MEASURE AT ALL.
        //
        // This took the hitbox of whatever gamemode the icon happened to be in when the level was
        // scanned - a ship, a ball, a cube - and used it as the wave's half height for the whole
        // level. That number is SKIN in slideTo, which is what keeps the route's CENTRE a half
        // height above a surface rather than sitting on it, so every resting and sliding stretch
        // comes out low by (15 - SKIN) units. The author already measured the failure at the other
        // extreme: "At 1 unit the route sat 14 units low on every single slide, the whole way
        // along." A window of 2 to 26 accepts almost anything and guarantees the wrong answer on
        // any level that does not open in wave.
        //
        // 15.0 and 9.0 are not guesses - they are the calibrated values, 1,276 measured big-wave
        // rests at surface+15 against 259 for the runner up. So only a genuine wave measurement is
        // allowed to replace them, and only if it is close enough to be credible.
        const bool isWaveNow = pl->m_player1->m_isDart;
        if (isWaveNow && hb > 10.0 && hb < 22.0) {
            g_rtHalfBig = hb;
        } else {
            log::info("[CI-HITBOX] not a wave measurement (dart={}, half={:.2f}) - keeping the "
                      "calibrated 15.0", isWaveNow ? 1 : 0, hb);
        }
    }
    log::info("[CI-REJECT] objects={} kept={}b {}s | wrongType={} outsideSection={} tooBig={} "
              "degenerate={} | slope: tooBig={} degenerate={} noYPos={} notCornerToCorner={}",
              rjSeen, (int)g_rtObst.size(), (int)g_rtSlopes.size(),
              rjType, rjOutside, rjBig, rjDegenerate,
              rjSlopeBig, rjSlopeDeg, rjSlopeNaN, rjSlopeCorner);
    {
        std::string t;
        for (int i = 0; i < 24; i++) if (rjByType[i]) t += fmt::format("{}:{} ", i, rjByType[i]);
        log::info("[CI-REJECT] rejected by GameObjectType -> {}", t);
    }
    // Where the geometry actually is. A route can only be held where something was collected, so
    // the gaps in this row are exactly the stretches where it is free to drift.
    if (!g_rtSecs.empty()) {
        double lx = 0.0;
        for (auto const& sc : g_rtSecs) lx = std::max(lx, (double)sc.x1);
        if (lx > 1.0) {
            const int NB = 40;
            int cnt[NB] = {0};
            auto bump = [&](double x0, double x1) {
                int a = (int)(x0 / lx * NB), b = (int)(x1 / lx * NB);
                if (a < 0) a = 0; if (b >= NB) b = NB - 1;
                for (int k = a; k <= b && k < NB; k++) cnt[k]++;
            };
            for (auto const& ob : g_rtObst)   bump((double)ob.x0, (double)ob.x1);
            for (auto const& sl : g_rtSlopes) bump((double)sl.x0, (double)sl.x1);
            std::string row;
            for (int k = 0; k < NB; k++)
                row += cnt[k] == 0 ? '.' : (cnt[k] < 10 ? '-' : (cnt[k] < 50 ? '+' : '#'));
            log::info("[CI-COVER] 0..{:.0f} in {} bins, '.'=nothing collected: {}", lx, NB, row);
        }
    }
}

// How far the route has to move vertically to fit the level, given that it cannot change shape.
//
// A segment covering [ymin, ymax] over the x range it shares with a block covering [by0, by1]
// overlaps that block exactly when the shift d satisfies
//
//      by0 - ymax - pad  <=  d  <=  by1 - ymin + pad
//
// which is a closed interval, in closed form, per pair. So the question "which heights are even
// possible" is a union of intervals on one axis - not a search, not a simulation, and exact. The
// answer is then the point of that axis nearest 0 that nothing covers, and where the surviving band
// is narrow enough to be a corridor rather than open sky, its CENTRE, because a corridor only wide
// enough for one route is telling you where that route is.
//
// licence is how far the route is allowed to move. The intervals are built over a WIDER window than
// that on purpose: recognising a corridor means seeing both of its walls, and a channel wider than
// the licence would otherwise look identical to open sky.
//
// Returns 0 when the current height already fits, a delta when it does not, and NaN when nothing
// within the licence fits - the caller's signal to ask again over a shorter stretch of level.
static double rtFitShift(std::vector<CCPoint> const& pts, double xStart, double xEnd,
                         double pad, double licence, bool quiet = false) {
    if (pts.size() < 2 || (g_rtObst.empty() && g_rtSlopes.empty())) return 0.0;
    const double window = licence + 100.0;
    static std::vector<std::pair<double, double>> forb;
    forb.clear();

    size_t oiLo = 0;
    for (size_t i = 0; i + 1 < pts.size(); i++) {
        double sx0 = (double)pts[i].x, sx1 = (double)pts[i + 1].x;
        if (sx1 <= xStart) continue;
        if (sx0 >= xEnd) break;
        double bx = sx0 < xStart ? xStart : sx0;
        double ex = sx1 > xEnd ? xEnd : sx1;
        if (ex <= bx) continue;
        double sy0 = (double)pts[i].y;
        double k = (sx1 - sx0) > 1e-6 ? ((double)pts[i + 1].y - sy0) / (sx1 - sx0) : 0.0;
        // Segments ascend in x and blocks are sorted by their left edge, so the window start only
        // ever moves forward. Backing it off by the widest block is what makes that safe.
        while (oiLo < g_rtObst.size() && (double)g_rtObst[oiLo].x0 < bx - (double)g_rtObstMaxW) oiLo++;
        for (size_t j = oiLo; j < g_rtObst.size() && (double)g_rtObst[j].x0 < ex; j++) {
            auto const& ob = g_rtObst[j];
            double cx0 = std::max(bx, (double)ob.x0), cx1 = std::min(ex, (double)ob.x1);
            if (cx1 <= cx0) continue;
            double ya = sy0 + k * (cx0 - sx0), yb = sy0 + k * (cx1 - sx0);
            double ymin = ya < yb ? ya : yb, ymax = ya < yb ? yb : ya;
            double a = (double)ob.y0 - ymax - pad, b = (double)ob.y1 - ymin + pad;
            if (b < -window || a > window) continue;
            forb.push_back({ a, b });
        }

        // Slopes, exactly. A wave corridor is a pair of them, and their bounding boxes forbid
        // every offset between the two walls at the standard 60-unit spacing - which is why they
        // used to be left out, and why a slope corridor had nothing to centre against.
        //
        // The surface is a straight line and so is the route over the same span, so the whole
        // constraint is one comparison of two lines. Which SIDE is solid is not read from a flag -
        // slopeFloorTop turned out to select a corner rather than a side, and getting it wrong put
        // the route under the corridor. It is taken from the route instead: the recorded run did
        // not pass through this surface, so whichever side of it the run is on is the open side,
        // and the surface may only be approached to within pad of it.
        for (auto const& sl : g_rtSlopes) {
            if ((double)sl.x1 <= bx) continue;
            if ((double)sl.x0 >= ex) break;
            double cx0 = std::max(bx, (double)sl.x0), cx1 = std::min(ex, (double)sl.x1);
            if (cx1 <= cx0) continue;
            double sw = (double)sl.x1 - (double)sl.x0;
            if (sw < 1e-6) continue;
            auto surf = [&](double xq) {
                double t = (xq - (double)sl.x0) / sw;
                return (double)sl.ya + ((double)sl.yb - (double)sl.ya) * t;
            };
            double pa = sy0 + k * (cx0 - sx0), pb = sy0 + k * (cx1 - sx0);
            double sa = surf(cx0), sb = surf(cx1);
            // gap = route minus surface; both linear, so the extremes are at the ends.
            double g0 = pa - sa, g1 = pb - sb;
            double gmin = g0 < g1 ? g0 : g1, gmax = g0 < g1 ? g1 : g0;
            double fa, fb;
            if (gmin > 0.0) {
                // Route runs above this surface: it may not be pushed down onto it.
                fa = -window; fb = -gmin + pad;
            } else if (gmax < 0.0) {
                // Route runs below it: it may not be pushed up into it.
                fa = -gmax - pad; fb = window;
            } else {
                continue;   // straddles it - the side cannot be told, so say nothing
            }
            // Both guards matter and neither was here. A surface far from the route produces a
            // bound outside the search window, and writing it unclamped yields an interval whose
            // low end is above its high end - which a sort-and-merge that assumes lo <= hi will
            // happily fold into the feasible set and invent a channel where there is none. That is
            // how the route ended up below the level rather than in the corridor.
            if (fb < fa) continue;
            if (fb < -window || fa > window) continue;
            forb.push_back({ fa, fb });
            // Straddling means the route already crosses the surface somewhere in this span, which
            // means the side cannot be told from it. Say nothing rather than guess a direction.
        }
    }
    if (forb.empty()) return 0.0;

    std::sort(forb.begin(), forb.end());
    size_t w = 0;
    for (size_t i = 0; i < forb.size(); i++) {
        if (w > 0 && forb[i].first <= forb[w - 1].second)
            forb[w - 1].second = std::max(forb[w - 1].second, forb[i].second);
        else forb[w++] = forb[i];
    }
    forb.resize(w);

    // The clear band around a candidate shift, or false if that shift is inside a block.
    auto clearAt = [&](double d, double& lo, double& hi) -> bool {
        lo = -window; hi = window;
        for (auto const& f : forb) {
            if (f.first <= d && f.second >= d) return false;
            if (f.second < d) { if (f.second > lo) lo = f.second; }
            else { if (f.first < hi) hi = f.first; break; }
        }
        return hi > lo;
    };

    // A band this narrow is a corridor with two walls, and its middle is the route. A wide one, or
    // one that runs off the end of what was looked at, is open level - and there the height that
    // was measured is better evidence than anything the geometry can say.
    auto isCorridor = [&](double lo, double hi) {
        return (hi - lo) <= 60.0 && lo > -window + 0.5 && hi < window - 0.5;
    };
    static int fitLogs = 0;
    if (!quiet && fitLogs < 24) {
        fitLogs++;
        int nAbove = 0, nBelow = 0;
        for (auto const& f : forb) { if (f.second < 0.0) nBelow++; else if (f.first > 0.0) nAbove++; }
        log::info("[CI-FIT] xEnd={:.0f} boxes={} slopes={} forb={} (below0={} above0={}) first3=[{:.1f},{:.1f}] [{:.1f},{:.1f}] [{:.1f},{:.1f}]",
                  xEnd, (int)g_rtObst.size(), (int)g_rtSlopes.size(), (int)forb.size(), nBelow, nAbove,
                  forb.size() > 0 ? forb[0].first : 0.0,  forb.size() > 0 ? forb[0].second : 0.0,
                  forb.size() > 1 ? forb[1].first : 0.0,  forb.size() > 1 ? forb[1].second : 0.0,
                  forb.size() > 2 ? forb[2].first : 0.0,  forb.size() > 2 ? forb[2].second : 0.0);
    }

    auto centre = [&](double lo, double hi) {
        double c = (lo + hi) * 0.5;
        return c < -licence ? -licence : (c > licence ? licence : c);
    };

    double lo = 0.0, hi = 0.0;
    if (clearAt(0.0, lo, hi)) {
        if (!quiet && fitLogs < 24)
            log::info("[CI-FIT]   d=0 is clear: band [{:.1f},{:.1f}] w={:.1f} corridor={} -> shift {:.1f}",
                      lo, hi, hi - lo, isCorridor(lo, hi) ? 1 : 0,
                      isCorridor(lo, hi) ? centre(lo, hi) : 0.0);
        return isCorridor(lo, hi) ? centre(lo, hi) : 0.0;
    }

    double cov[2] = { 0.0, 0.0 }; bool got = false;
    for (auto const& f : forb)
        if (f.first <= 0.0 && f.second >= 0.0) {
            cov[0] = f.first - 1.0; cov[1] = f.second + 1.0; got = true; break;
        }
    if (!got) return 0.0;

    // The two ways out of the block the route is currently inside. A corridor beats open sky even
    // when it is the further of the two: escaping a wall the SHORT way can mean stepping over it
    // into the space above the level, which fits everything and therefore means nothing, while the
    // side that lands in a two-walled channel is the side the run was actually on.
    // THE EASIEST WAY OUT, NOT THE NEAREST ONE.
    //
    // This used to break a tie by taking the smaller offset - the closest legal height. That is the
    // wrong question. A level routes a wave down the comfortable channel; the two-pixel slot beside
    // it is legal and is never where the run went. Preferring the nearest fit is exactly how the
    // line ends up threaded through a gap it should not be in, and then reads as passing through
    // blocks when the height is a few units out.
    //
    // So among the ways out, take the ROOMIEST. A corridor still beats open sky first - escaping a
    // wall the short way can mean stepping over it into the space above the level, which fits
    // everything and therefore means nothing - and distance only breaks a tie between two openings
    // of the same width.
    int best = -1; bool bestCorr = false; double bl = 0.0, bh = 0.0, bestRoom = -1.0;
    for (int i = 0; i < 2; i++) {
        if (std::fabs(cov[i]) > licence) continue;
        double l = 0.0, h = 0.0;
        if (!clearAt(cov[i], l, h)) continue;
        bool corr = isCorridor(l, h);
        const double room = h - l;
        const bool better =
            best < 0
            || (corr && !bestCorr)
            || (corr == bestCorr && room > bestRoom + 8.0)
            || (corr == bestCorr && std::fabs(room - bestRoom) <= 8.0
                && std::fabs(cov[i]) < std::fabs(cov[best]));
        if (better) { best = i; bestCorr = corr; bl = l; bh = h; bestRoom = room; }
    }
    if (best < 0) return NAN;
    return bestCorr ? centre(bl, bh) : cov[best];
}

// Build the whole route, every wave section of the level, in world coordinates.
// The height a dual is a mirror image about - read off the LEVEL, not off the portal.
//
// It is not the dual portal's own y, which is what this assumed for three builds. Measured on a
// real level, twenty-two samples across three dual regions: the midpoint of the two icons was 240.0
// at every single one of them, while the three portals sat at y=240, y=240 and y=135. The one with
// the portal at 135 mirrors about 240 like the others.
//
// The reason is that GD does not hold the pair together at all. The two icons are independent, with
// opposite gravity, and they stay symmetric only because the LEVEL is built symmetric - so the
// height is a property of the geometry and the geometry is the thing to ask. Where the two halves
// are a mirror image, this finds the line they are a mirror image about; where they are not, it
// says so and the caller falls back.
//
// Scored as an overlap rather than an agreement: an axis out in empty sky has every bin matching
// nothing against nothing and would win on a plain agreement count.
static double rtDualAxis(double x0, double x1, double& scoreOut, double& seenOut) {
    scoreOut = 0.0; seenOut = 0.0;
    if (g_rtObst.empty() || !(x1 > x0)) return NAN;
    const double B = 15.0;
    double yLo = 1e18, yHi = -1e18;
    size_t lo = 0;
    while (lo < g_rtObst.size() && (double)g_rtObst[lo].x1 < x0) lo++;
    for (size_t j = lo; j < g_rtObst.size(); j++) {
        if ((double)g_rtObst[j].x0 > x1) break;
        yLo = std::min(yLo, (double)g_rtObst[j].y0);
        yHi = std::max(yHi, (double)g_rtObst[j].y1);
    }
    if (!(yHi > yLo)) return NAN;
    // A tall level would make the grid enormous and the fit meaningless; a dual corridor pair is a
    // few hundred units at most, so the window is bounded around the geometry actually here.
    if (yHi - yLo > 1200.0) yHi = yLo + 1200.0;
    const int nb = (int)std::min(96.0, std::max(8.0, std::ceil((yHi - yLo) / B)));
    const int nx = (int)std::min(160.0, std::max(4.0, std::ceil((x1 - x0) / 30.0)));
    const double dx = (x1 - x0) / nx;
    std::vector<uint8_t> occ((size_t)nx * nb, 0);
    size_t roll = lo;
    for (int i = 0; i < nx; i++) {
        const double qx = x0 + (i + 0.5) * dx;
        while (roll < g_rtObst.size()
               && (double)g_rtObst[roll].x1 < qx - (double)g_rtObstMaxW) roll++;
        for (size_t j = roll; j < g_rtObst.size(); j++) {
            if ((double)g_rtObst[j].x0 > qx) break;
            if ((double)g_rtObst[j].x1 < qx) continue;
            int b0 = (int)std::floor(((double)g_rtObst[j].y0 - yLo) / B);
            int b1 = (int)std::floor(((double)g_rtObst[j].y1 - yLo) / B);
            if (b1 < 0 || b0 >= nb) continue;
            b0 = std::max(b0, 0); b1 = std::min(b1, nb - 1);
            for (int b = b0; b <= b1; b++) occ[(size_t)i * nb + b] = 1;
        }
    }
    // Candidate axes half a bin apart. For A = yLo + k*B/2 the mirror of bin b is bin k-b-1, which
    // falls out exactly - no rounding and no search.
    //
    // Every occupied bin has to have an occupied mirror, and the score is the fraction that do,
    // measured against ALL the solid geometry in the stretch rather than against the part that
    // happens to pair up. Scoring only the pairs lets an axis down at the bottom of the level match
    // a sliver of floor against itself and come out at 100%, which is what the first version did.
    long total = 0;
    for (size_t i = 0; i < occ.size(); i++) total += occ[i];
    seenOut = (double)total;
    if (total < 200) return NAN;
    double best = NAN, bestScore = 0.0;
    for (int k = 0; k <= 2 * nb; k++) {
        long matched = 0;
        for (int i = 0; i < nx; i++) {
            const uint8_t* row = &occ[(size_t)i * nb];
            for (int b = 0; b < nb; b++) {
                if (!row[b]) continue;
                const int m = k - b - 1;
                if (m >= 0 && m < nb && row[m]) matched++;
            }
        }
        const double sc = (double)matched / (double)total;
        if (sc > bestScore) { bestScore = sc; best = yLo + k * B * 0.5; }
    }
    scoreOut = bestScore;
    return best;
}

// WHERE THE WAVE ACTUALLY IS, ACCORDING TO THE GAME.
//
// Everything that has gone wrong on a level like Tidal Wave comes back to one thing: the sections
// are derived by pairing up gamemode portals, there are 99 of them, and the pairing is wrong. It
// decided half that level was wave, including two single stretches of thirteen thousand units laid
// over ship gameplay. The route then had no corridor to fly down, the height fit had nothing to
// improve against, observations had no section to attach to, and 2% of the level survived.
//
// GD already knows the answer. m_player1->m_isDart is true exactly when the player is a wave - no
// portals, no inference, no pairing. The mod reads it every frame already; it is how "entered" is
// computed. It was simply never written down.
//
// So it is written down: the x range of every stretch the player was really a wave for, with the
// height, gravity and size they entered it at. That is not an estimate of a section - it IS the
// section, and the three numbers the solver has been guessing come with it.
//
// The cost is honest: a level has to be played once before this exists for it. Until then the
// portal sections are used exactly as they are today, so nothing gets worse and the first run is no
// different from now. Cached per level like the x/time map, so it is learned once and kept.
struct WaveSeen { float x0, x1, y, flip, size; };
static std::vector<WaveSeen> g_waveSeen;      // ascending x, non-overlapping
static void waveNormalize();                  // defined below; waveLoad needs it
static int  g_waveLevel = -1;
static bool g_waveDirty = false;

static std::string waveKey(int levelID) { return fmt::format("wv-{}", levelID); }

static void waveLoad(int levelID) {
    if (g_waveLevel == levelID) return;
    g_waveSeen.clear();
    g_waveLevel = levelID;
    g_waveDirty = false;
    const std::string raw = Mod::get()->getSavedValue<std::string>(waveKey(levelID), std::string(""));
    size_t i = 0;
    while (i < raw.size()) {
        size_t e = raw.find(';', i);
        if (e == std::string::npos) e = raw.size();
        float a = 0, b = 0, y = 0, f = 1, z = 1;
        if (std::sscanf(raw.c_str() + i, "%f,%f,%f,%f,%f", &a, &b, &y, &f, &z) == 5 && b > a)
            g_waveSeen.push_back({ a, b, y, f, z });
        i = e + 1;
    }
    waveNormalize();
    if (!g_waveSeen.empty())
        log::info("[CI-WAVESEEN] {} wave stretch(es) remembered for this level", (int)g_waveSeen.size());
}

static void waveSave() {
    if (!g_waveDirty || g_waveLevel < 0) return;
    std::string out;
    for (auto const& w : g_waveSeen)
        out += fmt::format("{:.0f},{:.0f},{:.1f},{:.0f},{:.0f};", w.x0, w.x1, w.y, w.flip, w.size);
    Mod::get()->setSavedValue<std::string>(waveKey(g_waveLevel), out);
    g_waveDirty = false;
}

// One stretch, closed. Merged with anything it touches, because a level played in pieces from
// different StartPos points arrives as fragments of the same corridor.
// Sorted, and with nothing overlapping anything else.
//
// The merge below only ever absorbed the FIRST entry it touched, and what waveLoad reads back off
// disk was never merged at all - so a pair recorded across two sessions could overlap forever. On
// the real level that left 19183..28010 saying the wave entered at y=614 and 26085..26426, wholly
// inside it, saying y=555. Both became sections, the clip cut the first one short at the second's
// start, and the route stepped 50 units up at the join: "it looks like its above". Two observations
// of one place cannot both be the entry, so the earlier x0 wins - it is the one that was actually
// entered - and the union takes the wider span.
static void waveNormalize() {
    if (g_waveSeen.size() < 2) return;
    std::sort(g_waveSeen.begin(), g_waveSeen.end(),
              [](WaveSeen const& a, WaveSeen const& b) { return a.x0 < b.x0; });
    std::vector<WaveSeen> out;
    for (auto const& w : g_waveSeen) {
        if (!out.empty() && w.x0 <= out.back().x1 + 90.f) {
            if (w.x1 > out.back().x1) out.back().x1 = w.x1;   // height stays with the earlier entry
        } else out.push_back(w);
    }
    if (out.size() != g_waveSeen.size()) {
        log::info("[CI-WAVESEEN] {} stretch(es) overlapped and were merged into {}",
                  (int)g_waveSeen.size(), (int)out.size());
        g_waveSeen.swap(out);
        g_waveDirty = true;
    }
}

static void waveCommit(float x0, float x1, float y, float flip, float size) {
    if (!(x1 - x0 >= 200.f)) return;              // too short to be a section worth solving
    for (auto& w : g_waveSeen) {
        if (x0 <= w.x1 + 90.f && x1 >= w.x0 - 90.f) {
            // Extending forward is free. The ENTRY height is not touched once recorded: re-reading
            // it every attempt is how a drawn line starts moving between tries, which is the one
            // thing this feature is not allowed to do.
            if (x0 < w.x0) { w.x0 = x0; w.y = y; w.flip = flip; w.size = size; }
            if (x1 > w.x1) w.x1 = x1;
            g_waveDirty = true;
            return;
        }
    }
    g_waveSeen.push_back({ x0, x1, y, flip, size });
    waveNormalize();
    g_waveDirty = true;
    log::info("[CI-WAVESEEN] the run was a wave over x={:.0f}..{:.0f} (entered at y={:.0f}) - "
              "that is a section, not a guess", x0, x1, y);
}

// Per frame. Opens a stretch when the player becomes a wave and closes it when they stop being one,
// or when x jumps backwards, which is a respawn rather than the end of a corridor.
static float g_waveFrom = -1e9f, g_waveLastX = -1e9f, g_waveY = 0.f, g_waveF = 1.f, g_waveZ = 1.f;

static void waveNote(float x, float y, bool inWave, bool flip, bool mini) {
    const bool jumped = x < g_waveLastX - 60.f;
    if (g_waveFrom > -1e8f && (!inWave || jumped)) {
        waveCommit(g_waveFrom, g_waveLastX, g_waveY, g_waveF, g_waveZ);
        g_waveFrom = -1e9f;
    }
    if (inWave) {
        if (g_waveFrom < -1e8f) {
            g_waveFrom = x; g_waveY = y;
            g_waveF = flip ? -1.f : 1.f;
            g_waveZ = mini ? 2.f : 1.f;
        }
        g_waveLastX = x;
    } else {
        g_waveLastX = x;
    }
}

static void waveEndAttempt() {
    if (g_waveFrom > -1e8f) waveCommit(g_waveFrom, g_waveLastX, g_waveY, g_waveF, g_waveZ);
    g_waveFrom = -1e9f;
    waveSave();
}

// Is there a wave CORRIDOR at this x, and where is its middle?
//
// A wave section is a corridor. It has to be: the thing moves at 45 or 63 degrees and never stops,
// so the only way a level controls one is to put walls above and below it. That makes "is this
// stretch a wave section" a question about geometry rather than about portals - and on a level with
// 99 gamemode portals, geometry is the more reliable witness of the two.
//
// The same band-finding reanchorAt uses when the route gets lost, lifted out so it can be asked
// before anything is solved. Free intervals come from merging the solid ones; a slope contributes a
// surface with no thickness, which still divides a band in two.
static double rtBandAt(double x, double refY) {
    static std::vector<std::pair<double, double>> iv;
    iv.clear();
    for (auto const& ob : g_rtObst) {
        if ((double)ob.x0 > x) break;
        if ((double)ob.x1 < x) continue;
        iv.push_back({ (double)ob.y0, (double)ob.y1 });
    }
    for (auto const& sl : g_rtSlopes) {
        if ((double)sl.x0 > x) break;
        if ((double)sl.x1 < x) continue;
        double w = (double)sl.x1 - (double)sl.x0;
        if (w < 1e-6) continue;
        iv.push_back({ (double)sl.ya + ((double)sl.yb - (double)sl.ya)
                                       * ((x - (double)sl.x0) / w), 0.0 });
        iv.back().second = iv.back().first;
    }
    if (iv.size() < 2) return NAN;
    std::sort(iv.begin(), iv.end());
    size_t w = 0;
    for (size_t i = 0; i < iv.size(); i++) {
        if (w > 0 && iv[i].first <= iv[w - 1].second)
            iv[w - 1].second = std::max(iv[w - 1].second, iv[i].second);
        else iv[w++] = iv[i];
    }
    iv.resize(w);
    double best = NAN, bestD = 1e18;
    for (size_t k = 0; k + 1 < iv.size(); k++) {
        double lo = iv[k].second, hi = iv[k + 1].first, h = hi - lo;
        if (h < 40.0 || h > 260.0) continue;          // too tight to fly, or not a corridor
        double c = (lo + hi) * 0.5, d = std::fabs(c - refY);
        if (d < bestD) { bestD = d; best = c; }
    }
    return best;
}

// SUBDIVIDE THE PORTAL SECTIONS BY WHERE THE CORRIDOR ACTUALLY IS.
//
// The portals say where the gamemode changes. With 99 of them on one level, pairing them up is
// guesswork, and on Tidal Wave it guesses badly: the walk produced two single "wave sections" of
// 12,870 and 12,902 units - about forty seconds of continuous wave each - laid over ship and cube
// gameplay. Probing the route inside them found one to three blocks per position with nothing at
// all above, where a real wave corridor probes at thirty with walls both sides. The route had
// nothing to fly down, the height fit had nothing to improve against, and 2% of the level survived.
//
// So the portal sections are kept exactly as they are and only CUT UP: the stretches of one that
// are corridor-shaped become sections, and the stretches that are open air stop pretending to be.
// A level whose portals were already right is one long corridor from end to end and comes out
// unchanged, which is the point - this may not cost anything on the levels that already work.
static void rtSubdivideByCorridor() {
    if (g_rtSecs.empty() || (g_rtObst.empty() && g_rtSlopes.empty())) return;
    const double STEP = 30.0;
    const double GAP  = 150.0;      // a doorway is not the end of a corridor
    const double MIN  = 300.0;      // shorter than this is not worth a line

    std::vector<RtSection> out;
    int cut = 0;
    for (auto const& s : g_rtSecs) {
        if (!(s.x1 - s.x0 > MIN)) { out.push_back(s); continue; }

        // Walk it once, carrying the reference height forward, so the band picked at each x is the
        // one continuous with the last rather than whichever happens to be nearest the portal.
        std::vector<std::pair<double, double>> runs;     // x0, x1 of each corridor stretch
        std::vector<double> entry;                       // the corridor's middle where each begins
        double ref = (double)s.priorY;
        double runFrom = NAN, runEntry = 0.0, lastGood = NAN;
        for (double x = (double)s.x0; x <= (double)s.x1; x += STEP) {
            const double c = rtBandAt(x, ref);
            if (std::isfinite(c)) {
                ref = c;
                if (!std::isfinite(runFrom)) { runFrom = x; runEntry = c; }
                lastGood = x;
            } else if (std::isfinite(runFrom) && x - lastGood > GAP) {
                if (lastGood - runFrom >= MIN) { runs.push_back({ runFrom, lastGood }); entry.push_back(runEntry); }
                runFrom = NAN;
            }
        }
        if (std::isfinite(runFrom) && lastGood - runFrom >= MIN) {
            runs.push_back({ runFrom, lastGood }); entry.push_back(runEntry);
        }

        // Corridor end to end, give or take: leave it exactly as it was. This is the ordinary case
        // and it must stay bit-for-bit what it is today.
        if (runs.size() == 1 && runs[0].first <= (double)s.x0 + STEP
                             && runs[0].second >= (double)s.x1 - STEP) {
            out.push_back(s);
            continue;
        }
        if (runs.empty()) {
            log::info("[CI-SPLIT] section {:.0f}..{:.0f} has no wave corridor in it at all - the "
                      "portals put a wave section over {:.0f} units of open level",
                      s.x0, s.x1, s.x1 - s.x0);
            cut++;
            continue;
        }
        log::info("[CI-SPLIT] section {:.0f}..{:.0f} ({:.0f} units) is {} separate corridor(s), not "
                  "one - the rest of it is open level the portals called wave",
                  s.x0, s.x1, s.x1 - s.x0, (int)runs.size());
        cut++;
        for (size_t i = 0; i < runs.size(); i++) {
            RtSection t = s;
            t.x0 = (float)runs[i].first;
            t.x1 = (float)runs[i].second;
            // The first piece keeps the portal's mouth; the rest never went through one, so their
            // height comes from the corridor they sit in - and they are marked as what they are.
            if (runs[i].first > (double)s.x0 + STEP) {
                t.priorY = (float)entry[i];
                t.obs = false;
                t.fork = true;          // drawn, but faintly: this height was inferred, not read
            }
            t.solved = false;
            out.push_back(t);
        }
    }
    if (cut) {
        log::info("[CI-SPLIT] {} of {} portal section(s) were not one corridor; solving {} instead",
                  cut, (int)g_rtSecs.size(), (int)out.size());
        g_rtSecs.swap(out);
    }
}

// Is there a wave CORRIDOR at this x, and where is its middle?
//
// A wave section is a corridor. It has to be: the thing moves at 45 or 63 degrees and never stops,
// so the only way a level controls one is to put walls above and below it. That makes "is this
// stretch a wave section" a question about geometry rather than about portals - and on a level with
// 99 gamemode portals, geometry is the more reliable witness of the two.
//
// The same band-finding reanchorAt uses when the route gets lost, lifted out so it can be asked
// before anything is solved. Free intervals come from merging the solid ones; a slope contributes a
// surface with no thickness, which still divides a band in two.
// ===========================================================================================
// THE GAME'S OWN PHYSICS.
//
// Everything above this line is an attempt to answer one question - where does the icon go if you
// press these buttons at these times - by rebuilding Geometry Dash's physics from the outside.
// Sections, corridor subdivision, height fitting, clearance moves, a census of which block ids
// slide a wave: all of it approximates something the game computes exactly, sixty times a second,
// a few pointers away.
//
// So ask the game. A detached PlayerObject, seeded from the real one, driven by the macro's own
// inputs and stepped through PlayerObject::update and GJBaseGameLayer::checkCollisions, produces
// the true path. Not an estimate of it. Gravity portals, mini portals, speed changes, slopes,
// move triggers that open a corridor - none of them need to be understood here, because the real
// portal acts on a real player. A gradient a wave cannot fly cannot be produced at all.
//
// checkCollisions takes an arbitrary player and an ignoreDamage flag precisely so it can be run on
// a simulation that must not die. This is the same technique the trajectory mods use; they only
// ever look a second ahead, which is why the collision window below needs care.
//
// The old solver stays as the fallback. If this cannot run - no player to clone, a step that fails
// to advance - the mod draws what it drew before rather than nothing.

static PlayerObject* g_simP = nullptr;

static void simDestroy() {
    if (!g_simP) return;
    g_simP->removeFromParentAndCleanup(true);
    g_simP->release();
    g_simP = nullptr;
}

// Put the simulated icon into the gamemode THIS SECTION is flown in.
//
// The first version copied the mode flags off the live player, which is whatever the person happens
// to be riding at the moment the solve runs. Solve during a ship part and every wave section gets
// flown as a ship: the drawn line comes out as a series of smooth gravity arcs, and a wave cannot
// produce a curve at all. That is the whole of what was on screen.
//
// A wave section is a wave section whoever is playing and whatever they are doing, so the mode
// comes from the section. Speed still comes from the live player as the best available starting
// value - the speed portals the run passes correct it from there, because a real portal acts on a
// real player.
static void simSeed(PlayerObject* d, PlayerObject* s, float flip, float size) {
    d->toggleFlyMode(false, true);
    d->toggleDartMode(true, true);
    d->m_isDart  = true;
    d->m_isShip  = false; d->m_isBall   = false; d->m_isBird = false;
    d->m_isRobot = false; d->m_isSpider = false; d->m_isSwing = false;
    d->m_isSideways   = false;
    d->m_isUpsideDown = flip < 0.f;
    d->m_gravityMod   = flip < 0.f ? -1.f : 1.f;
    d->m_vehicleSize  = size >= 1.5f ? 0.6f : 1.0f;   // 2 is the mini wave's gradient, not its scale
    d->m_playerSpeed  = s->m_playerSpeed;
    d->m_isOnSlope    = false;
    d->m_yVelocity = 0.0; d->m_yVelocityBeforeSlope = 0.0;
    d->setRotation(0.f);
}

// The objects GD has indexed at this x.
//
// checkCollisions works against m_solidCollisionObjects and m_hazardCollisionObjects, which GD
// keeps as a rolling window around the REAL player - that is why the trajectory mods only ever look
// a second ahead. A whole-level path runs tens of thousands of units past it, where those arrays
// hold nothing relevant and the simulation would fly through the level untouched.
//
// m_sections is GD's own spatial index of every object by x, in hundred-unit buckets, so the
// objects the simulation needs are already sorted - they just have to be handed to it. Used only
// once the simulation is well clear of the real player, where the built-in arrays have stopped
// being about the same part of the level.
static void simObjectsAt(PlayLayer* pl, double x, gd::vector<GameObject*>& out) {
    out.clear();
    const int sec = (int)(x / 100.0);
    const int n = (int)pl->m_sections.size();
    for (int k = sec - 1; k <= sec + 1; k++) {
        if (k < 0 || k >= n) continue;
        auto* col = pl->m_sections[k];
        if (!col) continue;
        for (auto* cell : *col) {
            if (!cell) continue;
            for (auto* o : *cell) if (o) out.push_back(o);
        }
    }
}

// Run the real physics from (x0,y0) to x1, pressing and releasing where the macro says to.
// Returns false if it could not run, and the caller falls back to the old solver.
static bool rtGameSim(PlayLayer* pl, double x0, double x1, double y0, float flip, float size,
                      std::vector<std::pair<double, char>> const& tr,
                      std::vector<CCPoint>& pts, std::vector<uint8_t>& hold) {
    if (!pl || !pl->m_player1 || !(x1 > x0)) return false;
    if (!g_simP) {
        g_simP = PlayerObject::create(1, 1, pl, pl->m_objectLayer, false);
        if (!g_simP) return false;
        g_simP->retain();
        g_simP->setVisible(false);
    }
    PlayerObject* p = g_simP;
    simSeed(p, pl->m_player1, flip, size);
    p->setPosition(ccp((float)x0, (float)y0));

    // 240 Hz, the rate 2.2 steps its physics at. Anything coarser and the icon tunnels through
    // geometry that would have stopped it.
    const float dt = 1.0f / 240.0f;
    size_t ti = 0;
    bool down = false;
    // A press already in effect when the section opens: the macro was mid-hold coming in.
    while (ti < tr.size() && tr[ti].first <= x0) { down = tr[ti].second != 0; ti++; }
    if (down) p->pushButton(PlayerButton::Jump);

    pts.clear(); hold.clear();
    gd::vector<GameObject*> nearObs;  // NOT "near" - that is a Windows macro, third time
    int nearSec = INT_MIN;
    const double realX = pl->m_player1->getPositionX();
    double lastX = x0, lastRec = x0 - 1e9;
    double lastVy = 0.0; bool lastDown = down; int recStep = 0;
    const int MAXSTEP = 200000;
    int stuck = 0;
    for (int i = 0; i < MAXSTEP; i++) {
        const double px = p->getPositionX();
        if (px >= x1) break;
        while (ti < tr.size() && tr[ti].first <= px) {
            const bool want = tr[ti].second != 0;
            if (want != down) {
                if (want) p->pushButton(PlayerButton::Jump);
                else      p->releaseButton(PlayerButton::Jump);
                down = want;
            }
            ti++;
        }
        p->update(dt);
        // Close to the real player GD's own arrays are about this part of the level, so its own
        // routine is the whole answer. Far from it they are not, and the objects have to come from
        // the spatial index instead or the simulation meets nothing at all.
        if (std::fabs(px - realX) < 400.0) {
            pl->checkCollisions(p, dt, true);
        } else {
            const int sec = (int)(px / 100.0);
            if (sec != nearSec) { simObjectsAt(pl, px, nearObs); nearSec = sec; }
            if (!nearObs.empty())
                pl->collisionCheckObjects(p, &nearObs, (int)nearObs.size(), dt);
        }
        const double nx = p->getPositionX();
        // Not advancing means the simulation is wedged - a collision it cannot resolve, or a
        // gamemode this does not drive. Better to hand back to the old solver than to emit a
        // vertical wall of points.
        if (nx - lastX < 0.001) { if (++stuck > 240) return false; }
        else stuck = 0;
        lastX = nx;
        // RECORD THE TURNS, NOT A GRID.
        //
        // Sampling every ten units puts a point wherever the grid happens to fall, which is almost
        // never where the line actually bends - so the segment spanning a turn comes out at the
        // average of the two gradients either side of it. Measured: 715 segments at exactly 1.0 and
        // 108 at exactly 2.0, with a tail of 0.8, 0.9, 1.8, 1.9 that is nothing but this. The
        // physics was right and the sampling was blurring it, which reads as "the lines arent
        // straight 45 degrees".
        //
        // A wave only changes direction when its vertical velocity changes sign or a button
        // changes, so those are the points worth keeping. Everything between them is a straight
        // line that needs no samples at all.
        const double vy = p->m_yVelocity;
        const bool turned = (vy > 0.0) != (lastVy > 0.0);
        if (turned || down != lastDown || nx - lastRec >= 40.0) {
            pts.push_back(ccp((float)nx, p->getPositionY()));
            if (pts.size() > 1) hold.push_back(lastDown ? 1 : 0);
            lastRec = nx;
            recStep = i;
        }
        lastVy = vy; lastDown = down;
        // Wedged: still stepping, but no longer getting anywhere worth recording. A wave that meets
        // a wall head on keeps its x almost still while the stuck test above, which only asks for a
        // thousandth of a unit, never fires. Keep what was flown and stop, rather than spinning to
        // the step limit and reporting a section that quietly ends early.
        if (i - recStep > 960) {
            log::info("[CI-GAMESIM] {:.0f}..{:.0f} wedged at x={:.0f} - drawing the {} points it "
                      "flew before that", x0, x1, nx, (int)pts.size());
            break;
        }
    }
    if (down) p->releaseButton(PlayerButton::Jump);
    if (pts.size() < 3) return false;

    // IS THIS ACTUALLY A WAVE PATH?
    //
    // A wave flies in straight lines: between one input and the next its gradient cannot change, so
    // a bend without a button change is either a surface it met or something that is not a wave at
    // all. Seeding the mode off the live player produced a series of smooth gravity arcs - a bend at
    // every single step - and the mod drew them without hesitating, because nothing here was
    // checking that the answer had the shape of the question.
    //
    // Cheap, and it does not care about speed or size, only about how often the line bends when
    // nothing told it to. Anything this far from piecewise-straight is not worth drawing, so the
    // old solver takes over.
    {
        int bends = 0, seg = 0;
        for (size_t i = 1; i + 1 < pts.size(); i++) {
            const double ax = pts[i].x - pts[i-1].x, bx = pts[i+1].x - pts[i].x;
            if (ax < 1e-6 || bx < 1e-6) continue;
            seg++;
            const bool btnChanged = i < hold.size() && hold[i] != hold[i-1];
            if (btnChanged) continue;
            const double m1 = (pts[i].y - pts[i-1].y) / ax;
            const double m2 = (pts[i+1].y - pts[i].y) / bx;
            if (std::fabs(m2 - m1) > 0.05) bends++;
        }
        if (seg > 8 && bends * 2 > seg) {
            log::info("[CI-GAMESIM] rejected {:.0f}..{:.0f}: {} of {} segments bend with no input - "
                      "that is not a wave path, falling back to the solver", x0, x1, bends, seg);
            return false;
        }
    }
    if (hold.size() < pts.size()) hold.resize(pts.size(), (uint8_t)0);
    return true;
}

static bool g_ghost = false;                 // a ghost run is in progress: block anything permanent
static bool g_ghostTried = false;            // once per level, success or failure
static std::vector<CCPoint> g_ghostPath;     // where the icon actually went, x ascending
static std::vector<uint8_t> g_ghostHold;
static std::vector<uint8_t> g_ghostWave;
// THE SECOND ICON.
//
// A dual is two icons flying two different lines from the same inputs, and the replay only ever
// looked at m_player1 - so half of every dual was missing. The macro already distinguishes them:
// each action carries a p2 flag, and the replay was skipping those outright rather than sending
// them to the other player. Both are driven and both are recorded; the renderer has taken a second
// path since the dual work and simply had nothing to put in it.
static std::vector<CCPoint> g_ghostPath2;
static std::vector<uint8_t> g_ghostWave2, g_ghostHold2;
static bool g_ghostGood = false;             // did the replay finish the macro and get where it goes
// Whether the drawn route came from replaying the macro through the game, rather than from the
// inference solver. It changes what the player straying from it MEANS.
static bool g_rtFromGhost = false;
// BUILDING THE HIDDEN LEVEL RUNS THE HIDDEN LEVEL'S OWN SETUP.
//
// PlayLayer::create runs setup, which calls setupHasCompleted, which is a hook this mod owns - and
// that hook is now where the path is worked out. So creating the hidden level asked for a hidden
// level, which asked for a hidden level. g_ghLayer is only assigned after create returns, so it
// could not stop it: the stack ran out first. That is the crash on entering a level.
static bool g_ghBuilding = false;
// A REPLAY THAT DIES IS A REPLAY THAT IS WRONG.
//
// Death was blocked during a replay so a hiccup could not truncate the path. What it actually did
// was hide every divergence: the icon clips something, survives because it cannot die, and carries
// on drawing a route no mortal player can fly. Showcase playback proved it - the real icon followed
// the drawn line to 0.0 units over 921 frames and died anyway, because the line goes somewhere the
// macro never did.
//
// The macro completes this level. If the replay does not, it has diverged, and the honest thing is
// to say where rather than to cache the result.
static bool   g_ghDied = false;
static double g_ghDiedX = 0.0;
// ...but GD calls destroyPlayer while a level is being SET UP, not only when something kills you.
// Counting those made every replay die at x=1 on its second step with none of its inputs applied,
// so nothing was ever kept and no level got a path at all. The check arms once the run is actually
// under way; before that a death is the game arranging itself, not the macro going wrong.
static bool   g_ghDeathArmed = false;
// IS ANYTHING ELSE PRESSING BUTTONS ON THE HIDDEN LEVEL?
//
// The replay runs inside the same game as every other mod, and anything hooking GJBaseGameLayer
// sees the hidden level too - silicate appears 140 times in one session's log. If another mod adds
// or swallows an input there, the replay flies a run the macro never described, and the drawn path
// inherits it. Playback then follows that path to 0.0 units and dies, which is exactly the state
// this is in: the timing is right and the path is not.
//
// So count both sides. Every press this code sends, and every press the layer actually receives.
// If they differ, the replay is not alone with its own inputs and that is the answer.
static long long g_ghSent = 0, g_ghSeen = 0, g_ghDeaths = 0, g_ghDeathsP1 = 0;
// THE HIDDEN LEVEL IS A DIFFERENT PlayLayer, BUT THE SAME GJGameLevel.
//
// Both point at one level object, so everything the replay achieves is written to the player's
// record: it flew to 96% and GD duly called that a new best, showed the completion screen, and
// spawned them there. A separate layer is not a separate save. These are read before it starts and
// put back when it ends, whatever happened in between.
struct GhProgress {
    GJGameLevel* lvl = nullptr;
    int normal = 0, practice = 0, attempts = 0, new2 = 0, best = 0;
    bool done = false;
    void save(GJGameLevel* l) {
        lvl = l; if (!l) return;
        normal = (int)l->m_normalPercent.value();
        practice = l->m_practicePercent;
        attempts = (int)l->m_attempts.value();
        new2 = (int)l->m_newNormalPercent2.value();
        best = l->m_bestPoints;
    }
    void restore() {
        if (!lvl) return;
        lvl->m_normalPercent = normal;
        lvl->m_practicePercent = practice;
        lvl->m_attempts = attempts;
        lvl->m_newNormalPercent2 = new2;
        lvl->m_bestPoints = best;
        lvl = nullptr;
    }
};
static GhProgress g_ghProg;
static int  g_ghPhase = 0;            // 1 = the quick one from the spawn, 2 = the whole level
static bool g_ghFullQueued = false;   // the whole level still owes us a run
static std::vector<CCPoint> g_ghQuickPath;   // kept while the full replay is still working
static std::vector<uint8_t> g_ghQuickWave, g_ghQuickHold;
static std::string g_ghostKeyNow;            // so a path measured to be wrong can be thrown away     // was the icon a wave at this point

static void rtSolve(PlayLayer* pl) {
    g_rtOk = false; g_rtPts.clear(); g_rtHold.clear(); g_rtPts2.clear(); g_rtHold2.clear();
    // THE SECOND ICON, and where it exists. Worked out before the sections because the run through
    // a dual has to be built alongside the first icon rather than folded on top of it afterwards.
    struct DReg { double x0, x1, axis; };
    std::vector<DReg> g_dualRegions;
    const bool dualOn = Mod::get()->getSettingValue<bool>("dual-mirror");
    if (dualOn && !g_rtDual.empty()) {
        std::string row;
        for (auto const& q : g_rtDual)
            row += fmt::format("[{} x={:.0f} y={:.0f}] ", q.on ? "dual" : "solo", q.x, q.y);
        log::info("[CI-DUALPORTALS] {}", row);
        double from = NAN, sumY = 0.0; int nY = 0;
        for (auto const& q : g_rtDual) {
            if (q.on) {
                // Several dual portals at one x is a level putting one in each corridor, and the
                // pair enters through them - so the axis is their midpoint, which for the ordinary
                // single portal is just its own height.
                if (!std::isfinite(from)) { from = (double)q.x; sumY = 0.0; nY = 0; }
                if ((double)q.x - from < 30.0) { sumY += (double)q.y; nY++; }
            } else if (std::isfinite(from)) {
                g_dualRegions.push_back({ from, (double)q.x, nY ? sumY / nY : NAN }); from = NAN;
            }
        }
        if (std::isfinite(from))
            g_dualRegions.push_back({ from, 1e9, nY ? sumY / nY : NAN });
    }
    if (!g_rtGeoOk || g_rtSecs.empty() || !g_wxOk || g_actions.empty()) return;

    // A run the game actually flew beats everything below this line. No sections, no entry heights,
    // no gradient rules, no clearance moves - the icon went where it went, through the real portals
    // and the real platforms, and this is the record of it.
    // While the whole level is still being flown, the line on screen is the quick one from the
    // player's own spawn - the full replay is at 0% and has nothing to say about where they are
    // standing yet. It takes over the moment it is finished.
    const bool useQuick = g_ghPhase == 2 && g_ghQuickPath.size() > 8
                       && g_ghQuickWave.size() == g_ghQuickPath.size();
    const std::vector<CCPoint>& srcPts  = useQuick ? g_ghQuickPath : g_ghostPath;
    const std::vector<uint8_t>& srcWave = useQuick ? g_ghQuickWave : g_ghostWave;
    const std::vector<uint8_t>& srcHold = useQuick ? g_ghQuickHold : g_ghostHold;

    if (srcPts.size() > 8 && srcWave.size() == srcPts.size()) {
        // The replay covers the whole level, every gamemode of it. This mod draws where to fly a
        // WAVE - a cube's arc and a ship's climb are not wrong, they are just not what anyone asked
        // to see, and on screen they read as the line leaving for the sky. So keep the wave
        // stretches and mark the joins as gaps, which is what the renderer already understands.
        g_rtPts.clear(); g_rtHold.clear();
        int runs = 0; bool open = false;
        for (size_t i = 0; i < srcPts.size(); i++) {
            if (!srcWave[i]) { open = false; continue; }
            if (!open) { if (!g_rtPts.empty()) g_rtHold.push_back(2); open = true; runs++; }
            // hold[k] describes the SEGMENT from point k to point k+1, which is how ghostRun writes
            // it and how the renderer reads it. The segment arriving at point i is therefore
            // hold[i-1], not hold[i] - taking the value at the end point shifts every hold along by
            // one, so each stroke is drawn with the button state of the stroke before it.
            else if (!g_rtPts.empty())
                g_rtHold.push_back(i > 0 && i - 1 < srcHold.size() ? srcHold[i - 1] : 0);
            g_rtPts.push_back(srcPts[i]);
        }
        if (g_rtHold.size() < g_rtPts.size()) g_rtHold.resize(g_rtPts.size(), (uint8_t)0);

        // The second icon, filtered the same way. On a solo macro g_ghostPath2 is empty and this
        // does nothing; on a dual it is the other half of the answer, and the renderer has had a
        // slot for it all along.
        g_rtPts2.clear(); g_rtHold2.clear();
        if (g_ghostPath2.size() > 8 && g_ghostWave2.size() == g_ghostPath2.size()) {
            bool open2 = false;
            for (size_t i = 0; i < g_ghostPath2.size(); i++) {
                if (!g_ghostWave2[i]) { open2 = false; continue; }
                if (!open2) { if (!g_rtPts2.empty()) g_rtHold2.push_back(2); open2 = true; }
                else if (!g_rtPts2.empty())
                    g_rtHold2.push_back(i > 0 && i - 1 < g_ghostHold2.size() ? g_ghostHold2[i - 1] : 0);
                g_rtPts2.push_back(g_ghostPath2[i]);
            }
            if (g_rtHold2.size() < g_rtPts2.size()) g_rtHold2.resize(g_rtPts2.size(), (uint8_t)0);
            if (g_rtPts2.size() <= 2) { g_rtPts2.clear(); g_rtHold2.clear(); }
            else log::info("[CI-GHOST] and the second icon: {} of {} points are wave",
                           (int)g_rtPts2.size(), (int)g_ghostPath2.size());
        }

        g_rtOk = g_rtPts.size() > 2;
        g_rtFromGhost = g_rtOk;   // this route is a recording, not an inference
        log::info("[CI-GHOST] drawing the replayed run: {} of {} points are wave, in {} stretch(es)",
                  (int)g_rtPts.size(), (int)srcPts.size(), runs);
        if (g_rtOk) return;
        // Not enough to draw. Hand back a CLEAN slate: the solver below appends to g_rtPts, so the
        // one or two stray points left here would be spliced onto the front of its route and drawn
        // as a line reaching back to wherever the replay happened to stop.
        g_rtPts.clear(); g_rtHold.clear();
    }

    // Start from the scan's own sections every time. Both steps below rewrite the list, and the
    // list is the only copy: solving twice on a rewritten list compounds it, which is how a click
    // at x=19240 crept to 19263 over thirty solves and the drawn line moved while it was on screen.
    if (!g_rtSecsBase.empty()) g_rtSecs = g_rtSecsBase;

    // Cut the portal sections down to the stretches that are actually a wave corridor. A level
    // whose portals were already right is untouched by this.
    rtSubdivideByCorridor();

    // Then put what the game SAID over the top of what the portals implied.
    //
    // Merged, not swapped. The first version replaced the whole list with the observed set, so the
    // moment one stretch was seen the other twelve sections ceased to exist and the level went from
    // thirteen guesses to one 1,683-unit observation. That is worse than either.
    //
    // Observation is always partial: it covers what has been played and nothing else. So an
    // observed stretch replaces the guesses it overlaps and leaves the rest alone, and the level
    // gets more exact as it gets played rather than smaller.
    if (!g_waveSeen.empty()) {
        std::vector<RtSection> keep;
        int split = 0;
        // Which observations have already handed their height to a section. The first version asked
        // whether a section STARTED at the stretch's x0, and after subdivision none of them does -
        // so the one measured height on the level landed on nothing and every section fell back to
        // a guess. The stretch's height belongs to the first section it covers, whatever its x0.
        std::vector<char> given(g_waveSeen.size(), 0);
        for (auto const& sec : g_rtSecs) {
            const WaveSeen* cov = nullptr; size_t ci = 0;
            for (size_t wi = 0; wi < g_waveSeen.size(); wi++)
                if (sec.x0 < g_waveSeen[wi].x1 && sec.x1 > g_waveSeen[wi].x0) {
                    cov = &g_waveSeen[wi]; ci = wi; break;
                }
            if (!cov) { keep.push_back(sec); continue; }
            // AN OBSERVATION SUPPLIES A HEIGHT. IT DOES NOT ERASE THE LEVEL'S PORTALS.
            //
            // Replacing whole sections with one observed span looked harmless because a stretch the
            // run flew really is one continuous piece of wave. But a section is not only a span: it
            // carries the gravity and the size the icon flies it at, and those change at portals
            // INSIDE the span. On the drop the scan found four sections between 19183 and 30255,
            // and 26085..26426 has flip = -1 - 341 units of upside-down wave. The observed section
            // swallowed all four and carried flip = 1 across every one of them, so that stretch was
            // drawn the right way up while the run flew it inverted, and everything after it
            // inherited the wrong assumption.
            //
            // That is the drift. A section flying free cannot correct it, and no rigid move can
            // either, because the SHAPE is wrong rather than the placement. So each portal section
            // keeps its own gravity and size, and the observation gives its height to the one it
            // actually entered at.
            RtSection t = sec;
            t.x0 = std::max(sec.x0, cov->x0);
            t.x1 = std::min(sec.x1, cov->x1);
            if (!(t.x1 - t.x0 > 1.f)) continue;
            if (!given[ci]) {
                given[ci] = 1;
                t.priorY = cov->y; t.flip = cov->flip; t.size = cov->size;
                t.exact = true;
                t.obs = true; t.obsX = cov->x0; t.obsY = cov->y;
            }
            split++;
            keep.push_back(t);
        }
        // A stretch the run flew where the scan found no section at all is still a section: that is
        // the case the whole recording exists for.
        for (auto const& w : g_waveSeen) {
            bool any = false;
            for (auto const& k : keep) if (k.x0 < w.x1 && k.x1 > w.x0) { any = true; break; }
            if (any) continue;
            RtSection t;
            t.x0 = w.x0; t.x1 = w.x1; t.priorY = w.y;
            t.flip = w.flip; t.size = w.size;
            t.exact = true;
            t.obs = true; t.obsX = w.x0; t.obsY = w.y;
            keep.push_back(t);
        }
        const int replaced = split;
        std::sort(keep.begin(), keep.end(),
                  [](RtSection const& a, RtSection const& b) { return a.x0 < b.x0; });
        // Disjoint, or the finished route is not monotonic in x. An observed stretch that starts
        // inside a kept guess leaves the drawn points running 32085 -> 26085, and the renderer
        // looks up x by binary search: it stops dead at the reversal.
        for (size_t i = 0; i + 1 < keep.size(); i++)
            if (keep[i].x1 > keep[i + 1].x0) keep[i].x1 = keep[i + 1].x0;
        log::info("[CI-WAVESEEN] {} observed wave section(s) replace {} of {} guessed from {} "
                  "gamemode portals; {} guesses kept where nothing has been seen yet",
                  (int)g_waveSeen.size(), replaced, (int)g_rtSecs.size(), (int)g_rtGm.size(),
                  (int)g_rtSecs.size() - replaced);
        g_rtSecs.swap(keep);
    }

    // One ordered list of input transitions, in level positions. Which player is the flight path
    // is not always player one: a single player macro can tag all of its inputs as player two, and
    // replaying "player one" there finds nothing and dives in a straight line forever.
    bool p2 = pathPlayerIsP2();
    // The click positions, frozen for as long as this macro is loaded on this level.
    //
    // They come from the x/time map, which is learned live and sharpens as you play - so every
    // re-solve moved every turn slightly, and the whole drawn path crept forward: route 19240,
    // 19242, 19245, 19248, 19259, 19263 over twenty-five solves of one section. A path that
    // improves while you are looking at it is not a path you can read ahead on, which is the whole
    // point of drawing it.
    //
    // Learned positions still improve; they are taken at the next load rather than mid-level.
    static std::vector<std::pair<double, char>> trFrozen;
    static std::string trFor;
    static size_t trActs = 0;
    const std::string trKey = g_activeMacro + "|" + std::to_string(g_rtGeoLevel);
    std::vector<std::pair<double, char>> tr;
    tr.reserve(g_actions.size() * 2);
    // Follow whichever icon the macro actually recorded, by weight - not "player one unless
    // player one is completely empty". A dual macro can carry a handful of stray player-one inputs
    // alongside a full player-two run: Nine Circles measured 11 against 180, which was enough to
    // send the route down the near-empty stream and give it 22 transitions instead of 372, the
    // first of them 7,000 units into the section. The majority stream is the run.
    {
        int c1 = 0, c2 = 0;
        for (auto const& a : g_actions) {
            if (!(a.wxSweet > 0.f)) continue;
            if (a.p2) c2++; else c1++;
        }
        if (c1 + c2 > 0) p2 = (c2 > c1);
    }
    for (auto const& a : g_actions) {
        if (a.p2 != p2 || !(a.wxSweet > 0.f)) continue;
        double sx = (double)a.wxSweet;
        double rx = (a.wxRel > a.wxSweet) ? (double)a.wxRel : sx + 0.001;
        tr.push_back({ sx, 1 });
        tr.push_back({ rx, 0 });
    }
    {
        int nP1 = 0, nP2 = 0;
        for (auto const& a2 : g_actions) {
            if (!(a2.wxSweet > 0.f)) continue;
            if (a2.p2) nP2++; else nP1++;
        }
        log::info("[CI-TR] p2macro={} taking p2={} actions p1={} p2={} -> transitions={} first={:.0f} last={:.0f}",
                  pathPlayerIsP2() ? 1 : 0, p2 ? 1 : 0, nP1, nP2, (int)tr.size(),
                  tr.empty() ? -1.0 : tr.front().first, tr.empty() ? -1.0 : tr.back().first);
    }
    if (tr.size() < 2) return;
    std::stable_sort(tr.begin(), tr.end(),
                     [](std::pair<double, char> const& a, std::pair<double, char> const& b) {
                         return a.first < b.first;
                     });

    std::vector<CCPoint> pts; std::vector<uint8_t> hold;
    for (auto& s : g_rtSecs) {
        if (!(s.x1 > s.x0)) continue;
        // Past the end of the macro there is no input left to describe the section, and holding the
        // last known state across it would draw one long diagonal through the rest of the level.
        if ((double)s.x0 > tr.back().first + 200.0) continue;

        size_t t0 = 0; bool held0 = false;
        while (t0 < tr.size() && tr[t0].first <= (double)s.x0) { held0 = tr[t0].second != 0; t0++; }
        size_t p0 = 0;
        while (p0 < g_rtPortals.size() && (double)g_rtPortals[p0].x <= (double)s.x0) p0++;

        // Sliding, done where it belongs: inside the integration.
        //
        // A wave that meets a surface runs along it instead of through it - what players call D
        // blocks - and for a long time this was patched on afterwards, which cannot work. Measured
        // on the route the mod was actually drawing: y=189 at x=344, correct, then -310, then
        // +1936, then +5037 by x=21238. Thousands of units above a level whose geometry lives
        // between y=100 and y=400. Nothing above it, nothing below it, so the fit had nothing to
        // say in 49 of 59 windows and no patch could reach it.
        //
        // The cause is that free integration has nothing holding it: 254 transitions of +-1 and
        // +-2, and any imbalance between held and released compounds without limit. The real run
        // does not do that, and the reason is exactly the sliding - every slide is a stretch where
        // the run travels flat instead of climbing, and 129 of them across the recorded runs cancel
        // the imbalance. So it is not a correction to the shape. It IS the shape.
        //
        // Which is why it is here now, one step at a time against the level's own collision boxes,
        // rather than as a pass over a finished polyline.
        // Rolling window into the obstacle lists. Both are sorted by x0 and the simulation walks x
        // forwards, so the scan only ever needs to start where the last one did - without this a
        // 97,000-unit level with 17,738 boxes rescans from the beginning on all 9,700 steps, which
        // is 170 million comparisons for a single route.
        size_t simOb = 0, simSl = 0;
        bool simSupported = false;   // was there anything at all near the route on the last step
        // How much of the section the route spends INSIDE the level - sandwiched between a floor
        // and a ceiling. A wave section is a corridor by definition, so a route that is right
        // should be supported nearly all the way. A phantom section's route wanders where no wave
        // was ever meant to fly: section 28815's sits 400 units above its corridor, and every
        // portal inside it misses. If that separates them it is a test needing no recording.
        int simSteps = 0, simHeld = 0, simFatal = 0;
        // SKIN is the player's half height, because the route is the path of the icon's CENTRE and
        // a wave resting on a surface has its centre a half height above it, not a hair above it.
        // Measured two ways over the recorded runs. Taking every flat stretch inside a wave - a
        // slide - and looking at its height modulo 30, big waves land on 15 in 1,276 cases against
        // 259 for the runner up: they rest on block tops, which are multiples of 30, plus 15. The
        // runner up IS the mini value, 9, which is 15 x GD's 0.6 mini scale and not the 7.5 that
        // halving the icon suggests. Both agree with where runs come to rest on the ground itself:
        // 105 = 90 + 15 in ten files, 99 = 90 + 9 in nine more.
        //
        // At 1 unit the route sat 14 units low on every single slide, the whole way along.
        auto slideTo = [&](double xPrev, double x, double fromY, double toY, double SKIN) -> double {
            const double TOL = 20.0;
            while (simOb < g_rtObst.size()
                   && (double)g_rtObst[simOb].x1 < xPrev - (double)g_rtObstMaxW) simOb++;
            while (simSl < g_rtSlopes.size()
                   && (double)g_rtSlopes[simSl].x1 < xPrev - 460.0) simSl++;
            // The band the route is allowed to be in: nearest surface below it, nearest above it.
            //
            // The whole difficulty is telling a floor from a ceiling. Judging by where a surface
            // ends up cannot: a ramp climbs while the route moves, so a floor can finish the step
            // above the route - measured, a floor going 155.6 -> 159.1 under a route sitting at
            // 156.6 - and calling it a ceiling pushes the route down through it. Judging by where
            // the surface was is right where the same ramp segment spans both ends of the step, but
            // at a segment boundary there is nothing to read: extrapolating the NEW segment
            // backwards gives a slope the old one never had, which at x=661.8 turned a ceiling into
            // a floor and left the route riding along the top of the level.
            //
            // So identify the surfaces instead of re-deciding them. Read where the floor and the
            // ceiling were when the step began, then match each surface at the end of the step to
            // whichever it is nearest: a surface travels at most a block per step while a corridor
            // is the better part of a hundred units across, so there is nothing to confuse.
            double F0 = -1e18, C0 = 1e18;
            for (size_t j = simOb; j < g_rtObst.size(); j++) {
                auto const& ob = g_rtObst[j];
                if ((double)ob.x0 > xPrev) break;
                if ((double)ob.x1 < xPrev) continue;
                if ((double)ob.y1 <= fromY) { if ((double)ob.y1 > F0) F0 = (double)ob.y1; }
                else if ((double)ob.y0 >= fromY) { if ((double)ob.y0 < C0) C0 = (double)ob.y0; }
                else {
                    // Inside it. Treat only the nearer face as real, so the route is pushed OUT of
                    // the block. Recording both faces makes it a floor above and a ceiling below at
                    // once, which is a band the route can only satisfy by sitting in the middle of
                    // the solid - measured on a bogus ceiling slab, pinned dead centre for 97,000
                    // units without a single step of movement.
                    if ((double)ob.y1 - fromY <= fromY - (double)ob.y0) {
                        if ((double)ob.y1 > F0) F0 = (double)ob.y1;
                    } else {
                        if ((double)ob.y0 < C0) C0 = (double)ob.y0;
                    }
                }
            }
            for (size_t j = simSl; j < g_rtSlopes.size(); j++) {
                auto const& sl = g_rtSlopes[j];
                if ((double)sl.x0 > xPrev) break;
                if ((double)sl.x1 < xPrev) continue;
                double w = (double)sl.x1 - (double)sl.x0;
                if (w < 1e-6) continue;
                double v = (double)sl.ya
                         + ((double)sl.yb - (double)sl.ya) * ((xPrev - (double)sl.x0) / w);
                if (v <= fromY) { if (v > F0) F0 = v; } else { if (v < C0) C0 = v; }
            }

            double below = -1e18, above = 1e18;
            auto place = [&](double v) {
                bool isFloor;
                if (F0 > -1e17 && std::fabs(v - F0) <= TOL) isFloor = true;
                else if (C0 < 1e17 && std::fabs(v - C0) <= TOL) isFloor = false;
                else isFloor = (v <= fromY);         // new surface, nothing to inherit from
                if (isFloor) { if (v > below) below = v; }
                else         { if (v < above) above = v; }
            };
            for (size_t j = simOb; j < g_rtObst.size(); j++) {
                auto const& ob = g_rtObst[j];
                if ((double)ob.x0 > x) break;
                if ((double)ob.x1 < x) continue;
                // EVERY solid is a surface. The difference between sliding and dying is which FACE
                // is touched - a wave rides the top of a block and dies against its side - so it
                // is a property of the contact, not of the block. Deciding it from the object id
                // instead was measured against a census of 349 flat stretches, which found five
                // ids: on the real level those five are 0.9% of 20,483 solids, and ids 1338/1339
                // alone - the slope bodies, the surfaces a wave spends most of its time on - are
                // 62%. Gating on that list took the level away from the clamp and the route flew
                // through all of it.
                place((double)ob.y1); place((double)ob.y0);
            }
            for (size_t j = simSl; j < g_rtSlopes.size(); j++) {
                auto const& sl = g_rtSlopes[j];
                if ((double)sl.x0 > x) break;
                if ((double)sl.x1 < x) continue;
                double w = (double)sl.x1 - (double)sl.x0;
                if (w < 1e-6) continue;
                place((double)sl.ya
                      + ((double)sl.yb - (double)sl.ya) * ((x - (double)sl.x0) / w));
            }
            // Has it still got the level around it? NEAR the level, not sandwiched inside it.
            // Requiring a floor below AND a ceiling above is a corridor assumption, and the Tidal
            // Wave drop is a thin band of blocks in open space with nothing overhead for hundreds
            // of units. A correctly placed route there reads as adrift on almost every step, which
            // fires the re-anchor - 26 times in one run - and each of those is a visible jump in
            // the line and, eventually, a section that gives up part way through.
            simSupported = (F0 > -1e17 && fromY - F0 < 500.0)
                        || (C0 <  1e17 && C0 - fromY < 500.0);
            simSteps++; if (simSupported) simHeld++;
            double lo = (below > -1e17) ? below + SKIN : -1e18;
            double hi = (above <  1e17) ? above - SKIN :  1e18;
            if (lo > hi) return (lo + hi) * 0.5;      // thinner than the icon: split it
            const double outY = toY < lo ? lo : (toY > hi ? hi : toY);
            // A hazard is fatal on any face, so a route crossing one is a placement error, not a
            // surface to bend around. Hazards are deliberately kept out of the clamp above; this
            // only counts them, so the number says how wrong the height is rather than hiding it.
            for (auto const& hz : g_rtHaz) {
                if ((double)hz.x0 > x) break;
                if ((double)hz.x1 < x) continue;
                if (outY > (double)hz.y0 && outY < (double)hz.y1) { simFatal++; break; }
            }
            return outY;
        };

        // Where a lost route belongs. Every level has gaps in its floor, and a route that is even
        // slightly off finds one, drops through, and free-falls for the rest of the level with
        // nothing left to catch it - measured, to -11,043 over 97,000 units. Stopping there is
        // honest but costs most of the route, so instead: find the corridor and put it back.
        //
        // The corridor is read from the level rather than guessed. At one x the solid boxes give
        // filled intervals outright and each slope surface splits a band in two, so merging them
        // leaves the free bands exactly. A wave corridor is one bounded on BOTH sides and roughly
        // player-sized; open sky above the level is unbounded and is not a candidate. Where several
        // qualify the one nearest the last height the route was seen supported at wins, because
        // that is the only positional evidence there is.
        static std::vector<std::pair<double, double>> anIv;
        auto reanchorAt = [&](double x, double refY) -> double {
            anIv.clear();
            for (size_t j = simOb; j < g_rtObst.size(); j++) {
                auto const& ob = g_rtObst[j];
                if ((double)ob.x0 > x) break;
                if ((double)ob.x1 < x) continue;
                anIv.push_back({ (double)ob.y0, (double)ob.y1 });
            }
            for (size_t j = simSl; j < g_rtSlopes.size(); j++) {
                auto const& sl = g_rtSlopes[j];
                if ((double)sl.x0 > x) break;
                if ((double)sl.x1 < x) continue;
                double w = (double)sl.x1 - (double)sl.x0;
                if (w < 1e-6) continue;
                double sy = (double)sl.ya
                          + ((double)sl.yb - (double)sl.ya) * ((x - (double)sl.x0) / w);
                anIv.push_back({ sy, sy });          // a surface: no thickness, but it does divide
            }
            if (anIv.size() < 2) return NAN;
            std::sort(anIv.begin(), anIv.end());
            size_t w = 0;
            for (size_t i = 0; i < anIv.size(); i++) {
                if (w > 0 && anIv[i].first <= anIv[w - 1].second)
                    anIv[w - 1].second = std::max(anIv[w - 1].second, anIv[i].second);
                else anIv[w++] = anIv[i];
            }
            anIv.resize(w);
            double best = NAN, bestD = 1e18;
            for (size_t k = 0; k + 1 < anIv.size(); k++) {
                double lo = anIv[k].second, hi = anIv[k + 1].first;
                double h = hi - lo;
                if (h < 40.0 || h > 260.0) continue;      // too tight to fly, or not a corridor
                double c = (lo + hi) * 0.5, d = std::fabs(c - refY);
                if (d < bestD) { bestD = d; best = c; }
            }
            return best;
        };

        // The shape, from a given height. Inputs and portals are one merged walk, and each portal
        // is applied only if the route as built so far actually passes through it - which is what
        // makes the height and the shape one problem rather than two.
        //
        // collide=false is the free integration, used by the height fit: over the short window the
        // fit looks at, the two agree, and the fit needs a shape that MOVES when the height moves.
        // With collision every starting height produces a route that touches nothing, so the fit
        // would have no signal at all and would accept the first guess it was given.
        auto build = [&](double y0, bool collide = false) {
            pts.clear(); hold.clear();
            // Start at the macro's first input, not at the mouth of the section.
            //
            // Before the first recorded transition there is no information about the button at all,
            // and treating "no data" as "released" makes the route dive at 45 degrees all the way
            // to that first click. On a level that opens in wave, the first click can be a couple
            // of hundred units in - measured: first click at x=256, so the route fell 256 units
            // before it began, and then tracked the run's shape correctly for the rest of the
            // level at a height 250 units too low. That one dive was the whole of the "route is
            // under the corridor" bug; nothing after it was wrong.
            //
            // A run cannot have fallen through that stretch or it would not have survived it, so
            // the honest thing is to begin where the evidence begins.
            double startX = (double)s.x0;
            if (!tr.empty()) {
                size_t fi = t0;
                if (fi < tr.size() && tr[fi].first > startX) {
                    double lim = startX + 4000.0;
                    startX = tr[fi].first < lim ? tr[fi].first : lim;
                }
            }
            double cx = startX, cy = y0, fl = (double)s.flip, sz = (double)s.size;
            bool held = held0;
            size_t ti = t0, pi = p0;
            simOb = 0; simSl = 0; simSupported = true;
            simSteps = 0; simHeld = 0; simFatal = 0;
            int adrift = 0, reanchors = 0, hopeless = 0;
            uint8_t marked = 0;
            double lastGoodY = y0;
            pts.push_back(ccp((float)cx, (float)cy));
            int guard = 0;
            while (cx < (double)s.x1 && guard++ < 60000) {
                double nt = ti < tr.size() ? tr[ti].first : 1e18;
                double np = pi < g_rtPortals.size() ? (double)g_rtPortals[pi].x : 1e18;
                double nx = nt < np ? nt : np;
                if (nx > (double)s.x1) nx = (double)s.x1;
                // One straight run when nothing can interrupt it; short steps when the level can,
                // because a surface met half way along a stretch has to be met where it is.
                while (nx > cx) {
                    double dx = nx - cx;
                    if (collide && dx > 10.0) dx = 10.0;
                    double tx = cx + dx;
                    double ny = cy + (held ? 1.0 : -1.0) * fl * sz * dx;
                    uint8_t seg = (uint8_t)(held ? 1 : 0);
                    if (collide) {
                        ny = slideTo(cx, tx, cy, ny, sz > 1.5 ? g_rtHalfBig * 0.6 : g_rtHalfBig);
                        if (simSupported) { adrift = 0; lastGoodY = ny; }
                        else adrift++;
                        // Nine hundred units outside the level is not a wave any more.
                        if (adrift > 90) {
                            double r = reanchors < 40 ? reanchorAt(tx, lastGoodY) : NAN;
                            if (!std::isfinite(r)) {
                                // Nothing corridor-shaped here. That is not necessarily an error -
                                // a level with an open stretch has no corridor to be in and the
                                // free integration is right there - so keep going and ask again
                                // shortly. Only a route that has been lost for twelve thousand
                                // units with nowhere to return to has really gone, and then a line
                                // that stops where the mod stops knowing beats eighty thousand
                                // units of confident nonsense.
                                adrift = 60;
                                if (++hopeless > 40) { cx = (double)s.x1; break; }
                                hold.push_back((uint8_t)(seg | marked));
                                cx = tx; cy = ny;
                                pts.push_back(ccp((float)cx, (float)cy));
                                continue;
                            }
                            log::info("[CI-ANCHOR] lost by x={:.0f}, back into the corridor at "
                                      "y={:.0f} (was {:.0f}, last good {:.0f})",
                                      tx, r, ny, lastGoodY);
                            ny = r; adrift = 0; reanchors++;
                            marked = 4;          // inferred from here on, and drawn as such
                            seg = 2;             // and no line drawn across the jump itself
                        }
                    }
                    hold.push_back(seg == 2 ? (uint8_t)2 : (uint8_t)(seg | marked));
                    cx = tx; cy = ny;
                    pts.push_back(ccp((float)cx, (float)cy));
                }
                if (cx >= (double)s.x1) break;
                if (nt <= np) { held = tr[ti].second != 0; ti++; }
                else {
                    auto const& p = g_rtPortals[pi];
                    // A portal is taken if the route passes through its mouth, plus a tolerance,
                    // because MISSING one is far worse than taking one. A size portal changes the
                    // slope of every segment after it - mini flies at +-2 where big flies at +-1 -
                    // so a route that misses the portal back to big keeps the wrong shape for the
                    // rest of the level, drifts further, and then misses the next portal by more.
                    // Measured on a 97,000-unit level: the first miss was by TWELVE units at
                    // x=42,405, and the one after it missed by 213. One near miss took out half
                    // the level.
                    //
                    // The tolerance is the route's own uncertainty, not a fudge. The route is
                    // fitted and collision-constrained but not exact, and a portal sits in the
                    // corridor the run flies down - so a portal a block and a half from the route
                    // is one the run went through. Two hundred units away is not, and still misses.
                    // Height testing a portal needs a route that is already in the right place,
                    // and when it is not the two make each other worse: measured, 84 of 232
                    // portals missed, and a run that misses the portal back to normal gravity then
                    // flies the rest of the section upside down, which is a 2-units-per-unit
                    // divergence and the single largest error in the whole calibration.
                    //
                    // A macro that completed the level went through the portals on its path. That
                    // is stronger evidence than a height the route is still guessing at, so the
                    // ones that steer the whole section - gravity and size - are taken as passed.
                    // Decoy portals off the route exist, but they cost one section; missing a real
                    // one costs everything after it.
                    // Height tested again, which it was not for a while. Taking every gravity
                    // and size portal regardless was a response to 84 of 232 being missed - but
                    // the reason they were missed is that the route was in the wrong place, and on
                    // a dual "take them all" means the OTHER player's portals flip this route's
                    // gravity and size. That is a wave flying at the wrong slope, which is the
                    // 1-and-2-units-per-unit divergence the calibration kept finding.
                    //
                    // The test is worth trusting now: with fork sections no longer drawn, a
                    // section that is genuinely a wave tracks the recorded run to within 0.5 to 2.2
                    // units, so cy is real evidence rather than a guess. The tolerance stays,
                    // because missing a real portal still costs everything after it.
                    const double PTOL = 45.0;
                    bool takes = cy >= (double)p.y0 - PTOL && cy <= (double)p.y1 + PTOL;
                    if (collide && g_rtPortLogs < 24) {
                        g_rtPortLogs++;
                        log::info("[CI-PORTAL] x={:.0f} kind={} ({}) band {:.0f}..{:.0f} route y={:.0f}"
                                  " -> {}", p.x, p.kind,
                                  p.kind == 0 ? "gravity normal" : p.kind == 1 ? "gravity inverted"
                                  : p.kind == 2 ? "size big" : p.kind == 3 ? "size mini"
                                  : "TELEPORT",
                                  p.y0, p.y1, cy,
                                  (cy >= (double)p.y0 && cy <= (double)p.y1) ? "TAKEN"
                                      : (takes ? "TAKEN (within tolerance)" : "missed"));
                    }
                    if (takes) {
                        if (p.kind == 0) fl = 1.0;
                        else if (p.kind == 1) fl = -1.0;
                        else if (p.kind == 2) sz = 1.0;
                        else if (p.kind == 3) sz = 2.0;
                        else if (p.kind == 4) cy = (double)p.ty;   // placed, not nudged
                    }
                    pi++;
                }
            }
        };

        double y0 = (double)s.priorY;
        // An observation only counts if it could actually be USED. The route begins at the macro's
        // first input, which on a level that opens in wave can be several hundred units past the
        // section mouth - and the entry observation is taken at the spawn, before that. There is no
        // shape to back project through at a point the shape does not cover, so rtInterp correctly
        // returns nothing.
        //
        // What went wrong was the silence: the section still counted as observed, which narrowed
        // the fit's licence from 96 to 40 while leaving the height at the unimproved prior. The
        // first solve of an attempt ran unobserved, found the corridor 84 units up, and drew it
        // correctly; the spawn observation then landed, failed, and shackled the next solve to a
        // prior it could no longer escape - so the line dropped 84 units on the retry and stayed
        // there. An observation that cannot be applied must not be allowed to cost anything.
        bool useObs = false;
        if (s.obs) {
            // Back projected through the shape itself, so an observation taken half way into a
            // section anchors the whole of it, portals and all.
            build(0.0);
            if (pts.size() >= 2 && (double)s.obsX >= (double)pts.front().x - 1.0
                                && (double)s.obsX <= (double)pts.back().x + 1.0) {
                double rel = rtInterp(pts, (double)s.obsX);
                if (std::isfinite(rel)) { y0 = (double)s.obsY - rel; useObs = true; }
            }
        }
        if (s.obs && !useObs)
            log::info("[CI-SEC] observation at x={:.0f} is outside the route's span - ignored, "
                      "fitting from the prior with the full licence", s.obsX);

        // Thread it through the level. Shorter and shorter stretches on failure: the further into a
        // section you look the more likely some accumulated placement error makes every height
        // impossible, and the mouth is both the part that matters and the part that is most nearly
        // exact.
        //
        // How far the geometry is allowed to move it depends on how good the starting guess was. A
        // height read off the run itself is out by at most the distance travelled in one frame, so
        // 40 units is already generous, and a wider licence would let a tight level pull the route
        // into the corridor NEXT DOOR - confidently drawn, and wrong. The mouth of a portal is only
        // good to the half-height of the portal, so that one gets the full window.
        // Both ends, where both are known. The entry mouth says the run starts near priorY; the
        // exit mouth says it ends near exitY, and since the SHAPE is known that is a second reading
        // of the same starting height. Averaging two independent readings of one number beats
        // taking either alone, and disagreement between them is itself worth knowing.
        if (!useObs && std::isfinite(s.exitY) && (double)s.x1 - (double)s.x0 < 6000.0) {
            build(y0, false);
            if (pts.size() >= 2) {
                double shape = (double)pts.back().y - (double)pts.front().y;
                double fromExit = (double)s.exitY - shape;
                if (std::fabs(fromExit - y0) < 400.0) {
                    log::info("[CI-ENDS] section {:.0f}..{:.0f}: entry says {:.0f}, exit says "
                              "{:.0f} (they differ by {:.0f}) -> {:.0f}",
                              s.x0, s.x1, y0, fromExit, fromExit - y0, (y0 + fromExit) * 0.5);
                    y0 = (y0 + fromExit) * 0.5;
                }
            }
        }
        // An exact height gets almost no licence at all. What remains is for the hitbox and the
        // 10-unit sampling, not for the fit to go looking for a better-looking corridor: there is
        // nothing better than where the run actually was.
        const double licence = s.exact ? 12.0 : (useObs ? 40.0 : 96.0);
        const double base = y0;
        for (int it = 0; it < 4; it++) {
            build(y0);
            double d = NAN;
            for (double H : { 2400.0, 1200.0, 600.0, 300.0 }) {
                double xEnd = std::min((double)s.x1, (double)s.x0 + H);
                d = rtFitShift(pts, -1e18, xEnd, 4.0, licence);
                if (std::isfinite(d)) break;
            }
            if (!std::isfinite(d) || std::fabs(d) < 0.4) break;
            // The licence is on the total distance from the starting guess, not on each step of the
            // search - four passes of "up to 40" is a licence of 160, which is not what it says.
            y0 = std::max(base - licence, std::min(base + licence, y0 + d));
        }
        // If the macro recorded where it actually WAS, that is the route. No fitting, no
        // simulation, no accumulated error - and it is right from the frame the level loads, which
        // is the only moment it is any use.
        //
        // A GDR replay carries the player's position beside its inputs - per frame in the msgpack
        // format, per input in the binary one's physics extension - and this mod had been
        // discarding it and inferring a path instead. Inference cannot win: it must guess a
        // starting height from the level's geometry, and where the level is open, a corridor
        // taller than the 60 units the fit calls a corridor, the geometry constrains nothing and
        // the error has nowhere to be corrected. Measured on a real level that left the route 50
        // to 80 units out in exactly those stretches. A recorded position is not an estimate.
        {
            static size_t saidFor = (size_t)-1;
            if (g_macroPath.size() != saidFor) {
                saidFor = g_macroPath.size();
                int back = 0; float mn = 1e9f, mx = -1e9f;
                for (size_t i = 0; i < g_macroPath.size(); i++) {
                    if (i && g_macroPath[i].x < g_macroPath[i-1].x - 1.f) back++;
                    mn = std::min(mn, g_macroPath[i].x);
                    mx = std::max(mx, g_macroPath[i].x);
                }
                log::info("[CI-PATH] macro carries {} positions, x {:.0f}..{:.0f}, {} backward "
                          "steps ({} recording{})",
                          (int)g_macroPath.size(), mn, mx, back, back + 1,
                          back ? "s end to end" : "");
            }
        }
        bool fromRecord = false;
        // Only the ACTIVE macro's path. Any other is a different run down a different route.
        if (g_macroPath.size() >= 32 && g_macroPathSrc == g_activeMacro) {
            // Walk FORWARD from the section's start and stop at its end. Taking the first and last
            // index that happen to fall inside the section runs away the moment the array holds
            // more than one recording end to end: a 450-unit section came out with 29,088 points
            // spanning 8,295 to 71,881, which is the whole level. Anything that reads a range has
            // to be bounded by where it stops, not by where a match last occurred.
            size_t a = 0;
            while (a < g_macroPath.size() && (double)g_macroPath[a].x < (double)s.x0 - 1.0) a++;
            size_t b = a;
            float scan = -1e9f;
            while (b < g_macroPath.size()) {
                float qx = g_macroPath[b].x;
                if (qx > (double)s.x1 + 1.0) break;       // past the section
                if (qx < scan - 1.0f) break;              // a new recording started: stop here
                scan = qx;
                b++;
            }
            if (b > a) b--;
            // Only where the recording really covers the stretch. One that starts half way through
            // leaves the rest to be simulated exactly as before.
            if (b > a + 8 && (double)g_macroPath[a].x <= (double)s.x0 + 400.0
                          && (double)g_macroPath[b].x >= (double)s.x1 - 400.0) {
                pts.clear(); hold.clear();
                float lastX = -1e9f;
                for (size_t i = a; i <= b; i++) {
                    float px2 = g_macroPath[i].x;
                    if (px2 <= lastX) continue;        // x must ascend for everything downstream
                    lastX = px2;
                    if (!pts.empty())
                        hold.push_back(g_macroPath[i].y > pts.back().y ? 1 : 0);
                    pts.push_back(ccp(px2, g_macroPath[i].y));
                }
                if (pts.size() >= 8) {
                    fromRecord = true;
                    log::info("[CI-REC] section {:.0f}..{:.0f} drawn from the recording itself: "
                              "{} positions over x {:.0f}..{:.0f} - nothing inferred",
                              s.x0, s.x1, (int)pts.size(), pts.front().x, pts.back().x);
                } else { pts.clear(); hold.clear(); }
            }
        }
        // The height is chosen; now draw the route the way the wave actually flies it.
        //
        // An exact section flies FREE, and the attempt to constrain it is measured and rejected.
        //
        // The case for colliding was that flying free over 8,827 units drifts out of the channel
        // with nothing to correct against, which is true and visible. But the clamp pays for that
        // correction by bending the line where it makes contact, and a bend a wave cannot fly is a
        // worse lie than a line in the wrong place: one is unreadable, the other is merely off.
        // Measured across three builds of the same level - illegal gradients 11.7% flying free,
        // 13.1% colliding, and the clearance move cannot buy any of it back because a rigid
        // translation does not change a gradient.
        //
        // So the drift stays for now, and it is not fixed by pushing the line about. Its cause is
        // upstream: over a section that long the shape can only accumulate error if the transitions
        // it turns at are slightly wrong in x, and that map has never been checked against ground
        // truth.
        g_rtPortLogs = 0;
        // Ask the game first. Everything the old solver has to work out - the gravity flip at
        // 26085, the mini portals, the speed changes, which blocks slide and which kill, where a
        // move trigger has opened a corridor - the real physics simply does, because the real
        // portal acts on a real player. Only if it cannot run do we fall back to inference.
        bool bySim = false;
        if (!fromRecord) {
            // rtGameSim steps a PlayerObject at 240 Hz in a loop, which is the exact technique
            // this project measured as fatal: the icon dies whatever layer it is done on. It was
            // still being tried FIRST for every section, so on a level with no recorded path it is
            // what draws the line - and what the player sees as a path through spikes.
            bySim = false;
            if (bySim)
                log::info("[CI-GAMESIM] section {:.0f}..{:.0f} flown by the game's own physics: "
                          "{} points, no fitting and nothing to correct", s.x0, s.x1,
                          (int)pts.size());
            else
                build(y0, !s.exact);
        }

        // THE PATH OF LEAST RESISTANCE.
        //
        // The height fit only ever consulted solids, and on this level that is a minority of what
        // is there: 20,483 solid boxes against 24,025 hazards. Over half the geometry a wave has to
        // thread was invisible to the choice of height, which is why a route could be fitted neatly
        // into a corridor and still run through saw after saw.
        //
        // Hazards must not become surfaces - tried, and it made the drawn gradients worse (10.1% to
        // 15.9% illegal) while fixing no penetration, because a wave does not rest on a saw. They
        // belong in the SCORE instead: of the placements the licence allows, prefer the one that
        // meets least. That is also what makes the easy route win over a tight one - the intended
        // line through a section is usually the one with room around it, not the one that threads a
        // gap.
        //
        // Rigid translation only. A shift cannot add a corner, cannot change a gradient and cannot
        // move a turn off its click; it just puts the shape the macro already determined in a
        // better place. Exact sections are left alone - their height came from the game.
        // Exact sections are included, on a shorter leash.
        //
        // They were held out because their height came from the game and moving it looked like
        // discarding a measurement. That was right while they flew free, when no offset helped them
        // at all - measured, best move +0. Now that they collide the answer reverses: the drop meets
        // 35 solids and 6 saws where it is, and ten units up it meets almost none, 215 down to 49.
        // A measurement of where a run ENTERED is not a claim about every point downstream of it, so
        // half an icon of correction is allowed - enough to clear a saw, not enough to lose the
        // entry.
        // A path the game itself flew is already where it belongs - correcting it would only move
        // it away from the truth. The clearance pass is for inferred routes.
        if (!fromRecord && !bySim && pts.size() > 8) {
            auto meets = [&](double dy) {
                double score = 0.0;
                for (auto const& q : pts) {
                    const double qx = (double)q.x, qy = (double)q.y + dy;
                    for (auto const& ob : g_rtObst) {
                        if ((double)ob.x0 > qx) break;
                        if ((double)ob.x1 < qx) continue;
                        if (qy > (double)ob.y0 && qy < (double)ob.y1) {
                            score += std::min(qy - (double)ob.y0, (double)ob.y1 - qy);
                            break;
                        }
                    }
                    for (auto const& hz : g_rtHaz) {
                        if ((double)hz.x0 > qx) break;
                        if ((double)hz.x1 < qx) continue;
                        // A saw is worth more than a wall: clipping one ends the run outright,
                        // where brushing a block is often just a placement that is slightly out.
                        if (qy > (double)hz.y0 && qy < (double)hz.y1) { score += 30.0; break; }
                    }
                }
                return score;
            };
            const double base0 = meets(0.0);
            if (base0 > 0.0) {
                double bestDy = 0.0, bestS = base0;
                const double room = s.exact ? 16.0 : std::min(licence, 24.0);
                for (double dy = -room; dy <= room + 1e-9; dy += 2.0) {
                    if (std::fabs(dy) < 1e-9) continue;
                    const double sc = meets(dy);
                    // Ties go to the smaller move: with nothing to choose between two placements,
                    // the fitted height is the better guess.
                    if (sc < bestS - 1e-6
                        || (std::fabs(sc - bestS) <= 1e-6 && std::fabs(dy) < std::fabs(bestDy))) {
                        bestS = sc; bestDy = dy;
                    }
                }
                if (bestDy != 0.0) {
                    for (auto& q : pts) q.y += (float)bestDy;
                    log::info("[CI-CLEAR] section {:.0f}..{:.0f} moved {:+.0f} as a whole: met "
                              "{:.0f} of solid depth and saw, now {:.0f}. Same turns, same angles, "
                              "same clicks - only the placement changed.",
                              s.x0, s.x1, bestDy, base0, bestS);
                }
            }
        }

        // CALIBRATION. Only a fifth of macros carry their positions, and 2% of levels have even one
        // that does - so the recorded path alone can never be the feature. What those macros CAN do
        // is grade the simulation: here is what the mod worked out, and here is where the run
        // actually was, on this level's real geometry rather than a synthetic corridor. Every
        // divergence is a bug with an address. Fix them until the two agree and the simulation is
        // trustworthy for the other four fifths, which is the only way every user gets this.
        if (fromRecord && g_macroPath.size() >= 32) {
            std::vector<CCPoint> rec = pts; std::vector<uint8_t> recHold = hold;
            build(y0, true);                       // what the mod would have drawn unaided
            if (pts.size() >= 2 && rec.size() >= 2) {
                std::vector<double> err;
                double worst = 0.0, worstX = 0.0;
                for (size_t i = 0; i < rec.size(); i += 4) {
                    double qx = (double)rec[i].x;
                    double sy = rtInterp(pts, qx);
                    if (!std::isfinite(sy)) continue;
                    double e = std::fabs(sy - (double)rec[i].y);
                    err.push_back(e);
                    if (e > worst) { worst = e; worstX = qx; }
                }
                if (err.size() > 8) {
                    // The SHAPE of the disagreement names the cause, where its size cannot.
                    // A constant gap start to finish is the starting height and nothing else.
                    // A gap that grows is the shape itself - a wrong gravity, a wrong size, or
                    // slides - and no amount of anchoring will touch it. Signed, because a
                    // route that is consistently under is a different bug from one that swings.
                    double e0 = 0.0, e1 = 0.0, eMid = 0.0; int got = 0;
                    for (size_t i = 0; i < rec.size(); i++) {
                        double sy = rtInterp(pts, (double)rec[i].x);
                        if (!std::isfinite(sy)) continue;
                        double d = sy - (double)rec[i].y;
                        if (got == 0) e0 = d;
                        e1 = d;
                        got++;
                    }
                    {
                        int half = got / 2, seen = 0;
                        for (size_t i = 0; i < rec.size(); i++) {
                            double sy = rtInterp(pts, (double)rec[i].x);
                            if (!std::isfinite(sy)) continue;
                            if (seen++ == half) { eMid = sy - (double)rec[i].y; break; }
                        }
                    }
                    // How much of this stretch did the run spend travelling FLAT, and was there
                    // anything there for it to rest on? A wave cannot move horizontally under its
                    // own physics, so a flat run is a slide - and if the mod holds a surface at
                    // that height then its own collision should have produced the same slide. If
                    // it holds nothing, no simulation can ever reproduce it and the error is not
                    // a modelling failure but a missing wall.
                    // Was the entry portal even TAKEN? The scan assumes every gamemode portal is
                    // hit - "a level puts one where the route goes" - and on a dual, or a level
                    // with decoy portals, that is simply false. A phantom wave section gets a
                    // zigzag drawn through a stretch the run flew as a ship, which is the worst
                    // error in the whole calibration and no amount of geometry work touches it.
                    //
                    // The recording says where the run really was at that x, so the assumption can
                    // be checked rather than trusted.
                    {
                        double runY = NAN;
                        for (size_t i = 0; i < rec.size(); i++)
                            if ((double)rec[i].x >= (double)s.x0) { runY = (double)rec[i].y; break; }
                        // At every fork inside this section, which of the pair did the run go
                        // through - the higher or the lower? If a player is consistently one or
                        // the other, that is a rule needing no recording and no height estimate,
                        // and it settles the dual problem for every macro in the library.
                        for (size_t gk = 0; gk + 1 < g_rtGm.size(); gk++) {
                            if (g_rtGm[gk + 1].x - g_rtGm[gk].x > 30.f) continue;
                            double fx2 = (double)g_rtGm[gk].x;
                            if (fx2 < (double)s.x0 - 400.0 || fx2 > (double)s.x1 + 400.0) continue;
                            double ry3 = rtInterp(rec, fx2);
                            if (!std::isfinite(ry3)) continue;
                            double ya2 = (double)(g_rtGm[gk].y0 + g_rtGm[gk].y1) * 0.5;
                            double yb2 = (double)(g_rtGm[gk+1].y0 + g_rtGm[gk+1].y1) * 0.5;
                            bool tookA = std::fabs(ry3 - ya2) < std::fabs(ry3 - yb2);
                            double lo2 = std::min(ya2, yb2), hi2 = std::max(ya2, yb2);
                            double chosen = tookA ? ya2 : yb2;
                            log::info("[CI-FORK] x={:.0f}: portals at y={:.0f} and y={:.0f}, run "
                                      "was at y={:.0f} -> took the {} one (off by {:.0f}), "
                                      "wave={} | p2macro={}",
                                      fx2, lo2, hi2, ry3,
                                      std::fabs(chosen - hi2) < 1.0 ? "HIGHER" : "LOWER",
                                      std::fabs(ry3 - chosen),
                                      (tookA ? g_rtGm[gk].wave : g_rtGm[gk+1].wave) ? 1 : 0,
                                      pathPlayerIsP2() ? 1 : 0);
                        }
                        // Is this stretch SHAPED like a wave section? A wave flies down a
                        // corridor - walls above and below, close together - because that is the
                        // only way a level can control something that moves at 45 degrees and
                        // never stops. A ship stretch is open air by comparison. If that separates
                        // the phantom sections from the real ones then the level's own geometry
                        // says which gamemode it was built for, and every macro can use it - no
                        // recording, and no guess about where the run had got to.
                        int tight = 0, probes = 0;
                        for (double qx = (double)s.x0; qx <= (double)s.x1; qx += 30.0) {
                            double ry2 = rtInterp(rec, qx);
                            if (!std::isfinite(ry2)) continue;
                            probes++;
                            double up2 = 1e18, dn2 = -1e18;
                            for (auto const& ob : g_rtObst) {
                                if ((double)ob.x0 > qx) break;
                                if ((double)ob.x1 < qx) continue;
                                if ((double)ob.y0 > ry2 && (double)ob.y0 < up2) up2 = (double)ob.y0;
                                if ((double)ob.y1 < ry2 && (double)ob.y1 > dn2) dn2 = (double)ob.y1;
                            }
                            for (auto const& sl3 : g_rtSlopes) {
                                if ((double)sl3.x0 > qx) break;
                                if ((double)sl3.x1 < qx) continue;
                                double w3 = (double)sl3.x1 - (double)sl3.x0;
                                if (w3 < 1e-6) continue;
                                double sy3 = (double)sl3.ya
                                           + ((double)sl3.yb - (double)sl3.ya) * ((qx - (double)sl3.x0) / w3);
                                if (sy3 > ry2 && sy3 < up2) up2 = sy3;
                                if (sy3 < ry2 && sy3 > dn2) dn2 = sy3;
                            }
                            if (up2 < 1e17 && dn2 > -1e17 && (up2 - dn2) < 260.0) tight++;
                        }
                        // How hard is the macro clicking here? A wave is held and released
                        // constantly - it is the only way it moves - where a ship or a cube is
                        // clicked far more sparsely. If that separates the phantom sections from
                        // the real ones it is a test every macro can take, not just the fifth that
                        // recorded their positions.
                        int clicks = 0;
                        for (auto const& a3 : g_actions) {
                            if (a3.wxSweet < 0.f) continue;
                            if ((double)a3.wxSweet < (double)s.x0) continue;
                            if ((double)a3.wxSweet > (double)s.x1) break;
                            clicks++;
                        }
                        double per1k = ((double)s.x1 - (double)s.x0) > 1.0
                                     ? clicks * 1000.0 / ((double)s.x1 - (double)s.x0) : 0.0;
                        if (std::isfinite(runY))
                            log::info("[CI-DENSITY] section {:.0f}..{:.0f}: {:.0f}% of it is a "
                                      "corridor | {:.1f} clicks per 1000 | portal {}",
                                      s.x0, s.x1,
                                      probes ? 100.0 * tight / probes : 0.0, per1k,
                                      std::fabs(runY - (double)s.priorY) < 60.0 ? "taken"
                                                                                : "NOT TAKEN");
                        if (std::isfinite(runY))
                            log::info("[CI-ENTRY] section {:.0f}..{:.0f}: portal mouth says {:.0f}, "
                                      "the run was at {:.0f} ({:+.0f}) -> portal {}",
                                      s.x0, s.x1, s.priorY, runY, runY - (double)s.priorY,
                                      std::fabs(runY - (double)s.priorY) < 60.0 ? "taken"
                                                                                : "NOT TAKEN");
                    }
                    // Is this stretch even a WAVE for its whole length? A wave moves at exactly
                    // +-1 or +-2 and nothing else, so the recorded slopes say outright which parts
                    // of a "wave section" the run actually flew as a wave. The route turns on every
                    // click regardless - so a section that is only half wave gets a zigzag drawn
                    // through the half that is not, which is a surplus of turns and an error that
                    // grows with every one of them.
                    double waveX = 0.0, flatX = 0.0, spanX = 0.0, supported = 0.0;
                    for (size_t i = 1; i < rec.size(); i++) {
                        double dx = (double)rec[i].x - (double)rec[i-1].x;
                        if (!(dx > 1e-6)) continue;
                        spanX += dx;
                        double sl = ((double)rec[i].y - (double)rec[i-1].y) / dx;
                        double a2 = std::fabs(sl);
                        if (std::fabs(a2 - 1.0) < 0.03 || std::fabs(a2 - 2.0) < 0.03) waveX += dx;
                        if (a2 > 0.02) continue;
                        flatX += dx;
                        // is a collected surface within a player height of the run here?
                        double qy = (double)rec[i].y, qx = (double)rec[i].x;
                        bool touching = false;      // "near" is a Windows macro
                        for (auto const& ob : g_rtObst) {
                            if ((double)ob.x0 > qx) break;
                            if ((double)ob.x1 < qx) continue;
                            if (std::fabs((double)ob.y1 - qy) < 22.0
                             || std::fabs((double)ob.y0 - qy) < 22.0) { touching = true; break; }
                        }
                        if (!touching) for (auto const& sl2 : g_rtSlopes) {
                            if ((double)sl2.x0 > qx) break;
                            if ((double)sl2.x1 < qx) continue;
                            double w = (double)sl2.x1 - (double)sl2.x0;
                            if (w < 1e-6) continue;
                            double sy2 = (double)sl2.ya
                                       + ((double)sl2.yb - (double)sl2.ya) * ((qx - (double)sl2.x0) / w);
                            if (std::fabs(sy2 - qy) < 22.0) { touching = true; break; }
                        }
                        if (touching) supported += dx;
                    }
                    // Where the run TURNS is where it was clicked. A wave reverses direction on
                    // every press and release and nowhere else, so the turning points of the
                    // recorded path are the inputs, in the level's own coordinates. Comparing them
                    // with the route's corners tests the frame-to-x mapping directly - and a
                    // mapping that drifts puts every segment at the wrong length, which grows
                    // exactly like the errors left over after slides and portals are ruled out.
                    {
                        auto turns = [](std::vector<CCPoint> const& v, std::vector<double>& out) {
                            int prev = 0;
                            for (size_t i = 1; i < v.size(); i++) {
                                double dy = (double)v[i].y - (double)v[i-1].y;
                                if (std::fabs(dy) < 1e-6) continue;
                                int d = dy > 0 ? 1 : -1;
                                if (prev && d != prev) out.push_back((double)v[i-1].x);
                                prev = d;
                            }
                        };
                        std::vector<double> tr2, ts;
                        turns(rec, tr2); turns(pts, ts);
                        // pair them in order and measure the x offset between them
                        std::vector<double> dx;
                        size_t n = std::min(tr2.size(), ts.size());
                        for (size_t i = 0; i < n; i++) dx.push_back(ts[i] - tr2[i]);
                        if (dx.size() >= 4) {
                            std::sort(dx.begin(), dx.end());
                            log::info("[CI-TURNS] section {:.0f}..{:.0f}: run turns {} times, route "
                                      "turns {} | x offset between them: first {:+.0f} median "
                                      "{:+.0f} last {:+.0f}",
                                      s.x0, s.x1, (int)tr2.size(), (int)ts.size(),
                                      ts.empty() || tr2.empty() ? 0.0 : ts[0] - tr2[0],
                                      dx[dx.size()/2],
                                      n ? ts[n-1] - tr2[n-1] : 0.0);
                        }
                    }
                    log::info("[CI-SLIDE] section {:.0f}..{:.0f}: WAVE for {:.0f}% of it | flat "
                              "{:.0f}% (surface held for {:.0f}% of that) | error {:.1f}",
                              s.x0, s.x1, spanX > 0 ? 100.0 * waveX / spanX : 0.0,
                              spanX > 0 ? 100.0 * flatX / spanX : 0.0,
                              flatX > 0 ? 100.0 * supported / flatX : 0.0, err[err.size() / 2]);
                    // What is actually AT this section's mouth? The mask only acts on portals at
                    // one exact x, and it caught one phantom of five - so the other four either sit
                    // a few units apart, which is the same fork wearing a disguise, or they are not
                    // forks at all and need a different answer. Printing the neighbourhood is what
                    // decides which, and it is cheap.
                    {
                        std::string ctx;
                        for (auto const& q : g_rtGm) {
                            if ((double)q.x < (double)s.x0 - 150.0) continue;
                            if ((double)q.x > (double)s.x0 + 150.0) break;
                            ctx += fmt::format("[x{:.0f} {} y{:.0f}] ", q.x,
                                               q.wave ? "WAVE" : "non", (q.y0 + q.y1) * 0.5f);
                        }
                        log::info("[CI-MOUTH] section {:.0f}..{:.0f} (drawn={}) portals within 150 "
                                  "units of its start: {}",
                                  s.x0, s.x1, s.fork ? 0 : 1, ctx.empty() ? "none" : ctx);
                    }
                    // Did masking do the right thing? For a section the mod refused to draw, the
                    // recording says whether the run really went wave there. Suppressing a phantom
                    // is the point; suppressing a REAL wave section is the one way this can cost
                    // anything, and it has to be visible rather than inferred.
                    if (s.fork) {
                        double waveFrac = spanX > 0 ? 100.0 * waveX / spanX : 0.0;
                        log::info("[CI-MASKED] section {:.0f}..{:.0f} was not drawn | the run flew "
                                  "it as a wave for {:.0f}% of its length -> {}",
                                  s.x0, s.x1, waveFrac,
                                  waveFrac > 60.0 ? "*** WRONG, a real wave section was suppressed"
                                                  : "correct, this was a phantom");
                    }

                    std::sort(err.begin(), err.end());
                    log::info("[CI-CALIB] section {:.0f}..{:.0f} over {} samples: median {:.1f} "
                              "p90 {:.1f} worst {:.1f} at x={:.0f} | signed gap start {:+.0f} "
                              "mid {:+.0f} end {:+.0f} -> {}",
                              s.x0, s.x1, (int)err.size(), err[err.size() / 2],
                              err[(size_t)(err.size() * 0.9)], worst, worstX, e0, eMid, e1,
                              std::fabs(e1 - e0) < 25.0 ? "CONSTANT (starting height)"
                                                        : "GROWING (shape)");
                }
            }
            pts = rec; hold = recHold;             // the drawn route is still the recorded one
        }
        s.solvedY = (float)y0; s.solved = true;
        {
            bool dual = false; float dualFrom = -1.f;
            for (auto const& q : g_rtDual) {
                if (q.x > s.x0) break;
                dual = q.on; if (q.on) dualFrom = q.x;
            }
            log::info("[CI-DUALREGION] section {:.0f}..{:.0f} (drawn={}) is {} | {} dual/solo "
                      "portals on this level",
                      s.x0, s.x1, s.fork ? 0 : 1,
                      dual ? fmt::format("INSIDE a dual that began at x={:.0f}", dualFrom)
                           : std::string("solo"),
                      (int)g_rtDual.size());
        }
        log::info("[CI-SUPPORT] section {:.0f}..{:.0f} (drawn={}): route was inside the level for "
                  "{:.0f}% of it ({} of {} steps)",
                  s.x0, s.x1, s.fork ? 0 : 1,
                  simSteps ? 100.0 * simHeld / simSteps : -1.0, simHeld, simSteps);
        if (simFatal)
            log::info("[CI-FATAL] section {:.0f}..{:.0f}: passed a killing block {} time(s). These "
                      "are not slid along on purpose - a route that needs one is in the wrong "
                      "place, so this counts placement errors rather than hiding them in a curve.",
                      s.x0, s.x1, simFatal);
        log::info("[CI-SEC] x0={:.0f} x1={:.0f} prior={:.0f} obs={} used={} obsX={:.0f} obsY={:.0f} "
                  "base={:.0f} solvedY={:.0f} moved={:+.1f} licence={:.0f} flip={} size={} "
                  "route {:.0f}..{:.0f}{}",
                  s.x0, s.x1, s.priorY, s.obs ? 1 : 0, useObs ? 1 : 0, s.obsX, s.obsY,
                  base, y0, y0 - base, licence, s.flip, s.size,
                  pts.empty() ? 0.0 : (double)pts.front().x,
                  pts.empty() ? 0.0 : (double)pts.back().x,
                  (!pts.empty() && (double)pts.back().x < (double)s.x1 - 50.0)
                      ? " STOPPED - left the level" : "");

        // A WAVE ARRIVES AT A SURFACE; IT DOES NOT APPROACH ONE GRADUALLY.
        //
        // The walk samples every 10 units, so the step in which the icon meets a block covers two
        // different things: flight at one or two, then rest on the surface. Drawn as a single
        // segment it comes out at whatever the average was - and the player was looking straight at
        // it: "it curves at an angle a normal wave cant even do".
        //
        // Measured on the real level, from the dump: 141 of 590 drawn segments were at an illegal
        // gradient, and 112 of those were exactly 10 units long. One sampling step each. Not a
        // modelling error - a drawing one.
        //
        // So the landing step is split where the contact actually happened: flight at the legal
        // gradient up to the surface, then along it. Two legal segments instead of one impossible
        // one, and the corner lands where the icon really touched down.
        if (pts.size() > 1) {
            const double mag = (double)s.size;
            std::vector<CCPoint> sp; std::vector<uint8_t> sh;
            sp.reserve(pts.size() + 32); sh.reserve(hold.size() + 32);
            sp.push_back(pts.front());
            int split = 0;
            for (size_t i = 0; i + 1 < pts.size(); i++) {
                const double x0 = (double)pts[i].x, y0 = (double)pts[i].y;
                const double x1 = (double)pts[i + 1].x, y1 = (double)pts[i + 1].y;
                const uint8_t f = i < hold.size() ? hold[i] : 0;
                const double dx = x1 - x0, dy = y1 - y0;
                bool did = false;
                if (f != 2 && dx > 1e-6) {
                    const double m = dy / dx;
                    // Strictly between flat and flying: the signature of a landing. A gradient
                    // steeper than the icon can fly is a different animal - the resolver lifting it
                    // - and splitting that would only invent a corner.
                    if (std::fabs(m) > 0.03 && std::fabs(m) < mag - 0.03) {
                        const double xc = x0 + dy / (m > 0 ? mag : -mag);
                        if (xc > x0 + 1e-6 && xc < x1 - 1e-6) {
                            sp.push_back(ccp((float)xc, (float)y1));   // flight, then contact
                            sh.push_back(f);
                            sp.push_back(pts[i + 1]);                  // along the surface
                            sh.push_back(f);
                            split++; did = true;
                        }
                    }
                }
                if (!did) { sp.push_back(pts[i + 1]); sh.push_back(f); }
            }
            if (split) {
                pts.swap(sp); hold.swap(sh);
                log::info("[CI-LANDING] section {:.0f}..{:.0f}: {} sampling step(s) contained both "
                          "flight and a landing; split at the contact so every drawn segment is an "
                          "angle a wave can actually fly", s.x0, s.x1, split);
            }
        }

        // The simulation emits a point every 10 units, and most of them sit on a straight line.
        // Three points on one line under one button state are two points, so this puts a stretch
        // that never touched anything back to exactly the polyline it would have been, and nothing
        // downstream pays for the step size.
        if (pts.size() > 2) {
            std::vector<CCPoint> mp; std::vector<uint8_t> mh;
            mp.reserve(pts.size()); mh.reserve(hold.size());
            mp.push_back(pts.front());
            for (size_t i = 1; i + 1 < pts.size(); i++) {
                if (hold[i - 1] == hold[i]) {
                    double ax = (double)mp.back().x, ay = (double)mp.back().y;
                    double sx = (double)pts[i + 1].x - ax;
                    if (sx > 1e-6) {
                        double t = ((double)pts[i].x - ax) / sx;
                        double pred = ay + ((double)pts[i + 1].y - ay) * t;
                        if (std::fabs(pred - (double)pts[i].y) < 0.05) continue;
                    }
                }
                mp.push_back(pts[i]); mh.push_back(hold[i - 1]);
            }
            mp.push_back(pts.back()); mh.push_back(hold.back());
            pts.swap(mp); hold.swap(mh);
        }

        // What does the scan actually SEE around the finished route? Every pass here works from
        // g_rtObst and g_rtSlopes, so if those are empty where the route runs then no amount of
        // fitting or clamping can matter, and the fault is in what got collected rather than in
        // what was done with it. Reported as the nearest surface above and below at intervals: a
        // route in a wave corridor has both, close; a route with neither is nowhere near the level.
        {
            double fx = (double)pts.front().x, lx = (double)pts.back().x;
            double stepP = (lx - fx) / 10.0;
            if (stepP > 1.0) {
                std::string row;
                for (double qx = fx; qx <= lx; qx += stepP) {
                    double ry = rtInterp(pts, qx);
                    if (!std::isfinite(ry)) continue;
                    double up = 1e18, dn = -1e18; int nb = 0, ns = 0;
                    for (auto const& ob : g_rtObst) {
                        if ((double)ob.x0 > qx) break;
                        if ((double)ob.x1 < qx) continue;
                        nb++;
                        if ((double)ob.y0 > ry && (double)ob.y0 < up) up = (double)ob.y0;
                        if ((double)ob.y1 < ry && (double)ob.y1 > dn) dn = (double)ob.y1;
                    }
                    for (auto const& sl : g_rtSlopes) {
                        if ((double)sl.x0 > qx) break;
                        if ((double)sl.x1 < qx) continue;
                        ns++;
                        double w = (double)sl.x1 - (double)sl.x0;
                        if (w < 1e-6) continue;
                        double sy = (double)sl.ya
                                  + ((double)sl.yb - (double)sl.ya) * ((qx - (double)sl.x0) / w);
                        if (sy > ry && sy < up) up = sy;
                        if (sy < ry && sy > dn) dn = sy;
                    }
                    row += fmt::format("{:.0f}:y{:.0f}[{}b{}s ", qx, ry, nb, ns);
                    row += (up < 1e17 ? fmt::format("^{:.0f}", up - ry) : std::string("^-"));
                    row += (dn > -1e17 ? fmt::format("v{:.0f}] ", ry - dn) : std::string("v-] "));
                }
                log::info("[CI-PROBE] {}", row);
            }
        }

        // One x ascending array with holes: the segment joining two sections is marked as a gap so
        // nothing draws a line across the cube part in between. Bit 2 marks a section whose height
        // is still inferred rather than measured, so it can be drawn as the guess it is - it will
        // settle into place the first time the run reaches it, and a faint line saying "probably
        // here" is honest where a solid one saying "here" would not be.
        // A fork section is drawn as the guess it is, not thrown away.
        //
        // Dropping it was aimed at five phantom sections whose entry portal the run never passed
        // through, and as a way of never being confidently wrong it works. The cost only showed up
        // on a level built out of forks: ten of seventeen sections on Tidal Wave open on a
        // coincident pair, which is 20,130 units of wave with no line against 11,097 with one. The
        // mod was silent for eighty per cent of exactly the part it exists to help with.
        //
        // Silence is not the safe option here, it is just the invisible one. So a fork is drawn
        // with the same faint provenance an inferred height already uses - it reads as "probably
        // here" rather than "here" - and the moment the run actually reaches it, the observed
        // stretch makes it exact and it settles into place on its own.
        const uint8_t prov = (useObs && !s.fork) ? 0 : 4;
        if (s.fork)
            log::info("[CI-FORKSEC] section {:.0f}..{:.0f} opens on a coincident portal pair - "
                      "which portal the icon took is not knowable, so this is drawn as a guess",
                      s.x0, s.x1);
        if (!g_rtPts.empty()) g_rtHold.push_back(2);
        for (size_t i = 0; i < pts.size(); i++) {
            g_rtPts.push_back(pts[i]);
            if (i + 1 < pts.size()) g_rtHold.push_back(hold[i] | prov);
        }
    }
    // THE DUMP.  DIAGNOSTIC ONLY - REMOVE BEFORE RELEASE (see release.sh).
    //
    // Every fix in this feature was found by reading a log after the fact, and several of them
    // could not have worked at all: a guard with a zero denominator, a list swapped away before it
    // was used. That is a build-and-play cycle per hypothesis, which is an evening per guess.
    // Writing what the mod actually collected lets the same questions be asked offline in seconds,
    // against the real level rather than a synthetic corridor.
    //
    // T records are the transitions the route is BUILT from. The K records carry wxSweet, and
    // comparing drawn turns against that said 44% of them miss their click - on sections that fly
    // free and therefore cannot turn anywhere else. Two different numbers were being compared; this
    // exports the one the route actually turns on.
    {
        static int dumped = -1;
        const int lid = pl->m_level ? (int)pl->m_level->m_levelID : 0;
        if (lid && dumped != lid) {
            dumped = lid;
            const char* home = std::getenv("USERPROFILE");
            std::filesystem::path path = std::filesystem::path(home ? home : ".")
                                       / "Downloads" / fmt::format("ci-dump-{}.txt", lid);
            std::string o;
            o.reserve(1u << 20);
            o += fmt::format("# ClickIndicators dump  build {} {}\n", __DATE__, __TIME__);
            o += fmt::format("level {}\nendx {:.0f}\nfloor {:.1f}\n",
                             lid, (double)pl->m_endXPosition, (double)g_rtFloor);
            o += fmt::format("counts solids={} slopes={} hazards={} secs={} seen={} acts={} tr={}\n",
                             (int)g_rtObst.size(), (int)g_rtSlopes.size(), (int)g_rtHaz.size(),
                             (int)g_rtSecs.size(), (int)g_waveSeen.size(), (int)g_actions.size(),
                             (int)tr.size());
            for (auto const& b2 : g_rtObst)
                o += fmt::format("S {:.1f} {:.1f} {:.1f} {:.1f} {}\n",
                                 b2.x0, b2.x1, b2.y0, b2.y1, b2.id);
            for (auto const& sl : g_rtSlopes)
                o += fmt::format("L {:.1f} {:.1f} {:.1f} {:.1f} {}\n",
                                 sl.x0, sl.x1, sl.ya, sl.yb, sl.id);
            for (auto const& hz : g_rtHaz)
                o += fmt::format("H {:.1f} {:.1f} {:.1f} {:.1f}\n", hz.x0, hz.x1, hz.y0, hz.y1);
            for (auto const& sc : g_rtSecs)
                o += fmt::format("C {:.1f} {:.1f} {:.1f} {:.0f} {:.0f} {} {}\n",
                                 sc.x0, sc.x1, sc.priorY, sc.flip, sc.size,
                                 sc.exact ? 1 : 0, sc.drawn ? 1 : 0);
            for (auto const& w : g_waveSeen)
                o += fmt::format("W {:.1f} {:.1f} {:.1f} {:.0f} {:.0f}\n",
                                 w.x0, w.x1, w.y, w.flip, w.size);
            for (auto const& tp : tr)
                o += fmt::format("T {:.1f} {}\n", tp.first, (int)tp.second);
            for (auto const& a2 : g_actions)
                o += fmt::format("K {:.3f} {:.3f} {:.1f} {:.1f}\n",
                                 a2.pressTime, a2.releaseTime, (double)a2.px, (double)a2.wxSweet);
            for (size_t i = 0; i < g_rtPts.size(); i++)
                o += fmt::format("R {:.1f} {:.1f} {}\n", g_rtPts[i].x, g_rtPts[i].y,
                                 i < g_rtHold.size() ? (int)g_rtHold[i] : -1);
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);
            auto wr = geode::utils::file::writeString(path, o);
            const bool ok = wr.isOk() && std::filesystem::exists(path);
            log::info("[CI-DUMP] {} -> {} ({} bytes)", ok ? "wrote" : "FAILED", path.string(),
                      (int)o.size());
        }
    }

    g_rtOk = g_rtPts.size() >= 2;
    if (dbgLog())
        log::info("[CI-ROUTE] solved {} section(s), {} vertices, {} blocks, {} portals",
                  (int)g_rtSecs.size(), (int)g_rtPts.size(), (int)g_rtObst.size(),
                  (int)g_rtPortals.size());
}

// A height the run was actually at, which beats anything the geometry can infer. Taken once per
// section and then left alone: re-reading it every respawn is how a line starts moving between
// attempts, and a route that moves is the thing this exists to stop. It is only replaced when the
// route is somewhere the player provably is not.
static void rtNoteEntry(PlayLayer* pl, float px, float py) {
    if (!g_rtGeoOk || g_rtSecs.empty()) return;
    RtSection* s = nullptr;
    for (auto& c : g_rtSecs)
        if (px >= c.x0 - 40.f && px <= c.x1) { s = &c; break; }
    if (!s) return;
    // A height is only worth recording where the route has a height to compare it against. The
    // route starts at the macro's first input, so on a level that opens in wave the spawn is
    // BEFORE it - and an observation there describes nothing. Recording it anyway is not neutral:
    // it tells the solver the section is measured, which costs the fit more than half its licence.
    // An exact section has nothing to learn. Its height came from the game - the y the player was
    // at the instant they became a wave - and it is rebuilt from that record on every solve, which
    // resets the obsX/obsY this function writes. So it looked changed every single frame, bumped
    // the observation generation, and asked for another solve: thirty of them in one run. Each one
    // picked up a slightly better x/time map, the click positions crept forward with it, and the
    // whole drawn path shifted - "it kept switching positions", which is the one thing this must
    // never do.
    if (s->exact) return;

    bool usable = true;
    // A section that was solved but NOT drawn is the one that most needs this. The support gate
    // drops a section whose route never sat inside the level - and then rtRouteYAt has nothing to
    // return for it, which used to be read as "the player is outside the route" and the observation
    // was discarded. So the only measurement that could have fixed the height was refused precisely
    // because the height was wrong. Measured on Tidal Wave: obs=0 on all 84 solves, on a level where
    // the player was running through those sections the whole time.
    if (s->solved && s->drawn) {
        double ry = rtRouteYAt((double)px);
        if (!std::isfinite(ry)) usable = false;                       // outside the route entirely
        else if (s->obs && std::fabs(ry - (double)py) < 45.0) return; // already agrees, leave it be
    }
    bool changed = false;
    if (usable && (!s->obs || s->obsX != px || s->obsY != py)) {
        s->obs = true; s->obsX = px; s->obsY = py; changed = true;
    }

    // The live gravity and size ARE the mouth's whenever nothing between the mouth and here could
    // have changed them. That is a much bigger window than "near the mouth", and it matters: from a
    // Start Pos the run is first seen deep inside a section, so the 400-unit rule never fired and a
    // wrong portal reading stood for the whole level.
    bool gravBetween = false, sizeBetween = false;
    for (auto const& gp : g_rtPortals) {
        if (gp.x <= s->x0) continue;
        if (gp.x > px) break;
        if (gp.kind == 0 || gp.kind == 1) gravBetween = true;
        else sizeBetween = true;
    }
    float wasFlip = s->flip, wasSize = s->size;
    if (pl && pl->m_player1) {
        bool nearMouth = (px - s->x0 < 400.f);
        if (nearMouth || !gravBetween)
            s->flip = pl->m_player1->m_isUpsideDown ? -1.f : 1.f;
        if (nearMouth || !sizeBetween)
            s->size = pl->m_player1->m_vehicleSize < 1.f ? 2.f : 1.f;
    }
    // Only when something actually moved, so a respawn that tells us nothing new does not throw the
    // whole route away and solve it again.
    if (changed || s->flip != wasFlip || s->size != wasSize) g_rtObsGen++;
}

static void rtEnsure(PlayLayer* pl) {
    if (!pl) return;
    int lid = pl->m_level ? (int)pl->m_level->m_levelID : 0;
    int cnt = pl->m_objects ? pl->m_objects->count() : -1;
    // Level identity AND the layer pointer: two local levels both report id 0, and GD reuses the
    // address between levels, so neither key alone can tell one level's blocks from another's.
    //
    // The object COUNT is not a third key, and using it as one was the worst bug in this feature.
    //
    // GD destroys objects as a level plays. The count therefore falls during an attempt, this read
    // it as "a different level", and re-scanned - collecting whatever was left. Measured on Tidal
    // Wave, three scans of the SAME level in one session:
    //
    //     99 gm portals, 291 size portals, 12,616 solids, floor 475
    //     97 gm portals, 289 size portals,  7,691 solids, floor  91     <- mid-attempt rescan
    //     99 gm portals, 291 size portals, 12,616 solids, floor 475
    //
    // Thirty-nine per cent of the level gone, and the route then solved against the holes: it flew
    // through geometry that was still there, ended up 300 to 500 units from the run, and had to be
    // teleported back into a corridor 79 times. Different attempts cull at different points, so the
    // same section came out 18% supported once and 97% another - which is also why the line moved
    // between tries, the one thing this feature is not allowed to do.
    //
    // A count that GROWS is the level still loading, and that is worth another look. A count that
    // falls is the level being played. So the watermark only ever goes up, and the scan is the one
    // taken when the most of the level was there.
    const bool newLevel = (pl != g_rtGeoFor) || (lid != g_rtGeoLevel);
    if (newLevel) g_rtGeoCount = -1;
    if (!g_rtGeoOk || newLevel || cnt > g_rtGeoCount) {
        g_rtGeoFor = pl; g_rtGeoLevel = lid;
        if (cnt > g_rtGeoCount) g_rtGeoCount = cnt;
        rtScanLevel(pl);
        g_rtOk = false; g_rtSolvedGen = -1;
    }
    // Has a trigger moved anything the route rests on? Only objects with a group can be moved, so
    // this is a short list of cheap position reads rather than another walk of the level. A route
    // built against where a platform STARTED leaves the line down at the floor the platform was
    // built over, which is what a moving D block platform looks like when it has since risen.
    //
    // Throttled, and only when something really moved: a platform in motion would otherwise ask for
    // a fresh route on every frame of its travel.
    // Does the prediction match reality? For a platform that has actually moved, where the model
    // says it should be NOW against where it really is. Agreement means the whole chain works -
    // trigger found, group matched, timeline right, units right. A constant ratio between the two
    // means the offsets are stored in different units than assumed, and that is one number to fix
    // rather than a mystery to chase.
    {
        // Unconditional heartbeat. The comparison above went silent for several rounds and I read
        // the silence as "nothing wrong" three times over; it was one of these conditions failing.
        // So state them all, out loud, until the answer is in.
        static double lastBeat = -1e9;
        double lt = (double)pl->m_gameState.m_levelTime;
        if (lt > lastBeat + 2.0 || lt < lastBeat) {
            lastBeat = lt;
            double bx = 0.0, by = 0.0; bool bok = false;
            double rx = 0.0, ry = 0.0;
            if (!g_rtMovers.empty()) {
                RtMover const& m0 = g_rtMovers.front();
                bok = rtPredictOffsetAt(m0, lt, bx, by);
                if (m0.o) { rx = (double)m0.o->getPositionX() - (double)m0.x;
                            ry = (double)m0.o->getPositionY() - (double)m0.y; }
            }
            log::info("[CI-BEAT] levelTime={:.2f} movers={} triggers={} | first mover: predicted "
                      "({:+.1f},{:+.1f}) ok={} really moved ({:+.1f},{:+.1f})",
                      lt, (int)g_rtMovers.size(), (int)g_rtTrigs.size(), bx, by, bok ? 1 : 0,
                      rx, ry);
        }
    }
    // What the mod thinks is around the player, right now. The route drops 83 units below the run
    // at one spot and is within 3 units either side of it, which is a local hole rather than drift -
    // so the question is whether the surface the player is standing on is in the obstacle list at
    // all. If the run is resting on something and this reports nothing beneath it, the scan never
    // collected that platform and no amount of route work will put the line on top of it.
    if (pl->m_player1 && !g_rtObst.empty()) {
        static double lastH = -1e9;
        double lt = (double)pl->m_gameState.m_levelTime;
        if (lt < lastH) lastH = -1e9;
        if (lt > lastH + 1.0) {
            lastH = lt;
            double px = (double)pl->m_player1->getPositionX();
            double py = (double)pl->m_player1->getPositionY();
            double up = 1e18, dn = -1e18; int nb = 0, ns = 0;
            for (auto const& ob : g_rtObst) {
                if ((double)ob.x0 > px) break;
                if ((double)ob.x1 < px) continue;
                nb++;
                if ((double)ob.y0 > py && (double)ob.y0 < up) up = (double)ob.y0;
                if ((double)ob.y1 < py && (double)ob.y1 > dn) dn = (double)ob.y1;
            }
            for (auto const& sl : g_rtSlopes) {
                if ((double)sl.x0 > px) break;
                if ((double)sl.x1 < px) continue;
                double w = (double)sl.x1 - (double)sl.x0;
                if (w < 1e-6) continue;
                ns++;
                double sy = (double)sl.ya
                          + ((double)sl.yb - (double)sl.ya) * ((px - (double)sl.x0) / w);
                if (sy > py && sy < up) up = sy;
                if (sy < py && sy > dn) dn = sy;
            }
            double ry = rtRouteYAt(px);
            log::info("[CI-HERE] x={:.0f} player y={:.0f} route y={:.0f} | {} boxes {} slopes here "
                      "| nearest surface above {} below {}",
                      px, py, ry, nb, ns,
                      up < 1e17 ? fmt::format("{:.0f} ({:+.0f})", up, up - py) : "none",
                      dn > -1e17 ? fmt::format("{:.0f} ({:+.0f})", dn, dn - py) : "none");
        }
    }
    if (!g_rtWatch.empty()) {
        static double lastW = -1e9; static int wLogs = 0;
        double lt = (double)pl->m_gameState.m_levelTime;
        if (lt < lastW) { lastW = -1e9; wLogs = 0; }
        if (lt > lastW + 0.5 && wLogs < 14) {
            lastW = lt;
            int movedN = 0; std::string det;
            for (auto& w : g_rtWatch) {
                if (!w.o) continue;
                double dx = (double)w.o->getPositionX() - (double)w.x;
                double dy = (double)w.o->getPositionY() - (double)w.y;
                if (std::fabs(dx) < 6.0 && std::fabs(dy) < 6.0) continue;
                movedN++;
                if (movedN <= 4) {
                    std::string gs;
                    int ng = w.o->m_groupCount < 10 ? (int)w.o->m_groupCount : 10;
                    for (int gi = 0; gi < ng; gi++) gs += fmt::format("{},", w.o->getGroupID(gi));
                    det += fmt::format("[id{} type{} from({:.0f},{:.0f}) by({:+.0f},{:+.0f}) "
                                       "groups={}] ", w.o->m_objectID, (int)w.o->m_objectType,
                                       w.x, w.y, dx, dy, gs.empty() ? "none" : gs);
                }
            }
            if (movedN) {
                wLogs++;
                log::info("[CI-WATCH] t={:.1f} {} of {} collidable objects have moved: {}",
                          lt, movedN, (int)g_rtWatch.size(), det);
            }
        }
    }
    // Check the model against the level, once, early.
    //
    // A trigger whose move should already have finished is a claim that can be tested: look at an
    // object it moves and see whether it went anywhere. Measured on a real level, one trigger
    // claimed a 90 unit drop for 358 collidable objects, its move was over by t=1.4, and at every
    // beat from t=2 to t=18 those objects had moved exactly nothing. The route was drawn 90 units
    // under solid geometry on the strength of a trigger that does not fire.
    //
    // Why GD ignores it does not matter - disabled group, an activation condition, something 2.2
    // does that is not in the bindings. What matters is that the level is the authority and it can
    // be asked. Triggers that fail are struck out and the route is solved once more; that happens
    // inside the first few seconds, so the line the player is reading stays still thereafter.
    if (!g_rtTrigAudited && !g_rtMovers.empty()
        && (double)pl->m_gameState.m_levelTime > 3.0) {
        g_rtTrigAudited = true;
        int struck = 0;
        double now = (double)pl->m_gameState.m_levelTime;
        for (size_t ti = 0; ti < g_rtTrigs.size(); ti++) {
            RtMoveTrig& tr = g_rtTrigs[ti];
            if (tr.dead) continue;
            if (tr.fireT + (double)tr.dur > now - 0.3) continue;   // not finished yet, no verdict
            // Only where this trigger is the only one acting, so the comparison is unambiguous.
            for (auto const& m : g_rtMovers) {
                if (m.nTrig != 1 || m.trig[0] != (int)ti || !m.o) continue;
                double wantY = (double)tr.dy * g_rtMoveScale;
                double wantX = (double)tr.dx * g_rtMoveScale;
                double gotY = (double)m.o->getPositionY() - (double)m.y;
                double gotX = (double)m.o->getPositionX() - (double)m.x;
                if (std::fabs(wantX - gotX) > 20.0 || std::fabs(wantY - gotY) > 20.0) {
                    tr.dead = true; struck++;
                    log::info("[CI-AUDIT] trigger on group {} claimed ({:+.0f},{:+.0f}) by t={:.1f} "
                              "but its objects moved ({:+.0f},{:+.0f}) - struck out",
                              tr.group, wantX, wantY, tr.fireT + (double)tr.dur, gotX, gotY);
                }
                break;
            }
        }
        if (struck) {
            g_rtTrigStruck += struck;
            rtComposeGeometry();
            g_rtObsGen++;
            log::info("[CI-AUDIT] {} trigger(s) struck out; route re-solved against the level as it "
                      "actually is", struck);
        }
    }
    if (!g_rtMovers.empty()) {
        static int predLogs = 0;
        double now = (double)pl->m_gameState.m_levelTime;
        if (predLogs < 8 && now > 0.5) {
            for (auto const& m : g_rtMovers) {
                if (!m.o || predLogs >= 8) continue;
                double realX = (double)m.o->getPositionX() - (double)m.x;
                double realY = (double)m.o->getPositionY() - (double)m.y;
                double qx = 0.0, qy = 0.0;
                bool wants = rtPredictOffsetAt(m, now, qx, qy);
                // Report a disagreement in EITHER direction. Only logging objects that moved missed
                // the case that actually mattered: the model predicted a 90 unit drop for 358
                // blocks, none of them ever moved, and the silence read as "nothing to see".
                bool interesting = std::fabs(realX) > 8.0 || std::fabs(realY) > 8.0
                                || (wants && (std::fabs(qx) > 8.0 || std::fabs(qy) > 8.0));
                if (!interesting) continue;
                double px = 0.0, py2 = 0.0;
                bool got = rtPredictOffsetAt(m, now, px, py2);
                predLogs++;
                log::info("[CI-PREDICT] t={:.2f} object at base ({:.0f},{:.0f}) groups={} | really "
                          "moved ({:+.1f},{:+.1f}) | predicted ({:+.1f},{:+.1f}){} | ratio y={:.2f}",
                          now, m.base.origin.x, m.base.origin.y, m.nTrig,
                          realX, realY, px, py2, got ? "" : " NOT PREDICTED",
                          std::fabs(py2) > 0.01 ? realY / py2 : 0.0);
            }
        }
    }
    if (!g_rtMovers.empty()) {
        double now = (double)pl->m_gameState.m_levelTime;
        if (now < g_rtMoveAt) g_rtMoveAt = -1e9;          // a retry rewound the clock
        if (now - g_rtMoveAt > 0.2) {
            bool moved = false;
            for (auto const& m : g_rtMovers) {
                if (!m.o || m.predicted) continue;   // a predicted object is already where it will be
                if (std::fabs(m.o->getPositionX() - m.x) > 3.f
                 || std::fabs(m.o->getPositionY() - m.y) > 3.f) { moved = true; break; }
            }
            if (moved) {
                g_rtMoveAt = now;
                rtComposeGeometry();
                g_rtObsGen++;            // the geometry changed, so the route has to be solved again
                static int mvLogs = 0;
                if (dbgLog() && mvLogs < 10) {
                    mvLogs++;
                    log::info("[CI-SHIFT] collidable geometry moved at t={:.2f}; route rebuilt "
                              "against where it is now", now);
                }
            }
        }
    }
    void* sp = (void*)pl->m_startPosObject;
    float spx = pl->m_startPosObject ? pl->m_startPosObject->getPositionX() : 0.f;
    // Nothing here is per frame or per attempt. The route is only rebuilt when the macro changes,
    // the spawn moves the click positions, or a section learns its real height.
    if (g_rtSolvedGen == g_rtObsGen && g_rtMacro == g_activeMacro && g_rtActs == g_actions.size()
        && g_rtSpawn == sp && g_rtSpawnX == spx && g_rtWx == g_wxOk) return;
    g_rtSolvedGen = g_rtObsGen; g_rtMacro = g_activeMacro; g_rtActs = g_actions.size();
    g_rtSpawn = sp; g_rtSpawnX = spx; g_rtWx = g_wxOk;
    rtSolve(pl);
}

static void pulseVisible(PlayLayer* pl, bool v) {
    if (!pl || !pl->m_player1) return;
    if (auto* pp = pl->m_player1->getParent())
        if (auto* pn = pp->getChildByID("ci-pulse"_spr)) pn->setVisible(v);
}

static bool freeCamPan(enumKeyCodes key) {
    if (!Mod::get()->getSettingValue<bool>("free-camera")) return false;
    auto* pl = PlayLayer::get();
    if (!pl || !pl->m_objectLayer) return false;

    const float step = 64.f;
    float dx = 0.f, dy = 0.f;
    switch (key) {
        case KEY_Left:  case KEY_A: dx =  step; break;
        case KEY_Right: case KEY_D: dx = -step; break;
        case KEY_Up:    case KEY_W: dy = -step; break;
        case KEY_Down:  case KEY_S: dy =  step; break;
        default: return false;
    }
    if (!g_fcActive) {
        g_fcSaved = pl->m_objectLayer->getPosition();
        g_fcSavedCam = pl->m_gameState.m_cameraPosition;
        g_fcPan = CCPointZero;
        g_fcActive = true;
        if (auto* ov = ciOverlayNode(pl)) ov->setVisible(false);
        pulseVisible(pl, false);
    }
    pl->m_objectLayer->setPosition(pl->m_objectLayer->getPosition() + ccp(dx, dy));
    g_fcPan = g_fcPan + ccp(-dx, -dy);

    // Moving the layer alone showed empty space: GD culls objects to the sections around the CAMERA,
    // that culling runs per frame and the game is not stepping while paused, so anything not already
    // on screen when you paused was never made visible. Move the camera with the layer - it travels
    // the opposite way - and drive the cull by hand.
    pl->m_gameState.m_cameraPosition = pl->m_gameState.m_cameraPosition + ccp(-dx, -dy);

    // updateVisibility does nothing while paused - the section window stays pinned wherever it was
    // when you hit pause no matter what camera or player position it is given, so everything that
    // was not already on screen stays hidden. Show the objects in view directly instead. Only the
    // visibility flag is touched: nothing is activated, so no trigger can fire from scouting.
    float lx = pl->m_objectLayer->getPositionX(), ly = pl->m_objectLayer->getPositionY();
    CCSize win = CCDirector::sharedDirector()->getWinSize();
    float vl = -lx - 60.f, vr = -lx + win.width + 60.f;
    float vb = -ly - 60.f, vt = -ly + win.height + 60.f;

    static bool dumped = false;
    if (dbgLog() && !dumped) {
        dumped = true;
        int tot = 0, withParent = 0, vis = 0;
        float mnx = 1e9f, mxx = -1e9f, mny = 1e9f, mxy = -1e9f;
        for (auto* col : pl->m_sections) {
            if (!col) continue;
            for (auto* cell : *col) {
                if (!cell) continue;
                for (auto* o : *cell) {
                    if (!o) continue;
                    tot++;
                    if (o->getParent()) withParent++;
                    if (o->isVisible()) vis++;
                    float ox = o->getPositionX(), oy = o->getPositionY();
                    mnx = std::min(mnx, ox); mxx = std::max(mxx, ox);
                    mny = std::min(mny, oy); mxy = std::max(mxy, oy);
                }
            }
        }
        log::info("[CI-DUMP] objs={} parented={} visible={} x=[{:.0f},{:.0f}] y=[{:.0f},{:.0f}]",
                  tot, withParent, vis, mnx, mxx, mny, mxy);
        log::info("[CI-DUMP] layer pos=({:.0f},{:.0f}) scale={:.3f} win={:.0f}x{:.0f} sections={}",
                  lx, ly, pl->m_objectLayer->getScale(), win.width, win.height, pl->m_sections.size());
        if (pl->m_player1)
            log::info("[CI-DUMP] player=({:.0f},{:.0f})", pl->m_player1->getPositionX(),
                      pl->m_player1->getPositionY());
    }

    int inView = 0, shown = 0, noParent = 0;
    for (auto* col : pl->m_sections) {
        if (!col) continue;
        for (auto* cell : *col) {
            if (!cell) continue;
            for (auto* o : *cell) {
                if (!o) continue;
                float ox = o->getPositionX(), oy = o->getPositionY();
                if (ox < vl || ox > vr || oy < vb || oy > vt) continue;
                inView++;
                if (!o->getParent()) { noParent++; continue; }
                if (!o->isVisible()) { o->setVisible(true); shown++; }
            }
        }
    }
    if (dbgLog()) log::info("[CI-FC] layerX={:.0f} view=[{:.0f},{:.0f}] inView={} shown={} noParent={}",
              lx, vl, vr, inView, shown, noParent);
    return true;
}

// ---- click editor -------------------------------------------------------------------------
// A macro usually taps more than any given player needs. Paused, with the free camera on, the
// upcoming clicks are drawn as numbered markers and Z / C step through them while X strikes one
// out. A struck click stops being cued, sounded and graded, and the choice is remembered per level
// and macro. Drawn into its own node on PauseLayer because the game's overlay is frozen while paused.
static int g_editSel = -1;

static bool editorOn() {
    return Mod::get()->getSettingValue<bool>("free-camera") && g_snapOk && !g_actions.empty();
}

// Both editor sites projected from the frozen snapshot at one constant velocity, so on any level
// with a speed portal every marker past the next portal was wrong by the whole integrated
// difference and never converged - the pause menu is exactly where you look furthest ahead. One
// function so the drawn markers and the Z/C selection can never disagree about where a click is.
static float editorMarkX(double sweet) {
    if (!g_snapTab) return g_snapX + g_snapVx * (float)(sweet - g_snapT);
    return (float)xAfterDt((double)g_snapX, sweet - g_snapT, g_snapSeg);
}

static void editorDraw(PauseLayer* pause) {
    if (!pause) return;
    auto* node = typeinfo_cast<CCDrawNode*>(pause->getChildByID("ci-edit"_spr));
    auto* label = typeinfo_cast<CCLabelBMFont*>(pause->getChildByID("ci-edit-lbl"_spr));
    auto* pl = PlayLayer::get();
    if (!node || !pl || !pl->m_objectLayer) return;
    node->clear();
    if (!Mod::get()->getSettingValue<bool>("free-camera")) { if (label) label->setVisible(false); return; }
    if (!editorOn()) {
        // Still say it is on, so the keys are discoverable and it is obvious the feature is live
        // even with no macro loaded to edit.
        if (label) {
            label->setVisible(true);
            label->setString("Free camera on - arrows or WASD to look around");
        }
        return;
    }

    CCSize win = CCDirector::sharedDirector()->getWinSize();
    int shown = 0, selShown = -1;
    for (size_t i = 0; i < g_actions.size(); i++) {
        auto const& a = g_actions[i];
        float wx = editorMarkX(a.sweet);
        CCPoint p = node->convertToNodeSpace(pl->m_objectLayer->convertToWorldSpace(ccp(wx, g_snapPy)));
        if (p.x < -40.f || p.x > win.width + 40.f) continue;
        shown++;
        bool sel = (int)i == g_editSel;
        if (sel) selShown = (int)i;
        float r = sel ? 11.f : 7.f;
        ccColor4F col = a.muted ? ccColor4F{ 1.f, 0.35f, 0.35f, 0.95f } : ccColor4F{ 0.3f, 1.f, 0.5f, 0.95f };
        // Full height, so a click is findable against the level however far you have panned - the
        // marker alone is easy to lose once the camera moves off the player's line.
        ccColor4F lc = col; lc.a = sel ? 0.55f : 0.22f;
        drawSegOL(node, ccp(p.x, 0.f), ccp(p.x, win.height), sel ? 1.8f : 1.0f, lc);
        drawRingOL(node, p, r, sel ? 2.6f : 1.8f, col);
        if (a.muted) {   // struck out
            float d = r * 0.72f;
            drawSegOL(node, p + ccp(-d, -d), p + ccp(d, d), 2.2f, col);
            drawSegOL(node, p + ccp(-d, d), p + ccp(d, -d), 2.2f, col);
        }
    }
    if (label) {
        label->setVisible(true);
        if (selShown < 0)
            label->setString(fmt::format("Arrows / WASD to move - Z / C to pick a click - {} on screen, {} struck out",
                                         shown, muteCount()).c_str());
        else
            label->setString(fmt::format("Click {} of {}{} - X to {}, Z / C to move",
                                         selShown + 1, (int)g_actions.size(),
                                         g_actions[selShown].muted ? " (struck out)" : "",
                                         g_actions[selShown].muted ? "restore" : "remove").c_str());
    }
}

static void editorBuild(PauseLayer* pause) {
    if (!pause || pause->getChildByID("ci-edit"_spr)) return;
    auto* node = CCDrawNode::create();
    node->setID("ci-edit"_spr);
    applyAA(node);
    pause->addChild(node, 500);
    auto* l = CCLabelBMFont::create("", "bigFont.fnt");
    l->setID("ci-edit-lbl"_spr);
    l->setScale(0.34f);
    l->setAnchorPoint({ 0.5f, 0.f });
    CCSize win = CCDirector::sharedDirector()->getWinSize();
    l->setPosition({ win.width * 0.5f, 8.f });
    l->setVisible(false);
    pause->addChild(l, 501);
}

// Step the selection to the next click that is on screen, so Z / C never appear to do nothing.
static bool editorKey(PauseLayer* pause, enumKeyCodes key) {
    if (!editorOn()) return false;
    auto* pl = PlayLayer::get();
    if (!pl || !pl->m_objectLayer) return false;
    int n = (int)g_actions.size();
    if (n == 0) return false;

    if (key == KEY_Z || key == KEY_C) {
        int dir = (key == KEY_C) ? 1 : -1;
        CCSize win = CCDirector::sharedDirector()->getWinSize();
        int start = g_editSel < 0 ? (dir > 0 ? -1 : n) : g_editSel;
        for (int step = 1; step <= n; step++) {
            int i = ((start + dir * step) % n + n) % n;
            float wx = editorMarkX(g_actions[i].sweet);
            float sx = pl->m_objectLayer->convertToWorldSpace(ccp(wx, g_snapPy)).x;
            if (sx >= -40.f && sx <= win.width + 40.f) { g_editSel = i; break; }
        }
        editorDraw(pause);
        return true;
    }
    if (key == KEY_X && g_editSel >= 0 && g_editSel < n) {
        g_actions[g_editSel].muted = !g_actions[g_editSel].muted;
        muteSave();
        buildSchedule();   // the audio schedule is prebuilt, so it has to be rebuilt
        editorDraw(pause);
        return true;
    }
    return false;
}

static void freeCamRestore(PlayLayer* pl) {
    if (!g_fcActive) return;
    g_fcActive = false;
    if (!pl) return;
    if (pl->m_objectLayer) pl->m_objectLayer->setPosition(g_fcSaved);
    pl->m_gameState.m_cameraPosition = g_fcSavedCam;
    g_fcPan = CCPointZero;
    pl->updateVisibility(0.f);
    if (auto* ov = ciOverlayNode(pl)) ov->setVisible(true);
    pulseVisible(pl, true);
}

// ===========================================================================================
// THE GHOST RUN.
//
// Predicting where a wave goes cannot be made to work, and the dumps say why: a simulation started
// at a section boundary carries a guessed height, no velocity and no history, and in a corridor
// thirty units wide that is fatal within half a block. Measured - 117 sections, every one wedged
// against geometry between 38 and 80 units after its own start. Where a wave is at any x depends on
// everything that happened before it, and none of that is recoverable from the level.
//
// So do not predict it. PLAY it. The macro is a complete run of this level; the game is
// deterministic; the same inputs on the same level produce the same path every time. Step the whole
// game layer - not just a detached player, so triggers fire, platforms move and portals act exactly
// as they do in a real attempt - feed it the macro's inputs on their own frames, and write down
// where the icon actually went. That is not an estimate of the route. It is the route.
//
// Done once, before the player starts, and cached against the level so it never runs twice.
//
// SAFETY. This drives the real PlayLayer, so it must be impossible for it to reach the end and
// register a completion: a fabricated Tidal Wave clear would be indistinguishable from cheating and
// would land on the accounts of everyone who bought this. levelComplete and destroyPlayer are both
// hard-blocked for the duration, the run stops short of the end portal, and the level is reset from
// the start afterwards.


// DOES THE REPLAY MATCH THE PLAYER?
//
// "How can a replay be wrong" is the right question, and nothing in this mod could answer it. The
// replay is internally consistent - every point a wave, nearly every input applied - but internally
// consistent is not the same as identical to the run the macro came from. Two things could produce
// a path that drifts: the inputs landing a physics step early or late and compounding, or the
// replay meeting geometry in a state the real attempt never had.
//
// So measure it against the only ground truth there is: the person playing. Every frame the player
// is inside a stretch the replay covers, compare their height to the drawn one. If the replay is
// faithful the two agree wherever the same route is flown, and where they stop agreeing is exactly
// where the replay went wrong - reported in units, at an x, instead of guessed at from a screenshot.
static double g_devSum = 0.0; static int g_devN = 0; static double g_devMax = 0.0;
static double g_devMaxX = 0.0; static double g_devFirstX = -1.0;

static void ghostCheck(float px, float py) {
    // Against the line that is DRAWN, not the whole replay.
    //
    // g_ghostPath covers every gamemode of the level, and this was measuring the player's wave
    // against a cube's arc: on a level with no wave at all it reported the drawn path as 7,968
    // units out over 3,144 frames and then deleted a cache that was never wrong. The only honest
    // comparison is between where the player is and the line they can see.
    if (g_rtPts.size() < 2) return;
    if (px < g_rtPts.front().x || px > g_rtPts.back().x) return;
    size_t lo = 0, hi = g_rtPts.size() - 1;
    while (lo + 1 < hi) {
        const size_t mid = (lo + hi) / 2;
        if (g_rtPts[mid].x <= px) lo = mid; else hi = mid;
    }
    // A gap is where two stretches join across a part that is not wave. There is no line there to
    // be near or far from, so it is not evidence either way.
    if (lo < g_rtHold.size() && g_rtHold[lo] == 2) return;
    const double x0 = g_rtPts[lo].x, x1 = g_rtPts[hi].x;
    if (!(x1 > x0)) return;
    const double gy = g_rtPts[lo].y
                    + (g_rtPts[hi].y - g_rtPts[lo].y) * ((px - x0) / (x1 - x0));
    const double d = std::fabs((double)py - gy);
    g_devSum += d; g_devN++;
    if (d > g_devMax) { g_devMax = d; g_devMaxX = px; }
    // The first place it is more than half an icon out is where the drift began, and that is the
    // only x worth looking at - everything after it is downstream of the same mistake.
    if (g_devFirstX < 0.0 && d > 15.0) g_devFirstX = px;
}

static void ghostCheckReport() {
    if (g_devN < 30) { g_devSum = 0; g_devN = 0; g_devMax = 0; g_devFirstX = -1; return; }
    const double avg = g_devSum / g_devN;
    log::info("[CI-VERIFY] over {} frames the drawn path was {:.1f} units from where you actually "
              "flew on average, worst {:.0f} at x={:.0f}, first went wrong at x={:.0f}",
              g_devN, avg, g_devMax, g_devMaxX, g_devFirstX);
    // REPORTED, NOT ACTED ON.
    //
    // Discarding a path because the player was far from it reads the measurement backwards. The
    // player flies differently from the macro constantly - that is the entire reason they want a
    // guide - and switching StartPos, practising one corridor, or dying early all produce a large
    // average against a path that is perfectly correct. It cost a good path and a 23-second replay
    // for an average of 31 units, which is a player taking a slightly different line.
    //
    // Whether a replay is trustworthy is decided where it is produced, from things that cannot be
    // confused with playing badly: did it apply the macro's inputs, and did it get where the macro
    // goes. That gate stays. This number is for reading logs.
    g_devSum = 0; g_devN = 0; g_devMax = 0; g_devMaxX = 0; g_devFirstX = -1;
}

// Drop the drawn path without touching the caches. Used whenever the macro behind it goes away or
// is replaced: the cache is keyed per macro, so the right path is still there for whichever one
// comes next, but nothing stale may be drawn in the meantime.
static void ghostProcessReset();   // defined with the rest of the processing state

static void ghostForget() {
    g_ghostPath.clear(); g_ghostWave.clear(); g_ghostHold.clear();
    g_ghostPath2.clear(); g_ghostWave2.clear(); g_ghostHold2.clear();
    g_ghQuickPath.clear(); g_ghQuickWave.clear(); g_ghQuickHold.clear();
    g_rtPts.clear(); g_rtHold.clear(); g_rtPts2.clear(); g_rtHold2.clear();
    g_rtOk = false; g_rtFromGhost = false;
    g_ghostTried = false;        // a new macro deserves a fresh look
    ghostProcessReset();
}

static void recPathForget();   // defined with the recorder, below

static void ghostReset() {
    g_ghost = false; g_ghostTried = false; g_rtFromGhost = false;
    g_ghostPath.clear(); g_ghostHold.clear(); g_ghostWave.clear();
    g_ghostPath2.clear(); g_ghostHold2.clear(); g_ghostWave2.clear();
    g_ghQuickPath.clear(); g_ghQuickWave.clear(); g_ghQuickHold.clear();
    g_ghPhase = 0; g_ghFullQueued = false;
    recPathForget();
}

// Cached on disk against the level and the macro. Twelve seconds is a lot to ask once; it is not
// something to ask again every time somebody opens the level. A different macro is a different run
// and gets its own entry, so switching macros does not silently show the old one's path.
static std::string ghostKey(PlayLayer* pl) {
    const int lid = pl && pl->m_level ? (int)pl->m_level->m_levelID : 0;
    // The version is mixed in so a change to how a path is produced retires every cached copy of
    // the old one, here and on the server. A path recorded by an earlier build is not the same
    // answer, and silently reusing it would hide the very change that was made.
    // Only the whole-level path is ever cached, and that one does not depend on where anybody
    // started - so the spawn is deliberately not part of this.
    uint64_t h = 1469598103934665603ull ^ 13ull;
    for (auto const& a : g_actions) {
        const uint64_t v = (uint64_t)(a.pressTime * 1000.0) ^ ((uint64_t)(a.releaseTime * 1000.0) << 21);
        h = (h ^ v) * 1099511628211ull;
    }
    return fmt::format("gh-{}-{:x}", lid, h);
}

// One writer for the cache, because there were three and a dual needs both icons in all of them.
// The two paths are separated by a bar; a file with no bar is a solo macro, which is also every
// path written before duals were recorded.
static std::string ghostSerialise() {
    std::string out;
    out.reserve((g_ghostPath.size() + g_ghostPath2.size()) * 24 + 2);
    for (size_t i = 0; i < g_ghostPath.size(); i++)
        out += fmt::format("{:.1f},{:.1f},{},{};", g_ghostPath[i].x, g_ghostPath[i].y,
                           i < g_ghostWave.size() ? (int)g_ghostWave[i] : 0,
                           i < g_ghostHold.size() ? (int)g_ghostHold[i] : 0);
    out += '|';
    for (size_t i = 0; i < g_ghostPath2.size(); i++)
        out += fmt::format("{:.1f},{:.1f},{},{};", g_ghostPath2[i].x, g_ghostPath2[i].y,
                           i < g_ghostWave2.size() ? (int)g_ghostWave2[i] : 0,
                           i < g_ghostHold2.size() ? (int)g_ghostHold2[i] : 0);
    return out;
}

static bool ghostParse(std::string const& raw) {
    if (raw.empty()) return false;
    g_ghostPath.clear(); g_ghostHold.clear(); g_ghostWave.clear();
    g_ghostPath2.clear(); g_ghostHold2.clear(); g_ghostWave2.clear();
    const auto bar = raw.find('|');
    const std::string one = bar == std::string::npos ? raw : raw.substr(0, bar);
    const std::string two = bar == std::string::npos ? std::string() : raw.substr(bar + 1);
    for (int which = 0; which < 2; which++) {
    auto& P = which ? g_ghostPath2 : g_ghostPath;
    auto& W = which ? g_ghostWave2 : g_ghostWave;
    auto& H = which ? g_ghostHold2 : g_ghostHold;
    const std::string& src = which ? two : one;
    const char* c = src.c_str();
    while (*c) {
        float x = 0, y = 0; int w = 0, hd = 0;
        int n = 0;
        if (sscanf(c, "%f,%f,%d,%d;%n", &x, &y, &w, &hd, &n) != 4 || !n) break;
        P.push_back(ccp(x, y));
        W.push_back((uint8_t)w);
        H.push_back((uint8_t)hd);
        c += n;
    }
    }
    if (g_ghostPath.size() < 9) { g_ghostPath.clear(); g_ghostWave.clear(); g_ghostHold.clear(); return false; }
    return true;
}

static bool ghostLoad(PlayLayer* pl) {
    // Say the key, hit or miss. A path worked out in the menu and then not found at level load is
    // the difference between an instant level and a twenty-second freeze, and until now the two
    // cases looked identical from a log. If these keys ever differ between the two moments, that
    // is the bug and this is the line that shows it.
    const std::string k = ghostKey(pl);
    if (!ghostParse(Mod::get()->getSavedValue<std::string>(k))) {
        log::info("[CI-GHOST] no path stored under {} - it has to be worked out now", k);
        return false;
    }
    log::info("[CI-GHOST] {} points restored from {} - nothing to run",
              (int)g_ghostPath.size(), k);
    return true;
}

// THE PATH IS THE SAME FOR EVERYBODY.
//
// The run a macro produces depends only on the macro and the level, because the game is
// deterministic - so the ten seconds it costs to work out is ten seconds every customer would
// otherwise spend arriving at an identical answer. Whoever gets there first uploads it and nobody
// else ever computes it again.
//
// Asked before replaying, and the replay only happens if the answer comes back missing.
static void ghostServer(PlayLayer* pl, std::function<void(bool)> then) {
    const std::string key = ghostKey(pl);
    (void)geode::async::spawn(
        cgweb::WebRequest().userAgent(ciUserAgent())
            .timeout(std::chrono::seconds(8)).get(ciApi("path?k=" + key)),
        [key, then](cgweb::WebResponse res) {
            if (res.ok() && ghostParse(res.string().unwrapOr(""))) {
                Mod::get()->setSavedValue<std::string>(key, res.string().unwrapOr(""));
                log::info("[CI-GHOST] {} points came from the shared cache - nothing to replay",
                          (int)g_ghostPath.size());
                then(true);
                return;
            }
            then(false);
        });
}

static void ghostShare(std::string const& key, std::string const& data) {
    matjson::Value body;
    body["token"] = licToken();
    body["key"]   = key;
    body["data"]  = data;
    (void)geode::async::spawn(
        cgweb::WebRequest().userAgent(ciUserAgent()).timeout(std::chrono::seconds(20))
            .bodyJSON(body).post(ciApi("path")),
        [](cgweb::WebResponse res) {
            log::info("[CI-GHOST] shared this path with everyone else: HTTP {}", res.code());
        });
}

static void ghostSave(PlayLayer* pl) {
        const std::string out = ghostSerialise();
    Mod::get()->setSavedValue<std::string>(ghostKey(pl), out);
}

static bool ghostRun(PlayLayer* pl);

// Defined further down, next to the rest of the detached replay.
static bool ghostLayerBegin(GJGameLevel* lvl, bool fromSpawn, PlayLayer* borrow = nullptr);
static void ghostLayerCatchUp(double toX, double capMs);

// Once per level: use the cached path if this macro has been replayed before, otherwise replay it.
static void ghostMaybeRun(PlayLayer* pl) {
    if (g_ghostTried || !pl || g_actions.empty() || !g_rtGeoOk) return;
    g_ghostTried = true;
    g_ghostKeyNow = ghostKey(pl);
    if (ghostLoad(pl)) return;                 // this machine has already replayed this macro

    // Otherwise ask whether anyone else has. Only if nobody has does this install pay for it, and
    // then it hands the answer back so the next person does not. No notification either way: this
    // sits inside setupHasCompleted with the loading screen already up, and announcing a wait is
    // what turns an unremarkable moment of loading into something to sit and watch.
    const std::string key = g_ghostKeyNow;
    ghostServer(pl, [pl, key](bool got) {
        if (got) return;
        // The fetch took a network round trip and this pointer is raw. If the player has left the
        // level, or opened a different one, pl is either dead or somebody else's - and the key
        // captured above belongs to the level that ASKED, so re-deriving it here would file this
        // level's path under that one's name. Check both before touching anything.
        if (PlayLayer::get() != pl || !pl->m_player1) { g_ghostTried = false; return; }
        if (key != ghostKey(pl)) { g_ghostTried = false; return; }

        // Its own level, so nothing of this session is touched, and it can be stepped a slice at a
        // time afterwards. First, buy the road in front of wherever this attempt actually starts:
        // from a StartPos that is most of the level, and it has to be flown before it can be drawn.
        // Doing it here puts the wait on the loading screen instead of on the first thirty seconds
        // of practice. Ten seconds is the ceiling, once per level per macro, and every load after
        // this reads the finished path off disk.
        // The quick one first, from wherever this attempt spawns, so there is a line in front of
        // the player the moment the level opens. Then the whole level, in the background, because
        // that is the one that answers every StartPos they switch to afterwards.
        g_ghFullQueued = true;
        if (ghostLayerBegin(pl->m_level, true)) {
            g_ghPhase = 1;
            // No catch-up here. It is an unbroken loop capped at four seconds, and it runs from the
            // server-cache callback, which is asynchronous - so it never lands on the loading
            // screen where it was meant to, only mid-attempt as one frozen frame. The log measured
            // it at 1,174 ms of stopped game. The slicing below reaches the same place without
            // stopping anything, because the replay outruns the player about twice over.
            return;
        }
        if (ghostLayerBegin(pl->m_level, false)) { g_ghPhase = 2; g_ghFullQueued = false; return; }
        log::info("[CI-GHOST] could not build a level of its own - replaying on this one instead, "
                  "which costs a pause");
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = ghostRun(pl);
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0).count();
        log::info("[CI-GHOST] {} in {:.0f} ms", ok ? "replayed" : "failed", ms);
        // A failure here is usually "asked too early", so let the in-game trigger have another go
        // rather than leaving the level with no path for the whole session.
        // NEVER RETRY. THE ANSWER WILL NOT CHANGE.
        //
        // Clearing g_ghostTried on failure looked forgiving and was catastrophic: ghostMaybeRun is
        // called from postUpdate every frame, and g_ghostTried is its only re-entry guard. A
        // rejected replay clears the path, which makes ghostRun report failure, which clears the
        // guard - so the next frame runs the whole thing again. Another fetch, another multi-second
        // replay, and another resetLevelFromStart + startGame that throws the player out of the
        // attempt they are in the middle of. Same macro, same level, same deterministic result,
        // forever. That is the level being unplayable, and it is this line.
        if (!ok || !g_ghostGood) return;
        Mod::get()->setSavedValue<std::string>(key, std::string(""));   // replaced, not appended to
        ghostSave(pl);
        const std::string out = ghostSerialise();
        ghostShare(key, out);
    });
}

// ===========================================================================================
// THE REPLAY ON ITS OWN LEVEL.
//
// Everything that went wrong with this feature came from one decision: the replay borrowed the
// PlayLayer the player was standing in. It had to reset that level to the start and play it, which
// meant a multi-second freeze that could not be spread over frames - you cannot pause halfway
// through rewinding somebody's attempt - and it meant every side effect had to be caught by hand.
// A fabricated new best written to the save. The replay's own presses graded as the player's, and
// counted as jumps: 265 on a three-attempt run. A completion screen. Each one found after it had
// already happened to somebody.
//
// So give it its own level. A second PlayLayer, built for the same GJGameLevel, never added to the
// scene, stepped a few milliseconds at a time and thrown away when it is done. The player's session
// is not touched at all: there is nothing to reset, no counters to put back, no completion to
// block. And with nothing to keep in sync, the work can stop and resume whenever it likes, which is
// what removes the freeze.
//
// The one thing that must be handled carefully: PlayLayer::get() reads a global, and other mods
// read it constantly. The ghost layer is only ever visible through that global while this code is
// inside its own stepping loop, and it is put back immediately afterwards - so from anyone else's
// point of view the second level does not exist.

// AN INPUT HAPPENS ON A FRAME, NOT AT A TIME.
//
// The macro is recorded in frames at 240 a second - the loader says so: "read 351 clicks at 240
// ticks/sec". Firing an event when the clock has PASSED its timestamp lands it up to one physics
// step late, and by a different amount each time depending on where the step boundary falls. Over
// several hundred clicks that is a trajectory quietly diverging from the one the macro describes,
// which is the line sitting beside the icon rather than under it.
//
// The frame number is exact and the replay steps exactly 240 times a second, so matching frame to
// frame removes the error rather than reducing it.
// An input, with every way of saying WHEN it happens: the macro's own time, its frame, and the
// position along the level the icon was at. The last is what playback fires on where it is known -
// see showStep - because position is what actually decides whether the icon clears an obstacle.
struct GhEv { double t; long long f; bool down; float x = -1.f; };

// A press waiting to be handed to the engine. Collected by whatever is driving playback, then
// applied inside a $modify member so it can go through the ORIGINAL handleButton rather than the
// virtual one, which would carry it through this mod's hook and every other input mod's.
struct PendPress { bool down; bool p1; };

static PlayLayer* g_ghLayer = nullptr;      // the hidden level, or null when not running
static std::vector<GhEv> g_ghEv, g_ghEv2;
static size_t g_ghAi = 0, g_ghAi2 = 0;
static bool   g_ghDown = false, g_ghLastDown = false;
static bool   g_ghDown2 = false, g_ghLastDown2 = false;
static double g_ghLastX2 = -1e9;
static double g_ghLastX = -1e9, g_ghPvX = -1e18, g_ghPvY = 0.0, g_ghPvM = 1e18;
static double g_ghStopX = 0.0, g_ghEndX = 0.0, g_ghX0 = 0.0;
static double g_ghClockAdj = 0.0;     // the StartPos correction, when the replay spawned at one
static bool   g_ghFromStart = false;  // did it, so the cache key can say so
static size_t g_ghEvFrom = 0;         // the first input this replay was ever going to see
// TWO REPLAYS, AND ONLY THE SECOND ONE IS WORTH KEEPING.
//
// Starting at the player's spawn is what makes a path appear in time for practice, but it draws
// only the part in front of them - and players switch StartPos constantly, GD drops them at the
// last one used on load, and a switch backwards would find nothing there at all. The path worth
// having is the whole level, from 0%, because that one answers every spawn.
//
// So the fast one runs first to cover where they are standing right now, and the full one runs
// behind it while they play, unhurried, and takes over when it is done. Only the full one is
// cached, keyed by level and macro alone - it does not depend on where anybody started.
static long long g_ghSteps = 0;
static int    g_ghWedge = 0;
static double g_ghSpent = 0.0;

// The global swap, in one place so it cannot be got wrong in one of them.
struct GhScope {
    PlayLayer* prev;
    explicit GhScope(PlayLayer* use) {
        auto* gm = GameManager::sharedState();
        prev = gm->m_playLayer;
        gm->m_playLayer = use;
    }
    ~GhScope() { GameManager::sharedState()->m_playLayer = prev; }
};

// ===========================================================================================
// LET THE GAME FLY IT. JUST FASTER.
//
// Every version of this that stepped the level itself has failed the same way, and the last log
// says it plainly: "the icon was dead for 33075 of 26777 steps" - more deaths than steps, on the
// REAL PlayLayer. So it was never the hidden layer that was broken. Calling update(1/240) in a loop
// is not what a frame does, and the icon dies constantly whichever layer it is called on.
//
// Mega Hack plays these same macros and completes the level. It does not step anything: it lets the
// game run its own frames and injects inputs. Showcase playback does the same and flies far into
// the level correctly. That is the difference, and it is the whole of it.
//
// So this drives nothing. It turns the game's own clock up, lets the game step itself exactly as it
// always does, injects the macro's inputs on their frames, and writes down where the icon goes.
// The level plays itself at speed for a few seconds and is then rewound. Slower than a loop that
// does not work.
static bool   g_ffOn = false;         // computing a path by fast-forwarding the real level
static float  g_ffMul = 1.f;
static std::vector<GhEv> g_ffEv, g_ffEv2;
static size_t g_ffAi = 0, g_ffAi2 = 0;
static bool   g_ffDown = false, g_ffDown2 = false;
static double g_ffStopX = 0.0, g_ffLastX = -1e9;
static int    g_ffDeaths = 0;
static bool   g_ffArmed = false;   // past the spawn, so a death means something
static long long g_ffPrevFrame = -1;   // the last physics frame that received input
static bool   g_ffWanted = false;      // a path is needed; start once the level is really running
static int    g_ffFrames = 0;          // frames since the run began, for arming the death check

// Arm the two input timelines for the level about to be flown.
static void ffArm(PlayLayer* pl) {
    g_ffEv.clear(); g_ffEv2.clear();
    const bool p2macro = pathPlayerIsP2();
    // Same correction: the frame the macro recorded, not one derived from a time.
    auto frameOf = [](double t) { return (long long)std::llround(t * 240.0); };
    for (auto const& a : g_actions) {
        const bool second = p2macro ? !a.p2 : a.p2;
        auto& into = second ? g_ffEv2 : g_ffEv;
        const long long pf = a.pressFrame   >= 0 ? a.pressFrame   : frameOf(a.pressTime);
        const long long rf = a.releaseFrame >= 0 ? a.releaseFrame : frameOf(a.releaseTime);
        into.push_back({ a.pressTime, pf, true });
        if (a.releaseTime > a.pressTime) into.push_back({ a.releaseTime, rf, false });
    }
    auto byT = [](GhEv const& a, GhEv const& b) { return a.t < b.t; };
    std::sort(g_ffEv.begin(), g_ffEv.end(), byT);
    std::sort(g_ffEv2.begin(), g_ffEv2.end(), byT);
    g_ffAi = 0; g_ffAi2 = 0; g_ffDown = false; g_ffDown2 = false;
    g_ffLastX = -1e9; g_ffDeaths = 0; g_ffArmed = false; g_ffPrevFrame = -1; g_ffFrames = 0;
    g_ghostPath.clear(); g_ghostWave.clear(); g_ghostHold.clear();
    g_ghostPath2.clear(); g_ghostWave2.clear(); g_ghostHold2.clear();
    const double endX = pl ? (double)pl->m_endXPosition : 0.0;
    g_ffStopX = endX - std::max(600.0, endX * 0.02);
}

// One physics substep's worth of input, called from processCommands - the same place showcase
// playback injects, which is the only injection point that has ever produced a flight.
// HOW A MACRO BOT ACTUALLY DOES THIS.
//
// Read from xdBot's playback (zilko/xdBot, src/main.cpp handlePlaying and src/global.cpp
// getCurrentFrame) - the technique, written here from scratch, not its code. Four things it does
// that this did not, and each one is a way to be a frame out:
//
//  1. The frame comes from m_gameState.m_currentProgress - the game's OWN physics frame counter -
//     not from levelTime multiplied by an assumed 240. That is exactly what xdBot uses for imported
//     macros like these, and it cannot drift however the clock is scaled, which is why a bot can
//     play at 12x without dying and this could not.
//  2. Inputs are applied AFTER the original processCommands, not before it.
//  3. Nothing is applied twice within one physics frame - if the counter has not advanced, the call
//     is skipped entirely.
//  4. A dead player has its buttons released rather than being fed more input.

static void ffInject(GJBaseGameLayer* gl, std::vector<PendPress>& out) {
    if (!g_ffOn || !gl) return;
    auto* pl = PlayLayer::get();
    if (!pl || !pl->m_player1) return;

    if (pl->m_player1->m_isDead) {
        pl->m_player1->releaseAllButtons();
        if (pl->m_player2) pl->m_player2->releaseAllButtons();
        return;
    }

    // Same correction as showcase: the macro's own clock, via seconds.
    const double mfps = g_macroFps > 1.0 ? g_macroFps : 240.0;
    const long long fr = (long long)std::llround(gl->m_gameState.m_levelTime * mfps);

    // EVERY INPUT, NOT THE STATE THEY ADD UP TO.
    //
    // This collapsed the pending events into a final held/released state and only pressed when that
    // state CHANGED. Eclipse does not: it polls every input due at this frame and calls handleButton
    // for each one (src/hacks/Bot/Bot.cpp, processCommands).
    //
    // The difference is invisible at 1x and fatal at 12x. The frame counter advances in blocks when
    // the clock is scaled, so a press and its release routinely fall in the same poll - and a
    // collapse turns that tap into nothing at all. Every quick click in the macro was being
    // swallowed, which is why a real bot survives at speed and this died in a tenth of a second.
    //
    // No frame dedup either, for the same reason: the cursor only ever moves forward, so an input
    // is consumed exactly once whether or not the counter moved since the last call.
    while (g_ffAi < g_ffEv.size() && g_ffEv[g_ffAi].f <= fr) {
        out.push_back({ g_ffEv[g_ffAi].down, true });
        g_ffDown = g_ffEv[g_ffAi].down;
        g_ffAi++;
    }
    while (g_ffAi2 < g_ffEv2.size() && g_ffEv2[g_ffAi2].f <= fr) {
        out.push_back({ g_ffEv2[g_ffAi2].down, false });
        g_ffDown2 = g_ffEv2[g_ffAi2].down;
        g_ffAi2++;
    }
    g_ffPrevFrame = fr;
}

// ===========================================================================================
// RECORD THE RUN THAT ACTUALLY HAPPENS.
//
// Five attempts to work the path out by replaying the macro have all failed the same way - the icon
// dies within a second, however it is driven. But the user can play the very same macro through
// Eclipse and it completes the level. So stop trying to reproduce that run and simply watch it.
//
// This drives nothing and injects nothing. While it is on, every frame of gameplay is written down:
// where the icon is, whether it is a wave, whether the button is held. Whatever is flying the level
// - a bot, or the person - the result is a real flight through real geometry, which is the one
// thing none of the replays has produced.
//
// It is also the experiment that splits the problem in half. If a path recorded this way draws
// correctly under the icon, then the drawing, the caching and the wave-stretch filtering are all
// sound and the fault is entirely in the replay. If it does not, the fault is downstream and every
// hour spent on the replay was spent in the wrong place.
static bool   g_recOnPath = false;
static int    g_recFrames = 0;
static double g_recLastX = -1e9, g_recMinX = 1e18, g_recMaxX = -1e18;
static int    g_recDeaths = 0, g_recWaveFrames = 0;
static double g_recBeganAt = 0.0;
static int    g_recNextReport = 0;
static double g_recPrevVy = 0.0;
static bool   g_recHadVy = false;

// The furthest run so far this session, and the one that will be saved.
static std::vector<CCPoint> g_rpBest, g_rpBest2;
static std::vector<uint8_t> g_rpW, g_rpH, g_rpW2, g_rpH2;
static double g_rpReach = -1e18;
// Once a run has been kept, this level is done for the session. Without it the recorder re-armed
// the moment the level was completed - with the icon sitting at the end portal - and immediately
// "reached the end" again, replacing a perfect 16,112-frame recording with 1,831 points all at
// x=53955. A good path must not be replaceable by a degenerate one.
static bool g_rpSaved = false;
static void recPathForget();

// EVERY ATTEMPT ADDS TO THE PATH.
//
// Keeping only the single furthest run threw away everything a practising player does: they die
// constantly, reach different distances, and practise one corridor at a time from a StartPos. A run
// that covered 0..5000 was discarded the moment a later one reached 6000, even though the second
// might have started at 4000 and known nothing about the first half.
//
// The macro defines one route, so two runs over the same x describe the same place. Merging by
// position therefore just fills the gaps: whatever has been flown is drawn, and the line grows
// across a session instead of restarting with each attempt.
static void recPathKeepBest() {
    if (g_ghostPath.size() < 9) return;
    const size_t was = g_rpBest.size();

    struct Pt { float x, y; uint8_t w, h; };
    std::vector<Pt> all;
    all.reserve(g_rpBest.size() + g_ghostPath.size());
    for (size_t i = 0; i < g_rpBest.size(); i++)
        all.push_back({ g_rpBest[i].x, g_rpBest[i].y,
                        i < g_rpW.size() ? g_rpW[i] : (uint8_t)0,
                        i < g_rpH.size() ? g_rpH[i] : (uint8_t)0 });
    for (size_t i = 0; i < g_ghostPath.size(); i++)
        all.push_back({ g_ghostPath[i].x, g_ghostPath[i].y,
                        i < g_ghostWave.size() ? g_ghostWave[i] : (uint8_t)0,
                        i < g_ghostHold.size() ? g_ghostHold[i] : (uint8_t)0 });
    std::sort(all.begin(), all.end(), [](Pt const& a, Pt const& b) { return a.x < b.x; });

    g_rpBest.clear(); g_rpW.clear(); g_rpH.clear();
    float lastX = -1e9f;
    for (auto const& q : all) {
        // Two units, not twenty. The recorder now records a point exactly where the icon turns, and
        // a coarse merge would throw those corners straight back away - which is the same chord that
        // made the line look curved. Two runs following the same macro sit within a unit or so of
        // each other over the same ground, so this still collapses them.
        if (q.x - lastX < 2.f) continue;
        lastX = q.x;
        g_rpBest.push_back(ccp(q.x, q.y));
        g_rpW.push_back(q.w);
        if (g_rpBest.size() > 1) g_rpH.push_back(q.h);
    }
    if (g_rpH.size() < g_rpBest.size()) g_rpH.resize(g_rpBest.size(), (uint8_t)0);

    if (g_ghostPath2.size() > g_rpBest2.size()) {
        g_rpBest2 = g_ghostPath2; g_rpW2 = g_ghostWave2; g_rpH2 = g_ghostHold2;
    }
    g_rpReach = g_rpBest.empty() ? -1e18 : (double)g_rpBest.back().x;
    log::info("[CI-REC] this attempt added {} points - the path now covers x={:.0f}..{:.0f} with {}",
              (int)g_rpBest.size() - (int)was,
              g_rpBest.empty() ? 0.0 : (double)g_rpBest.front().x, g_rpReach,
              (int)g_rpBest.size());
}

// Everything the recorder holds, dropped when the level is left.
static void recPathForget() {
    // Whatever was being recorded belonged to the macro that just went away. Continuing would file
    // one macro's run under another's key.
    g_recOnPath = false;
    g_rpSaved = false; g_rpReach = -1e18;
    g_rpBest.clear(); g_rpW.clear(); g_rpH.clear();
    g_rpBest2.clear(); g_rpW2.clear(); g_rpH2.clear();
}

static void recPathReset() {
    g_recFrames = 0; g_recLastX = -1e9; g_recMinX = 1e18; g_recMaxX = -1e18;
    g_recDeaths = 0; g_recWaveFrames = 0; g_recNextReport = 0;
    g_recPrevVy = 0.0; g_recHadVy = false;
    g_recBeganAt = nowSeconds();
    g_ghostPath.clear(); g_ghostWave.clear(); g_ghostHold.clear();
    g_ghostPath2.clear(); g_ghostWave2.clear(); g_ghostHold2.clear();
}

static void recPathStep(PlayLayer* pl) {
    if (!pl || !pl->m_player1) return;
    auto* p1 = pl->m_player1;
    const double x = p1->getPositionX(), y = p1->getPositionY();
    if (p1->m_isDead) return;
    g_recFrames++;
    if (p1->m_isDart) g_recWaveFrames++;
    if (x < g_recMinX) g_recMinX = x;
    if (x > g_recMaxX) g_recMaxX = x;

    // RECORD THE CORNER, NOT THE GRID.
    //
    // Sampling purely on distance puts a point wherever the grid falls, which is almost never where
    // the icon actually turns - so the drawn segment spanning a turn is a chord straight across it.
    // A wave zig-zagging every 40 units gets its corners shaved off by 20-unit sampling, and a run
    // of shaved corners is exactly what reads as a curved line.
    //
    // A wave changes direction when its vertical velocity changes sign. That frame IS the corner,
    // and between two corners the line is straight and needs nothing recorded at all.
    const double vy = p1->m_yVelocity;
    const bool turned = g_recHadVy && ((vy > 0.0) != (g_recPrevVy > 0.0));
    g_recPrevVy = vy; g_recHadVy = true;
    if (!turned && x - g_recLastX < 20.0) return;
    g_recLastX = x;
    g_ghostPath.push_back(ccp((float)x, (float)y));
    g_ghostWave.push_back(p1->m_isDart ? 1 : 0);
    if (g_ghostPath.size() > 1)
        g_ghostHold.push_back(p1->m_holdingButtons[1] ? 1 : 0);
    if (auto* p2 = pl->m_player2) {
        g_ghostPath2.push_back(ccp(p2->getPositionX(), p2->getPositionY()));
        g_ghostWave2.push_back(p2->m_isDart ? 1 : 0);
        if (g_ghostPath2.size() > 1)
            g_ghostHold2.push_back(p2->m_holdingButtons[1] ? 1 : 0);
    }

    // A running commentary, so a log from someone else's machine is enough to see what happened.
    if ((int)x / 5000 > g_recNextReport) {
        g_recNextReport = (int)x / 5000;
        log::info("[CI-REC] x={:.0f} y={:.0f} | {} points, {} of {} frames as a wave, {} death(s), "
                  "{:.1f}s", x, y, (int)g_ghostPath.size(), g_recWaveFrames, g_recFrames,
                  g_recDeaths, nowSeconds() - g_recBeganAt);
    }
}

// Keep what has been flown so far, under the key for this level and macro.
static void recPathStore(PlayLayer* pl) {
    if (!pl || g_rpBest.size() < 9 || g_actions.empty()) return;
    auto sp = g_ghostPath; auto sw = g_ghostWave; auto sh = g_ghostHold;
    auto sp2 = g_ghostPath2; auto sw2 = g_ghostWave2; auto sh2 = g_ghostHold2;
    g_ghostPath = g_rpBest;   g_ghostWave = g_rpW;   g_ghostHold = g_rpH;
    g_ghostPath2 = g_rpBest2; g_ghostWave2 = g_rpW2; g_ghostHold2 = g_rpH2;
    ghostSave(pl);
    g_ghostPath = sp; g_ghostWave = sw; g_ghostHold = sh;
    g_ghostPath2 = sp2; g_ghostWave2 = sw2; g_ghostHold2 = sh2;
}

static void recPathFinish(PlayLayer* pl, const char* why) {
    recPathKeepBest();
    if (g_rpBest.size() < 9) {
        log::info("[CI-REC] {} - the best run of this session only reached {} points, nothing worth "
                  "keeping", why, (int)g_rpBest.size());
        return;
    }
    // A path is filed under the macro it belongs to. With no macro loaded the key is the hash of an
    // empty input list, which nothing will ever look for - and the guide is not drawn at all
    // without one. Recording in that state produces a file nobody can use, so say so instead.
    if (g_actions.empty()) {
        log::info("[CI-REC] {} with {} points, but NO MACRO IS LOADED. A path is stored against the "
                  "macro it belongs to, so there is nothing to file this under. Download or pick a "
                  "macro for this level first, then record.", why, (int)g_rpBest.size());
        return;
    }
    g_ghostPath = g_rpBest;   g_ghostWave = g_rpW;   g_ghostHold = g_rpH;
    g_ghostPath2 = g_rpBest2; g_ghostWave2 = g_rpW2; g_ghostHold2 = g_rpH2;
    g_rpSaved = true;
    const double endX = pl ? (double)pl->m_endXPosition : 0.0;
    const double covered = g_recMaxX - (g_recMinX < 1e17 ? g_recMinX : 0.0);
    int waves = 0;
    for (auto w : g_ghostWave) waves += w ? 1 : 0;
    log::info("[CI-REC] {}: {} points from x={:.0f} to {:.0f} ({:.0f}% of the level), {} of them "
              "wave, {} death(s), {:.1f}s of play",
              why, (int)g_ghostPath.size(), g_recMinX, g_recMaxX,
              endX > 0 ? 100.0 * covered / endX : 0.0, waves, g_recDeaths,
              nowSeconds() - g_recBeganAt);
    if (pl) {
        ghostSave(pl);
        log::info("[CI-REC] saved under {} - reopen the level and the guide is drawn from this run",
                  ghostKey(pl));
    }
}

static StartPosObject* g_ffStart = nullptr;
static double g_ffBeganAt = 0.0;

// Begin. Rewinds the level to 0%, arms the macro, and turns the clock up. Everything after this is
// the game running itself.
static bool ffBegin(PlayLayer* pl) {
    if (!pl || !pl->m_level || g_actions.empty() || g_ffOn) return false;
    g_ghProg.save(pl->m_level);
    g_ffStart = pl->m_startPosObject;
    pl->m_startPosObject = nullptr;      // the macro's clock is level time from 0%
    pl->m_isTestMode = true;             // nothing this produces may count
    pl->resetLevelFromStart();
    pl->startGame();
    if (!pl->m_player1) { pl->m_startPosObject = g_ffStart; g_ghProg.restore(); return false; }
    ffArm(pl);
    if (g_ffEv.empty() && g_ffEv2.empty()) { pl->m_startPosObject = g_ffStart; g_ghProg.restore(); return false; }
    g_ffMul = 12.f;
    g_ffOn = true;
    g_ffBeganAt = nowSeconds();
    log::info("[CI-FF] working the path out by playing the level at {}x - {} inputs, to x={:.0f}",
              (int)g_ffMul, (int)(g_ffEv.size() + g_ffEv2.size()), g_ffStopX);
    return true;
}

// Called every frame while computing: write down where the icons are.
static void ffRecord(PlayLayer* pl) {
    if (!g_ffOn || !pl || !pl->m_player1) return;
    auto* p1 = pl->m_player1;
    const double x = p1->getPositionX(), y = p1->getPositionY();
    if (x - g_ffLastX >= 20.0) {
        g_ghostPath.push_back(ccp((float)x, (float)y));
        g_ghostWave.push_back(p1->m_isDart ? 1 : 0);
        if (g_ghostPath.size() > 1) g_ghostHold.push_back(g_ffDown ? 1 : 0);
        g_ffLastX = x;
        if (auto* p2 = pl->m_player2) {
            g_ghostPath2.push_back(ccp(p2->getPositionX(), p2->getPositionY()));
            g_ghostWave2.push_back(p2->m_isDart ? 1 : 0);
            if (g_ghostPath2.size() > 1) g_ghostHold2.push_back(g_ffDown2 ? 1 : 0);
        }
    }
}

// Whether the replay owns its level (a hidden copy) or borrowed the one being played.
static bool g_ghOwned = true;
static StartPosObject* g_ghBorrowedStart = nullptr;

static void ghostLayerDrop() {
    if (!g_ghLayer) { g_ghProg.restore(); return; }
    auto* dead = g_ghLayer;
    const bool owned = g_ghOwned;
    g_ghLayer = nullptr;              // clear FIRST: the guards below key off it
    if (!owned) {
        // A borrowed level is handed back exactly as it was found: StartPos returned, and rewound
        // so the player begins where they expect rather than wherever the replay left the icon.
        const bool wasGhost = g_ghost;
        g_ghost = true;
        dead->m_startPosObject = g_ghBorrowedStart;
        dead->resetLevelFromStart();
        g_ghost = wasGhost;
        g_ghBorrowedStart = nullptr;
        g_ghOwned = true;
    }
    g_ghProg.restore();
    dead->release();
    g_ghEv.clear();
}

// Build the hidden level and get it running. Returns false if anything about it looks wrong, and
// the caller falls back to the in-place replay.
// REPLAY ON THE LEVEL THE PLAYER IS IN, NOT A HIDDEN COPY OF IT.
//
// The hidden layer never flew anything: 35,922 and 46,300 deaths, all of them the flight icon, in
// runs of about that many steps. Whatever PlayLayer::create leaves undone for a layer that is never
// added to a scene, setupHasCompleted alone does not repair it - and no amount of tuning above it
// could matter while the icon was dead.
//
// The decisive evidence came from outside this mod: Mega Hack plays these same macros on the real
// layer and they complete the level. The inputs are right and the macro is right; a real PlayLayer
// flies them. So use one. The in-place version is also the only one that ever measured correct -
// 0.0 units against the player over 8,039 frames - and every guard it needs already exists:
// completion blocked four ways, percentages saved and restored, jumps discarded, and the replay's
// own presses kept out of grading.
//
// `borrow` is the level being played. Given one, nothing is created and the level is handed back
// exactly as it was found.
static bool ghostLayerBegin(GJGameLevel* lvl, bool fromSpawn, PlayLayer* borrow) {
    if (g_ghLayer || !lvl || g_actions.empty()) return false;

    g_ghEv.clear();
    g_ghEv.reserve(g_actions.size() * 2);
    g_ghEv2.clear();
    // WHICH TAG IS THE FLIGHT PATH IS NOT ALWAYS "PLAYER ONE".
    //
    // A single-player macro can arrive with every input tagged player two - this one has 692 of
    // them against 48 for player one, on a level with one icon. The replay drove the 48, survived
    // on invincibility, and drew a path from almost no inputs; showcase playback sent the real 692
    // to a player that is not the icon on screen, and died. The rest of this mod has always known
    // about this and asks pathPlayerIsP2(); the replay was the one place that assumed.
    const bool p2macro = pathPlayerIsP2();
    auto frameOf = [](double t) { return (long long)std::llround(t * 240.0); };
    for (auto const& a : g_actions) {
        const bool second = p2macro ? !a.p2 : a.p2;
        auto& into = second ? g_ghEv2 : g_ghEv;
        const long long pf = a.pressFrame   >= 0 ? a.pressFrame   : frameOf(a.pressTime);
        const long long rf = a.releaseFrame >= 0 ? a.releaseFrame : frameOf(a.releaseTime);
        into.push_back({ a.pressTime, pf, true });
        if (a.releaseTime > a.pressTime) into.push_back({ a.releaseTime, rf, false });
    }
    auto byTime = [](GhEv const& a, GhEv const& b) { return a.t < b.t; };
    std::sort(g_ghEv.begin(), g_ghEv.end(), byTime);
    std::sort(g_ghEv2.begin(), g_ghEv2.end(), byTime);
    // A single-player macro tags nothing p2, so the second list is simply empty and everything
    // below behaves exactly as it did.
    if (g_ghEv.empty() && g_ghEv2.empty()) return false;

    g_ghProg.save(lvl);          // nothing this run does may reach the player's record

    if (borrow) {
        g_ghOwned = false;
        g_ghLayer = borrow;
        g_ghLayer->retain();
        g_ghBorrowedStart = g_ghLayer->m_startPosObject;
        const bool wasGhost = g_ghost;
        g_ghost = true;
        // Rewound to 0%, with the StartPos out of the way, so the replay runs on the clock the
        // macro was recorded against. Both are put back in ghostLayerDrop.
        g_ghLayer->m_startPosObject = nullptr;
        g_ghLayer->m_isTestMode = true;
        g_ghLayer->resetLevelFromStart();
        g_ghLayer->startGame();
        g_ghost = wasGhost;
        if (!g_ghLayer->m_player1) { ghostLayerDrop(); return false; }
        g_ghFromStart = false;
        g_ghClockAdj = 0.0;
    } else {

    PlayLayer* mk = nullptr;
    g_ghBuilding = true;
    {
        // Built with the global still pointing at the real level for as short a time as possible;
        // create() will set it to the new one, and the scope guard puts it back on the way out.
        GhScope keep(GameManager::sharedState()->m_playLayer);
        mk = PlayLayer::create(lvl, false, false);
    }
    g_ghBuilding = false;
    if (!mk) return false;
    mk->retain();
    mk->setVisible(false);
    // NOBODY TICKS THIS LEVEL BUT ME.
    //
    // PlayLayer schedules itself on creation, and cocos's scheduler does not care that it was never
    // added to a scene - it ticks it every frame regardless. So the game was running two entire
    // levels at once, one of them for nothing, on top of the slices this code was already asking
    // for. That is the three frames a second, and it would have got worse with every level opened.
    // Stepped only by ghostLayerSlice, deliberately and by the millisecond.
    mk->unscheduleAllSelectors();
    mk->pauseSchedulerAndActions();

    g_ghLayer = mk;
    {
        GhScope use(g_ghLayer);
        // startGame reaches resetLevel and the rest of the mod's own hooks. They belong to the
        // level being played, not to this one.
        const bool wasGhost = g_ghost;
        g_ghost = true;
        // FINISH SETTING THE LEVEL UP BEFORE PLAYING IT.
        //
        // PlayLayer::create builds the layer; setupHasCompleted is what the normal play path calls
        // afterwards to finish the job - and a detached layer that is never added to a scene never
        // gets it. So the replay has been flying a level that was never finished: the icon dies in
        // the first moments and stays dead, which is the 35,922 deaths in a 30,000-step run. Every
        // number downstream of that - inputs applied, distance reached, legal angles - describes a
        // corpse being carried forward by the suppression, which is why they all looked healthy.
        g_ghBuilding = true;              // this mod's own hooks stay out of it
        g_ghLayer->setupHasCompleted();
        g_ghBuilding = false;
        g_ghLayer->m_isTestMode = true;      // belt and braces; nothing here should ever count
        // startGame plays the level's song. There is no level being played - the person is looking
        // at a menu - so the song has to be silenced going in and stopped coming out, or a macro
        // download starts blasting the track of a level nobody has entered.
        auto* fmod = FMODAudioEngine::sharedEngine();
        const float bgWas = fmod ? fmod->getBackgroundMusicVolume() : 1.f;
        if (fmod) fmod->setBackgroundMusicVolume(0.f);
        g_ghLayer->startGame();
        if (fmod) {
            fmod->stopAllMusic(false);       // whatever it just queued, before it can be heard
            fmod->setBackgroundMusicVolume(bgWas);
        }
        g_ghost = wasGhost;
    }
    if (!g_ghLayer->m_player1) { ghostLayerDrop(); return false; }

    // START WHERE THE PLAYER STARTS.
    //
    // The replay began at 0% and flew the whole level to reach the icon. From a StartPos at 92%
    // that is 92% of a level simulated to draw the last 8% - and it is the case practice is always
    // in. But GD spawns at a StartPos with a defined state, and that state is precisely what the
    // player is about to fly, so a replay from there describes the same thing for a fraction of the
    // work.
    //
    // The catch is the clock. A macro's timestamps are level time from 0%, and at a StartPos the
    // game seeds m_levelTime with the spawn's own canonical time - which is why raw m_levelTime is
    // the macro's clock only from 0%. Every other reader in this file corrects for that with
    // g_startOffset, and now so does this: the offset applies to the detached level for the same
    // reason it applies to the real one, because both spawned at the same StartPos.
    // The full pass deliberately throws the StartPos away: it is flying the level, not somebody's
    // practice attempt, and its answer has to be true for every spawn.
    if (!fromSpawn && g_ghLayer->m_startPosObject) {
        GhScope use(g_ghLayer);
        g_ghLayer->m_startPosObject = nullptr;
        g_ghLayer->resetLevelFromStart();
        g_ghLayer->startGame();
        if (!g_ghLayer->m_player1) { ghostLayerDrop(); return false; }
    }
    g_ghFromStart = fromSpawn && g_ghLayer->m_startPosObject != nullptr;
    g_ghClockAdj = g_ghFromStart ? (double)g_startOffset : 0.0;

    }   // end of the create-a-hidden-level path

    g_ghEndX = (double)g_ghLayer->m_endXPosition;
    if (!(g_ghEndX > 0.0)) { ghostLayerDrop(); return false; }
    g_ghStopX = g_ghEndX - std::max(600.0, g_ghEndX * 0.02);
    g_ghX0 = (double)g_ghLayer->m_player1->getPositionX();

    // BOTH ICONS, NOT ONE.
    //
    // Only the player-one half of this was ever rewound. g_ghAi2, g_ghDown2, g_ghLastDown2,
    // g_ghLastX2 and the second path are file-scope, and the in-place version that reset them was
    // dropped when this was rewritten. So on the pass that actually gets cached the second cursor
    // is already past the end of its own list, and the second icon is flown with ZERO of its inputs
    // - 361 of them on the macro in the logs - while g_ghDown2 still holds the previous pass's
    // button against a brand-new level.
    //
    // On a dual that icon crashes, a real attempt ends there, and the replay carries on because
    // destroyPlayer is suppressed. The drawn line then covers ground no real run reaches, which is
    // exactly a faithful record of a run the macro does not produce. The second path also
    // accumulated across passes and was written to the cache and shared from there.
    g_ghostPath.clear(); g_ghostWave.clear(); g_ghostHold.clear();
    g_ghostPath2.clear(); g_ghostWave2.clear(); g_ghostHold2.clear();
    // Everything the macro asked for before this spawn already happened, as far as this replay is
    // concerned. Leaving a cursor at zero would fire the whole level's inputs in one frame.
    {
        const double t0 = g_ghLayer->m_gameState.m_levelTime + g_ghClockAdj;
        g_ghAi = 0;
        while (g_ghAi < g_ghEv.size() && g_ghEv[g_ghAi].t <= t0) g_ghAi++;
        g_ghEvFrom = g_ghAi;
        g_ghAi2 = 0;
        while (g_ghAi2 < g_ghEv2.size() && g_ghEv2[g_ghAi2].t <= t0) g_ghAi2++;
    }
    g_ghDown = false; g_ghLastDown = false;
    g_ghDown2 = false; g_ghLastDown2 = false; g_ghLastX2 = -1e9;
    g_ghLastX = -1e9; g_ghPvX = -1e18; g_ghPvY = 0.0; g_ghPvM = 1e18;
    g_ghSteps = 0; g_ghWedge = 0; g_ghSpent = 0.0;
    g_ghDied = false; g_ghDiedX = 0.0; g_ghDeathArmed = false; g_ghSent = 0; g_ghSeen = 0;
    g_ghDeaths = 0; g_ghDeathsP1 = 0;
    g_ghostGood = false;
    log::info("[CI-GHOST] replaying on a level of its own - {} inputs for the flight path, {} for "
              "the second icon (macro tags its path as player {}), to x={:.0f} of {:.0f}. The level "
              "being played is not touched.",
              (int)g_ghEv.size(), (int)g_ghEv2.size(), p2macro ? 2 : 1, g_ghStopX, g_ghEndX);
    return true;
}

// One slice. Returns true when the whole replay is finished.
static bool ghostLayerSlice(double budgetMs) {
    if (!g_ghLayer || !g_ghLayer->m_player1) return true;
    const auto t0 = std::chrono::steady_clock::now();
    const float step = 1.0f / 240.0f;

    GhScope use(g_ghLayer);
    g_ghost = true;                       // the mod's own hooks stay out of this level's business
    bool done = false;
    for (;;) {
        auto* p1 = g_ghLayer->m_player1;
        const double px = p1->getPositionX();
        if (px >= g_ghStopX) { done = true; break; }

        const double t = g_ghLayer->m_gameState.m_levelTime + g_ghClockAdj;
        const long long fr = (long long)std::llround(t * 240.0);
        bool want = g_ghDown;
        while (g_ghAi < g_ghEv.size() && g_ghEv[g_ghAi].f <= fr) { want = g_ghEv[g_ghAi].down; g_ghAi++; }
        if (want != g_ghDown) { g_ghSent++; g_ghLayer->handleButton(want, 1, true); g_ghDown = want; }
        // The other icon, on its own timeline. isPlayer1 = false is the whole difference.
        bool want2 = g_ghDown2;
        while (g_ghAi2 < g_ghEv2.size() && g_ghEv2[g_ghAi2].f <= fr) { want2 = g_ghEv2[g_ghAi2].down; g_ghAi2++; }
        if (want2 != g_ghDown2) { g_ghSent++; g_ghLayer->handleButton(want2, 1, false); g_ghDown2 = want2; }

        g_ghLayer->update(step);
        g_ghSteps++;

        const double nx = p1->getPositionX(), ny = p1->getPositionY();
        // Armed after a hundred steps so the game's own spawn-time destroyPlayer calls are not
        // counted. The run does NOT stop on a death any more: it stopped at step 102 on every macro
        // of every level, which is the instant this arms - so the first thing destroyPlayer did
        // after arming ended the replay before its first input was even due, and nothing on the
        // level could ever produce a path. Whatever fires there is not a death worth trusting yet.
        // Recorded so it can be found; not obeyed.
        if (!g_ghDeathArmed && g_ghSteps > 100 && nx > g_ghX0 + 30.0) g_ghDeathArmed = true;
        if (nx - g_ghLastX < 0.001) { if (++g_ghWedge > 240 * 4) { done = true; break; } }
        else g_ghWedge = 0;

        double m = g_ghPvM;
        if (g_ghPvX > -1e17 && nx - g_ghPvX > 1e-9) m = (ny - g_ghPvY) / (nx - g_ghPvX);
        // A CHORD IS NOT THE PATH.
        //
        // Only corners and a sample every 250 units were kept, and the drawn line between two
        // stored points is a straight chord. Wherever the real trajectory bends more gently than
        // the threshold - riding a slope, easing through a speed change - the chord cuts across it
        // and the line sits beside the icon instead of under it. That is the "off centre in some
        // spots, not others": it is off exactly where the path is not perfectly straight.
        //
        // A gradient step of 0.005 is a fortieth of the shallowest angle a wave flies, so a bend
        // that survives it is genuinely a straight line.
        const bool bent = p1->m_isDart && g_ghPvM < 1e17 && std::fabs(m - g_ghPvM) > 0.005;
        if (bent && g_ghPvX > -1e17) {
            g_ghostPath.push_back(ccp((float)g_ghPvX, (float)g_ghPvY));
            g_ghostWave.push_back(p1->m_isDart ? 1 : 0);
            if (g_ghostPath.size() > 1) g_ghostHold.push_back(g_ghLastDown ? 1 : 0);
            g_ghLastX = g_ghPvX;
        }
        g_ghPvX = nx; g_ghPvY = ny; g_ghPvM = m;

        // And a hard sample often enough that no chord can be long enough to matter. Sixty units
        // is two blocks; the collapse below removes every one of these that sits on a straight
        // line, so on a clean 45-degree flight this costs nothing at all.
        if (g_ghDown != g_ghLastDown || nx - g_ghLastX >= 60.0) {
            g_ghostPath.push_back(ccp((float)nx, (float)ny));
            g_ghostWave.push_back(p1->m_isDart ? 1 : 0);
            if (g_ghostPath.size() > 1) g_ghostHold.push_back(g_ghLastDown ? 1 : 0);
            g_ghLastX = nx;
        }
        g_ghLastDown = g_ghDown;

        // And record it, on the same terms: a corner where it bends, otherwise every 250 units.
        if (auto* p2 = g_ghLayer->m_player2) {
            const double x2 = p2->getPositionX(), y2 = p2->getPositionY();
            if (x2 > 1.0 && (g_ghDown2 != g_ghLastDown2 || x2 - g_ghLastX2 >= 60.0)) {
                g_ghostPath2.push_back(ccp((float)x2, (float)y2));
                g_ghostWave2.push_back(p2->m_isDart ? 1 : 0);
                if (g_ghostPath2.size() > 1) g_ghostHold2.push_back(g_ghLastDown2 ? 1 : 0);
                g_ghLastX2 = x2;
            }
            g_ghLastDown2 = g_ghDown2;
        }

        // Out of time for this frame. Everything above is state, so picking up here next frame is
        // the same computation - there is no notion of "the replay was interrupted".
        if (std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count() >= budgetMs) break;
    }
    g_ghost = false;
    g_ghSpent += std::chrono::duration<double, std::milli>(
                     std::chrono::steady_clock::now() - t0).count();
    return done;
}

static void ghostLayerFinish(PlayLayer* real);

// GET IN FRONT OF THE PLAYER BEFORE THEY START.
//
// Practice happens from a StartPos, and a StartPos at 92% is 92% of a level the replay has not
// flown yet. Slicing a few milliseconds a frame, the line would reach the icon something like
// thirty-five seconds into play - which for the person practising is the feature simply not
// working, and it is the case they will be in almost every time.
//
// So the road in front of them is bought up front, while the loading screen is still up and a wait
// is what a loading screen is for. This is only affordable because the replay has its own level
// now: nothing of the player's is being reset, so there is no attempt to interrupt and no state to
// put back. It stops the moment it is far enough ahead, and it stops anyway at the cap - a level
// nobody wants to wait for is worse than a line that fills in as they go.
static void ghostLayerCatchUp(double toX, double capMs) {
    if (!g_ghLayer) return;
    const auto t0 = std::chrono::steady_clock::now();
    while (g_ghLayer) {
        if (g_ghLayer->m_player1 && (double)g_ghLayer->m_player1->getPositionX() >= toX) break;
        if (ghostLayerSlice(20.0)) { ghostLayerFinish(nullptr); return; }   // finished outright
        if (std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count() >= capMs) break;
    }
    const double got = g_ghLayer && g_ghLayer->m_player1
                     ? (double)g_ghLayer->m_player1->getPositionX() : 0.0;
    log::info("[CI-GHOST] flew to x={:.0f} before handing the level over (wanted {:.0f}, {:.0f} ms) "
              "- the rest fills in while playing", got, toX,
              std::chrono::duration<double, std::milli>(
                  std::chrono::steady_clock::now() - t0).count());
}

// PROCESS THE MACRO WHEN IT ARRIVES, NOT WHEN IT IS NEEDED.
//
// The work cannot be made much cheaper - it is a real flight through the level, forty thousand
// physics steps of it - so the only thing left to choose is when it happens. Doing it at the moment
// the level opens is the worst possible time: it is the one moment the player is waiting on
// something. Doing it when a macro is downloaded or uploaded is the best, because that is already
// an action with a wait attached and a progress bar in front of it. Nobody minds a download taking
// a few more seconds; everybody minds a level that stutters when they press play.
//
// Same replay, same detached level, same cache. Only the moment changes - and after it, opening the
// level is instant from any StartPos, forever.
static GJGameLevel* g_procLevel = nullptr;    // retained while a macro is being processed
static double g_procFrom = 0.0, g_procTo = 1.0;

static bool ghostProcessBusy() { return g_procLevel != nullptr; }

// The memo that stops the level page asking sixty times a second. A file-scope pair rather than a
// function static, so an abandoned run can clear it - otherwise the level it gave up on can never
// be attempted again for the rest of the session.
static int    g_procLastLvl = -1;
static size_t g_procLastN   = (size_t)-1;
static void ghostProcessReset() { g_procLastLvl = -1; g_procLastN = (size_t)-1; }

static double ghostProcessFrac() {
    if (!g_ghLayer || !g_ghLayer->m_player1 || g_procTo <= g_procFrom) return 0.0;
    const double at = (double)g_ghLayer->m_player1->getPositionX();
    const double f = (at - g_procFrom) / (g_procTo - g_procFrom);
    return f < 0.0 ? 0.0 : (f > 1.0 ? 1.0 : f);
}

// AN ABANDONED RUN MUST LEAVE NOTHING BEHIND.
//
// Leaving the menu mid-run poisoned everything after it: the macros tried next all came out as
// straight lines along the floor, which is what a replay looks like when it flies with no inputs.
// Stopping only dropped the hidden level and released the level object - it left the half-built
// path in g_ghostPath for the next level to draw, the events list and phase where they were, and
// the "already looked at this" memo latched. Interrupting a computation has to undo the whole of
// it, not the part that owns memory.
static void ghostProcessReset();

static void ghostProcessStop() {
    if (g_procLevel) { g_procLevel->release(); g_procLevel = nullptr; }
    if (g_ghPhase == 3) { ghostLayerDrop(); }
    g_ghPhase = 0;
    g_ghost = false;
    g_ghBuilding = false;
    g_ghEv.clear();
    g_ghAi = 0; g_ghEvFrom = 0;
    g_ghostPath.clear(); g_ghostWave.clear(); g_ghostHold.clear();
    g_ghQuickPath.clear(); g_ghQuickWave.clear(); g_ghQuickHold.clear();
    g_ghostGood = false;
    g_ghProg.restore();
    ghostProcessReset();     // and let it be attempted again
}

// Returns false when there is nothing to do - no macro, no level data yet, or already worked out.
static bool ghostProcessStart(GJGameLevel* lvl) {
    // Say why, every time. Four builds in a row did nothing here and looked identical from the
    // outside - no line in the log, no bar on screen, no way to tell a refusal from a hook that was
    // never called. A silent early return is not acceptable in something this hard to observe.
    g_procReady = false;
    auto no = [](const char* why) { log::info("[CI-PROC] not now: {}", why); return false; };

    // A MENU CANNOT FLY A LEVEL.
    //
    // This used to build a hidden PlayLayer here, because a macro popup has no real one to borrow.
    // That hidden layer never flew anything - tens of thousands of deaths per run - and the only
    // thing that does work is replaying on the level the player is actually in. There is no such
    // level on this screen, so there is nothing honest to do with a macro here except take it and
    // let the level itself work the path out when it opens.
    //
    // Kept as one early return rather than deleting the callers, so the popup and the page still
    // ask and simply get told no with a reason.
    return no("a path needs the level open - it will be worked out when you play it");
    if (!lvl)             return no("no level object");
    if (g_procLevel)      return no("already working one out");
    if (g_ghLayer)        return no("a replay is already running");
    if (g_actions.empty())return no("no macro loaded");
    // Asked every frame by the level page, so answer the common case - "already looked at this
    // one" - before doing anything that costs. Without this the macro is re-hashed sixty times a
    // second for as long as anybody reads a level description.
    const int lid0 = (int)lvl->m_levelID;
    if (lid0 == g_procLastLvl && g_actions.size() == g_procLastN) return false;
    // The level has to be on disk to be flown. A level GD has not downloaded yet has no objects to
    // collide with, and pressing play is what fetches it - so that case is left to the replay that
    // runs in-game.
    if (lvl->m_levelNotDownloaded)   return no("GD has not downloaded this level yet");
    if (lvl->m_levelString.empty())  return no("the level data is not loaded yet");
    // Already known: nothing to process.
    {
        const int lid = (int)lvl->m_levelID;
        uint64_t h = 1469598103934665603ull ^ 13ull;
        for (auto const& a : g_actions) {
            const uint64_t v = (uint64_t)(a.pressTime * 1000.0)
                             ^ ((uint64_t)(a.releaseTime * 1000.0) << 21);
            h = (h ^ v) * 1099511628211ull;
        }
        if (!Mod::get()->getSavedValue<std::string>(fmt::format("gh-{}-{:x}", lid, h)).empty()) {
            g_procReady = true;      // so the popup can say so rather than implying nothing happened
            return no("already worked out - this level is ready");
        }
    }
    // Only remembered once the answer is "no, and it will still be no next frame". The memo used to
    // be written before the level-string check below, so the very first look - on a page where GD
    // has not fetched the level data yet - latched permanently and no later attempt could ever run.
    g_procLastLvl = lid0; g_procLastN = g_actions.size();
    if (!ghostLayerBegin(lvl, false)) return no("could not build a level to fly");
    g_ghPhase = 3;                      // processing, outside any PlayLayer
    g_procLevel = lvl;
    g_procLevel->retain();
    g_procFrom = g_ghX0;
    g_procTo = g_ghStopX;
    log::info("[CI-PROC] working out this macro's path now, while it is being set up - the level "
              "will open instantly afterwards");
    return true;
}

// One slice, from a menu. Returns true when it has finished (or given up).
static bool ghostProcessTick() {
    if (g_ghPhase != 3 || !g_ghLayer) return true;
    if (!ghostLayerSlice(8.0)) return false;
    // Finished: judge and cache it against the level it was flown on, not a PlayLayer.
    auto* lvl = g_procLevel;
    ghostLayerFinish(nullptr);
    if (!g_ghostGood && g_ghSteps > 0 && g_ghDeathsP1 > g_ghSteps / 4) {
        g_procWhy = "Could not work this one out - the replay never flew the level";
    } else if (!g_ghostGood) {
        g_procWhy = g_ghDied
            ? fmt::format("This macro dies at x={:.0f} - it does not complete the level", g_ghDiedX)
            : fmt::format("This macro only covers part of the level ({} of {} inputs used)",
                          (int)g_ghAi, (int)g_ghEv.size());
    } else g_procWhy.clear();
    if (g_ghostGood && lvl) {
        const int lid = (int)lvl->m_levelID;
        uint64_t h = 1469598103934665603ull ^ 13ull;
        for (auto const& a : g_actions) {
            const uint64_t v = (uint64_t)(a.pressTime * 1000.0)
                             ^ ((uint64_t)(a.releaseTime * 1000.0) << 21);
            h = (h ^ v) * 1099511628211ull;
        }
        const std::string out = ghostSerialise();
        const std::string k = fmt::format("gh-{}-{:x}", lid, h);
        Mod::get()->setSavedValue<std::string>(k, out);
        log::info("[CI-PROC] done - {} points saved under {}. This level opens instantly from now "
                  "on.", (int)g_ghostPath.size(), k);
    }
    ghostProcessStop();
    return true;
}

// WORK IT OUT WHILE THE LEVEL IS LOADING, NOT WHILE IT IS BEING PLAYED.
//
// This is the only moment that satisfies both constraints. The level's objects exist - GD builds
// them before this point, which is why the level page could never do it - and the player has not
// started yet, so a pause here is part of loading a level rather than the game breaking in their
// hands. It happens once per level per macro and is cached forever.
//
// Deliberately synchronous. Slicing it across frames was an attempt to hide the cost, and it put
// the work exactly where it must not be: in gameplay, at four to eight milliseconds of every frame
// for half a minute. A single wait on a loading screen is both shorter in total and honest.
static void ffFinish(PlayLayer* pl) {
    if (!g_ffOn) return;
    g_ffOn = false; g_ffMul = 1.f;
    const double took = nowSeconds() - g_ffBeganAt;
    const double got = g_ghostPath.empty() ? 0.0 : (double)g_ghostPath.back().x;
    const bool ok = g_ffDeaths == 0 && g_ghostPath.size() > 8 && got >= g_ffStopX * 0.8;
    log::info("[CI-FF] done in {:.1f}s: {} points, reached x={:.0f} of {:.0f}, {} death(s). {}",
              took, (int)g_ghostPath.size(), got, g_ffStopX, g_ffDeaths,
              ok ? "Keeping it." : "NOT keeping it - this is not a flight.");
    if (pl) {
        pl->m_startPosObject = g_ffStart;
        pl->resetLevelFromStart();
    }
    g_ffStart = nullptr;
    g_ghProg.restore();
    if (ok) {
        g_ghostGood = true;
        if (pl) ghostSave(pl);
        g_procWhy.clear();
    } else {
        g_ghostGood = false;
        g_ghostPath.clear(); g_ghostWave.clear(); g_ghostHold.clear();
        g_ghostPath2.clear(); g_ghostWave2.clear(); g_ghostHold2.clear();
        g_procWhy = g_ffDeaths
            ? fmt::format("The macro died {} time(s) playing this level", g_ffDeaths)
            : "The macro did not get through the level";
        // Said out loud. This explanation was being written to a variable whose only two readers
        // are both behind dead branches, so when a path failed the mod worked out exactly why and
        // told nobody - the player just got no line and no reason.
        Notification::create("Click Indicators: " + g_procWhy, NotificationIcon::Warning, 4.f)->show();
    }
}

static bool ghostPrepare(PlayLayer* pl) {
    if (!pl || !pl->m_level || g_actions.empty()) return false;
    if (g_ghLayer) return false;
    if (ghostLoad(pl)) return true;          // already known: nothing to do, and nothing to wait for

    const auto t0 = std::chrono::steady_clock::now();
    if (!ghostLayerBegin(pl->m_level, false, pl)) return false;   // the real level, not a copy
    g_ghPhase = 2;
    // BOUNDED. An unbounded loop here is a hard stop with no repaint, and on a long level that is
    // twenty seconds of a window Windows will offer to close for you - which is what "it almost
    // crashed, it froze" is. Ten seconds is already a long time to hold a game still; past that it
    // hands back and finishes in the background, where the sliced route can pick it up without
    // stopping anything.
    const double CAP = 10000.0;
    bool done = false;
    while (g_ghLayer) {
        if (ghostLayerSlice(250.0)) { done = true; break; }
        if (std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count() >= CAP) break;
    }
    if (!done) {
        log::info("[CI-PROC] ten seconds was not enough for this one - the rest finishes while "
                  "playing, and it is cached once it does");
        g_ghostTried = false;      // let the sliced route carry on from here
        return false;
    }
    ghostLayerFinish(pl);
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0).count();
    log::info("[CI-PROC] worked the path out during the load, in {:.0f} ms. This level is now "
              "instant, from any StartPos, and nothing runs while playing.", ms);
    return !g_ghostPath.empty();
}

// Judge it, tidy it, keep it, and let the hidden level go.
static void ghostLayerFinish(PlayLayer* real) {
    if (!g_ghLayer) return;
    const double reached = g_ghostPath.empty() ? 0.0 : (double)g_ghostPath.back().x;
    // Judged on the inputs from the spawn onwards, not the whole macro: a replay that begins at 92%
    // was never going to apply the first nine hundred of them.
    const double want = (double)(g_ghEv.size() > g_ghEvFrom ? g_ghEv.size() - g_ghEvFrom : 0);
    const double got  = (double)(g_ghAi > g_ghEvFrom ? g_ghAi - g_ghEvFrom : 0);
    const bool inputsDone = want <= 0.0 || got >= want * 0.95;
    const double wantX = g_ghStopX - g_ghX0;
    const bool wentFar = wantX <= 0.0 || (reached - g_ghX0) >= wantX * 0.80;
    // A death USED to disqualify the path. It cannot, yet: every macro on a level started failing
    // at exactly the same x=106, which is not something the macros have in common - it is the
    // replay dying in the same place whatever it is given, before its first input is even due. A
    // check that rejects everything is worse than no check, because it takes the working case with
    // it. Recorded and reported, not acted on, until the death itself is understood.
    // A DEAD RUN IS NOT A PATH.
    //
    // The icon dying for most of the run means there is no flight to record, and every other
    // measure here is meaningless: inputs cannot fail to apply and distance cannot fail to be
    // covered when nothing is allowed to end the run. Drawing that is worse than drawing nothing -
    // it is a confident line straight through spikes, and the player has no way to know.
    const bool mostlyDead = g_ghSteps > 0 && g_ghDeathsP1 > g_ghSteps / 4;
    g_ghostGood = inputsDone && wentFar && g_ghostPath.size() > 8 && !mostlyDead;
    if (mostlyDead)
        log::info("[CI-GHOST] REFUSING this path: the icon was dead for {} of {} steps. There is no "
                  "flight here to draw.", (long long)g_ghDeathsP1, (long long)g_ghSteps);
    if (g_ghDied)
        log::info("[CI-GHOST] the replay died at x={:.0f} after {} steps, clock {:.2f}s, {} of {} "
                  "inputs applied. Kept anyway - see the note in the source.",
                  g_ghDiedX, (int)g_ghSteps,
                  g_ghLayer ? g_ghLayer->m_gameState.m_levelTime : 0.0,
                  (int)g_ghAi, (int)g_ghEv.size());

    if (g_ghostGood && g_ghostPath.size() > 2) {
        std::vector<CCPoint> pp; std::vector<uint8_t> ww, hh;
        pp.push_back(g_ghostPath[0]); ww.push_back(g_ghostWave[0]);
        for (size_t i = 1; i + 1 < g_ghostPath.size(); i++) {
            const double ax = g_ghostPath[i].x - pp.back().x;
            const double bx = g_ghostPath[i + 1].x - g_ghostPath[i].x;
            if (ax > 1e-6 && bx > 1e-6 && g_ghostWave[i] == ww.back()) {
                const double m1 = (g_ghostPath[i].y - pp.back().y) / ax;
                const double m2 = (g_ghostPath[i + 1].y - g_ghostPath[i].y) / bx;
                if (std::fabs(m2 - m1) < 0.01) continue;
            }
            pp.push_back(g_ghostPath[i]); ww.push_back(g_ghostWave[i]);
            hh.push_back(i - 1 < g_ghostHold.size() ? g_ghostHold[i - 1] : 0);
        }
        pp.push_back(g_ghostPath.back()); ww.push_back(g_ghostWave.back());
        hh.push_back(g_ghostHold.empty() ? 0 : g_ghostHold.back());
        g_ghostPath.swap(pp); g_ghostWave.swap(ww); g_ghostHold.swap(hh);
    }

    int nWave = 0, legal = 0, tot = 0;
    for (size_t i = 0; i < g_ghostWave.size(); i++) nWave += g_ghostWave[i] ? 1 : 0;
    for (size_t i = 0; i + 1 < g_ghostPath.size(); i++) {
        if (i >= g_ghostWave.size() || !g_ghostWave[i] || !g_ghostWave[i + 1]) continue;
        const double dx = g_ghostPath[i + 1].x - g_ghostPath[i].x;
        if (dx <= 1e-6) continue;
        const double m = std::fabs((g_ghostPath[i + 1].y - g_ghostPath[i].y) / dx);
        tot++;
        for (double L : { 0.0, 0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 4.0 })
            if (std::fabs(m - L) < 0.02) { legal++; break; }
    }
    // A handful of deaths is a replay that diverged somewhere. Thousands is a replay whose icon was
    // dead before it started, and nothing else in this log means anything in that case.
    log::info("[CI-GHOST] deaths during the replay: {} in total, {} of them the flight icon{}",
              (long long)g_ghDeaths, (long long)g_ghDeathsP1,
              g_ghDeaths > g_ghSteps / 4
                  ? " - THAT IS MOST OF THE RUN. The icon was dead for it, so nothing else reported "
                    "here describes a real flight."
                  : ".");
    log::info("[CI-GHOST] inputs: this code sent {}, the level received {}{}",
              (long long)g_ghSent, (long long)g_ghSeen,
              g_ghSeen != g_ghSent
                  ? " - SOMETHING ELSE IS PRESSING BUTTONS ON THE REPLAY. Its path is then not the "
                    "macro's run, and anything following that path dies where the extra input put "
                    "it."
                  : " - nothing else touched it.");
    log::info("[CI-GHOST] done on its own level: {} points ({} wave), {} of {} inputs, reached "
              "x={:.0f} of {:.0f}, {:.1f}% of drawn segments at an angle a wave can fly. {} physics "
              "steps over {:.0f} ms of work, spread across frames - no freeze.",
              (int)g_ghostPath.size(), nWave, (int)g_ghAi, (int)g_ghEv.size(), reached, g_ghStopX,
              tot ? 100.0 * legal / tot : 0.0, (int)g_ghSteps, g_ghSpent);

    // IF THE QUICK PASS ALREADY FLEW THE WHOLE LEVEL, IT IS THE WHOLE LEVEL.
    //
    // The quick pass starts at the player's spawn - but with no StartPos that spawn IS 0%, so
    // g_ghEvFrom is zero and it flies the entire level end to end. It was then thrown into
    // g_ghQuickPath, never cached, and a second pass was started to fly the identical level again:
    // 10.7 seconds of CPU delivered at 8 ms a frame, about 22 seconds at half framerate, for an
    // answer already in hand. The log said so plainly - 344 of 344 inputs, reached x=27487 - and
    // then began "replaying on a level of its own" a second time with the same numbers.
    if (g_ghPhase == 1 && g_ghEvFrom == 0 && g_ghostGood) {
        log::info("[CI-GHOST] the quick pass started at 0% and flew the whole level, so it IS the "
                  "whole level - keeping it and skipping the second run entirely");
        g_ghFullQueued = false;
        g_ghPhase = 2;          // fall through to the keep-and-cache path below
    }

    if (g_ghPhase == 1) {
        // The quick one. Never cached - it is only true for the spawn it started at, and the whole
        // level is already on its way. It stays on screen until that arrives.
        if (g_ghostGood) {
            g_ghQuickPath = g_ghostPath; g_ghQuickWave = g_ghostWave; g_ghQuickHold = g_ghostHold;
            log::info("[CI-GHOST] the part in front of the player is ready ({} points). Now flying "
                      "the whole level in the background so every StartPos is covered.",
                      (int)g_ghQuickPath.size());
        }
        ghostLayerDrop();
        g_ghPhase = 0;
        if (g_ghFullQueued && real && real->m_level && ghostLayerBegin(real->m_level, false)) {
            g_ghPhase = 2; g_ghFullQueued = false;
        }
        return;
    }

    if (!g_ghostGood) {
        log::info("[CI-GHOST] NOT keeping it: {} of {} inputs, reached x={:.0f} of {:.0f}",
                  (int)g_ghAi, (int)g_ghEv.size(), reached, g_ghStopX);
        g_ghostPath.clear(); g_ghostWave.clear(); g_ghostHold.clear();
    } else if (real) {
        // The whole level, and the only one worth keeping: it answers every spawn, so the quick
        // path has nothing left to do.
        ghostSave(real);
        g_ghQuickPath.clear(); g_ghQuickWave.clear(); g_ghQuickHold.clear();
        log::info("[CI-GHOST] the whole level is done and cached - every StartPos is covered now, "
                  "and this level will not be replayed again");
    }
    ghostLayerDrop();
    g_ghProg.restore();
    g_ghPhase = 0;
}

// Replay the macro through the real game layer and write down where the icon goes.
static bool ghostRun(PlayLayer* pl) {
    if (!pl || !pl->m_player1 || g_actions.empty()) return false;
    const double endX = (double)pl->m_endXPosition;
    if (!(endX > 0.0)) return false;

    auto* fmod = FMODAudioEngine::sharedEngine();
    const float bg = fmod ? fmod->getBackgroundMusicVolume() : 1.f;
    const float sfx = fmod ? fmod->getEffectsVolume() : 1.f;
    if (fmod) { fmod->setBackgroundMusicVolume(0.f); fmod->setEffectsVolume(0.f); }

    g_ghost = true;
    // Test mode is what GD itself uses for a run that must not count. The hard blocks above are the
    // real defence; this is the one the game already trusts.
    const bool wasTest = pl->m_isTestMode;
    pl->m_isTestMode = true;
    // THE MACRO'S CLOCK IS LEVEL TIME FROM 0%.
    //
    // The loop below matches the macro's timestamps against raw m_levelTime. Every other reader of
    // g_actions in this file uses m_levelTime + g_startOffset, and that offset is nonzero exactly
    // when a StartPos is set - trackStartOffset returns immediately when there is none, which is
    // why playing from 0% stays bit-exact. Measured, the correction is 0.07 to 0.14 seconds: 17 to
    // 34 physics steps, applied to the FIRST click and every one after it.
    //
    // That is the whole divergence. With no StartPos the replay matched the player to 0.0 units
    // over 8,039 frames; with one, it went wrong at x=28, x=30, x=224 - the first click every time.
    // So the StartPos comes out for the duration and the replay runs on the clock the macro was
    // recorded against, rather than being corrected to it.
    StartPosObject* const savedStart = pl->m_startPosObject;
    pl->m_startPosObject = nullptr;
    // A replay must leave no progress behind. Reaching 92% of a level the player has never beaten
    // is a new personal best as far as the game is concerned, and it would be written to their save
    // as though they had flown it. Read the numbers now and put them back afterwards, whatever the
    // run does in between.
    const int savedNormal   = pl->m_level ? (int)pl->m_level->m_normalPercent.value() : 0;
    const int savedPractice = pl->m_level ? pl->m_level->m_practicePercent : 0;
    const int savedAttempts = pl->m_level ? (int)pl->m_level->m_attempts.value() : 0;
    const int savedNew2     = pl->m_level ? (int)pl->m_level->m_newNormalPercent2.value() : 0;
    const bool savedDone    = pl->m_hasCompletedLevel;
    pl->resetLevelFromStart();
    // The level has to be RUNNING, not merely loaded. Called from setupHasCompleted the level is
    // built but has not started, so stepping update() moves nothing: the replay sat at x=0 for 960
    // frames and gave up with a single recorded point. startGame is what a real attempt does at the
    // same moment, and it is undone by the reset at the end.
    pl->startGame();

    // The rate 2.2 steps its physics at. The macro's times are in seconds of level time, so the
    // two only line up if this is the same clock the game uses.
    const double step = 1.0 / 240.0;
    // Stop well short of the end, as a FRACTION. The first version subtracted 300 units, which on
    // this level is a third of one percent - the replay sailed to 100% and the completion screen
    // came up. The last stretch of a level is not worth a fabricated clear on somebody's account,
    // so the replay gives it up: eight percent, or 1,200 units, whichever leaves more room.
    // Eight percent of the level was being given up, and on a 22,508-unit level that is 1,800 units
    // in which a whole mini wave section can sit and never be replayed - which is exactly what went
    // missing. That margin was chosen when distance was the ONLY thing standing between a replay
    // and a fabricated completion. It is not any more: levelComplete, showEndLayer, showNewBest and
    // playEndAnimationToPos are all refused outright while a ghost runs, m_isTestMode is set, and
    // the level's percentages and attempt count are read before and written back after. Distance is
    // now the fifth line of defence rather than the first, so it can be a short one.
    double stopX = endX - std::max(600.0, endX * 0.02);
    if (endX > 0.0)
        log::info("[CI-GHOST] replaying to x={:.0f} of {:.0f} - the last {:.0f} units are given up "
                  "so this can never reach the end portal", stopX, endX, endX - stopX);
    // Nothing past the last wave is worth simulating. This mod draws wave sections; once the run is
    // clear of the final one, every remaining frame is spent computing a path that will be thrown
    // away. On a level whose wave content ends early that is most of the cost, and the run is
    // shorter, which also means fewer frames in which it can drift.
    double lastWave = 0.0;
    for (auto const& sc : g_rtSecs) lastWave = std::max(lastWave, (double)sc.x1);
    if (lastWave > 0.0) stopX = std::min(stopX, lastWave + 600.0);

    // ONE ORDERED LIST OF EVENTS, NOT TWO SEPARATE QUESTIONS.
    //
    // The first version asked "has any press happened by now" and then, separately, "has any
    // release happened by now" - and after the very first click the second was true forever, so the
    // button spent the whole level released apart from the single frame a press fired. The wave
    // dived, missed its portals, and only 37 of 1,927 recorded points came out as wave at all on a
    // level that is mostly wave. It still reached 92%, because death is blocked during a replay -
    // so getting to the end was never evidence that any of it was right.
    //
    // A press and a release are just two instants on one timeline. Sorted together and applied in
    // order, the button holds for exactly as long as the macro says it does.
    struct Ev { double t; bool down; };
    std::vector<Ev> ev;
    ev.reserve(g_actions.size() * 2);
    for (auto const& a : g_actions) {
        if (a.p2) continue;                       // player one drives the path
        ev.push_back({ a.pressTime, true });
        if (a.releaseTime > a.pressTime) ev.push_back({ a.releaseTime, false });
    }
    std::sort(ev.begin(), ev.end(), [](Ev const& a, Ev const& b) { return a.t < b.t; });
    if (ev.empty()) { g_ghost = false; return false; }

    g_ghostPath.clear(); g_ghostHold.clear(); g_ghostWave.clear();
    g_ghostPath2.clear(); g_ghostHold2.clear(); g_ghostWave2.clear();
    g_ghAi2 = 0; g_ghDown2 = false; g_ghLastDown2 = false; g_ghLastX2 = -1e9;
    g_ghostGood = false;
    // Where the replay began, so "did it get far enough" is measured over the distance it was
    // actually asked to cover rather than from the level origin.
    const double x0Start = pl->m_player1 ? (double)pl->m_player1->getPositionX() : 0.0;
    // Self-checking rather than assumed: if the clock is not where a 0% run starts, the macro's
    // times do not mean what this loop is about to treat them as meaning.
    log::info("[CI-GHOST] frame of reference: startPos={}, x0={:.0f}, clock={:.3f}s, offset={:+.3f}s",
              savedStart ? "yes" : "no", x0Start, pl->m_gameState.m_levelTime, g_startOffset);
    double t = 0.0, lastX = -1e9;
    double updNs = 0.0; long long steps = 0;
    bool lastDown = false;
    // The previous STEP, so a corner can be spotted the frame it happens.
    double pvX = -1e18, pvY = 0.0, pvM = 1e18;
    bool down = false, downNext = false;
    size_t ai = 0;
    const int MAXF = 240 * 60 * 12;                 // twelve minutes of level time, then give up
    int wedge = 0;
    for (int f = 0; f < MAXF; f++) {
        auto* p1 = pl->m_player1;
        if (!p1) break;
        const double x = p1->getPositionX();
        if (x >= stopX) break;

        // THE GAME'S CLOCK, NOT A COUNT OF STEPS.
        //
        // A macro's times are in m_levelTime seconds - that is the clock everything else in this
        // mod grades against - and adding 1/240 per iteration is a different quantity. update()
        // does its own delta handling, which is what getModifiedDelta exists for, so the two drift
        // apart and every input lands at the wrong moment. The replay held the button correctly and
        // still came out with 25 wave points in 1,979, because "correctly" was measured against the
        // wrong timeline. Reading the clock the game keeps cannot drift from it.
        t = pl->m_gameState.m_levelTime;
        while (ai < ev.size() && ev[ai].t <= t) { downNext = ev[ai].down; ai++; }
        if (downNext != down) { pl->handleButton(downNext, 1, true); down = downNext; }

        // Where the time actually goes. Everything this loop does besides update() is a handful of
        // comparisons, so if the total is dominated by update() there is nothing here to optimise
        // and the answer has to come from running it less often rather than faster.
        const auto uS = std::chrono::steady_clock::now();
        pl->update((float)step);
        updNs += (double)std::chrono::duration_cast<std::chrono::nanoseconds>(
                     std::chrono::steady_clock::now() - uS).count();
        steps++;

        const double nx = p1->getPositionX();
        if (nx - lastX < 0.001) { if (++wedge > 240 * 4) break; } else wedge = 0;
        // A WAVE TURNS WHEN A BUTTON CHANGES. NOTHING ELSE.
        //
        // Recording on a change of vertical velocity sign looked equivalent and is not: velocity
        // wobbles across zero on contact, on a slope, and at the moment of a press, so it fired far
        // more often than the icon actually changed direction and chopped clean 45-degree runs into
        // a staircase of twelve-unit steps. The button is the real event, it is known exactly, and
        // between two of them the line is straight by definition.
        // RECORD WHERE THE LINE ACTUALLY BENDS.
        //
        // A button change is not the only thing that turns a wave: meeting a surface does too, and
        // so does a speed portal, and neither announces itself. Sampling on the button plus every
        // forty units means those corners fall BETWEEN two samples, and the segment spanning one
        // comes out at the average of the gradients either side - which is the curve on screen at
        // an angle no wave can fly. It was never the physics; it is where the physics was read.
        //
        // The trajectory's own gradient, step to step, is the honest signal. When it changes, that
        // frame is a corner, and the point recorded is the corner itself rather than somewhere past
        // it. Between two corners the line is straight and needs nothing recorded at all.
        const double ny = p1->getPositionY();
        double stepM = pvM;
        if (pvX > -1e17 && nx - pvX > 1e-9) stepM = (ny - pvY) / (nx - pvX);
        // Only where the line will actually be drawn. A cube's arc and a ship's climb bend on
        // every single step by their nature, so corner-detecting them turned a level into 15,505
        // points, almost none of which are ever shown. A wave flies straight between corners, which
        // is what makes the corner worth catching in the first place.
        const bool bent = p1->m_isDart && pvM < 1e17 && std::fabs(stepM - pvM) > 0.02;
        if (bent && pvX > -1e17) {
            g_ghostPath.push_back(ccp((float)pvX, (float)pvY));
            g_ghostWave.push_back(p1->m_isDart ? 1 : 0);
            if (g_ghostPath.size() > 1) g_ghostHold.push_back(lastDown ? 1 : 0);
            lastX = pvX;
        }
        pvX = nx; pvY = ny; pvM = stepM;

        const bool turned = (down != lastDown);
        // Only a safety net now. Corners are caught above the frame they happen, so a point every
        // forty units was adding samples in the middle of straight lines - the very places a
        // straddling sample turns a clean angle into an average.
        if (turned || nx - lastX >= 250.0) {
            g_ghostPath.push_back(ccp((float)nx, p1->getPositionY()));
            g_ghostWave.push_back(p1->m_isDart ? 1 : 0);
            if (g_ghostPath.size() > 1) g_ghostHold.push_back(lastDown ? 1 : 0);
            lastX = nx;
        }
        lastDown = down;
    }

    if (down) pl->handleButton(false, 1, true);

    // One point per corner. Between two button changes a wave flies a straight line, so every
    // sample taken along it says the same thing as the two on either side - and a run of points on
    // one straight line is what a renderer turns into a chain of separate quads, each with its own
    // outline, which is the stepped edge on screen. Collapsing them costs nothing and the drawn
    // line becomes the handful of long strokes it actually is.
    if (g_ghostPath.size() > 2) {
        std::vector<CCPoint>  pp; std::vector<uint8_t> ww, hh;
        pp.push_back(g_ghostPath[0]);
        ww.push_back(g_ghostWave[0]);
        for (size_t i = 1; i + 1 < g_ghostPath.size(); i++) {
            const double ax = g_ghostPath[i].x - pp.back().x;
            const double bx = g_ghostPath[i + 1].x - g_ghostPath[i].x;
            const bool sameMode = g_ghostWave[i] == ww.back();
            const bool sameHold = i < g_ghostHold.size() && !hh.empty() ? g_ghostHold[i] == hh.back() : true;
            if (ax > 1e-6 && bx > 1e-6 && sameMode && sameHold) {
                const double m1 = (g_ghostPath[i].y - pp.back().y) / ax;
                const double m2 = (g_ghostPath[i + 1].y - g_ghostPath[i].y) / bx;
                // Tighter than the sampling above, or the collapse throws away exactly the detail
                // that was just paid for.
                if (std::fabs(m2 - m1) < 0.002) continue;     // still the same straight line
            }
            pp.push_back(g_ghostPath[i]);
            ww.push_back(g_ghostWave[i]);
            hh.push_back(i < g_ghostHold.size() ? g_ghostHold[i] : 0);
        }
        pp.push_back(g_ghostPath.back());
        ww.push_back(g_ghostWave.back());
        hh.push_back(g_ghostHold.empty() ? 0 : g_ghostHold.back());
        {
        // What the drawn line will actually look like, in one number. A wave flies at a gradient
        // set by its size and the speed it is travelling at - 0.5, 1, 1.5, 2, 3, 4 - or flat when
        // it is resting on something. Anything else is an angle no wave can hold, and now that
        // corners are recorded where they happen there should be almost none.
        int legal = 0, tot = 0;
        for (size_t i = 0; i + 1 < g_ghostPath.size(); i++) {
            // Wave segments only. Everything else is a gamemode this mod does not draw, and
            // including it made the figure read 15.4% while the drawn line was fine.
            if (i >= g_ghostWave.size() || !g_ghostWave[i] || !g_ghostWave[i + 1]) continue;
            const double dx = g_ghostPath[i + 1].x - g_ghostPath[i].x;
            if (dx <= 1e-6) continue;
            const double m = std::fabs((g_ghostPath[i + 1].y - g_ghostPath[i].y) / dx);
            tot++;
            for (double L : { 0.0, 0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 4.0 })
                if (std::fabs(m - L) < 0.02) { legal++; break; }
        }
        log::info("[CI-GHOST] {} physics steps, {:.0f} ms of them inside the game's own update "
              "({:.0f} us per step) - the rest of this loop is comparisons",
              (int)steps, updNs / 1e6, steps ? updNs / steps / 1000.0 : 0.0);
    // How far the replay's own clock ended from the macro's last input. If the two ever drift, this
    // is the number that says so - a frame is 4.2ms, so anything past a few tens of milliseconds is
    // a replay that is no longer flying the macro it was given.
    if (!g_ghEv.empty())
        log::info("[CI-GHOST] finished at frame {}, macro's last input was frame {} ({:+.0f} ms)",
                  (long long)std::llround((g_ghLayer && g_ghLayer->m_player1
                      ? g_ghLayer->m_gameState.m_levelTime : 0.0) * 240.0),
                  g_ghEv.back().f,
                  ((g_ghLayer ? g_ghLayer->m_gameState.m_levelTime : 0.0) - g_ghEv.back().t) * 1000.0);
    log::info("[CI-GHOST] {:.1f}% of {} drawn segments are at an angle a wave can actually fly",
                  tot ? 100.0 * legal / tot : 0.0, tot);
    }
    log::info("[CI-GHOST] {} samples collapsed to {} corners", (int)g_ghostPath.size(), (int)pp.size());
        g_ghostPath.swap(pp); g_ghostWave.swap(ww); g_ghostHold.swap(hh);
    }

    pl->m_startPosObject = savedStart;
    pl->resetLevelFromStart();
    if (pl->m_level) {
        pl->m_level->m_normalPercent      = savedNormal;
        pl->m_level->m_practicePercent    = savedPractice;
        pl->m_level->m_attempts           = savedAttempts;
        pl->m_level->m_newNormalPercent2  = savedNew2;
    }
    pl->m_hasCompletedLevel = savedDone;
    pl->m_uncommittedJumps  = 0;
    pl->m_isTestMode = wasTest;
    g_ghost = false;
    if (fmod) { fmod->setBackgroundMusicVolume(bg); fmod->setEffectsVolume(sfx); }
    if (g_ghostHold.size() < g_ghostPath.size()) g_ghostHold.resize(g_ghostPath.size(), (uint8_t)0);

    // WAS THIS A REPLAY, OR JUST SOMETHING THAT HAPPENED?
    //
    // Every result was being written to disk and uploaded, however bad. One in the logs applied all
    // twelve of a macro's twelve inputs, wandered for 26 seconds, and produced 52 wave points out of
    // 159 - and that was then served back on every later load, and offered to everyone else, with
    // nothing able to replace it. A cache that cannot tell a good answer from a bad one turns one
    // bad run into a permanent one.
    //
    // A faithful replay finishes the macro's inputs and gets where the macro goes. Both are known
    // here, so both are checked before anything is kept.
    const double reached = g_ghostPath.empty() ? 0.0 : (double)g_ghostPath.back().x;
    const double wantX   = stopX - x0Start;
    const bool inputsDone = ev.empty() || (double)ai >= (double)ev.size() * 0.95;
    const bool wentFar    = wantX <= 0.0 || (reached - x0Start) >= wantX * 0.80;
    // A death USED to disqualify the path. It cannot, yet: every macro on a level started failing
    // at exactly the same x=106, which is not something the macros have in common - it is the
    // replay dying in the same place whatever it is given, before its first input is even due. A
    // check that rejects everything is worse than no check, because it takes the working case with
    // it. Recorded and reported, not acted on, until the death itself is understood.
    // A DEAD RUN IS NOT A PATH.
    //
    // The icon dying for most of the run means there is no flight to record, and every other
    // measure here is meaningless: inputs cannot fail to apply and distance cannot fail to be
    // covered when nothing is allowed to end the run. Drawing that is worse than drawing nothing -
    // it is a confident line straight through spikes, and the player has no way to know.
    const bool mostlyDead = g_ghSteps > 0 && g_ghDeathsP1 > g_ghSteps / 4;
    g_ghostGood = inputsDone && wentFar && g_ghostPath.size() > 8 && !mostlyDead;
    if (mostlyDead)
        log::info("[CI-GHOST] REFUSING this path: the icon was dead for {} of {} steps. There is no "
                  "flight here to draw.", (long long)g_ghDeathsP1, (long long)g_ghSteps);
    if (g_ghDied)
        log::info("[CI-GHOST] the replay died at x={:.0f} after {} steps, clock {:.2f}s, {} of {} "
                  "inputs applied. Kept anyway - see the note in the source.",
                  g_ghDiedX, (int)g_ghSteps,
                  g_ghLayer ? g_ghLayer->m_gameState.m_levelTime : 0.0,
                  (int)g_ghAi, (int)g_ghEv.size());
    if (!g_ghostGood)
        log::info("[CI-GHOST] NOT keeping this replay: {} of {} inputs applied, reached x={:.0f} of "
                  "{:.0f}. A partial run is worse than no line at all - it looks authoritative and "
                  "is not.", (int)ai, (int)ev.size(), reached, stopX);

    const double got = reached;
    int nWave = 0;
    for (auto w : g_ghostWave) nWave += w ? 1 : 0;
    // Whether the two timelines actually line up. If the clock ran to 240 seconds and the macro's
    // last input was at 190, or only a third of the events were ever reached, the inputs are being
    // applied against the wrong scale and nothing downstream of that can be right.
    log::info("[CI-GHOST] clock reached {:.1f}s, macro's last input at {:.1f}s, {} of {} inputs "
              "applied", t, ev.empty() ? 0.0 : ev.back().t, (int)ai, (int)ev.size());
    if (!g_ghostGood) { g_ghostPath.clear(); g_ghostWave.clear(); g_ghostHold.clear(); }
    log::info("[CI-GHOST] replayed the macro through the real game: {} points, {} of them wave, "
              "reached x={:.0f} (stopping at {:.0f} of {:.0f}). Nothing was completed, no progress "
              "was recorded, and the level was reset.",
              (int)g_ghostPath.size(), nWave, got, stopX, endX);
    return g_ghostPath.size() > 8;
}


class $modify(ClickGuidePlayLayer, PlayLayer) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("PlayLayer::levelComplete", Priority::VeryLate);
        (void)self.setHookPriority("PlayLayer::destroyPlayer", Priority::VeryLate);
    }

    struct Fields {
        // sticky - flipping safe mode off mid-run doesnt rescue the attempt
        bool safeTripped = false;
        CCDrawNode* overlay = nullptr;
        CCDrawNode* pulse = nullptr;   // sits under the icon, so pulse doesnt cover it
        CCLabelBMFont* hud[5] = { nullptr, nullptr, nullptr, nullptr, nullptr };
        float prevX = 0.f, vxAvg = 0.f; bool vxInit = false;
        float tabBad = 0.f;   // level-seconds the speed table has disagreed with the measurement
        bool tabWas = false;  // last logged state of the speed table, to log only transitions
        size_t segWas = (size_t)-1;   // speed segment last frame, to catch a portal crossing
        // The frame the run enters a wave section is the one place the route's height can be read
        // off reality instead of inferred, so it is worth catching. Everything else about the route
        // lives outside the layer, in the solver - it is a property of the level, not of the run.
        bool wasWave = false;
        int  trajAtt = -1;
        // Rolling distance between the drawn route and the icon. The route is hidden while
        // this is large, because that means the macro is not describing this run.
        float rtMiss = 0.f;
        double predT = 0.0; float predX = 0.f;   // self-check: where we said the player would be
        double advAvg = 0.0;   // level time a frame has been worth lately, at whatever rate
        int wxTry = 0;         // bounded retries at placing the macro's clicks
        double syncLog = 0.0;  // next level time to report clock-vs-position agreement
        double drawLog = 0.0;  // next level time to report what is actually being drawn
        size_t nextTick = 0; double prevRaw = -1.0e9;
        double lastPlayPress = -1.0e9; // cluster refractory
        double recPrevT = -1.0e9;
        CCNode* recBadge = nullptr;
        CCDrawNode* recDot = nullptr;
        CCLabelBMFont* recStats = nullptr;
        float recBlink = 0.f;
    };

    void setupHasCompleted() {
        // This is the hidden level setting itself up, inside PlayLayer::create, called from the
        // code below. It is not a level anybody is playing and none of the mod's work applies to
        // it - least of all working out a path, which is what created it.
        if (g_ghBuilding) { PlayLayer::setupHasCompleted(); return; }
        // Resolved BEFORE the original runs. setup calls resetLevel internally, and resetLevel
        // decides whether this attempt counts by asking guideActive() - which until now still
        // answered for the PREVIOUS level's macro, so the first attempt of every guided level was
        // counted and the first attempt of a macro-less level was wrongly voided. Nothing in this
        // block depends on anything setup does.
        // priority: already loaded -> cached -> auto fetch -> none
        int rawID = m_level ? (int)m_level->m_levelID : 0;
        int origID = m_level ? (int)m_level->m_originalLevel : 0;
        int levelID = (rawID > 0) ? rawID : origID;
        bool autoFetch = Mod::get()->getSettingValue<bool>("auto-fetch");
        if (g_webActive && g_webLevelID == levelID && !g_actions.empty()) {
            // already good
        } else if (levelID > 0 && levelCacheExists(levelID)) {
            // Only mark it loaded if it actually loaded, or a folder of unreadable files pins
            // g_webLevelID and the level is never retried for the rest of the session.
            if (loadActionsFromCacheFolder(levelID)) markWebLoaded(levelID);
            else clearGuide();
        } else if (autoFetch && levelID > 0) {
            clearGuide();
            fetchAndLoadForLevel(levelID);
        } else {
            clearGuide();
        }

        PlayLayer::setupHasCompleted();

        // Worked out HERE, on the loading screen, while the level's objects exist and the player
        // has not started. Once per level per macro, then cached forever.
        //
        // If the macro has not arrived yet - auto-fetch is a network call and may still be in
        // flight - there is nothing to work out, and the sliced in-game route below covers it for
        // this attempt. Next time the macro is on disk and this branch takes it.
        if (!g_actions.empty()) {
            g_ghostTried = true;          // nothing else may start a replay
            if (ghostLoad(this)) {
                log::info("[CI-FF] already worked out - nothing to play");
            } else {
                // OFF BY DEFAULT, DELIBERATELY.
                //
                // Working the path out by playing the level has not been made to work: five
                // approaches, and the icon dies within a second on every one of them. While it is
                // unreliable it must not be able to spoil a level for someone who only wants the
                // click indicators, which are the part of this mod that works. It stays available
                // to turn on for anyone helping to test it.
                //
                // Not started from here in any case: ffBegin calls resetLevelFromStart and
                // startGame, and doing that from inside the level's own setup produces a respawn
                // storm. It begins on the first frame where the level is genuinely running.
                // RETIRED. Working the path out by replaying the macro has been attempted five
                // ways - inference, stepping a detached layer, stepping the real one, a 12x
                // fast-forward, and copying Eclipse's injection exactly - and the icon dies within a
                // second every time. Meanwhile watching a real run gives a path that measures 0.0
                // units against the player over 16,112 frames.
                //
                // So the path comes from what actually happens on this level, not from an attempt
                // to reproduce it. The setting is left in place for anyone willing to help test the
                // replay, and does nothing unless deliberately turned on.
                if (Mod::get()->getSettingValue<bool>("auto-path")) {
                    log::info("[CI-FF] auto-path is on - this is the experimental replay, and it has "
                              "never successfully flown a level. The learned path is the reliable "
                              "one.");
                    g_ffWanted = true;
                }
            }
        }

        if (g_slcTrimmed && !g_actions.empty())
            Notification::create("Macro had a restart - timing may be slightly off",
                                 NotificationIcon::Warning)->show();

        if (!m_fields->overlay) {
            auto overlay = CCDrawNode::create();
            applyAA(overlay);
            overlay->setID("ci-overlay"_spr);
            // Parented to the UI layer, not to PlayLayer. A level's visual effects - shader
            // triggers, screen shake, camera rotation, fades - are applied to the gameplay layers,
            // so a cue living there gets blurred, shaken and tinted along with the level, which is
            // the one thing a timing cue must never be. UILayer is where GD puts the pause button
            // and the checkpoint menu precisely because it is outside all of that.
            //
            // Nothing else has to change: every position is computed as
            // overlay->convertToNodeSpace(m_objectLayer->convertToWorldSpace(...)), which is
            // relative to wherever the overlay actually lives, so the projection self-corrects and
            // the cue still tracks the player exactly as before.
            // And nothing a level does to its own colours reaches in here. UILayer is outside the
            // shader and tint chain already, but a fade applied to an ancestor would still cascade
            // down by default - and a guide that dims when the level dims is unreadable exactly
            // when the level is busiest.
            overlay->setCascadeColorEnabled(false);
            overlay->setCascadeOpacityEnabled(false);
            CCNode* host = m_uiLayer ? (CCNode*)m_uiLayer : (CCNode*)this;
            host->addChild(overlay, 1000);
            m_fields->overlay = overlay;
            CCSize win = CCDirector::sharedDirector()->getWinSize();
            for (int i = 0; i < 5; i++) {
                auto l = CCLabelBMFont::create("", "bigFont.fnt");
                l->setAnchorPoint({ 0.f, 0.f }); l->setScale(0.35f);
                l->setPosition(8.f, 8.f + (4 - i) * 19.f);
                l->setCascadeOpacityEnabled(true);   // setString rebuilds glyphs, keep opacity
                l->setOpacity(100);
                overlay->addChild(l, 5); m_fields->hud[i] = l;
            }
            auto badge = CCNode::create();
            badge->setPosition(10.f, win.height - 26.f);
            badge->setVisible(false);
            auto panel = CCScale9Sprite::create("square02b_001.png");
            panel->setContentSize({ 150.f, 26.f });
            panel->setAnchorPoint({ 0.f, 0.5f });
            panel->setPosition({ 0.f, 0.f });
            panel->setColor({ 8, 10, 16 });
            panel->setOpacity(165);
            badge->addChild(panel);
            auto dot = CCDrawNode::create();
            applyAA(dot);
            dot->setPosition({ 14.f, 0.f });
            badge->addChild(dot, 2);
            auto rl = CCLabelBMFont::create("REC", "bigFont.fnt");
            rl->setAnchorPoint({ 0.f, 0.5f }); rl->setScale(0.36f);
            rl->setPosition({ 26.f, 0.f });
            rl->setColor({ 255, 235, 235 });
            badge->addChild(rl, 2);
            auto rs = CCLabelBMFont::create("", "bigFont.fnt");
            rs->setAnchorPoint({ 1.f, 0.5f }); rs->setScale(0.30f);
            rs->setPosition({ 142.f, 0.f });
            rs->setColor({ 190, 196, 210 });
            badge->addChild(rs, 2);
            overlay->addChild(badge, 8);
            m_fields->recBadge = badge; m_fields->recDot = dot; m_fields->recStats = rs;
        }
        m_fields->prevX = m_player1 ? m_player1->getPositionX() : 0.f;
        m_fields->vxInit = false; m_fields->nextTick = 0; m_fields->prevRaw = -1.0e9;
        m_fields->tabBad = 0.f;
        m_fields->lastPlayPress = -1.0e9; g_cal.lastWall = -1.0;
        calibLoad(); spawnFixLoad(this); calibFreeze(m_player1);
        licRefresh(); licTick();
        recReset(); m_fields->recPrevT = -1.0e9;
        // Unconditional: the offset no longer depends on g_actions, so a macro that arrives late
        // from the auto-fetch no longer lands against an offset of zero.
        ensureSpeedTable(this);
        // Local and copied levels all report m_levelID 0, so keying on it alone put every one of
        // them in a single "xt-0" bucket, feeding one level's position/time map to another. Fall
        // back to the original id, then to the level's own name.
        {
            int xid = m_level ? (int)m_level->m_levelID : 0;
            if (xid <= 0 && m_level) xid = (int)m_level->m_originalLevel;
            if (xid > 0) xtLoad(xid);
            else if (m_level) xtLoadNamed(m_level->m_levelName);
            else xtLoad(0);
            // The wave stretches this level has been seen to have. Same key, same lifetime: what
            // the game said about where the wave is, kept between sessions.
            waveLoad(xid > 0 ? xid : g_xtLevel);
        }
        {
            // Frame-stepping the level was tried here and scored 0.124s against measured data
            // where GD's own continuous model scores 0.021s - so the residual is not portal
            // quantisation and the simulation is not used. Kept behind the debug flag only as a
            // scoreboard, so the next idea can be measured against the same ground truth.
            std::vector<XtPt> observed = g_xt;   // whatever a real run measured, if anything
            if (dbgLog()) xtSimulate(this);
            if (dbgLog() && observed.size() > 8 && g_xt.size() > 8) {
                double e = 0.0; int n = 0;
                for (size_t i = observed.size() / 8; i < observed.size();
                     i += std::max<size_t>(1, observed.size() / 12)) {
                    double sim = xtTimeAt(observed[i].x);
                    if (sim > 0.0) { e += std::fabs(sim - observed[i].t); n++; }
                }
                if (n) log::info("[CI-SIM] simulated vs measured over {} pts: mean |error| = {:.4f}s", n, e / n);
            }
            g_xt = std::move(observed);
            g_xtNextX = g_xt.empty() ? 0.f : g_xt.back().x + 200.f;
        }
        resolveStartOffset(this);
        buildActionPositions(this);

        // Checks our speed profile against GD's own x-at-time on the way in. Agreement to a few
        // units means the portal table is right end to end; a constant ratio would mean the speed
        // constants are scaled wrong, and divergence only after a trigger names what it cannot
        // model. Costs three lines in the log once per level.
        if (dbgLog() && g_segsOk)
            for (float tp : { 2.f, 10.f, 30.f })
                log::debug("[CI-SPD] posForTime({:.0f}) = {:.1f}   table = {:.1f}", tp,
                           this->posForTime(tp).x, xAfterDt(0.0, (double)tp, 0));

        // We have measured ground truth for this level, so score the candidate models against it.
        // GD's own PlayLayer::timeForPos ignores time warp; LevelTools exposes the flag that
        // PlayLayer hides. If one of these matches the observed map, the correction works on every
        // level with no run needed to learn it.
        if (dbgLog() && g_xt.size() > 8 && m_levelSettings && m_speedObjects) {
            int sp = (int)m_levelSettings->m_startSpeed;
            bool plat = m_isPlatformer;
            double eP = 0.0, eW0 = 0.0, eW1 = 0.0; int n = 0;
            for (size_t i = g_xt.size() / 8; i < g_xt.size(); i += std::max<size_t>(1, g_xt.size() / 12)) {
                CCPoint at = ccp(g_xt[i].x, 0.f);
                double obs = g_xt[i].t;
                double a1 = (double)this->timeForPos(at, 0, 0, false, 0);
                double a2 = (double)LevelTools::timeForPos(at, m_speedObjects, sp, 0, 0, false, plat, false, false, 0);
                double a3 = (double)LevelTools::timeForPos(at, m_speedObjects, sp, 0, 0, false, plat, true, false, 0);
                eP += std::fabs(a1 - obs); eW0 += std::fabs(a2 - obs); eW1 += std::fabs(a3 - obs); n++;
                if (n <= 4)
                    log::info("[CI-MODEL] x={:.0f} observed={:.3f} | playLayer={:.3f} warpOn={:.3f} warpOff={:.3f}",
                              g_xt[i].x, obs, a1, a2, a3);
            }
            if (n) log::info("[CI-MODEL] mean |error| over {} pts: playLayer={:.3f}s warpOn={:.3f}s warpOff={:.3f}s",
                             n, eP / n, eW0 / n, eW1 / n);
        }

        // The macro's clock and the level's clock have to tick at the same RATE. An error in the
        // macro's frame rate is invisible at the level start and grows with time - which is why a
        // deep StartPos looks broken while playing from the beginning looks fine. A ratio that is
        // not 1 to within a few parts per thousand means no offset can ever align this macro.
        if (dbgLog() && !g_actions.empty() && g_segsOk && m_endXPosition > 1.f) {
            double lvl = integrateToX(this, m_endXPosition, nullptr);
            double mac = g_actions.back().releaseTime;
            log::info("[CI-RATE] macro={:.3f}s level={:.3f}s ratio={:.5f} fps={:.2f} clicks={} endX={:.0f}",
                      mac, lvl, lvl > 0.0 ? mac / lvl : -1.0, g_fps, (int)g_actions.size(),
                      m_endXPosition);
        }

        // resetLevel runs during setup, before the macro for THIS level has been loaded or cleared,
        // so the latch it set was based on the previous level's actions. Re-evaluate now that the
        // guide state is settled, otherwise entering a macro-less level straight after a macro'd one
        // would keep the latch and block a run that should count.
        m_fields->safeTripped = safeModeOn();
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        if (g_ghBuilding) return;   // the hidden level is mid-construction; none of this is for it

        // A path being worked out: write down where the icon is, and stop when it has gone far
        // enough, or died, or the macro has run out and it is going nowhere.
        // RECORDING. Nothing is driven here - it only watches whatever is flying the level.
        {
            const bool want = Mod::get()->getSettingValue<bool>("record-path");
            // Not once it is done, and not if the icon is already at the end - a recording that
            // starts there has nowhere to go and describes nothing.
            const bool atEnd = m_player1
                && (double)m_player1->getPositionX() >= (double)m_endXPosition - 1200.0;
            if (want && !g_recOnPath && !g_rpSaved && !atEnd && this->isGameplayActive()) {
                g_recOnPath = true;
                recPathReset();
                log::info("[CI-REC] recording this run as the path for level {} - {} macro inputs "
                          "loaded, level is {:.0f} long. Nothing is being driven; whatever plays "
                          "this level is what gets written down.",
                          m_level ? (int)m_level->m_levelID : 0, (int)g_actions.size(),
                          (double)m_endXPosition);
            }
            if (!want && g_recOnPath) { g_recOnPath = false; recPathFinish(this, "turned off"); }
            if (g_recOnPath && this->isGameplayActive()) {
                recPathStep(this);
                if (m_player1 && (double)m_player1->getPositionX() >= (double)m_endXPosition - 600.0) {
                    g_recOnPath = false;
                    recPathFinish(this, "reached the end");
                }
            }
        }

        // Start it here, on a real frame, once the level is actually playing.
        if (g_ffWanted && !g_ffOn && this->isGameplayActive() && m_player1) {
            g_ffWanted = false;
            ffBegin(this);
        }

        if (g_ffOn) {
            ffRecord(this);
            const double x = m_player1 ? (double)m_player1->getPositionX() : 0.0;
            // A block of travel past the spawn: everything before that is the level arranging
            // itself, and a death there is not the macro failing.
            // ARM ON THE GAME'S OWN FRAME COUNTER.
            //
            // Counting calls to this function was worthless: it runs per physics substep, not per
            // rendered frame, so at 12x twenty of them pass in about seven milliseconds and the
            // guard armed instantly - still counting the spawn's deaths. Exactly twelve of them,
            // every run, on every macro, which is not something macros have in common.
            //
            // m_currentProgress is the level's own physics frame. One second of level time is a
            // whole second of flying, far past any spawning, and cheap to be sure of.
            const long long pf = (long long)m_gameState.m_currentProgress;
            if (!g_ffArmed && pf > 240 && x > 200.0) {
                g_ffArmed = true;
                log::info("[CI-FF] watching for deaths from frame {} (x={:.0f}) onwards", pf, x);
            }
            const bool doneInputs = g_ffAi >= g_ffEv.size() && g_ffAi2 >= g_ffEv2.size();
            if (x >= g_ffStopX || g_ffDeaths > 0
                || (doneInputs && nowSeconds() - g_ffBeganAt > 60.0))
                ffFinish(this);
            return;                     // nothing else runs while the level is flying itself
        }
        // A ghost run drives this same layer thousands of times in a single frame. None of the
        // mod's per-frame work belongs to it: it would record the ghost as wave stretches the
        // player flew, re-grade clicks nobody pressed, and re-solve the route mid-replay.
        if (g_ghost) return;

        // SPEND THE TIME WHERE IT IS NEEDED: STAY AHEAD OF THE PLAYER.
        //
        // A fixed slice is the wrong shape. The replay costs about 0.58 ms per physics step, so
        // five milliseconds a frame advances it roughly twice as fast as the level is played - it
        // does pull ahead, but only by about a second at the start, which is exactly where a guide
        // has to be furthest ahead. And once it is thirty seconds down the level, more speed buys
        // nothing at all.
        //
        // So it is paid by how much road is left in front of the player. Close behind, it gets most
        // of a frame and catches up in moments; comfortably ahead, it idles at a trickle and costs
        // nothing. A first load is never instant - a level has to be flown before it can be drawn -
        // but the part in front of the icon is there within a second and stays there.
        if (g_ghLayer) {
            const double meX = m_player1 ? (double)m_player1->getPositionX() : 0.0;
            const double ahead = g_ghLayer->m_player1
                               ? (double)g_ghLayer->m_player1->getPositionX() - meX : 0.0;
            const double budget = ahead < 1500.0  ? 12.0    // barely ahead, or behind: push
                                : ahead < 6000.0  ? 6.0     // a few seconds of road: keep pace
                                                  : 2.0;    // far ahead: idle
            // The background pass gets a trickle: it is not needed for anything on screen right
            // now, and the quick path is already covering the player.
            // The background pass used a flat eight milliseconds - half a frame at sixty - every
            // frame for its whole duration, ignoring the ladder computed above and taking it
            // whether or not anything on screen needed it. Phase 1 self-throttles because its ghost
            // outruns the player and the ladder drops it to two; phase 2 never throttled at all.
            // It gets the same ladder now, with a lower ceiling, because nothing it produces is
            // needed this frame.
            const double use = g_ghPhase == 2 ? std::min(budget, 4.0) : budget;
            if (ghostLayerSlice(use)) ghostLayerFinish(this);
        }

        // RETIRED. This started the detached hidden replay - the one whose own comment records
        // 35,922 and 46,300 deaths, all of them the flight icon - and sliced it into gameplay
        // frames. It has never produced a flight, and while it runs it fights the fast-forward for
        // g_ghostPath. The fast-forward at level load is the only producer now.
        // setStartPosObject is inlined on Windows and cannot be hooked, so no hook fires at the
        // moment the spawn changes. This compare is the guarantee that a switch is never missed,
        // whichever code path or other mod moved it.
        syncStartOffset(this);
        trackStartOffset(this);   // must precede every read of g_startOffset below
        freeCamRestore(this);
        auto overlay = m_fields->overlay;
        if (!overlay || !m_player1) return;

        // Pulse is drawn into its own node parented alongside the icon, one z below it, so it
        // passes behind the character instead of sitting on top of the thing you are watching.
        // Taking the z from the player rather than a guessed constant keeps it right whatever
        // GD parents the icon to.
        if (auto* pp = m_player1->getParent()) {
            if (!m_fields->pulse || m_fields->pulse->getParent() != pp) {
                if (m_fields->pulse) m_fields->pulse->removeFromParent();   // icon was re-parented
                auto pn = CCDrawNode::create();
                applyAA(pn);
                pn->setID("ci-pulse"_spr);
                pp->addChild(pn, m_player1->getZOrder() - 1);
                m_fields->pulse = pn;
            }
        }
        if (m_fields->pulse) m_fields->pulse->clear();

        if (!g_wxOk && !g_actions.empty() && m_fields->wxTry < 120) {
            m_fields->wxTry++;   // bounded: an unreadable level must not retry every frame forever
            buildActionPositions(this);
        }

        auto mod = Mod::get();
        g_recOn = licOK() && g_recActive;
        {
            double rt = m_gameState.m_levelTime;
            double adv = rt - m_fields->recPrevT;
            if (adv < 0.0 || adv > std::max(0.25, 8.0 * std::max(m_fields->advAvg, 0.02)))
                recRewind(rt + g_startOffset);
            m_fields->recPrevT = rt;
            if (m_fields->recBadge) {
                m_fields->recBadge->setVisible(g_recOn);
                if (g_recOn) {
                    m_fields->recBlink += dt;
                    float pulse = 0.55f + 0.45f * sinf(m_fields->recBlink * 5.2f);
                    if (m_fields->recDot) {
                        m_fields->recDot->clear();
                        m_fields->recDot->drawDot(ccp(0.f, 0.f), 6.f,
                            ccColor4F{ 1.f, 0.22f, 0.22f, pulse });
                    }
                    if (m_fields->recStats)
                        m_fields->recStats->setString(
                            fmt::format("{} clicks   {:.1f}s", recClicks(), recLength()).c_str());
                }
            }
        }

        // A stretch of the level with no help, so you can practise a part properly and still be
        // guided through the rest. Two people asked for opposite halves of the same thing - one
        // wanted the opening bare, one wanted a section in the middle bare - so this is a range
        // to hide rather than a range to show.
        bool hiddenHere = false;
        {
            double hFrom = mod->getSettingValue<double>("hide-from");
            double hTo   = mod->getSettingValue<double>("hide-to");
            // Percent is meaningless in a platformer, where the level has no single end.
            if (hTo > hFrom && !m_level->isPlatformer()) {
                float pct = this->getCurrentPercent();
                hiddenHere = pct >= (float)hFrom && pct <= (float)hTo;
            }
        }

        if (!licOK() || !licGate() || !mod->getSettingValue<bool>("enabled") || g_actions.empty()
            || g_startAlign == StartAlign::Failed || hiddenHere) {
            overlay->clear();
            for (auto* l : m_fields->hud) if (l) l->setVisible(false);
            return;
        }

        g_volume = (float)mod->getSettingValue<double>("volume");
        g_soundPack = mod->getSettingValue<std::string>("sound-pack");
        bool feedbackOn = mod->getSettingValue<bool>("feedback");
        bool statsOn    = mod->getSettingValue<bool>("stats-hud");

        bool doTicks = mod->getSettingValue<bool>("ticks");
        bool relSound = mod->getSettingValue<bool>("release-sound");
        bool tightPitch = mod->getSettingValue<bool>("tight-pitch");
        bool doGo = mod->getSettingValue<bool>("ring");
        std::string modeS = mod->getSettingValue<std::string>("indicator-mode");
        hwModeSync(modeS);   // writes only when it actually changes
        int mode = modeS == "ring" ? 1 : modeS == "converge" ? 2 : modeS == "pulse" ? 3
                                                                : modeS == "highway" ? 4 : 0;
        bool doLines = mod->getSettingValue<bool>("lines");
        bool linesClassicOnly = mod->getSettingValue<bool>("lines-classic-only");
        bool doNotch = mod->getSettingValue<bool>("notches");
        bool reduceFlash = mod->getSettingValue<bool>("reduce-flashing");
        double kLookahead = mod->getSettingValue<double>("lookahead");
        double holdThresh = mod->getSettingValue<double>("hold-threshold");
        ccColor3B cc = mod->getSettingValue<ccColor3B>("line-color");
        bool gmGuide = gamemodeGuideOn(m_player1); // this gamemode's guide turned on?
        // x is not a function of time in platformer - you accelerate, stop and turn around - so
        // nothing that projects a position along x can be right there. The time-domain styles
        // still can, since they draw at the player and count down in seconds.
        if (m_isPlatformer) { doLines = false; doNotch = false; if (mode == 0) mode = 1; }

        double raw = m_gameState.m_levelTime + g_startOffset;
        // Discount a deliberate clock correction, which is not elapsed time. spawnVoteFix can move
        // g_startOffset by six frames or more in one go, and without this the difference below
        // reads that as the level having jumped: a positive step spikes the g_cal.h rate estimate
        // for a couple of dozen frames, mis-scaling audio lead, hold length and the graded error,
        // and a negative one goes below zero and trips the respawn branch, which bumps g_attempt
        // and re-opens every click already pressed this run for grading a second time.
        double advance = raw - m_fields->prevRaw - g_offsetStep;
        g_offsetStep = 0.0;
        // How much level time a frame is worth depends on the playback rate. Under a 20x speedhack
        // an ordinary frame advances 0.33s, and the old fixed 0.25s cap read that as a respawn on
        // EVERY frame - continuously resetting the audio cursor, the velocity estimate and the
        // calibration freeze. That is why the guide fell apart at speed while normal play was
        // perfect. Compare against what a frame has actually been worth lately instead.
        double advExp = m_fields->advAvg > 1e-6 ? m_fields->advAvg : 0.02;
        bool jumped = (advance < 0.0) || (advance > std::max(0.25, 8.0 * advExp));
        if (!jumped && advance > 1e-6)
            m_fields->advAvg += 0.05 * (advance - m_fields->advAvg);
        if (jumped) {   // death / respawn / checkpoint
            m_fields->nextTick = 0;
            // prevX was left at the pre-death position, so the first frame back produced a huge
            // bogus dx that only the outlier cap was catching.
            m_fields->vxInit = false; g_cal.lastWall = -1.0;
            m_fields->prevX = m_player1->getPositionX(); m_fields->tabBad = 0.f;
            calibFreeze(m_player1);
        }

        double wall = nowSeconds();
        if (!jumped && advance > 0.0 && g_cal.lastWall > 0.0) {
            double dW = wall - g_cal.lastWall;
            if (dW > 1e-5 && dW < 0.1) {
                // Clamped at 4 before, so anything above 4x saturated and everything scaled by h
                // - audio lead, notch offsets, grading normalisation - was wrong by the excess.
                float hi = clmp((float)(advance / dW), 0.05f, 64.0f);
                g_cal.h += 0.1f * (hi - g_cal.h);
            }
        }
        g_cal.lastWall = wall;
        if (g_cal.hFrames < 10000) g_cal.hFrames++;
        if (!std::isfinite(g_cal.h) || g_cal.h <= 0.f) { g_cal.h = 1.f; g_cal.hFrames = 0; }

        // One fixed offset, taken from the setting. The version that learned it from your own
        // presses moved the cue between attempts, and a mark that shifts is worse to play against
        // than one that sits still even when the still one is slightly out. Players said so and
        // they were right - the mark is meant to be a place.
        float lead = (float)mod->getSettingValue<double>("lead");
        g_cal.leadShown = lead;

        if (jumped) {
            while (m_fields->nextTick < g_events.size() && g_events[m_fields->nextTick].time - lead <= raw) m_fields->nextTick++;
        }
        m_fields->prevRaw = raw;

        ensureSpeedTable(this);   // before the rate measurement below, which reads the table

        float x = m_player1->getPositionX(), py = m_player1->getPositionY();
        float dx = x - m_fields->prevX; float adx = dx < 0.f ? -dx : dx;
        // advance, not dt: it is level time by construction, so this is units per LEVEL second -
        // the unit every horizon below is in - at any framerate and under any speedhack, and the
        // outlier cap scales with it for free. The gain is a time constant for the same reason:
        // a flat 0.12 per frame settled four times slower on a 60Hz display than on 240Hz.
        if (!jumped && advance > 1e-6 && adx < 700.f * (float)advance + 60.f) {
            float inst = dx / (float)advance;
            if (!m_fields->vxInit) { m_fields->vxAvg = inst; m_fields->vxInit = true; }
            else m_fields->vxAvg += clmp(1.f - expf(-(float)advance / 0.08f), 0.f, 1.f)
                                  * (inst - m_fields->vxAvg);
        }
        m_fields->prevX = x; float vx = m_fields->vxAvg;

        bool tabOK = g_segsOk && !m_isPlatformer && !m_player1->m_isGoingLeft;
        size_t seg0 = tabOK ? segAtX((double)x) : 0;
        // Dash orbs, slopes and teleport triggers move the player at a speed no portal explains,
        // and the table would be confidently wrong about those forever where the measurement at
        // least converges. So the measurement stays on as a referee. The threshold is in level
        // seconds and decays, so the few frames it needs to catch up after a portal cannot trip
        // it - only a sustained disagreement can.
        if (tabOK && m_fields->vxInit && g_segs[seg0].v > 1.0) {
            double r = (double)vx / g_segs[seg0].v;
            m_fields->tabBad = clmp(m_fields->tabBad
                + ((r < 0.75 || r > 1.34) ? (float)advance : -(float)advance), 0.f, 2.f);
        }
        if (m_fields->tabBad >= 0.5f) tabOK = false;
        // The speed table is the whole speed-portal fix, and three separate conditions can silently
        // switch it off. Log every transition with the reason rather than guess which one fired.
        if (dbgLog() && tabOK != m_fields->tabWas) {
            m_fields->tabWas = tabOK;
            log::info("[CI-TAB] table {} - segsOk={} platformer={} goingLeft={} tabBad={:.2f} "
                      "vx={:.0f} segV={:.0f} segs={} x={:.0f}",
                      tabOK ? "ON" : "OFF", g_segsOk, m_isPlatformer,
                      (bool)m_player1->m_isGoingLeft, m_fields->tabBad, vx,
                      g_segs.empty() ? -1.0 : g_segs[seg0].v, (int)g_segs.size(), x);
        }
        auto projX = [&](double ahead) -> float {
            return tabOK ? (float)xAfterDt((double)x, ahead, seg0) : x + vx * (float)ahead;
        };
        // The exact frame a speed portal is crossed, with both projections side by side. If the
        // guide jumps here, the two numbers say by how much and which one is moving.
        if (dbgLog() && tabOK && seg0 != m_fields->segWas) {
            log::info("[CI-SEG] cross x={:.0f} segV={:.0f} vx={:.0f} | proj(1.0s) table={:.0f} ema={:.0f}",
                      x, g_segs[seg0].v, vx, xAfterDt((double)x, 1.0, seg0), x + vx * 1.0f);
        }
        m_fields->segWas = seg0;

        // No slope sampling here any more. The wave's slope is an exact engine constant - see
        // waveSlope() - and a measurement of it can only be noise, biased low: one display frame
        // holds four physics steps, so any frame containing a direction change returns |2k-n|/n of
        // the truth, and last-write-wins latched whichever bad frame came last.

        // Only a run that began at the level start is on real time; a spawned run's clock is the
        // canonical value we are trying to correct, so recording from it would bake the error in.
        // A 0% run is real time by definition. A spawned run is too, but only once its clock has
        // been corrected against already-measured ground truth - so the map grows forward from the
        // regions it already covers instead of writing the model's error in as if it were observed.
        bool realClock = !g_startKeyObj || (g_offsetLocked && g_offsetObserved);
        if (realClock && this->isGameplayActive() && x >= g_xtNextX && raw > 0.0) {
            if (g_xt.empty() || x > g_xt.back().x) {
                g_xt.push_back({ x, (float)raw });
                g_xtDirty = true;
            }
            g_xtNextX = std::floor(x / 200.f) * 200.f + 200.f;
        }

        // The one number that discriminates. err is GD's own canonical time for where the player
        // actually is, minus the clock the cue selection runs on - taken from the SAME map that
        // placed the cues, so unlike the old probe it cannot be fooled by our speed table carrying
        // the same StartPos override the player has. off is what the tracker had to absorb.
        if (dbgLog() && this->isGameplayActive() && raw >= m_fields->syncLog) {
            m_fields->syncLog = raw + 0.5;
            double gdT = canonTimeAtX(this, x);
            log::info("[CI-ERR] t={:.3f} x={:.0f} gdT={:.3f} err={:+.3f} off={:+.3f} dxSpawn={:.0f} tab={:+.3f} startpos={}",
                      raw, x, gdT, gdT - raw, g_startOffset, x - g_startKeyX,
                      integrateToX(this, x, nullptr) - raw, g_startKeyObj ? 1 : 0);
        }

        // Classic draws every mark by projecting a TIME into a world X, so if the cues sit in the
        // wrong place that projection is wrong - and this measures it directly instead of arguing
        // about it. Predict where the player will be in one second, then check a second later.
        // err near 0 means projection is sound and the fault is elsewhere entirely.
        if (dbgLog() && m_fields->predT > 0.0 && raw >= m_fields->predT) {
            log::info("[CI-PRED] t={:.2f} predicted={:.0f} actual={:.0f} err={:+.0f} segV={:.0f} startpos={}",
                      raw, m_fields->predX, x, x - m_fields->predX,
                      g_segs.empty() ? -1.0 : g_segs[seg0].v, g_startKeyObj ? 1 : 0);
            m_fields->predT = 0.0;
        }
        if (m_fields->predT <= 0.0 && this->isGameplayActive()) {
            m_fields->predT = raw + 1.0;
            m_fields->predX = projX(1.0);
        }

        bool playAudio = doTicks && gmGuide && licOK() && this->isGameplayActive();
        {
            int played = 0;
            while (m_fields->nextTick < g_events.size() && g_events[m_fields->nextTick].time - lead <= raw) {
                auto const& ev = g_events[m_fields->nextTick];
                if (playAudio && played < 8 && ev.idx >= 0 && ev.idx < (int)g_actions.size()) {
                    auto const& a = g_actions[ev.idx];
                    if (ev.type == 1) {
                        double holdReal = (a.releaseTime - a.pressTime) / std::max(g_cal.h, 0.05f);
                        if (relSound && holdReal >= 0.10) { playRelease(); played++; }
                    } else {
                        double gapReal = (ev.time - m_fields->lastPlayPress) / std::max(g_cal.h, 0.05f);
                        if (gapReal >= 0.05) {
                            double Wr = (a.winEnd - a.winStart) / std::max(g_cal.h, 0.05f);
                            int tier = !tightPitch ? 0 : (Wr > 0.06 ? 0 : (Wr > 0.025 ? 1 : 2));
                            playPress(tier);
                            m_fields->lastPlayPress = ev.time; played++;
                        }
                    }
                }
                m_fields->nextTick++;
            }
        }

        double t = raw + lead;

        const float cr = cc.r / 255.f, cg = cc.g / 255.f, cb = cc.b / 255.f;
        const float op = (float)Mod::get()->getSettingValue<double>("indicator-opacity");
        g_cueContrast = (float)Mod::get()->getSettingValue<double>("cue-contrast");
        g_cueOpacity = op;
        auto fade = [op](ccColor4F c) { c.a *= op; return c; };
        auto fadeDark = [op](ccColor4F c) { c.a *= op * op * op; return c; };
        const ccColor4F sweetC = fade({ 0.92f, 1.f, 0.96f, 0.95f });
        // The hold band was 0.12, eleven times fainter than the press line, and in the non-classic
        // modes another 0.72 on top of that put it at 0.086 - effectively invisible over a level, at
        // any opacity. It is a primary indicator, so it is drawn like one.
        // Full alpha at the top of the opacity slider, so "1" means solid rather than "as solid
        // as the hold bar was ever allowed to be". It was capped at 0.30 idle and 0.55 armed, so
        // turning opacity all the way up still left it washed out over a bright level. The armed
        // state is signalled by the colour change, which reads fine at full alpha.
        const ccColor4F holdC = fade({ cr, cg, cb, 1.0f });
        const ccColor4F holdArm = fade({ 0.49f, 1.f, 0.75f, 1.0f });
        const ccColor4F strikeIdle = fade({ 1.f, 1.f, 1.f, 0.55f });
        const ccColor4F strikeArm = fade({ 0.17f, 0.85f, 0.41f, 0.95f });

        CCSize win = CCDirector::sharedDirector()->getWinSize();
        const float bot = -20.f, top = win.height + 20.f;
        auto sx = [&](float worldX) -> float {
            return overlay->convertToNodeSpace(m_objectLayer->convertToWorldSpace(ccp(worldX, py))).x;
        };

        overlay->clear();
        float strikeX = sx(x);

        // ---- wave route --------------------------------------------------------------------
        //
        // Drawn first, so every other cue paints on top of it.
        //
        // Nothing is solved here. The route is a fixed list of world coordinates worked out once,
        // in the WAVE ROUTE block above, so all that happens per frame is a clip and a projection:
        // the whole path is on screen from the first frame of the attempt instead of growing out of
        // the player, and it does not move while you fly along it. The part already flown is drawn
        // too, faintly - a route you can still see behind you is how you tell whether you came in
        // above or below it.
        {
            bool wantTraj = gmGuide && mod->getSettingValue<bool>("trajectory")
                         && !m_isPlatformer && g_wxOk && !g_actions.empty();
            if (wantTraj) {
                // Scan first: on the very frame the run enters its first wave section there is no
                // section list yet to attach the observation to, and that observation is the best
                // one there is - it is taken at the mouth, before any drift.
                rtEnsure(this);
                bool isWave = m_player1->m_isDart;
                bool entered = isWave && !m_fields->wasWave;
                // A respawn inside a section is evidence too - you got there alive, so you are on
                // the real route. rtNoteEntry ignores it unless the route says otherwise by 45
                // units or more, so this cannot make the line drift between attempts.
                // Written down, every frame, whatever the portals think. This is the only
                // statement about where the wave is that cannot be wrong.
                if (this->isGameplayActive()) {
                    waveNote(x, py, isWave, m_player1->m_isUpsideDown,
                             m_player1->m_vehicleSize < 1.f);
                    // The same sample, used to hold the replay to account.
                    if (isWave) ghostCheck(x, py);
                }

                bool respawned = isWave && m_fields->trajAtt != g_attempt;
                m_fields->trajAtt = g_attempt;
                m_fields->wasWave = isWave;
                if ((entered || respawned) && this->isGameplayActive()) {
                    rtNoteEntry(this, x, py);
                    rtEnsure(this);   // a no-op unless the observation was actually taken
                }

                // Does the route actually describe THIS run? Measured, not assumed: the icon is
                // on the route by definition when the macro matches the level, so a persistent gap
                // between the two means it does not - a macro recorded on another version, another
                // route, or simply too sparse to be this section. Nine Circles with a 168-click
                // macro puts the route 1,300 units above the player and draws it just as
                // confidently as a correct one, and a confidently wrong route is worse than none.
                //
                // Smoothed so a single frame mid-portal cannot blank it, and it recovers by itself
                // the moment the two agree again.
                if (g_rtOk && this->isGameplayActive()) {
                    double ry = rtRouteYAt((double)x);
                    if (std::isfinite(ry)) {
                        double off = std::fabs(ry - (double)py);
                        m_fields->rtMiss = m_fields->rtMiss * 0.92f + (float)off * 0.08f;
                    }
                    // What the clearance actually IS, measured on the run rather than derived. A
                    // wave held flat for a few frames is sliding, so the gap between the icon and
                    // the surface underneath it is exactly the number the route should be resting
                    // at - in the mod's own coordinates, against the mod's own collected geometry.
                    static float restPrev = -1e9f; static int restFlat = 0, restLogs = 0;
                    if (dbgLog() && m_player1 && m_player1->m_isDart && !g_rtObst.empty()) {
                        if (std::fabs(py - restPrev) < 0.35f) restFlat++; else restFlat = 0;
                        restPrev = py;
                        if (restFlat == 4 && restLogs < 10) {
                            double lo = -1e18, hi = 1e18;
                            for (auto const& ob : g_rtObst) {
                                if ((double)ob.x0 > (double)x) break;
                                if ((double)ob.x1 < (double)x) continue;
                                if ((double)ob.y1 <= (double)py && (double)ob.y1 > lo) lo = (double)ob.y1;
                                if ((double)ob.y0 >= (double)py && (double)ob.y0 < hi) hi = (double)ob.y0;
                            }
                            if ((lo > -1e17 && (double)py - lo < 60.0)
                                || (hi < 1e17 && hi - (double)py < 60.0)) {
                                restLogs++;
                                log::info("[CI-REST] sliding at x={:.0f} y={:.1f} | surface below "
                                          "{:.1f} gap {:.1f} | surface above {:.1f} gap {:.1f} | "
                                          "mini={} using={:.2f}",
                                          x, py, lo, lo > -1e17 ? (double)py - lo : -1.0,
                                          hi, hi < 1e17 ? hi - (double)py : -1.0,
                                          m_player1->m_vehicleSize < 1.f ? 1 : 0,
                                          m_player1->m_vehicleSize < 1.f ? g_rtHalfBig * 0.6
                                                                         : g_rtHalfBig);
                            }
                        }
                    }
                }
                // Two corridors' worth. Below this the route is worth looking at even when it is
                // not perfectly centred; above it, it is describing something else.
                //
                // None of that applies to a route the game itself flew. That one is the macro's own
                // run: the player being far from it does not cast doubt on the route, it says the
                // player is off it - which is the exact moment a guide is worth having. Hiding it
                // then is the feature deciding to be useless precisely when it is needed, and it is
                // why the line vanished after going off the macro's line and did not come back.
                const bool rtTrust = g_rtFromGhost || m_fields->rtMiss < 130.f;
                // One corridor. At this distance the run is demonstrably ON the route, which is
                // better evidence for the height than the way it was arrived at - so a section
                // marked "inferred" at solve time stops being drawn as a guess.
                //
                // It has to be evidence rather than provenance, because provenance stopped meaning
                // anything here: the entry observation is taken at the spawn, and on a level that
                // opens in wave the route does not start until the macro's first input, so the
                // observation is correctly discarded and EVERY section is marked inferred forever.
                // The whole path then draws at a third of its colour whatever the opacity setting
                // says, which is not a warning, it is just faint.
                const bool rtSure = g_rtFromGhost || m_fields->rtMiss < 60.f;

                if (g_rtOk && rtTrust) {
                    const double half = (double)win.width * 0.5 + 140.0;
                    const double xL = (double)x - half, xR = (double)x + half;
                    const ccColor4F cHold = fade(ccColor4F{ cr, cg, cb, 0.95f });
                    const ccColor4F cFall = fade(ccColor4F{ cr, cg, cb, 0.55f });
                    const ccColor4F cPast = fade(ccColor4F{ cr, cg, cb, 0.20f });
                    const ccColor4F cNode = fade(ccColor4F{ 1.f, 1.f, 1.f, 0.75f });
                    // A step down from certain, not a whisper. These used to sit at a third of the
                    // solid line, which at any opacity setting reads as "the mod is broken" rather
                    // than "this part is inferred".
                    const ccColor4F cGuessH = fade(ccColor4F{ cr, cg, cb, 0.68f });
                    const ccColor4F cGuessF = fade(ccColor4F{ cr, cg, cb, 0.40f });
                    int shown = 0;
                    // ONE LINE, or a chain of pieces.
                    //
                    // The route used to be drawn a segment at a time, each with its own width and
                    // brightness for held against released. Two things came of that. The obvious
                    // one is that the path visibly changed thickness at every single click, so a
                    // continuous flight read as a row of parts butted end to end. The subtler one
                    // is that drawSegment rounds its ends, so consecutive segments overlap by half
                    // a cap at every corner and that overlap composites twice - and since the black
                    // rim is two to four times wider than the line, a wave route came out beaded
                    // from one end to the other.
                    //
                    // Nothing is lost by dropping the thickness, because on a wave it was saying
                    // what the shape already says: up IS held and down IS released, there is no
                    // other way for it to move. The corner dots still mark every input. What is
                    // gained is that the whole visible stretch becomes one run of one style, and a
                    // run can be stroked as a single continuous line with mitred corners and no
                    // overlap anywhere in it.
                    const bool solid = Mod::get()->getSettingValue<bool>("route-solid");
                    auto drawPoly = [&](std::vector<CCPoint> const& P,
                                        std::vector<uint8_t> const& H) {
                        if (P.size() < 2) return;
                        size_t lo = 0, hi = P.size() - 1;
                        while (hi - lo > 1) {
                            size_t m = (lo + hi) / 2;
                            if ((double)P[m].x <= xL) lo = m; else hi = m;
                        }
                        // NO SHAKE COMPENSATION.
                        //
                        // Subtracting applyShake's offset was meant to hold the line still through
                        // a screen shake. It does the opposite: the projection through
                        // m_objectLayer does not already carry that offset, so taking it out does
                        // not cancel a movement, it ADDS one - the line jitters while the level is
                        // steady. Left alone, the route rides the same transform as the geometry it
                        // describes, which is also the only way it stays pointing at the right
                        // blocks.

                        std::vector<CCPoint> run, dots;
                        int runKey = -1; float runW = 0.f; ccColor4F runC{ 0.f, 0.f, 0.f, 0.f };
                        auto flush = [&]() {
                            if (run.size() >= 2) {
                                drawPolyOL(overlay, run.data(), (int)run.size(), runW, runC);
                                shown += (int)run.size() - 1;
                            }
                            run.clear();
                        };
                        for (size_t i = lo; i + 1 < P.size(); i++) {
                            if ((double)P[i].x > xR) break;
                            const uint8_t f = i < H.size() ? H[i] : 0;
                            if (f == 2) { flush(); continue; }   // the hole between two sections
                            const bool held = (f & 1) != 0, guess = (f & 4) != 0 && !rtSure;
                            const bool past = (double)P[i + 1].x < (double)x;
                            // Behind the player and inferred still change the line, because those
                            // say something the shape cannot. Held does not, any more.
                            const int key = (past ? 1 : 0) | (guess ? 2 : 0)
                                          | ((solid || past) ? 0 : (held ? 4 : 0));
                            const float w = solid ? (past ? 1.9f : (guess ? 2.2f : 2.6f))
                                                  : (guess ? 1.8f
                                                           : (past ? 1.5f : (held ? 3.0f : 2.1f)));
                            const ccColor4F c =
                                guess ? ((solid || held) ? cGuessH : cGuessF)
                                      : (past ? cPast : ((solid || held) ? cHold : cFall));
                            if (key != runKey) { flush(); runKey = key; runW = w; runC = c; }
                            // HELD STILL THROUGH A SHAKE.
                            //
                            // The projection goes through m_objectLayer, so anything moving that
                            // layer moves the route with it - and a screen shake moves it hard. But
                            // a shake is a VISUAL effect: the icon's real coordinates do not change
                            // by a single unit while it happens, so a guide that jumps around with
                            // it is showing motion that is not occurring. The route is a statement
                            // about where to be in the level, and that does not shudder.
                            //
                            // applyShake reports the offset the game is currently displacing the
                            // view by; taking it back out leaves the line where the level really
                            // is. When nothing is shaking it is zero and this costs nothing.
                            if (run.empty())
                                run.push_back(overlay->convertToNodeSpace(
                                    m_objectLayer->convertToWorldSpace(P[i])));
                            run.push_back(overlay->convertToNodeSpace(
                                m_objectLayer->convertToWorldSpace(P[i + 1])));
                            // Every corner is an input. Marking the ones still ahead makes the route
                            // readable as a sequence of clicks rather than as a shape - and with the
                            // width gone these are now the only thing saying where the presses are,
                            // so they are collected and drawn ON TOP rather than being painted over
                            // by the next stretch of line.
                            if (!past && !guess && i + 1 < H.size() && H[i + 1] != f
                                && H[i + 1] != 2)
                                dots.push_back(run.back());
                        }
                        flush();
                        for (auto const& d : dots) overlay->drawDot(d, 2.7f, cNode);
                    };
                    drawPoly(g_rtPts, g_rtHold);
                    // The second icon. Same weight as the first - in a dual neither of them is the
                    // one you are allowed to get wrong.
                    drawPoly(g_rtPts2, g_rtHold2);
                    if (dbgLog() && raw > m_fields->drawLog) {
                        m_fields->drawLog = raw + 1.0;
                        double ry = rtRouteYAt((double)x);
                        log::info("[CI-ROUTE] verts={} shown={} secs={} x={:.0f} py={:.0f} "
                                  "routeY={:.0f} off={:+.0f} p2macro={}",
                                  (int)g_rtPts.size(), shown, (int)g_rtSecs.size(), x, py,
                                  ry, std::isfinite(ry) ? (double)py - ry : 0.0,
                                  pathPlayerIsP2() ? 1 : 0);
                    }
                }
            }
        }

        // How far the drawn route is from the icon, sampled along the run. This is the number
        // that separates "anchored at the wrong height" (a constant offset) from "the shape is
        // drifting" (an offset that grows with distance) - and they are different bugs.
        {
            // Per attempt, not per process: a static high-water mark meant a long level silenced
            // every level played after it in the same session.
            static double rtDivNext = -1e9;
            static int rtDivAtt = -1;
            if (rtDivAtt != g_attempt) { rtDivAtt = g_attempt; rtDivNext = -1e9; }
            if ((double)x > rtDivNext + 240.0) {
                rtDivNext = (double)x;
                double ry = rtRouteYAt((double)x);
                if (std::isfinite(ry))
                    log::info("[CI-DIV] x={:.0f} player={:.0f} route={:.0f} off={:+.0f} mini={} flip={}",
                              x, py, ry, ry - (double)py,
                              m_player1->m_vehicleSize < 1.f ? 1 : 0,
                              m_player1->m_isUpsideDown ? 1 : 0);
                // The mirror rests on one claim: entering a dual, the second icon starts on the
                // first, and the opposite gravity is the whole of what separates them - so the two
                // stay symmetric about that entry height forever after. That is a claim about GD,
                // not about this code, so it gets measured rather than trusted. If the real pair is
                // not symmetric about the axis the mirror used, this line says so outright and by
                // how much, and no one has to read it off a screen.
                // Player 2 exists and reports itself visible for the whole level, parked at
                // y=0 - so the first version of this check measured a dead icon against a mirror
                // read 20,000 units outside its own range, and printed a confident "off by +250"
                // about nothing. It travels with the run only while the dual is actually on.
                if (m_player2 && !g_rtPts2.empty()
                    && (double)m_player2->getPositionX() > 1.0
                    && std::fabs((double)m_player2->getPositionX() - (double)x) < 60.0
                    && (double)x >= (double)g_rtPts2.front().x
                    && (double)x <= (double)g_rtPts2.back().x) {
                    const double p2y = (double)m_player2->getPositionY();
                    size_t lo2 = 0, hi2 = g_rtPts2.size() - 1;
                    while (hi2 - lo2 > 1) {
                        size_t m2 = (lo2 + hi2) / 2;
                        if ((double)g_rtPts2[m2].x <= (double)x) lo2 = m2; else hi2 = m2;
                    }
                    double my = NAN;
                    if (lo2 + 1 < g_rtPts2.size()
                        && (lo2 >= g_rtHold2.size() || g_rtHold2[lo2] != 2)) {
                        double ax2 = (double)g_rtPts2[lo2].x, bx2 = (double)g_rtPts2[lo2 + 1].x;
                        double k2 = bx2 - ax2 > 1e-6 ? ((double)x - ax2) / (bx2 - ax2) : 0.0;
                        my = (double)g_rtPts2[lo2].y
                           + ((double)g_rtPts2[lo2 + 1].y - (double)g_rtPts2[lo2].y) * k2;
                    }
                    if (std::isfinite(my) && std::isfinite(ry))
                        log::info("[CI-DUALAXIS] x={:.0f} p1={:.0f} p2={:.0f} | the game mirrors "
                                  "about y={:.0f}, the drawn pair about y={:.0f} ({:+.0f}) | "
                                  "mirror off by {:+.0f}, and the player is {:+.0f} off the route",
                                  x, py, p2y, (py + p2y) * 0.5, (ry + my) * 0.5,
                                  (ry + my) * 0.5 - (py + p2y) * 0.5,
                                  my - p2y, (double)py - ry);
                }
            }
        }

        g_snapX = x; g_snapVx = vx; g_snapPy = py; g_snapT = t; g_snapOk = true;
        g_snapSeg = seg0; g_snapTab = tabOK;

        int nearestIdx = -1; bool anyArmed = false;
        // A press is matched to the nearest click and dropped past 0.20s, so a click is only
        // provably unpressed once we are further past it than that - otherwise a late press that
        // still counts would be called a miss first and then graded, saying both things at once.
        constexpr double kMissGrace = 0.26;
        int missIdx = -1;
        for (size_t i = 0; i < g_actions.size(); i++) {
            auto& a = g_actions[i];
            if (nearestIdx < 0 && !a.muted && a.sweet >= t - 1.0 / g_fps) nearestIdx = (int)i;
            if (!a.muted && t >= a.winStart && t <= a.winEnd) anyArmed = true;
            // The click nobody made. Without this the guide only ever reports on presses that
            // happened, so skipping one is completely silent and the NEXT press gets matched to
            // the NEXT click and judged perfect against it - a green verdict for a run that has
            // already left the macro's route. Only counted for clicks that were actually live
            // this attempt: a.lockAtt is set the first frame a click becomes visible, so anything
            // the player spawned past was never theirs to make.
            if (!a.muted && a.gradedAtt != g_attempt && a.missAtt != g_attempt
                && a.lockAtt == g_attempt && t > a.winEnd + kMissGrace) {
                a.missAtt = g_attempt;
                if (missIdx < 0) missIdx = (int)i;
            }
        }
        if (missIdx >= 0) {
            g_streak = 0;
            if (mod->getSettingValue<bool>("feedback") && gmGuide) {
                while (auto* o = overlay->getChildByID("ci-miss"_spr)) o->removeFromParent();
                auto ml = CCLabelBMFont::create("MISSED", "bigFont.fnt");
                ml->setID("ci-miss"_spr);
                ml->setColor({ 255, 80, 80 });
                ml->setScale(0.5f);
                ml->setPosition(
                    overlay->convertToNodeSpace(m_objectLayer->convertToWorldSpace(ccp(x, py)))
                    + ccp(0.f, 46.f));
                overlay->addChild(ml, 21);
                ml->runAction(CCSequence::create(
                    CCSpawn::create(CCMoveBy::create(0.7f, ccp(0.f, 26.f)), CCFadeOut::create(0.7f), nullptr),
                    CCRemoveSelf::create(), nullptr));
            }
        }

        if (gmGuide && mode != 3)   // pulse has its own reticle, dont double up
            drawSegOL(overlay, ccp(strikeX, bot), ccp(strikeX, top), anyArmed ? 2.8f : 1.4f,
                anyArmed ? strikeArm : strikeIdle);

        if (doLines && gmGuide) {
            float ld = (mode == 0) ? 1.f : 0.72f;
            auto dimc = [ld](ccColor4F c) { c.a *= ld; return c; };
            bool bands = !(linesClassicOnly && mode != 0);
            bool holdBar = mod->getSettingValue<bool>("hold-bar");
            // Where GD says the run is at this instant. Every mark below is placed as a
            // DIFFERENCE against this, anchored on the player's real x: both posForTime calls
            // carry the same accumulated error, so it cancels, and what is left is the exact
            // displacement over the next second or two. Our own speed table disagrees with GD by
            // 0.17s on one level and 0.33s on another - about a hundred units - so it is not good
            // enough to place a mark with, even though it is fine for deciding a speed.
            double pNow = (double)this->posForTime((float)t).x;
            bool canonOK = pNow > 0.0;
            // What is actually on screen. dx is how far ahead of the player each mark is drawn;
            // it should be about (speed * dt). If it is not, the drawing is the bug, and if it is
            // then the marks are right and what the player is judging is something else.
            // Keyed on x, not time, so a run from 0% and a run from a StartPos can be lined up at
            // the same place in the level and the selected clicks compared directly.
            if (dbgLog() && this->isGameplayActive() && x >= m_fields->drawLog) {
                m_fields->drawLog = std::floor(x / 200.f) * 200.f + 200.f;
                std::string ln; int shown = 0;
                for (auto const& a : g_actions) {
                    if (a.muted) continue;
                    if (t >= a.winEnd && t >= a.releaseTime) continue;
                    if (a.sweet - t > kLookahead) continue;
                    if (shown++ >= 3) break;
                    ln += fmt::format(" [dt={:+.3f} wx={:.0f} dx={:+.0f}]",
                                      a.sweet - t, a.wxSweet, a.wxSweet - x);
                }
                log::info("[CI-DRAW] x={:.0f} t={:.3f} gdT={:.3f} sp={} shown={}{}",
                          x, raw, canonTimeAtX(this, x), g_startKeyObj ? 1 : 0, shown, ln);
            }
            for (auto& a : g_actions) {
                if (a.muted) continue;
                // Culling on time alone let a mark vanish while it was still in front of the
                // player: the clock passes the window end a few frames before you physically get
                // there. A mark is a place, so it stays until you have actually gone past it.
                if (t >= a.winEnd && t >= a.releaseTime
                    && (a.lockAtt != g_attempt || x >= a.lockWx)) continue;
                if (a.sweet - t > kLookahead) continue;
                a.stLead = lead; a.stH = g_cal.h; a.stashed = true; // remember what this cue was drawn with
                bool armed = (t >= a.winStart && t <= a.winEnd);
                // Worked out once, the first frame this click is visible, and then frozen for
                // the attempt. Projecting every frame meant any error in the speed model showed
                // up as the mark creeping forward and settling as it got closer - a click is a
                // place, so the mark holds still. The projection runs from the player's live
                // position over the level's speed table, so it carries no accumulated error the
                // way integrating from the level start did, and reads the portals rather than
                // estimating velocity, so a speedhack cannot move it either.
                if (a.lockAtt != g_attempt) {
                    a.lockWx = canonOK
                        ? (float)((double)x + ((double)this->posForTime((float)a.sweet).x - pNow))
                        : projX(a.sweet - t);
                    a.lockAtt = g_attempt;
                }
                size_t li = segAtX((double)a.lockWx);
                float sweetWX = a.lockWx;
                // The hold bar used to be exempt from this setting, on the reasoning that it is
                // the only hold indicator the non-Classic styles have. But the setting says the
                // lines go, so they go - a leftover bar is the one thing still drawn on the level
                // after the player asked for that to stop.
                if (bands && holdBar && (a.releaseTime - a.pressTime) >= holdThresh) {
                    float leftWX = (t >= a.sweet) ? x : sweetWX;
                    float relWX = (float)xAfterDt((double)a.lockWx, a.releaseTime - a.sweet, li);
                    if (relWX > leftWX + 0.5f) {
                        // A mirror portal puts a negative scaleX on the object layer, so from there
                        // on sx() maps increasing world x to DECREASING screen x. These two are
                        // ordered in world space, so after a mirror r landed left of l, the width
                        // test could never pass, and the hold bar and window band simply stopped
                        // being drawn for the rest of the level. Order them on screen instead.
                        float lsx = sx(leftWX), rsx = sx(relWX);
                        float l = std::min(lsx, rsx), r = std::max(lsx, rsx);
                        // No dimc here: in Ring/Converge/Pulse this is the only hold indicator there
                        // is, so dimming it was taking away the one thing that shows the hold.
                        if (r > l + 0.5f) overlay->drawSegment(ccp((l + r) * 0.5f, bot), ccp((l + r) * 0.5f, top),
                            (r - l) * 0.5f, armed ? holdArm : holdC);
                        // No separate line at the end of a hold: the bar already ends there, and a
                        // second vertical among the press lines read as another click to make.
                    }
                }
                if (bands && t < a.winEnd) {
                    // Just the press line. The window band behind it was a translucent slab per
                    // upcoming click, so several of them stacked into a wash of colour over the
                    // level and the one line that says WHERE to press got lost in it.
                    float px = sx(sweetWX);
                    drawSegOL(overlay, ccp(px, bot), ccp(px, top),
                              (mode == 0 ? (armed ? 3.2f : 2.0f) : (armed ? 2.6f : 1.6f)), dimc(sweetC));
                }
            }
        }

        if (doGo && gmGuide && mode == 4) {
            HwNote lane[24];
            int nc = 0;
            for (auto& a : g_actions) {
                if (nc >= 24) break;
                if (a.muted) continue;
                double ahead = a.sweet - t;
                double hold = a.releaseTime - a.pressTime;
                if (hold < holdThresh) hold = 0.0;
                if (ahead + hold < -0.05 || ahead > kLookahead) continue;   // keep holds until served
                a.stLead = lead; a.stH = g_cal.h; a.stashed = true;
                lane[nc].lead = (float)ahead;
                lane[nc].hold = (float)hold;
                lane[nc].armed = (t >= a.winStart && t <= a.winEnd);
                lane[nc].done = (a.gradedAtt == g_attempt);
                nc++;
            }
            // Position is a fraction of the screen so it lands in the same place on any resolution,
            // and the scale takes the lane's width and height together so it keeps its proportions.
            float hwS = (float)mod->getSettingValue<double>("hw-scale");
            float hwPX = (float)mod->getSettingValue<double>("hw-x");
            float hwPY = (float)mod->getSettingValue<double>("hw-y");
            drawHighway(overlay, ccp(win.width * hwPX, win.height * hwPY),
                        48.f * hwS, win.height * 0.52f * hwS,
                        (float)kLookahead, lane, nc, (float)raw, g_btnDown, cr, cg, cb,
                        // Its own opacity. The lane sits in the corner over the level's artwork
                        // while the guide lines sit on the gameplay itself, so the weight that
                        // works for one is rarely the weight that works for the other.
                        (float)mod->getSettingValue<double>("hw-opacity"),
                        mod->getSettingValue<bool>("hw-guides"));
        }

        if (doGo && gmGuide && nearestIdx >= 0) {
            auto& a = g_actions[nearestIdx];
            double ahead = a.sweet - t;
            a.stLead = lead; a.stH = g_cal.h; a.stashed = true;
            CCPoint pc = overlay->convertToNodeSpace(m_objectLayer->convertToWorldSpace(ccp(x, py)));

            if (mode == 0) {
                if (ahead <= 1.0 && ahead > -0.05) {
                    if (a.lockAtt != g_attempt) {
                        double pn = (double)this->posForTime((float)t).x;
                        a.lockWx = pn > 0.0
                            ? (float)((double)x + ((double)this->posForTime((float)a.sweet).x - pn))
                            : projX(a.sweet - t);
                        a.lockAtt = g_attempt;
                    }
                    CCPoint pip = overlay->convertToNodeSpace(m_objectLayer->convertToWorldSpace(ccp(a.lockWx, py)));
                    overlay->drawDot(pip, 8.2f, fadeDark(ccColor4F{ 0.f, 0.f, 0.f, 0.55f }));
                    overlay->drawDot(pip, 6.5f, fade(ccColor4F{ 0.92f, 1.f, 0.96f, 0.95f }));
                    overlay->drawDot(pip, 3.0f, fade(ccColor4F{ 0.13f, 0.9f, 0.45f, 1.f }));
                }
            } else if (mode != 4) {
                float approachT = clmp((float)kLookahead * 0.5f, 0.5f, 1.2f);
                // Queued clicks first, so the main pair always draws over them.
                if (mode == 2) {
                    float qs[8]; int qn = 0;
                    for (size_t i = (size_t)nearestIdx; i < g_actions.size() && qn < 8; i++) {
                        if (g_actions[i].muted) continue;
                        double ah = g_actions[i].sweet - t;
                        if (ah <= approachT) continue;   // the main cue has it
                        if (ah > kLookahead) break;
                        qs[qn++] = (float)ah;
                    }
                    if (qn) drawConvergeQueue(overlay, pc, bot, top, qs, qn, approachT,
                                              1.f, cr, cg, cb, op);
                }
                if (ahead <= approachT && ahead > -0.06) {
                    bool armed = (t >= a.winStart && t <= a.winEnd);
                    float frac = clmp((float)(ahead / approachT), 0.f, 1.f);
                    float ease = powf(frac, 0.6f);   // moves fastest at the press instant
                    if (mode == 3 && m_fields->pulse) {
                        // Same cue, drawn in the icon's parent space so it can sit under it.
                        CCNode* pp = m_fields->pulse->getParent();
                        CCPoint pcp = pp->convertToNodeSpace(m_objectLayer->convertToWorldSpace(ccp(x, py)));
                        float bp = pp->convertToNodeSpace(overlay->convertToWorldSpace(ccp(0.f, bot))).y;
                        float tp = pp->convertToNodeSpace(overlay->convertToWorldSpace(ccp(0.f, top))).y;
                        drawCue(m_fields->pulse, mode, pcp, bp, tp, ease, armed, 1.f, cr, cg, cb, op);
                    } else {
                        drawCue(overlay, mode, pc, bot, top, ease, armed, 1.f, cr, cg, cb, op);
                    }
                }
            }

            // No ignition flash. drawDot renders a large radius as a hard square, so what was
            // meant as a brief glow on the icon at the press moment came out as a white cube
            // sitting over the player - covering the one thing they are watching.
            if (doNotch && mode == 0 && g_cal.h >= 0.6f && ahead > 0.0 && ahead <= 0.18 * g_cal.h + 0.03) {   // notches classic only
                const float offs[3] = { 0.05f, 0.10f, 0.15f };
                for (float off : offs) {
                    // Measured back from where the click IS, so they stay pinned to it whatever
                    // the player is doing. g_cal.h converts the real-time offsets to level time.
                    float nWX = projX(off * g_cal.h);
                    CCPoint nc = overlay->convertToNodeSpace(m_objectLayer->convertToWorldSpace(ccp(nWX, py)));
                    overlay->drawSegment(ccp(nc.x, nc.y - 11.f), ccp(nc.x, nc.y + 11.f), 1.2f,
                        fade(ccColor4F{ 1.f, 1.f, 1.f, 0.35f }));
                }
            }
        }

        if (g_tracerTTL > 0) {   // press-error flash, ~18 frame fade
            g_tracerTTL--;
            if (gmGuide) {
                float a01 = g_tracerTTL / 18.f;
                float cap = reduceFlash ? 0.4f : 0.75f;
                ccColor4F tc = g_tracerGood ? ccColor4F{ 0.2f, 1.f, 0.4f, cap * a01 }
                                            : ccColor4F{ 1.f, 0.32f, 0.32f, cap * a01 };
                overlay->drawSegment(ccp(strikeX, bot), ccp(strikeX, top), 3.2f, fade(tc));
            }
        }

        for (auto* l : m_fields->hud) if (l) l->setVisible(statsOn && feedbackOn && gmGuide);
        if (feedbackOn && gmGuide) {
            // The release verdict. Spelled out rather than given as a signed frame count, because
            // the whole reason it exists is to answer "the mod said PERFECT so why did I die" -
            // and a bare "-7" next to a green PERFECT does not answer that. Only shown when the
            // hold was actually wrong, so it stays quiet on every hold the player got right.
            if (g_relActive) {
                g_relActive = false;
                double rf = g_relFrames;
                int ri = (int)(rf < 0 ? rf - 0.5 : rf + 0.5);
                std::string rtxt = ri < 0 ? fmt::format("LET GO {} EARLY", -ri)
                                          : fmt::format("HELD {} TOO LONG", ri);
                while (auto* old = overlay->getChildByID("ci-judge-rel"_spr)) old->removeFromParent();
                auto rl = CCLabelBMFont::create(rtxt.c_str(), "bigFont.fnt");
                rl->setID("ci-judge-rel"_spr);
                rl->setColor({ 255, 176, 64 });   // amber: not the green/blue the press verdict uses
                rl->setScale(0.40f);
                CCPoint rc = overlay->convertToNodeSpace(m_objectLayer->convertToWorldSpace(ccp(x, py)));
                // Below the press verdict, so a short hold showing both does not stack them.
                rl->setPosition(rc + ccp(0.f, 14.f));
                overlay->addChild(rl, 20);
                rl->runAction(CCSequence::create(
                    CCSpawn::create(CCMoveBy::create(0.9f, ccp(0.f, 34.f)), CCFadeOut::create(0.9f), nullptr),
                    CCRemoveSelf::create(), nullptr));
            }
            if (g_fbActive) {
                g_fbActive = false;
                double f = g_fbFrames;
                int fi = (int)(f < 0 ? f - 0.5 : f + 0.5);
                std::string txt; ccColor3B col;
                if (fi == 0) { txt = "PERFECT"; col = { 60, 255, 90 }; }
                else {
                    txt = (fi > 0 ? "+" : "") + std::to_string(fi);
                    col = (fi >= -1 && fi <= 1) ? ccColor3B{ 170, 240, 130 }
                        : f > 0 ? ccColor3B{ 255, 110, 60 } : ccColor3B{ 90, 190, 255 };
                }
                // One judgement on screen at a time. Each press added a fresh label and left the
                // last one running its fade, so two clicks close together drew EARLY and LATE over
                // each other and read as a single garbled word.
                while (auto* old = overlay->getChildByID("ci-judge"_spr)) old->removeFromParent();
                auto lbl = CCLabelBMFont::create(txt.c_str(), "bigFont.fnt");
                lbl->setID("ci-judge"_spr);
                lbl->setColor(col); lbl->setScale(0.55f);
                CCPoint pc = overlay->convertToNodeSpace(m_objectLayer->convertToWorldSpace(ccp(x, py)));
                lbl->setPosition(pc + ccp(0.f, 34.f));
                overlay->addChild(lbl, 20);
                lbl->runAction(CCSequence::create(
                    CCSpawn::create(CCMoveBy::create(0.8f, ccp(0.f, 45.f)), CCFadeOut::create(0.8f), nullptr),
                    CCRemoveSelf::create(), nullptr));

                // Highway keeps your eyes on the lane, so the judgement has to appear there too -
                // spelled out rather than a frame count, the way a rhythm game calls it.
                if (mode == 4) {
                    // Positioned off the lane's own settings, not the defaults it used to assume,
                    // so it follows the lane when it is moved or resized. Kept on screen so a tall
                    // lane does not push the judgement off the top.
                    float hwS = (float)mod->getSettingValue<double>("hw-scale");
                    float laneX = win.width * (float)mod->getSettingValue<double>("hw-x");
                    float laneTop = win.height * (float)mod->getSettingValue<double>("hw-y")
                                  + win.height * 0.52f * hwS;
                    float jy = clmp(laneTop + 26.f, 40.f, win.height - 16.f);
                    while (auto* old = overlay->getChildByID("ci-judge-hw"_spr)) old->removeFromParent();
                    const char* word = fi == 0 ? "PERFECT" : (f > 0 ? "LATE" : "EARLY");
                    auto j = CCLabelBMFont::create(word, "bigFont.fnt");
                    j->setID("ci-judge-hw"_spr);
                    j->setColor(col); j->setScale(0.62f);
                    j->setPosition({ laneX, jy });
                    overlay->addChild(j, 21);
                    j->runAction(CCSequence::create(
                        CCScaleTo::create(0.09f, 0.82f), CCScaleTo::create(0.10f, 0.62f),
                        CCDelayTime::create(0.30f), CCFadeOut::create(0.35f),
                        CCRemoveSelf::create(), nullptr));
                    if (fi != 0) {
                        auto amt = CCLabelBMFont::create(
                            fmt::format("{} frame{}", fi < 0 ? -fi : fi, (fi == 1 || fi == -1) ? "" : "s").c_str(),
                            "bigFont.fnt");
                        amt->setID("ci-judge-hw"_spr);
                        amt->setColor(col); amt->setScale(0.34f);
                        amt->setPosition({ laneX, clmp(jy - 16.f, 24.f, win.height - 16.f) });
                        overlay->addChild(amt, 21);
                        amt->runAction(CCSequence::create(CCDelayTime::create(0.40f),
                            CCFadeOut::create(0.35f), CCRemoveSelf::create(), nullptr));
                    }
                }

                // The answer to "every press said PERFECT and I died anyway".
                //
                // In ship, wave, UFO and swing your height is the integral of everything you have
                // done since the last landing, so small press errors do not cancel - they add up
                // into a position that is nowhere near the macro's, while each individual press is
                // still judged on time. PERFECT is the truth about that one press and says nothing
                // about where the run has drifted to.
                //
                // g_driftSec has been accumulating exactly that for a long time and nothing ever
                // read it. Say it out loud once it is big enough to be the reason you are dying.
                if (gmIntegrates(liveGamemode(m_player1))) {
                    double dfr = g_driftSec * kPhysFps;
                    double adfr = dfr < 0 ? -dfr : dfr;
                    if (adfr >= 2.5 && !g_driftShown) {
                        g_driftShown = true;
                        while (auto* o = overlay->getChildByID("ci-drift"_spr)) o->removeFromParent();
                        auto d = CCLabelBMFont::create(
                            fmt::format("DRIFTING {}{:.0f}f", dfr > 0 ? "+" : "-", adfr).c_str(),
                            "bigFont.fnt");
                        d->setID("ci-drift"_spr);
                        d->setColor({ 255, 140, 40 });
                        d->setScale(0.42f);
                        d->setPosition(pc + ccp(0.f, 58.f));
                        overlay->addChild(d, 20);
                        d->runAction(CCSequence::create(
                            CCDelayTime::create(0.8f), CCFadeOut::create(0.6f),
                            CCRemoveSelf::create(), nullptr));
                    } else if (adfr < 1.0) {
                        g_driftShown = false;   // back on line, so it may warn again later
                    }
                }
            }
            int acc = g_total > 0 ? (100 * g_good / g_total) : 0;
            std::string lastStr = g_total == 0 ? "-" : (g_lastIn ? "0f" :
                ((g_lastFrames > 0 ? "+" : "") + std::to_string((int)(g_lastFrames < 0 ? g_lastFrames - 0.5 : g_lastFrames + 0.5)) + "f"));
            int wi[3] = { -1,-1,-1 }; double wv[3] = { 0,0,0 };
            for (size_t i = 0; i < g_clickStats.size(); i++) {
                auto& s = g_clickStats[i]; if (s.count < 2) continue;
                double a2 = s.sumAbs / s.count;
                for (int j = 0; j < 3; j++) if (a2 > wv[j]) { for (int kk = 2; kk > j; kk--) { wv[kk] = wv[kk - 1]; wi[kk] = wi[kk - 1]; } wv[j] = a2; wi[j] = (int)i; break; }
            }
            std::string worst = "Worst:";
            for (int j = 0; j < 3; j++) { if (wi[j] < 0) break; auto& s = g_clickStats[wi[j]]; int av = (int)(s.sumFrames / s.count + (s.sumFrames < 0 ? -0.5 : 0.5)); worst += " #" + std::to_string(wi[j] + 1) + "(" + (av > 0 ? "+" : "") + std::to_string(av) + ")"; }

            int leadMs = (int)(lead * 1000.f + 0.5f);
            ErrStats hudSt = errStats();
            int spreadMs = (int)lround(hudSt.n >= 8 ? hudSt.sd : sqrtf(g_cal.Sbar) * 1000.f);
            // The lead is one fixed number now, so there is no "calibrating" state to report.
            // Spread is still worth showing - it is how consistent YOU are, which the readout
            // measures and no longer acts on. The rate multiplier only appears off 1x.
            std::string calib = "Lead " + std::to_string(leadMs) + "ms  spread " + std::to_string(spreadMs) + "ms";
            if (g_cal.h < 0.92f || g_cal.h > 1.08f) calib += "  x" + std::to_string((int)(g_cal.h * 100 + 0.5f)) + "%";
            if (g_startAlign == StartAlign::Approx) calib += "  [start pos: approx]";
            // Honest about which it is: a spawn on an unmapped stretch is within ~20 units,
            // not exact, and one run from 0% is what closes that.
            if (g_startKeyObj && !g_offsetObserved)
                calib += g_anchored ? "  [start pos: anchored]" : "  [start pos: estimated]";

            if (statsOn && m_fields->hud[0]) m_fields->hud[0]->setString(("In-window " + std::to_string(acc) + "%  (" + std::to_string(g_good) + "/" + std::to_string(g_total) + ")").c_str());
            if (statsOn && m_fields->hud[1]) m_fields->hud[1]->setString(("Streak " + std::to_string(g_streak) + "  (best " + std::to_string(g_bestStreak) + ")").c_str());
            if (statsOn && m_fields->hud[2]) m_fields->hud[2]->setString(("Last " + lastStr).c_str());
            if (statsOn && m_fields->hud[3]) m_fields->hud[3]->setString(worst.c_str());
            if (statsOn && m_fields->hud[4]) m_fields->hud[4]->setString(calib.c_str());
            int hudOp = (int)(100 * op);
            for (auto* l : m_fields->hud) if (l) l->setOpacity(hudOp);   // setString resets glyph opacity
        }
    }

    bool attemptIsUnsafe() {
        if (safeModeOn()) { m_fields->safeTripped = true; return true; }
        return m_fields->safeTripped;
    }

    void levelComplete() {
        // A ghost run must never register a completion. Test mode is not enough here: this is a
        // run nobody made, and it must leave no trace of having reached the end at all.
        if (g_ghost) return;
        if (!attemptIsUnsafe()) { PlayLayer::levelComplete(); return; }
        bool prev = m_isTestMode;
        m_isTestMode = true;
        PlayLayer::levelComplete();
        m_isTestMode = prev;
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        // A death while a path is being worked out means the macro did not fly this level - the run
        // is abandoned and nothing is kept. Blocked, so the level is not reset out from under the
        // computation before it can tidy up.
        if (g_recOnPath) {
            // A DEATH ENDS AN ATTEMPT, NOT THE RECORDING.
            //
            // Finishing on the first death meant finishing on the SPAWN's destroyPlayer call, every
            // single attempt: "death #1 at x=1 after 1 points", four thousand times in one session.
            // The same trap that has now caught three separate pieces of this feature.
            //
            // So a death simply ends that attempt. The furthest run of the session is kept, and the
            // next attempt starts a fresh recording - which is also what a practising player wants,
            // since they will reach further on some tries than others.
            g_recDeaths++;
            recPathKeepBest();
            // Written out after every attempt. A session is dozens of runs and the game has crashed
            // during this work before; a path that only reaches disk on a clean exit is a path that
            // gets lost. The write is a few tens of kilobytes and happens on a death, where a frame
            // is already being spent respawning.
            if (auto* rp = PlayLayer::get()) recPathStore(rp);
            recPathReset();
        }
        if (g_ffOn) {
            // GD calls this while a level is spawning, not only when something kills you. Counting
            // those aborted the run at 0.0 seconds with 12 "deaths" before the icon had moved a
            // single unit - the same trap that disqualified every replay two builds ago. Only count
            // once the run is genuinely under way.
            if (g_ffArmed) {
                if (!g_ffDeaths) {
                    auto* pl = PlayLayer::get();
                    log::info("[CI-FF] FIRST death at frame {}, x={:.0f} - this is where the macro "
                              "stopped working, and it is a real one",
                              pl ? (long long)pl->m_gameState.m_currentProgress : -1,
                              player ? (double)player->getPositionX() : 0.0);
                }
                g_ffDeaths++;
            }
            return;
        }
        // A death during a replay is the most useful thing it can report: the macro completes this
        // level, so if the replay does not, this is exactly where it stopped following it. Noted
        // and the run ended, rather than shrugged off so the path can keep growing into somewhere
        // the run never went.
        if (g_ghost) {
            // It reported player ONE's position for whichever player died, which is why every
            // macro on a level appeared to die at the same x: that number was never a death
            // position at all, it was p1's x at the moment the arming test first passed. Count them
            // properly, and say which icon it was.
            g_ghDeaths++;
            const bool isP1 = g_ghLayer && player == g_ghLayer->m_player1;
            if (isP1) g_ghDeathsP1++;
            if (!g_ghDied && g_ghDeathArmed && isP1) {
                g_ghDied = true;
                g_ghDiedX = player ? (double)player->getPositionX() : 0.0;
            }
            return;
        }
        if (!attemptIsUnsafe()) { PlayLayer::destroyPlayer(player, object); return; }
        bool prev = m_isTestMode;
        m_isTestMode = true;
        PlayLayer::destroyPlayer(player, object);
        m_isTestMode = prev;
    }

    // The completion screen appeared during a ghost run, and a distance margin was the wrong
    // safeguard: 300 units of an 82,333-unit level is nothing, and the replay reached 100%. These
    // are the other doors to the same room, and they are all shut for the duration.
    void playEndAnimationToPos(cocos2d::CCPoint position) {
        if (g_ghost) return;
        PlayLayer::playEndAnimationToPos(position);
    }

    void showCompleteEffect() {
        if (g_ghost) return;
        PlayLayer::showCompleteEffect();
    }

    // Not a completion at all - a NEW BEST. The replay stops at 92%, which on a level nobody has
    // finished is a personal record, so the game did exactly what it should and put the end screen
    // up for it. The screen is the visible half; the damage is the number behind it.
    void showEndLayer() {
        if (g_ghost) return;
        PlayLayer::showEndLayer();
    }

    void showNewBest(bool newReward, int orbs, int diamonds, bool demonKey, bool noRetry, bool noTitle) {
        if (g_ghost) return;
        PlayLayer::showNewBest(newReward, orbs, diamonds, demonKey, noRetry, noTitle);
    }

    // Not built on iOS: the compiler there inlines PlayLayer::commitJumps, so there is no
    // address to hook and Geode turns the attempt into a compile error rather than a mod that
    // quietly does nothing. Every other platform keeps it.
    //
    // What iOS loses is the guard below, not correctness of play: a replay's presses and the
    // presses made during an unsafe attempt can still reach the account's jump count there.
    // Both of those are already gated behind settings that ship off, so the exposure is small -
    // and the alternative is not shipping an iOS build at all.
#ifndef GEODE_IS_IOS
    void commitJumps() {
        // Nothing a replay did counts as a jump. It presses hundreds of times and none of them
        // happened.
        if (g_ghost) { m_uncommittedJumps = 0; return; }
        if (attemptIsUnsafe()) m_uncommittedJumps = 0;
        PlayLayer::commitJumps();
    }
#endif

    void resetLevel() {
        // Every attempt, not just on the way out: the number is only useful next to the run that
        // produced it, and a session is dozens of runs.
        if (!g_ghost) ghostCheckReport();
        // attempt counter isnt gated either, put it back manually. annoying
        // The increment belongs to the attempt that is STARTING, so the decision has to be made
        // on the state THAT attempt will run under. Deciding it from the attempt that just ended
        // discarded an increment for a run which then counted for percentage, completion and the
        // leaderboard - and kept one for a run that did not.
        auto* gsm = GameStatsManager::sharedState();
        int attemptsBefore = m_level ? (int)m_level->m_attempts : 0;
        int statBefore = gsm ? gsm->getStat("2") : 0;

        PlayLayer::resetLevel();
        // The only other recheck is on entering a level, and this is a practice mod - people sit
        // on one hard level for hours without ever re-entering it. Without this, "one device at a
        // time" could go a whole session unenforced. licTick's own LIC_RECHECK gate keeps it to
        // one request per half hour however many times you die.
        licTick();

        m_fields->safeTripped = safeModeOn();
        if (m_fields->safeTripped) {
            if (m_level) m_level->m_attempts = attemptsBefore;
            if (gsm) gsm->setStat("2", statBefore);
        }

        // Only if the spawn point actually moved. Re-deriving it on every respawn added it on top
        // of a checkpoint-restored level time that already contained it, so every practice death
        // threw the guide further ahead - which is most of what "it's all messed up" was.
        syncStartOffset(this);
        // Re-measured on every attempt that starts at the spawn, so a stale spawn state left by a
        // StartPos switcher heals next try instead of poisoning the session. NOT on a checkpoint
        // respawn: that restores a clock from elsewhere in the level, and measuring it against the
        // spawn's time is exactly the double count this replaced.
        // Unconditional now: the correction is measured against where the player actually IS,
        // not against the spawn, so a checkpoint respawn is just another position.
        g_offsetLocked = false; g_trackPrevLT = -1.0e9;
        xtSave();   // don't lose a run's mapping if the game never reaches onQuit
        waveEndAttempt();   // and close whatever wave stretch this attempt was in the middle of
        // The drop streak is a per-attempt suspicion, not a verdict on the spawn point. Latching it
        // for the session left the guide dark until the player left the level.
        g_startAlign = g_startAlignResolved;

        m_fields->prevX = m_player1 ? m_player1->getPositionX() : 0.f;
        m_fields->vxInit = false; m_fields->prevRaw = -1.0e9; g_streak = 0; g_dropStreak = 0;
        m_fields->syncLog = 0.0; m_fields->drawLog = 0.0;
        m_fields->tabBad = 0.f;
        m_fields->lastPlayPress = -1.0e9; g_cal.lastWall = -1.0;
        g_cal.h = 1.f; g_cal.hFrames = 0;   // dont carry h across attempts
        g_driftSec = 0.0; g_driftShown = false; g_btnDown = false;
        if (m_fields->overlay) m_fields->overlay->clear();
        if (m_fields->pulse) m_fields->pulse->clear();
    }

    void onQuit() {
        xtSave();   // keep what this run mapped, so a later spawn can be corrected with it
        waveEndAttempt();
        if (g_recOnPath) { g_recOnPath = false; recPathFinish(this, "left the level"); }
        ghostCheckReport();
        simDestroy();   // the simulated player belongs to this PlayLayer and dies with it
        ghostReset();   // the replay belongs to this level and this macro, nothing else

        // A recording could only ever be saved from the pause menu's STOP button - but the way a
        // practice session actually ends is by leaving the level, and that path threw the entire
        // run away without a word. Reported by a customer who lost one that way and assumed it
        // had saved. What is worth keeping is read off here, before the level goes.
        int  recLid = 0;
        if (m_level) {
            recLid = (int)m_level->m_levelID;
            if (recLid <= 0) recLid = (int)m_level->m_originalLevel;
        }
        int   recN    = recClicks();
        float recSecs = recLength();
        bool  offer   = g_recActive && recN > 0 && recLid > 0;

        // Leaving a level must not carry live state into the next one: the free camera's saved
        // layer position was applied to the NEXT level's object layer, the paused click editor
        // projected markers from this level's last live frame, and the recorder silently re-armed.
        g_fcActive = false; g_snapOk = false; g_recActive = false; g_recOn = false;
        PlayLayer::onQuit();

        if (!offer) return;
        // Deferred by a frame: this runs while the layer is being torn down, and a popup parented
        // to a scene that is going away goes away with it. The buffer outlives both - it is only
        // cleared when another level starts, which cannot happen before this has been answered.
        Loader::get()->queueInMainThread([recLid, recN, recSecs] {
            geode::createQuickPopup("Save Recording",
                fmt::format("You left the level while still recording.\n"
                            "<cy>{} clicks over {:.1f}s</c>", recN, recSecs),
                "Discard", "Save",
                [recLid](FLAlertLayer*, bool save) {
                    if (!save) { recReset(); return; }
                    std::string msg;
                    bool ok = recSave(recLid, msg);
                    Notification::create(msg, ok ? NotificationIcon::Success
                                                 : NotificationIcon::Warning)->show();
                });
        });
    }

    void resetLevelFromStart() {
        PlayLayer::resetLevelFromStart();
        syncStartOffset(this);   // clears m_startPosObject, so the offset has to go back to zero
    }
};

// A hold is two inputs and the mod only ever judged the first one.
//
// In ship, UFO, wave, robot and swing the input IS the hold: how long you keep it down is what
// decides where you end up, and the press on its own decides almost nothing. So a player could
// press on exactly the right frame, let go far too early, die - and be told PERFECT, because
// nothing ever looked at the half that killed them. That is the whole of "I hit a perfect click
// and still died", and it made the verdict least trustworthy in precisely the gamemodes people
// buy a practice mod for.
//
// Graded against the click the PRESS was matched to, not searched for again. The release belongs
// to that hold by construction, and a second nearest-neighbour search over release times would
// inherit every mismatch the press matcher can already make, on top of its own.
static void gradeRelease(double relT) {
    int idx = g_heldIdx;
    g_heldIdx = -1;                       // one verdict per hold, however the button behaves
    if (idx < 0 || g_heldAtt != g_attempt || idx >= (int)g_actions.size()) return;
    // Always judged in ship / wave / UFO / swing, opt-in everywhere else. In those modes the hold
    // IS the input, so a press verdict on its own is a half-truth: a press on exactly the right
    // frame followed by a release six frames long reads as a green PERFECT and kills you, which is
    // precisely the "it said perfect and I still died" case. On cube a release means nothing and
    // the extra line is just noise, so there it stays behind the setting.
    bool integrating = false;
    if (auto* pl = PlayLayer::get()) integrating = gmIntegrates(liveGamemode(pl->m_player1));
    if (!integrating && !Mod::get()->getSettingValue<bool>("release-grading")) return;
    auto const& a = g_actions[idx];
    // A tap has no hold to get wrong, and judging the 1-frame gap at the end of one would bury
    // the press verdict under noise.
    if ((a.releaseTime - a.pressTime) < Mod::get()->getSettingValue<double>("hold-threshold")) return;
    double dF = (relT - a.releaseTime) * kPhysFps;
    // Only speak up when it is actually wrong. A correct release needs no comment.
    if (dF > -1.5 && dF < 1.5) return;
    g_relActive = true; g_relFrames = dF;
}

#ifdef CI_DEV_UNLOCK
// SHOWCASE PLAYBACK - DEVELOPER BUILDS ONLY.
//
// Plays the loaded macro on the real player, so the run can be recorded following the drawn path
// exactly. It is the same frame-indexed timeline the hidden replay uses, pointed at the icon on
// screen instead of the one nobody can see.
//
// Compiled out of every shipped binary. CI_DEV_UNLOCK is off by default and CMake hard-fails if a
// CI build turns it on, so this cannot reach a customer: a macro bot inside a paid mod is a
// cheating tool whoever wrote it, and it would be the author's name on it.
//
// It also refuses to let a played run count. Test mode is forced while it is active, so nothing it
// reaches can be submitted - a showcase needs the footage, not the leaderboard entry.
static bool  g_showOn = false;
static std::vector<GhEv> g_showEv, g_showEv2;
static size_t g_showAi = 0, g_showAi2 = 0;
static bool  g_showDown = false, g_showDown2 = false;
// When the button currently held is due to come up, in level-time seconds. Zero when nothing is
// held or the hold length is not known.
static double g_showHoldUntil = 0.0;
static int   g_showAtt = -1;
// How many presses have been logged THIS attempt. It used to be a function-local static, which
// never reset - so the budget was spent on attempt one and attempt two produced four lines before
// going quiet. The attempt that matters is usually not the first one, and the presses that matter
// are the last ones before it dies, so the counter restarts with the run.
static int   g_showSaid = 0;
// Set once per attempt when the player dies, so the death is reported with the state it happened
// in rather than being inferred from the frame counter going quiet.
static bool  g_showSaidDeath = false;

static void showArm(GJBaseGameLayer* gl) {
    g_showEv.clear(); g_showEv2.clear();
    // Same question, same answer: whichever tag the flight path is under drives the real icon.
    // PASS THE PLAYER FLAG THROUGH. DO NOT SECOND-GUESS IT.
    //
    // pathPlayerIsP2 exists for the click indicators, where a macro that tags every input player
    // two has to be read as single-player or the cues hang off the wrong icon. It only fires when
    // there is not a single player-one input, and this macro has 64 of them against 940 - so the
    // flip did not happen, the 940 went to player two, and player one flew unattended from the
    // start with its first input at frame 24706, which is 81% into the level.
    //
    // Eclipse does not decide any of this. It passes the recorded flag straight to the engine:
    //     GJBaseGameLayer::handleButton(input->down, input->button, !input->player2)
    // Whatever the macro says a player pressed, that player presses. A dual gets both, a
    // single-player recording gets one, and no heuristic can be wrong about it.
    const bool p2macro = false;
    // THE FRAME THE MACRO RECORDED, NOT A FRAME DERIVED FROM A TIME.
    //
    // llround(t * 240) only recovers the original frame if the macro was recorded at exactly 240,
    // and being one frame out in a tight corridor is a death. The parser now keeps the recorded
    // frame, so use it; the derived value stays as the fallback for anything that predates it.
    auto frameOf = [](double t) { return (long long)std::llround(t * 240.0); };
    for (auto const& a : g_actions) {
        const bool second = p2macro ? !a.p2 : a.p2;
        auto& into = second ? g_showEv2 : g_showEv;
        const long long pf = a.pressFrame   >= 0 ? a.pressFrame   : frameOf(a.pressTime);
        const long long rf = a.releaseFrame >= 0 ? a.releaseFrame : frameOf(a.releaseTime);
        into.push_back({ a.pressTime, pf, true,  a.wxSweet });
        if (a.releaseTime > a.pressTime) into.push_back({ a.releaseTime, rf, false, a.wxRel });
    }
    auto byT = [](GhEv const& a, GhEv const& b) { return a.t < b.t; };
    std::sort(g_showEv.begin(), g_showEv.end(), byT);
    std::sort(g_showEv2.begin(), g_showEv2.end(), byT);
    g_showAi = 0; g_showAi2 = 0; g_showDown = false; g_showDown2 = false;
    g_showHoldUntil = 0.0;
    g_showSaid = 0; g_showSaidDeath = false;
    if (auto* pl = PlayLayer::get()) pl->m_isTestMode = true;   // nothing here may be submitted
    // Say whether the frames are the macro's own or reconstructed - that is the difference between
    // a replay that follows it and one that drifts a frame at a time into a wall.
    int exact = 0;
    for (auto const& a2 : g_actions) if (a2.pressFrame >= 0) exact++;
    if (!g_showEv.empty() && !g_showEv2.empty())
        log::info("[CI-SHOW] both players have inputs - first for player one is frame {}, for player "
                  "two frame {}. If one starts far later than the other, that player is not flying "
                  "the opening of this level.",
                  g_showEv.front().f, g_showEv2.front().f);
    log::info("[CI-SHOW] playing this macro: {} inputs for player one, {} for player two. {} of {} "
              "carry the frame the macro recorded{}. Test mode is forced - this run cannot be "
              "submitted.",
              (int)g_showEv.size(), (int)g_showEv2.size(), exact, (int)g_actions.size(),
              exact == (int)g_actions.size() ? "" : " - the rest are reconstructed from a time, "
              "which is only right at 240 ticks a second");
}

static void showStep(GJBaseGameLayer* gl, std::vector<PendPress>& out) {
    if (!g_showOn || !gl) return;
    auto* pl = PlayLayer::get();
    if (!pl) return;
    if (g_actions.empty()) return;
    // NO isGameplayActive GATE.
    //
    // Skipping frames where the game does not call itself "active" does not pause the macro's
    // clock - the frame counter keeps advancing. So every input due during those frames is stepped
    // over, and the moment the gate opens the loop below fires all of them at once: a press and its
    // release together, which is a tap the icon never makes. That is a death at the FIRST click,
    // exactly as reported, and it would look identical to the input never arriving.
    //
    // Eclipse has no such gate - it plays whenever it is in playback state.
    // Re-armed on each attempt, because the clock goes back to the spawn and so must the cursor.
    if (g_showAtt != g_attempt) {
        g_showAtt = g_attempt;
        g_tick = (long long)std::llround(gl->m_gameState.m_levelTime * 240.0);   // spawn offset
        showArm(gl);
        log::info("[CI-SHOW] armed at frame {} - the macro's first input is frame {}",
                  (long long)gl->m_gameState.m_currentProgress,
                  g_showEv.empty() ? -1 : g_showEv.front().f);
        // THE TICK RATE HAS TO MATCH, AND NOTHING HERE CAN MAKE IT.
        //
        // If the game is stepping physics at a different rate from the one the macro was recorded
        // at, the macro's frames land between the game's and no amount of care about when to press
        // can put them back. Two clocks at 240 and 480 have no common frame for half the inputs.
        // This is a property of the setup, not of the macro or of this code, so say it plainly
        // rather than trying to compensate for it.
        const double lt0 = gl->m_gameState.m_levelTime;
        if (lt0 > 0.05) {
            const double tick = (double)gl->m_gameState.m_currentProgress / lt0;
            const double mf = g_macroFps > 1.0 ? g_macroFps : 240.0;
            if (std::fabs(tick - mf) > mf * 0.1)
                log::info("[CI-SHOW] the game is stepping physics about {:.0f} times a second and "
                          "this macro was recorded at {:.0f}. They have to match for a replay to be "
                          "exact - turn off any TPS or physics bypass and try again.", tick, mf);
        }
    }

    // The game's own frame counter, and every input applied - the same two corrections as the path
    // computation. A collapsed state drops any tap whose press and release land in one poll, and a
    // clock derived from levelTime drifts the moment the game is not running at 1x.
    // WHAT to press. The pressing itself happens in the hook class below, because it has to call
    // the ORIGINAL handleButton and only a $modify member can do that.
    //
    // This called gl->handleButton, which is the virtual - so every synthetic press went through
    // this mod's own hook and through every other input mod loaded (Mega Hack, silicate) before
    // reaching the game. Any of them may grade it, record it, delay it or swallow it outright.
    // Eclipse calls GJBaseGameLayer::handleButton from inside its own hook, which bypasses all of
    // that and hands the input straight to the engine - and Eclipse plays this macro without dying.
    // THE MACRO'S CLOCK, NOT THE GAME'S FRAME COUNTER.
    //
    // The log settles it: presses fired exactly on their frame numbers and the button state
    // toggled correctly, yet the icon covered 31 units in 57 counted frames - 0.54 units a frame,
    // when the slowest speed in the game is 1.05. m_currentProgress is not ticking at the rate the
    // macro's frames are counted in; on a setup with a TPS bypass it does not have to. So frame 104
    // of the macro arrived when the level was really about 43 physics frames in, every click landed
    // roughly twice as early as it should, and the first one was fatal.
    //
    // Eclipse never meets this because it records and plays with the same counter. A macro from a
    // file carries its own rate, and the only clock both sides agree on is seconds: the macro's
    // frame divided by its own framerate is a time, and m_levelTime is that same time.
    // DO WHAT THE WORKING BOTS DO. NOTHING ELSE.
    //
    // xdBot, on an imported macro (src/global.cpp getCurrentFrame):
    //     if (!g.macro.xdBotMacro && g.state == state::playing)
    //         frame = pl->m_gameState.m_currentProgress;
    // Eclipse, on every macro (src/hacks/Bot/Bot.cpp):
    //     while ((input = s_bot.poll(m_gameState.m_currentProgress)) != std::nullopt)
    //         GJBaseGameLayer::handleButton(input->down, input->button, !input->player2);
    //
    // Both match the macro's frame numbers against m_currentProgress raw - no framerate
    // conversion, no position, none of the machinery invented here. Both play these files without
    // dying. Everything below this line that was not one of those two things has been removed,
    // because inventing a better clock than the one the working implementations use has cost a day
    // and produced nothing that survives a level.
    // Real ticks since the attempt began - the same quantum the macro's frames are counted in.
    // ONE TICK EARLY, BECAUSE THE INPUT IS CONSUMED BY THE NEXT STEP.
    //
    // handleButton does not move anything. It sets the button state, and the step that follows is
    // what reads it - so an input handed over on tick N first affects the physics of tick N+1, and
    // arrives one tick late however early in the tick it is handed over. That is why moving the
    // injection to before the original processCommands changed nothing at all.
    //
    // Measured against the recording rather than reasoned about. Ashley Wave Trials carries its
    // own position for every frame (xdBot frameFixes), so the replay can be checked against the
    // run it came from, press by press:
    //
    //     shift    mean |x error|    max |x error|
    //       0          1.22              1.54
    //      +1          0.23              0.49
    //      +2          1.04              1.53
    //
    // 1.22 units at 0.5x speed is 1.15 frames, and at 1x it is 1.16 - the same lateness at two
    // different speeds, from the first input to the last, which is a fixed offset and not drift.
    // Firing a tick earlier removes it and leaves a fifth of a unit, which is below the resolution
    // of anything here.
    static constexpr long long CI_PLAYBACK_LEAD = 1;
    const long long frRaw = g_tick + CI_PLAYBACK_LEAD;
    auto isDueRaw = [&](GhEv const& e) { return e.f <= frRaw; };

    // FIRE ON POSITION WHERE IT IS KNOWN.
    //
    // Matching the macro's clock to the level's got playback from the first click to three percent,
    // and what is left is drift: the macro's timeline measures about one percent short of the
    // level's, which is ten frames of error by three percent in and fatal in a tight corridor. No
    // amount of care with a clock removes that, because two clocks that disagree by a ratio always
    // will.
    //
    // The icon's POSITION does not drift. A click recorded at x=1030 belongs at x=1030 whatever the
    // clock says, and position is also what actually decides whether the icon clears an obstacle.
    // The mod already places every click along the level with the engine's own posForTime, which
    // its logs measure at three tenths of a percent against the table - three times better than the
    // clock. Where a click has no position the clock is still there to fall back on.
    const double mfps = g_macroFps > 1.0 ? g_macroFps : 240.0;
    const long long fr = (long long)std::llround(gl->m_gameState.m_levelTime * mfps);
    auto* pp = pl->m_player1;
    const double px = pp ? (double)pp->getPositionX() : -1e18;
    // PRESS WHERE IT BELONGS. HOLD FOR AS LONG AS IT WAS HELD.
    //
    // Firing both the press and the release on position gets each within a unit or two, which
    // sounds close and is not: the two errors are independent, so a hold recorded as 40 frames can
    // come out as 38 or 42. A wave's height is decided by nothing except how long the button is
    // held, so a hold two frames short is a trajectory two frames' worth too low - every time,
    // always in the same place, which is exactly what dying at the same spot looks like.
    //
    // So position decides WHERE the press happens, and the macro's own recorded hold decides how
    // long it lasts. A duration cannot drift the way two absolute clocks do.
    // Releasing after the recorded hold duration was tried and measured worse - it died earlier
    // than releasing on position. Both are within a frame or two; neither is the problem.
    const double lt = gl->m_gameState.m_levelTime;
    (void)lt;
    const bool raw = Mod::get()->getSettingValue<bool>("playback-raw-frames");
    auto isDue = [&](GhEv const& e) {
        if (raw) return isDueRaw(e);
        if (e.x > 0.f && px > -1e17) return px >= (double)e.x;
        return e.f <= fr;
    };
    // The first few presses, with the state they land in. If playback still dies at the first
    // click, this says whether the press was ever issued, at which frame, and where the icon was -
    // which is the one thing guesswork has not been able to establish.
    // The run dies with the state it dies in, and reading it back off a frozen frame counter
    // afterwards loses everything about how it got there. Said once, with the numbers that decide
    // whether a wave clears an obstacle.
    if (!g_showSaidDeath) {
        auto* pd = pl->m_player1;
        if (pd && pd->m_isDead) {
            g_showSaidDeath = true;
            log::info("[CI-SHOW] DIED at frame {} | x={:.0f} y={:.0f} yVel={:.2f} size={:.2f} flip={} "
                      "speed={:.2f} | {} of {} inputs had been played, next was due at frame {}",
                      fr, (double)pd->getPositionX(), (double)pd->getPositionY(),
                      (double)pd->m_yVelocity, (double)pd->m_vehicleSize, pd->m_isUpsideDown ? 1 : 0,
                      (double)pd->m_playerSpeed, (int)g_showAi, (int)g_showEv.size(),
                      g_showAi < g_showEv.size() ? g_showEv[g_showAi].f : -1);
        }
    }

    int& said = g_showSaid;
    if (said < 90 && g_showAi < g_showEv.size() && isDue(g_showEv[g_showAi])) {
        said++;
        auto* p1 = pl->m_player1;
        // yVel, mini, flip and speed are here because they are what decides where a wave goes
        // next, and because the slopes between presses come out as clean +/-1 and +/-2 right up to
        // the death - so whatever is wrong is not the shape of the flight, it is which side of a
        // portal the icon was on when the shape changed.
        log::info("[CI-SHOW] press #{} due at frame {}, game is at frame {} | x={:.0f} y={:.0f} "
                  "yVel={:.2f} dart={} size={:.2f} flip={} speed={:.2f} holding={} | {} inputs still to come",
                  said, g_showEv[g_showAi].f, fr,
                  p1 ? (double)p1->getPositionX() : -1.0, p1 ? (double)p1->getPositionY() : -1.0,
                  p1 ? (double)p1->m_yVelocity : 0.0,
                  p1 && p1->m_isDart ? 1 : 0, p1 ? (double)p1->m_vehicleSize : 0.0,
                  p1 && p1->m_isUpsideDown ? 1 : 0, p1 ? (double)p1->m_playerSpeed : 0.0,
                  p1 && p1->m_holdingButtons[1] ? 1 : 0,
                  (int)(g_showEv.size() - g_showAi));
        log::info("[CI-SHOW]   {} on {} | icon x={:.0f}, belongs at x={:.0f} | real tick {}, "
                  "substep counter {}",
                  g_showEv[g_showAi].down ? "PRESS" : "release",
                  raw ? "real physics ticks" :
                  g_showEv[g_showAi].x > 0.f ? "position" : "the clock", px,
                  (double)g_showEv[g_showAi].x, fr,
                  (long long)gl->m_gameState.m_currentProgress);
    }
    while (g_showAi < g_showEv.size() && isDue(g_showEv[g_showAi])) {
        const auto& e = g_showEv[g_showAi];
        out.push_back({ e.down, true });
        g_showDown = e.down;
        g_showAi++;
    }
    while (g_showAi2 < g_showEv2.size() && isDue(g_showEv2[g_showAi2])) {
        out.push_back({ g_showEv2[g_showAi2].down, false });
        g_showDown2 = g_showEv2[g_showAi2].down;
        g_showAi2++;
    }
}
#endif

// The game's own clock, turned up while a path is being worked out. This is the only thing this
// feature changes about how the level runs - everything else is the game doing exactly what it
// always does, which is the entire point.
class $modify(ClickGuideFF, cocos2d::CCScheduler) {
    void update(float dt) {
        cocos2d::CCScheduler::update(g_ffOn ? dt * g_ffMul : dt);
    }
};

class $modify(ClickGuideInput, GJBaseGameLayer) {

    static void onModify(auto& self) {
        // Innermost, deliberately. This body calls the original and then grades what happened,
        // so it must not run for an input that a mod further out chose to swallow - the trainer
        // would report a click the icon never made, and the recorder would save it into a macro
        // the level never received. Innermost also means the engine has actually processed the
        // press by the time we timestamp it, which is what the timestamp is claiming.
        //
        // It was previously unset, which is Priority::Normal by default rather than by intent.
        // Ties at Normal are broken by a sort that is not stable and is re-run whenever any mod
        // adds a hook, so with a second input mod installed the ordering was unspecified and
        // could differ between two launches of the same game.
        (void)self.setHookPriority("GJBaseGameLayer::handleButton", Priority::VeryLate);
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        // A path being worked out drives the level from here, per substep, because that is the only
        // injection point that has ever produced a flight rather than a corpse.
        // ONE INJECTION PER TICK, ON THE TICK.
        //
        // This ran on every call - the half-tick, the tick, and the extra button substep - so a
        // press and its release each landed on whichever substep the clock happened to round to,
        // independently. For a wave that is fatal and nothing else is: hold LENGTH is the only
        // thing that sets its height, so a press a substep late with a release a substep early is a
        // hold two substeps short, in the same place, every attempt. That is the death at three
        // percent, and it is why changing WHICH substep never helped.
        // PRESS FIRST, THEN LET THE ENGINE STEP THE TICK.
        //
        // This called the original first and injected afterwards, so an input recorded on frame N
        // was handed to the engine only after frame N had already been simulated - it took effect
        // on N+1. Press and release shifted together, so the hold length survived and the flight
        // kept its shape: that is why every slope between presses comes out as a clean +/-1, or
        // +/-2 while mini, right up to the moment it dies. What does not survive is WHERE the
        // shape sits, and one tick is about one unit at half speed and 1.3 at 1x. The log shows
        // exactly that signature - the icon sitting about three units behind where the click
        // belongs by the time it reaches the speed portal, and dying twelve units later.
        //
        // A bot presses before the step, because the step is what consumes the input:
        //     while ((input = s_bot.poll(m_gameState.m_currentProgress)) != std::nullopt)
        //         GJBaseGameLayer::handleButton(input->down, input->button, !input->player2);
        // The counter is therefore advanced first as well, so "due on frame N" means due on the
        // tick about to be simulated rather than the one just finished.
        if (!isHalfTick) {           // half of a tick is not a frame the macro knows about
            g_tick++;
            if (g_ffOn) {
                std::vector<PendPress> pend;
                ffInject(this, pend);
                for (auto const& q : pend) GJBaseGameLayer::handleButton(q.down, 1, q.p1);
            }
#ifdef CI_DEV_UNLOCK
            driveShowcase();
#endif
        }
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }

#ifdef CI_DEV_UNLOCK
    // ONCE PER PHYSICS STEP, NOT ONCE PER FRAME.
    //
    // The path this produces is right - the replay reaches 98% of the level with 681 of 686 inputs
    // and measures 0.0 units against the player - and playing the very same inputs still died. The
    // difference was where they were injected: update() runs once per rendered frame, about sixty
    // times a second, while the physics runs at two hundred and forty. A press meant for one
    // physics step therefore landed up to three steps late, and in a tight wave corridor three
    // steps is the wall. The hidden replay never had this problem because it steps 1/240 itself and
    // injects before each one.
    //
    // processCommands is called per substep, which is the granularity a macro is recorded at.
    // Restored. When the shared processCommands hook was added, this became a stub that only read
    // the setting and never injected anything, so showcase playback silently stopped working.
    void driveShowcase() {
        g_showOn = Mod::get()->getSettingValue<bool>("showcase-play");
        if (!g_showOn || g_ghost || g_ffOn) return;
        std::vector<PendPress> pend;
        showStep(this, pend);
        // Straight to the engine. GJBaseGameLayer::handleButton here is the original, so the press
        // does not pass through this mod's hook or any other mod's on its way in.
        for (auto const& q : pend) GJBaseGameLayer::handleButton(q.down, 1, q.p1);
    }
#endif

    void handleButton(bool down, int button, bool isPlayer1) {
        // Counted before the original runs, so this is every press the hidden level receives from
        // anywhere - this code, or any other mod that hooks the same entry point.
        if (g_ghost) g_ghSeen++;
        GJBaseGameLayer::handleButton(down, button, isPlayer1);   // super first, dont swallow input
        // A ghost run drives this same entry point hundreds of times to replay the macro, and none
        // of those presses were made by anyone. Grading them marks clicks the player never
        // attempted, recording them writes a macro out of a replay, and the jump counter climbs -
        // 265 jumps on a three-attempt run in the logs. The physics above still has to happen,
        // because that is how the replay flies; everything the MOD does with a press does not.
        if (g_ghost) return;
        if (g_recOn) {
            if (auto* rp = PlayLayer::get())
                if (rp->isGameplayActive())
                    recCapture(m_gameState.m_levelTime + g_startOffset + kPressLag, down, isPlayer1,
                               rp->m_player1 ? rp->m_player1->getPositionX() : -1.f);
        }
        if (button != 1) return;   // jumps only
        if (isPlayer1) g_btnDown = down;   // live state, so the guide can say hold vs not hold
        if (!down) {
            // The release half. Everything below this point is press-only and always was.
            if (isPlayer1 && !g_actions.empty() && g_startAlign != StartAlign::Failed
                && Mod::get()->getSettingValue<bool>("enabled") && licOK() && licGate())
                gradeRelease(m_gameState.m_levelTime + g_startOffset + kPressLag);
            return;
        }
        if (g_actions.empty() || g_startAlign == StartAlign::Failed) return;
        if (!Mod::get()->getSettingValue<bool>("enabled")) return;
        if (!licOK() || !licGate()) return;
        auto* pl = PlayLayer::get();
        if (!pl || !pl->isGameplayActive()) return;
        if (!gamemodeGuideOn(pl->m_player1)) return;

        double pressT = m_gameState.m_levelTime + g_startOffset + kPressLag;

        // Collected BEFORE the match below, and deliberately including the presses the match is
        // about to throw away. When the clock is out by more than the match radius those are not
        // stray taps - they are the presses that carry the offset, and discarding them first is
        // exactly how the old anchor ended up reading its own blind spot back to itself.
        if (g_startKeyObj && !g_offsetObserved && !g_spawnFixed && !g_spawnGiveUp
            && g_startAlign != StartAlign::Failed && g_spawnVotes.size() < 40) {
            g_spawnVotes.push_back(pressT);
            spawnVoteFix();
            // Grade this press against the corrected clock too, rather than letting the one that
            // completed the fix be judged on the reading it just disproved.
            if (g_spawnFixed) pressT = m_gameState.m_levelTime + g_startOffset + kPressLag;
        }

        int bi = -1; double bestDiff = 1e9;
        for (size_t i = 0; i < g_actions.size(); i++) {
            if (g_actions[i].muted) continue;   // struck out: not the player's mistake to own
            double ad = std::fabs(pressT - g_actions[i].sweet);
            if (ad < bestDiff) { bestDiff = ad; bi = (int)i; }
        }
        if (bi < 0 || bestDiff > 0.20) {
            g_cal.dropped++;
            if (dbgLog() && g_startKeyObj)
                log::debug("[CI-LT] drop: pressT={:.3f} nearest={:.3f} diff={:+.3f} offset={:.3f}",
                           pressT, bi >= 0 ? g_actions[bi].sweet : -1.0,
                           bi >= 0 ? pressT - g_actions[bi].sweet : 0.0, g_startOffset);
            // Teleport portals and time warp can make both sources agree on a wrong offset, which
            // nothing static can catch. Twelve presses running with no macro click anywhere near
            // them is not a player missing, it is the guide pointing somewhere else in the level.
            if (g_startKeyObj && ++g_dropStreak >= 12) {
                // Twelve straight misses right after a press-vote fix means the fix was wrong.
                // Put the clock back and let the votes start over, rather than switching the guide
                // off and leaving the bad offset installed. Nothing else ever clears it -
                // syncStartOffset returns early while the StartPos has not moved - so it would
                // otherwise survive every respawn and show this error on each one. The worst case
                // has to be "exactly how it behaved before any of this existed", and this is what
                // makes that true. Twice, then stop trying, so it cannot sit in a loop.
                if (g_spawnFixed) {
                    g_offsetStep += g_spawnPrevOffset - g_startOffset;   // postUpdate discounts it
                    g_startOffset = g_spawnPrevOffset;
                    // The same fix wrote this spawn's correction, and that outlives the attempt
                    // and the session - so it has to come back out too. Rolling the clock back
                    // while leaving it stored would promote a one-attempt mistake into every
                    // future visit to this spawn, which is the one way anything here could do
                    // lasting harm rather than spoil a single run.
                    if (!g_spawnListKey.empty()) {
                        // An explicit zero, not a deletion: this spawn is now known to want no
                        // correction, which also stops a neighbour's value being interpolated
                        // back onto it on the next visit.
                        spawnFixStore((double)g_startKeyX, g_spawnCanonRaw, 0.0);
                        g_spawnErr = 0.0;
                        if (g_spawnCanonRaw > 0.0) g_spawnTime = g_spawnCanonRaw;
                        buildActionPositions(PlayLayer::get());
                    }
                    g_spawnFixed = false; g_anchored = false; g_spawnHoldSeeded = false;
                    g_spawnVotes.clear();
                    g_dropStreak = 0;
                    g_offsetLocked = false;
                    if (++g_spawnRollbacks >= 2) g_spawnGiveUp = true;
                    if (dbgLog())
                        log::info("[CI-ANCHOR] vote fix rolled back after 12 misses -> offset {:+.3f}{}",
                                  g_startOffset, g_spawnGiveUp ? " (giving up)" : "");
                    return;
                }
                if (g_startAlign != StartAlign::Failed) {
                    g_startAlign = StartAlign::Failed;
                    Notification::create("Start pos: guide is out of sync here - turned off",
                                         NotificationIcon::Error)->show();
                }
            }
            return;
        }
        g_dropStreak = 0;

        auto& a = g_actions[bi];
        // Set before the once-per-attempt guard further down: a second press on the same click
        // still starts a hold, and its release still has to be judged against something.
        g_heldIdx = bi; g_heldAtt = g_attempt;
        // Ground truth, and the only measurement not derived from posForTime: where the player
        // actually pressed against where the mark for that press was drawn. dx is the error in the
        // units the player sees. Correct guide => dx near 0 whatever the run.
        // The spawn clock is no longer anchored from here. It used to take the median error of
        // presses matched to their nearest click, which is only meaningful once the clock is
        // already close - and if it were already close there would be nothing to correct. See
        // spawnVoteFix, which runs above on the raw press times before any of them are matched
        // or discarded.
        if (dbgLog()) {
            float ppx = pl->m_player1 ? pl->m_player1->getPositionX() : -1.f;
            log::info("[CI-PRESS] x={:.0f} t={:.3f} markWx={:.0f} dx={:+.0f} dt={:+.3f} dxExpect={:+.0f} sp={}",
                      ppx, pressT, a.lockWx, a.lockWx - ppx, pressT - a.sweet,
                      -(pressT - a.sweet) * (g_segs.empty() ? 311.0 : g_segs[segAtX((double)ppx)].v),
                      g_startKeyObj ? 1 : 0);
        }
        if (a.gradedAtt == g_attempt) return;   // one sample per action per attempt (panic triple taps)
        a.gradedAtt = g_attempt;
        // Measured against the macro's actual press, not the edge of a band. The old test reported
        // zero for anything inside the band - +-24ms at 1x, which is nearly six frames at 240fps -
        // so it said you were on time for presses that were nowhere near the right frame. The
        // window is a visual "press around here" hint; it was never a verdict and cannot be one,
        // because nothing here knows what was survivable. So report the frame delta and let it
        // speak for itself.
        double dFrames = (pressT - a.sweet) * kPhysFps;
        int fi = (int)std::llround(dFrames);
        bool onFrame = (fi == 0);              // same frame the macro pressed on
        bool close = (fi >= -1 && fi <= 1);    // within a frame either way
        double af = dFrames < 0 ? -dFrames : dFrames;

        g_total++;
        if (close) { g_good++; g_streak++; if (g_streak > g_bestStreak) g_bestStreak = g_streak; }
        else g_streak = 0;
        g_lastFrames = dFrames; g_lastIn = onFrame;
        if (bi < (int)g_clickStats.size()) { auto& s = g_clickStats[bi]; s.count++; s.sumFrames += dFrames; s.sumAbs += af; }
        g_fbActive = true; g_fbFrames = dFrames; g_fbIn = onFrame;
        g_tracerTTL = 18; g_tracerGood = close;

        {
            int gmNow = liveGamemode(pl->m_player1);
            if (gmIntegrates(gmNow)) g_driftSec += (pressT - a.sweet);
            else g_driftSec = 0.0;   // a landing re-anchors you, so the error stops carrying
        }

        if (g_cal.hFrames >= 15 && g_startAlign == StartAlign::Exact) {   // dist even with auto-cal off
            int gmH = liveGamemode(pl->m_player1);
            int ctxH = (gmH * 2 + (a.cluster ? 1 : 0)) & 15;
            float hH = a.stashed ? a.stH : g_cal.h;
            errPush((float)((pressT - a.sweet) / std::max(hH, 0.05f) * 1000.0), ctxH, close);
        }

        // The calibrator that used to run from here is gone. It shifted the cue to match how
        // the player had been clicking, which meant the mark moved between attempts - and a
        // moving mark is worse than a slightly wrong stationary one. Everything above this line
        // still runs: the frame verdict, the streak, and the error histogram the stats readout
        // draws, none of which ever moved anything.
        return;
    }
};

// Every button already on the level page, in that page's own coordinates.
//
// GD's buttons move between versions, screen sizes and texture packs, and other mods add their
// own to the same corner - so any fixed position is a collision waiting to happen, and this one
// was landing on the like/rate button. Rather than guess a spot that happens to be free today,
// look at what is actually there.
static void collectButtonRects(cocos2d::CCNode* n, cocos2d::CCNode* space,
                               std::vector<cocos2d::CCRect>& out, int depth = 0) {
    if (!n || depth > 4) return;                     // deep enough for GD's menus, cheap enough to run
    auto* kids = n->getChildren();
    if (!kids) return;
    for (int i = 0; i < kids->count(); i++) {
        auto* c = typeinfo_cast<cocos2d::CCNode*>(kids->objectAtIndex(i));
        if (!c || !c->isVisible() || !c->getParent()) continue;
        if (typeinfo_cast<cocos2d::CCMenuItem*>(c)) {
            auto sz = c->getContentSize();
            float w = sz.width * c->getScaleX(), h = sz.height * c->getScaleY();
            auto p = space->convertToNodeSpace(c->getParent()->convertToWorldSpace(c->getPosition()));
            out.push_back(cocos2d::CCRect(p.x - w * 0.5f, p.y - h * 0.5f, w, h));
        }
        collectButtonRects(c, space, out, depth + 1);
    }
}

// A NODE OF OUR OWN, BECAUSE THE PAGE'S UPDATE CANNOT BE HOOKED.
//
// The level-page work was written as update() and onExit() overrides on a $modify of
// LevelInfoLayer, and neither ever ran. Geode only hooks functions the target class itself
// declares, and LevelInfoLayer declares neither - it inherits them from CCNode and CCLayer. The
// generated modify header lists sixty-one hookable members and update is not among them. It
// compiles, it links, and the bodies sit in a vtable no live object uses. Three sessions of logs
// with no [CI-PROC] line at all, and no way to tell from the code that it was dead.
//
// This is a class the mod constructs, so its virtuals are its own and simply work - the same reason
// MacroListPopup::update has always worked. It is added as a child of the page and carries the
// GJGameLevel, which is what the macro popup lacks.
class PathPageDriver : public cocos2d::CCNode {
public:
    Ref<GJGameLevel> m_level;
    CCLabelBMFont*   m_lbl = nullptr;
    bool m_fetched = false, m_waiting = false, m_loaded = false;

    static PathPageDriver* create(GJGameLevel* lvl) {
        auto* n = new PathPageDriver();
        n->m_level = lvl;
        n->autorelease();
        n->scheduleUpdate();
        return n;
    }

    void update(float) override {
        auto* lvl = m_level.data();
        if (!lvl) return;
        const int lid = (int)lvl->m_levelID;

        if (ghostProcessBusy()) {
            if (!m_lbl) {
                m_lbl = CCLabelBMFont::create("", "goldFont.fnt");
                m_lbl->setScale(0.45f);
                auto win = CCDirector::sharedDirector()->getWinSize();
                m_lbl->setPosition({ win.width * 0.5f, 18.f });
                this->addChild(m_lbl);
            }
            m_lbl->setString(
                fmt::format("Working out the path... {:.0f}%", ghostProcessFrac() * 100.0).c_str());
            if (ghostProcessTick()) {
                if (!g_procWhy.empty()) {
                    m_lbl->setColor({ 255, 170, 90 });
                    m_lbl->setString(g_procWhy.c_str());
                } else if (m_lbl) { m_lbl->removeFromParent(); m_lbl = nullptr; }
            }
            return;
        }

        if (!m_fetched && lid > 0 && licOK()
            && Mod::get()->getSettingValue<bool>("auto-fetch") && !levelCacheExists(lid)) {
            m_fetched = true; m_waiting = true;
            log::info("[CI-PROC] fetching this level's macro from the page");
            fetchAndLoadForLevel(lid);
            return;
        }
        // Either the fetch has landed, or the macro was already on disk when the page opened.
        //
        // The macro FILE existing is not the same as the macro being loaded: g_actions is filled by
        // loadActionsFromCacheFolder, which until now only ever ran when a level started. So on the
        // page the file was there, g_actions was empty, and the condition below could never be
        // true - which is the whole reason this has never once got as far as ghostProcessStart.
        // THE PAGE DOES NOT CHOOSE MACROS, AND CANNOT FLY LEVELS.
        //
        // It briefly did both. Loading a macro here picked a different one from the level - the
        // same level went from 659 inputs to 48 between one run and the next, and 48 inputs across
        // a whole level is a straight line - and the path was then cached against that wrong macro,
        // so the level restored it and drew it. Choosing belongs to the level, which already does
        // it correctly.
        //
        // And it could never have flown anything anyway: GD does not build a level's objects until
        // Play is pressed, so m_levelString is empty here. The log said so 2,318 times in one
        // session.
        //
        // Fetching is the whole of the page's job, and it is genuinely useful: the macro is on disk
        // by the time Play is pressed, so the load can work the path out behind its own screen.
        if (levelCacheExists(lid)) m_waiting = false;
    }

    void onExit() override {
        if (ghostProcessBusy()) {
            log::info("[CI-PROC] left the page - stopping; it is deterministic, so it will start "
                      "again next time");
            ghostProcessStop();
        }
        CCNode::onExit();
    }
};

class $modify(ClickGuideLevelInfo, LevelInfoLayer) {
    struct Fields {
        CCLabelBMFont* proc = nullptr;
        bool fetched = false;   // this page has already asked for the level's macro
        bool waiting = false;   // and is waiting for it to land so the path can be worked out
    };

    // The macro's path is worked out HERE, on the level page, while the player is choosing what to
    // play. It is the same replay that used to run when the level opened; the only thing that
    // changed is that it now happens where a wait is expected and can be shown honestly, instead of
    // where it is felt as the game breaking.
    void onEnterTransitionDidFinish() {
        LevelInfoLayer::onEnterTransitionDidFinish();
        // A node of our own does the work: this class's update() is not hookable and never ran.
        g_pageLevel = m_level;   // the macro popup needs the level object, not just its id
        if (m_level && !this->getChildByID("cg-path-driver"_spr)) {
            auto* d = PathPageDriver::create(m_level);
            d->setID("cg-path-driver"_spr);
            this->addChild(d);
            log::info("[CI-PROC] page open for level {} - watching for a macro to work out",
                      (int)m_level->m_levelID);
        }
        if (!m_level) return;
        if (!licOK()) return;
        if (this->getChildByID("cg-fetch-menu"_spr)) return; // don't double-add
        auto spr = CCSprite::createWithSpriteFrameName("GJ_downloadBtn_001.png");
        if (!spr) return;
        spr->setScale(0.7f);
        bool cached = levelCacheExists((int)m_level->m_levelID);
        if (cached) spr->setColor({ 120, 255, 130 }); // green = macros already saved for this level
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(ClickGuideLevelInfo::onFetchMacros));
        btn->setID("cg-fetch-btn"_spr);
        auto menu = CCMenu::create();
        menu->setID("cg-fetch-menu"_spr);
        menu->addChild(btn);
        if (cached) {
            auto tag = CCLabelBMFont::create("saved", "bigFont.fnt");
            tag->setScale(0.3f); tag->setColor({ 120, 255, 130 });
            tag->setPosition({ 0.f, -16.f }); btn->addChild(tag);
        }
        auto win = CCDirector::sharedDirector()->getWinSize();
        // Start where it has always been, then step up the right edge until the spot is free,
        // and along the bottom if the whole edge is taken. Our own button is not in the list
        // yet, so it cannot dodge itself.
        std::vector<cocos2d::CCRect> taken;
        collectButtonRects(this, this, taken);
        float bx = win.width - 35.f, by = 35.f;
        constexpr float R = 27.f;   // half the button plus a margin, so it does not merely touch
        for (int tries = 0; tries < 10; tries++) {
            cocos2d::CCRect mine(bx - R, by - R, R * 2.f, R * 2.f);
            bool hit = false;
            for (auto const& r : taken) if (r.intersectsRect(mine)) { hit = true; break; }
            if (!hit) break;
            by += 54.f;
            if (by > win.height - 55.f) { by = 35.f; bx -= 54.f; }
        }
        menu->setPosition(bx, by);
        this->addChild(menu, 100);
    }

    void onFetchMacros(CCObject*) {
        if (!m_level) return;
        int levelID = (int)m_level->m_levelID;
        if (levelID <= 0) {
            Notification::create("Click Indicators: no online macros for local levels", NotificationIcon::Warning)->show();
            return;
        }
        if (auto* p = MacroListPopup::create(levelID, (int)m_level->m_originalLevel)) p->show();
    }
};

class $modify(ClickGuidePause, PauseLayer) {
    static void onModify(auto& self) {
        // Custom Keybinds and Mega Hack both hook PauseLayer::keyDown, and at default priority they
        // ran ahead of this one and returned without calling down, so it never fired at all - the
        // free camera keys were dead for that reason alone. Run outermost so the key is seen first.
        // Anything not a pan key still falls through to them untouched.
        (void)self.setHookPriority("PauseLayer::keyDown", Priority::VeryEarly);
    }

    void keyDown(enumKeyCodes key, double timestamp) {
        if (editorKey(this, key)) return;
        if (freeCamPan(key)) { editorDraw(this); return; }
        PauseLayer::keyDown(key, timestamp);
    }

    void customSetup() {
        PauseLayer::customSetup();
        if (licOK()) { editorBuild(this); editorDraw(this); }

        // PauseLayer does not turn keyboard input on, so keyDown is never dispatched to it and the
        // free camera / click editor keys were dead no matter what the setting said. Registering it
        // is what makes the hook fire. Only done when the feature is on, so nothing changes for
        // anyone not using it, and unhandled keys still fall through to the original.
        if (Mod::get()->getSettingValue<bool>("free-camera")) this->setKeyboardEnabled(true);
        if (!licOK()) return;   // unlicensed = no in-game UI
        if (this->getChildByID("cg-pause-menu"_spr)) return;   // don't double-add

        CCMenuItemSpriteExtra* btn = nullptr;
        if (auto* circle = CircleButtonSprite::create(CCSprite::create(), CircleBaseColor::Green,
                                                      CircleBaseSize::Medium)) {
            if (auto* icon = geode::createModLogo(Mod::get())) {
                CCSize cs = circle->getContentSize();
                float w = icon->getContentSize().width;
                if (w > 1.f) icon->setScale((cs.width * 0.62f) / w);
                icon->setPosition(ccp(cs.width * 0.5f, cs.height * 0.5f));
                circle->addChild(icon, 1);
            }
            circle->setScale(0.8f);
            btn = CCMenuItemSpriteExtra::create(circle, this, menu_selector(ClickGuidePause::onClickGuide));
        }
        if (!btn) {   // fall back to a plain labelled button if the sprites can't be built
            auto spr = ButtonSprite::create("Click Indicators", "bigFont.fnt", "GJ_button_05.png", 0.8f);
            if (!spr) return;
            spr->setScale(0.5f);
            btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(ClickGuidePause::onClickGuide));
        }
        btn->setID("cg-pause-btn"_spr);

        auto win = CCDirector::sharedDirector()->getWinSize();
        auto menu = CCMenu::create();
        menu->setID("cg-pause-menu"_spr);
        menu->addChild(btn);

        menu->setPosition(win.width - 34.f, 34.f);   // bottom-right of the pause screen
        this->addChild(menu, 100);

        buildRecordControl();
    }

    // rebuild every state change. if we build once, stop button stays after recording ends
    void buildRecordControl() {
        if (auto* old = this->getChildByID("cg-rec-ui"_spr)) old->removeFromParent();
        if (!Mod::get()->getSettingValue<bool>("record-button") || !licOK()) return;
        auto win = CCDirector::sharedDirector()->getWinSize();
        auto wrap = CCNode::create();
        wrap->setID("cg-rec-ui"_spr);
        this->addChild(wrap, 100);

        auto rmenu = CCMenu::create();
        auto glyph = CCDrawNode::create();
        applyAA(glyph);
        if (g_recActive) {                        // stop: a white square
            CCPoint sq[4] = { ccp(-6.f,-6.f), ccp(6.f,-6.f), ccp(6.f,6.f), ccp(-6.f,6.f) };
            glyph->drawPolygon(sq, 4, ccColor4F{ 1.f, 1.f, 1.f, 1.f }, 0.f, ccColor4F{ 0,0,0,0 });
        } else {                                  // record: a red dot
            glyph->drawDot(ccp(0.f, 0.f), 8.5f, ccColor4F{ 1.f, 0.25f, 0.25f, 1.f });
        }
        auto holder = CCNode::create();
        holder->setContentSize({ 20.f, 20.f });
        glyph->setPosition({ 10.f, 10.f });
        holder->addChild(glyph);
        if (auto* circ = CircleButtonSprite::create(CCSprite::create(),
                g_recActive ? CircleBaseColor::Red : CircleBaseColor::Gray,
                CircleBaseSize::Medium)) {
            CCSize cs = circ->getContentSize();
            holder->setPosition(ccp(cs.width * 0.5f - 10.f, cs.height * 0.5f - 10.f));
            circ->addChild(holder, 1);
            circ->setScale(0.85f);
            auto rbtn = CCMenuItemSpriteExtra::create(circ, this,
                menu_selector(ClickGuidePause::onToggleRecord));
            rmenu->addChild(rbtn);
        }
        rmenu->setPosition(win.width * 0.5f, 44.f);
        wrap->addChild(rmenu);

        auto cap = CCLabelBMFont::create(g_recActive ? "STOP" : "RECORD", "bigFont.fnt");
        cap->setScale(0.3f);
        cap->setColor(g_recActive ? ccColor3B{ 255, 120, 120 } : ccColor3B{ 190, 195, 205 });
        cap->setPosition({ win.width * 0.5f, 20.f });
        wrap->addChild(cap);
    }

    int recLevelID() {
        if (auto* pl = PlayLayer::get()) if (pl->m_level) {
            int lid = (int)pl->m_level->m_levelID;
            return lid > 0 ? lid : (int)pl->m_level->m_originalLevel;
        }
        return 0;
    }

    void onToggleRecord(CCObject*) {
        if (!g_recActive) {                       // start: throw away anything half-captured
            recStart();
            buildRecordControl();
            Notification::create("Recording - play the level", NotificationIcon::Success)->show();
            this->onResume(nullptr);              // straight back into the level
            return;
        }
        recStop();                                // stop: confirm before writing anything
        buildRecordControl();                     // back to a RECORD button straight away
        int clicks = recClicks();
        float len = recLength();
        if (clicks <= 0) {
            Notification::create("Nothing was recorded", NotificationIcon::Warning)->show();
            return;
        }
        int lid = recLevelID();
        geode::createQuickPopup("Save Recording",
            fmt::format("Record this macro?\n<cy>{} clicks over {:.1f}s</c>", clicks, len),
            "Discard", "Save",
            [lid](FLAlertLayer*, bool save) {
                if (!save) { recReset(); return; }
                if (lid <= 0) {
                    Notification::create("Can't save a recording here", NotificationIcon::Warning)->show();
                    return;
                }
                std::string msg;
                bool ok = recSave(lid, msg);
                Notification::create(msg, ok ? NotificationIcon::Success : NotificationIcon::Warning)->show();
            });
    }

    void onClickGuide(CCObject*) {
        int lid = 0;
        if (auto* pl = PlayLayer::get()) if (pl->m_level) {
            lid = (int)pl->m_level->m_levelID;
            if (lid <= 0) lid = (int)pl->m_level->m_originalLevel;
        }
        if (auto* p = ClickGuidePopup::create(lid)) p->show();
    }
};

class $modify(ClickIndicatorsMenu, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;
        licRefresh();
        // Once per launch, ignoring LIC_RECHECK. Starting the game is exactly when the
        // second person on a shared login turns up, and it is also the one moment where
        // waiting half an hour to find out you are signed out is worst - you would get
        // it mid-run instead. Returning to the menu later does not repeat it.
        static bool startupChecked = false;
        if (!startupChecked) { startupChecked = true; licValidate(nullptr); }
        // Same once-per-launch reasoning, and it reads the licence state licRefresh() just
        // restored from cache, so it does not have to wait on licValidate coming back.
        updCheck();
        if (this->getChildByID("ci-menu-btn"_spr)) return true;

        CCMenuItemSpriteExtra* btn = nullptr;
        if (auto* circle = CircleButtonSprite::create(CCSprite::create(), CircleBaseColor::Green,
                                                      CircleBaseSize::Medium)) {
            if (auto* icon = geode::createModLogo(Mod::get())) {
                CCSize cs = circle->getContentSize();
                CCSize is = icon->getContentSize();
                // Size off the larger dimension, and fall back to a fixed scale if the logo reports
                // no usable size - left unscaled it renders at full resolution and hides the circle.
                float big = std::max(is.width, is.height);
                icon->setScale(big > 1.f ? (cs.width * 0.60f) / big : 0.22f);
                icon->setPosition(ccp(cs.width * 0.5f, cs.height * 0.5f));
                circle->addChild(icon, 1);
            }
            btn = CCMenuItemSpriteExtra::create(circle, this,
                menu_selector(ClickIndicatorsMenu::onClickIndicators));
        }
        if (!btn) return true;
        btn->setID("ci-menu-btn"_spr);

        if (auto* row = typeinfo_cast<CCMenu*>(this->getChildByID("bottom-menu"))) {
            // Match whatever the buttons already in the row are, rather than guessing a scale -
            // other mods add to this row too and GD's own sizes differ between versions.
            float target = 0.f;
            if (auto* kids = row->getChildren()) {
                for (unsigned int i = 0; i < kids->count(); i++)
                    if (auto* sib = typeinfo_cast<CCNode*>(kids->objectAtIndex(i)))
                        target = std::max(target, sib->getScaledContentSize().height);
            }
            float own = btn->getContentSize().height;
            if (target > 1.f && own > 1.f) btn->setScale(target / own);
            row->addChild(btn);
            row->updateLayout();
        } else {
            auto win = CCDirector::sharedDirector()->getWinSize();
            auto menu = CCMenu::create();
            menu->setID("ci-menu"_spr);
            menu->addChild(btn);
            menu->setPosition(win.width - 34.f, 34.f);
            this->addChild(menu, 100);
        }
        return true;
    }

    void onClickIndicators(CCObject*) {
        licRefresh();
        if (licOK()) { if (auto* p = ClickGuidePopup::create(0)) p->show(); }
        else         { if (auto* p = LicensePopup::create())    p->show(); }
    }
};
