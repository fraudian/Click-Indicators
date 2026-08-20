#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

extern double g_macroFps;   // ticks a second the macro's frame numbers are counted in

struct Action {
    double pressTime = 0.0;
    float px = -1.f;
    double releaseTime = 0.0;
    // THE FRAME THE MACRO ACTUALLY RECORDED.
    //
    // A macro stores its inputs as frame numbers. The parser divided them by the recording's
    // framerate to get these times and threw the frames away - and replaying then multiplied the
    // times back by an assumed 240 to guess the frames again. That round trip is only lossless when
    // the macro was recorded at exactly 240, and it is what a replay stands or falls on: an input
    // one frame late in a tight corridor is a death. Eclipse never does it; it drives the recorded
    // frame directly, which is why the same macro plays there and dies here.
    //
    // Kept alongside the times, because everything else in the mod - cue drawing, grading, the
    // x-position map - is built on seconds and works.
    long long pressFrame = -1, releaseFrame = -1;
    double winStart = 0.0;
    double winEnd = 0.0;
    double sweet = 0.0;
    bool cluster = false;
    bool p2 = false;     // a dual macro interleaves both players; the flight path replays only one
    float stLead = 0.f; float stH = 1.f; bool stashed = false;
    int   gradedAtt = -1;
    // Set once this click's window has gone by with no press at all. Distinct from gradedAtt,
    // which only ever records a press that DID happen - so a click nobody pressed used to leave
    // no trace anywhere, and the next press would be matched to the NEXT click and judged perfect
    // against it. That is the "it said PERFECT and I still died" case on cube: the guide is
    // reporting on a click you made, having said nothing about the one you skipped.
    int   missAtt = -1;
    int   support = 1;   // agreeing macros behind winStart/winEnd; 1 = no measured spread
    bool  muted = false; // player marked this click unnecessary: no cue, no sound, not graded
    // Where in the level this click happens. A fixed property of the macro and the level, worked
    // out once - so drawing it needs no velocity, no projection, and nothing about playback rate.
    float wxSweet = -1.f, wxStart = -1.f, wxEnd = -1.f, wxRel = -1.f;
    // Latched the first frame this click becomes visible in an attempt, then never moved.
    // A click happens in one place; the mark must not creep as the player approaches it.
    float lockWx = -1.f; int lockAtt = -1;
};

// The path the recording actually flew, when the macro carries it.
//
// A GDR replay stores the player's position alongside its inputs - per frame in the msgpack
// format's frameFixes, per input in the binary format's physics extension - and the mod has been
// throwing it away and simulating a path instead. Simulation can only ever approximate: it has to
// infer the starting height from geometry, and in an open stretch the level constrains nothing, so
// the error has nowhere to be corrected. A recorded position needs none of that. It is where the
// run WAS, exact, and it is in the file before the level has finished loading.
struct MacroPos { float x, y; };
extern std::vector<MacroPos> g_macroPath;   // x ascending; empty when the macro carries no positions
// Which file those positions came from. Without it the path is just "whatever was parsed last",
// and the mod parses every macro it finds for a level - so the route was being drawn from one
// recording while the clicks came from another.
extern std::string g_macroPathSrc;

// The macro vault key, set by the licence code once an account has supplied one, and empty
// otherwise. Files the mod wrote are sealed with it; without it they do not parse at all, which is
// the point - a leaked pack of the library is worth nothing on its own.
//
// A plain byte array rather than a callback, because parseMacroFile is called from the game thread
// during a level load and this has to cost nothing when there is nothing to do.
extern std::vector<unsigned char> g_vaultKey;   // 32 bytes, or empty

// Until this passes, a macro file that is NOT sealed is still read. After it, only sealed files
// and files the player chose themselves are. Same date as the licence grant migration.
extern bool g_vaultStrict;

extern bool g_slcTrimmed;

double parseMacroFile(const std::filesystem::path& path, std::vector<Action>& out);
bool   isMacroExt(const std::string& ext);
