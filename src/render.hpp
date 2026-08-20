#pragma once

#include <Geode/Geode.hpp>

extern float g_cueContrast;
extern float g_cueOpacity;

void applyAA(cocos2d::CCDrawNode* n);

void drawRing(cocos2d::CCDrawNode* n, cocos2d::CCPoint c, float r, float w, cocos2d::ccColor4F col);
void drawSegOL(cocos2d::CCDrawNode* n, cocos2d::CCPoint a, cocos2d::CCPoint b, float w, cocos2d::ccColor4F col);

// One continuous stroke through many points, instead of one call per segment. Segments drawn
// separately overlap at every corner by half a round cap, and that overlap blends twice - which on
// the wave route, where the black rim is several times wider than the line, is what makes a single
// path read as a row of pieces butted together.
void drawPolyOL(cocos2d::CCDrawNode* n, const cocos2d::CCPoint* p, int count, float w,
                cocos2d::ccColor4F col);
void drawRingOL(cocos2d::CCDrawNode* n, cocos2d::CCPoint c, float r, float w, cocos2d::ccColor4F col);
void drawCue(cocos2d::CCDrawNode* n, int mode, cocos2d::CCPoint c, float bot, float top,
             float ease, bool armed, float s, float cr, float cg, float cb, float op);

void drawConvergeQueue(cocos2d::CCDrawNode* n, cocos2d::CCPoint c, float bot, float top,
                       const float* leads, int count, float approachT, float s,
                       float cr, float cg, float cb, float op);

struct HwNote {
    float lead = 0.f;   // seconds until this press reaches the bar
    float hold = 0.f;   // hold length in seconds, 0 for a tap
    bool armed = false;
    bool done = false;   // already registered this attempt
};

void drawHighway(cocos2d::CCDrawNode* n, cocos2d::CCPoint base, float laneW, float laneH, float span,
                 const HwNote* notes, int count, float phase, bool btnDown,
                 float cr, float cg, float cb, float op, bool guides = true);
