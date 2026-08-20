#include "render.hpp"

#include <cmath>

using namespace geode::prelude;

float g_cueContrast = 0.6f;
float g_cueOpacity = 1.f;

static inline float clmp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// The dark passes are the widest geometry and sit under the bright core. At full opacity the core
// is nearly solid and hides them, but as opacity drops the core turns see-through and uncovers
// them, so the cue became a dark smudge instead of fading - at 0.5 it drew 5.7x more black than at
// 1.0. Fading the dark passes by opacity squared keeps the composite close to linear and keeps the
// visible ink at least 88% bright colour at every setting.
static inline void darkAlpha(float baseA, float k, float& halo, float& edge) {
    float o = clmp(g_cueOpacity, 0.f, 1.f);
    float d = o * o;
    halo = k > 0.02f ? baseA * 0.30f * k * d : 0.f;
    edge = baseA * (0.55f + 0.35f * k) * d;
}

// The dark passes used to be sized additively (w + 10.8px at default contrast), which is enormous
// next to a 1.8px line: on anything short or jointed - a ring's chords, the converge arrow tips -
// each pass became a fat blob and they piled up into a scalloped mess. Sizing them as a multiple of
// the line keeps the rim proportional at every width.
static inline float edgeWidth(float w, float k) { return w * (1.35f + 0.45f * k) + 0.5f; }
static inline float haloWidth(float w, float k) { return w * (2.1f + 1.5f * k) + 1.0f; }

// CCDrawNode has antialiasing switched off: its fragment shader computes the distance to the edge
// into v_texcoord and then discards it with step(), so every line comes out stair-stepped. cocos's
// own smoothstep version is in that file, commented out. This is that shader, per-node on our own
// draw nodes only - patching the shared program in CCShaderCache would restyle every draw node in
// the game, including the editor's.
//
// The vertex source is INLINED rather than using cocos2d::ccPositionColorLengthTexture_vert, and
// that is not a style choice. ccShaders.h declares it `extern CC_DLL const GLchar*`, but on win32
// CC_DLL expands to NOTHING - ACTUAL_CC_DLL is the one that is __declspec(dllimport)
// (platform/win32/CCPlatformDefine.h:11-15). So the read resolves through the import library to a
// jump thunk's machine code instead of to the string, yielding 0x25FF00000FB925FF: a non-canonical
// address. glShaderSource is called with a NULL length array, so the DRIVER strlen()s it and takes
// a #GP, which Windows reports as an access violation at 0xFFFFFFFFFFFFFFFF inside the GL driver.
// No compiler or linker warning. Never read `extern CC_DLL` data from a mod.
static const char* const kAAVert = "attribute vec4 a_position;\n"
                                   "attribute vec2 a_texcoord;\n"
                                   "attribute vec4 a_color;\n"
                                   "varying vec4 v_color;\n"
                                   "varying vec2 v_texcoord;\n"
                                   "void main() {\n"
                                   "    v_color = vec4(a_color.rgb * a_color.a, a_color.a);\n"
                                   "    v_texcoord = a_texcoord;\n"
                                   "    gl_Position = CC_MVPMatrix * a_position;\n"
                                   "}\n";

// CC_MVPMatrix is deliberately not declared - compileShader prepends the uniform block. No #version
// means GLSL 1.10, where fwidth is core, and no #extension line: it would follow the prepended
// tokens, which is why cocos commented its own out. The floor on the derivative matters because
// smoothstep with edge0 == edge1 is undefined and zero-area geometry does occur.
static const char* const kAAFrag = "varying vec4 v_color;\n"
                                   "varying vec2 v_texcoord;\n"
                                   "void main() {\n"
                                   "    float aa = max(length(fwidth(v_texcoord)), 0.0001);\n"
                                   "    gl_FragColor = v_color * smoothstep(0.0, aa, 1.0 - length(v_texcoord));\n"
                                   "}\n";

static CCGLProgram* aaShader() {
    static CCGLProgram* prog = nullptr;
    static bool tried = false;
    if (tried) return prog;

    auto* dir = CCDirector::sharedDirector();
    if (!dir || !dir->getOpenGLView()) return nullptr;   // no GL context yet, try again later
    tried = true;

    auto* p = new CCGLProgram();
    if (!p->initWithVertexShaderByteArray(kAAVert, kAAFrag)) {
        p->release();
        return nullptr;
    }
    // "a_texcoord" is a literal on purpose: kCCAttributeNameTexCoord is "a_texCoord" with a capital
    // C and would silently fail to bind, leaving the driver to fetch from the wrong stream.
    p->addAttribute(kCCAttributeNamePosition, kCCVertexAttrib_Position);
    p->addAttribute("a_texcoord", kCCVertexAttrib_TexCoords);
    p->addAttribute(kCCAttributeNameColor, kCCVertexAttrib_Color);
    p->link();

    // link() cannot be trusted to report failure in a release cocos build, so ask GL directly.
    GLuint id = p->getProgram();
    GLint linked = GL_FALSE;
    if (id) glGetProgramiv(id, GL_LINK_STATUS, &linked);
    bool ok = id && linked == GL_TRUE
        && glGetAttribLocation(id, "a_position") == (GLint)kCCVertexAttrib_Position
        && glGetAttribLocation(id, "a_texcoord") == (GLint)kCCVertexAttrib_TexCoords
        && glGetAttribLocation(id, "a_color") == (GLint)kCCVertexAttrib_Color;
    if (!ok) {
        log::warn("[Click Indicators] smooth lines unavailable on this driver, using stock shader");
        p->release();
        return nullptr;
    }
    p->updateUniforms();
    p->retain();   // held for the process; no node's destructor can outlive it
    prog = p;
    return prog;
}

void applyAA(CCDrawNode* n) {
    if (!n) return;
    if (!Mod::get()->getSettingValue<bool>("smooth-lines")) return;
    // Never setShaderProgram(nullptr): CCDrawNode::draw() does getShaderProgram()->use().
    if (auto* p = aaShader()) n->setShaderProgram(p);
}

// One continuous outline. Built from separate chords this had a joint every few pixels, and each
// joint double-covered its neighbour, so a translucent ring came out beaded - and the dark passes,
// being far wider than the chords were long, piled into overlapping blobs.
void drawRing(CCDrawNode* n, CCPoint c, float r, float w, ccColor4F col) {
    unsigned int seg = (unsigned int)clmp(r * 1.1f, 40.f, 128.f);
    n->drawCircle(c, r, ccColor4F{ 0.f, 0.f, 0.f, 0.f }, w, col, seg);
}

void drawSegOL(CCDrawNode* n, CCPoint a, CCPoint b, float w, ccColor4F col) {
    float k = clmp(g_cueContrast, 0.f, 1.f);
    float halo, edge;
    darkAlpha(col.a, k, halo, edge);
    if (halo > 0.002f) n->drawSegment(a, b, haloWidth(w, k), ccColor4F{ 0.f, 0.f, 0.f, halo });
    if (edge > 0.002f) n->drawSegment(a, b, edgeWidth(w, k), ccColor4F{ 0.f, 0.f, 0.f, edge });
    n->drawSegment(a, b, w, col);
}


void drawRingOL(CCDrawNode* n, CCPoint c, float r, float w, ccColor4F col) {
    float k = clmp(g_cueContrast, 0.f, 1.f);
    float halo, edge;
    darkAlpha(col.a, k, halo, edge);
    // Cap the dark rings so a wide halo can never swallow a small ring's hole.
    float cap = w + r * 0.55f;
    if (halo > 0.002f) drawRing(n, c, r, clmp(haloWidth(w, k), w, cap), ccColor4F{ 0.f, 0.f, 0.f, halo });
    if (edge > 0.002f) drawRing(n, c, r, clmp(edgeWidth(w, k), w, cap), ccColor4F{ 0.f, 0.f, 0.f, edge });
    drawRing(n, c, r, w, col);
}

// mode 1=ring 2=converge 3=pulse. ease goes 1 -> 0 at the press
void drawCue(CCDrawNode* n, int mode, CCPoint c, float bot, float top,
             float ease, bool armed, float s, float cr, float cg, float cb, float op) {
    auto fade = [op](ccColor4F x) { x.a *= op; return x; };
    ccColor4F soft = fade(ccColor4F{ cr, cg, cb, 0.42f });
    ccColor4F cue  = armed ? fade(ccColor4F{ 0.25f, 1.f, 0.45f, 1.f })
                           : fade(ccColor4F{ cr, cg, cb, 0.95f });
    float wid = armed ? 3.8f : 2.2f;
    switch (mode) {
        case 1: {   // ring
            drawRingOL(n, c, 16.f * s, 1.8f, soft);
            for (int k = 0; k < 4; k++) {
                float ang = 0.7853982f + 1.5707963f * k;
                CCPoint d = ccp(cosf(ang), sinf(ang));
                drawSegOL(n, c + d * (16.f * s), c + d * (23.f * s), 1.7f, soft);
            }
            drawRingOL(n, c, (16.f + 104.f * ease) * s, wid, cue);
            if (armed) drawRingOL(n, c, 16.f * s, wid, cue);
            break; }
        case 2: {   // converge
            float dx = 112.f * ease * s;
            for (int sg = -1; sg <= 1; sg += 2) {
                float X = c.x + sg * dx, tip = (sg > 0 ? -10.f : 10.f) * s;
                drawSegOL(n, ccp(X, bot), ccp(X, top), wid, cue);
                drawSegOL(n, ccp(X, c.y + 10.f * s), ccp(X + tip, c.y), wid, cue);
                drawSegOL(n, ccp(X, c.y - 10.f * s), ccp(X + tip, c.y), wid, cue);
            }
            break; }
        case 3: {   // pulse
            // It used to be one line that got brighter, which told you nothing - there was no
            // reference for how bright was bright enough. It now grows outward to a fixed pair of
            // marks: when the moving edges reach them, press. The marks are the whole point.
            float g = 1.f - ease;             // 0 when it appears, 1 at the press
            const float R = 46.f * s;         // where the edges land, and where the marks sit
            float hw = R * g;
            float mh = 26.f * s;

            for (int sg = -1; sg <= 1; sg += 2) {
                float X = c.x + sg * R;
                drawSegOL(n, ccp(X, c.y - mh), ccp(X, c.y + mh), armed ? 3.4f : 2.2f,
                          armed ? cue : fade(ccColor4F{ cr, cg, cb, 0.30f + 0.30f * g }));
            }

            // A body behind the edges, so the growth reads as mass rather than two stray lines.
            if (hw > 0.5f) {
                CCPoint q[4] = { ccp(c.x - hw, bot), ccp(c.x + hw, bot),
                                 ccp(c.x + hw, top), ccp(c.x - hw, top) };
                n->drawPolygon(q, 4, fade(ccColor4F{ cr, cg, cb, 0.05f + 0.11f * g }),
                               0.f, ccColor4F{ 0.f, 0.f, 0.f, 0.f });
            }
            for (int sg = -1; sg <= 1; sg += 2)
                drawSegOL(n, ccp(c.x + sg * hw, bot), ccp(c.x + sg * hw, top), wid, cue);
            break; }
    }
}

// Converge only ever showed the next press. These are the ones after it: the same two walls,
// placed further out and faded by distance, so upcoming clicks slide in from the sides and hand
// straight over to the main pair when they become next. Spacing is linear rather than the main
// cue's eased curve - that curve compresses hard at distance and would pile a whole second of
// clicks into a few pixels.
void drawConvergeQueue(CCDrawNode* n, CCPoint c, float bot, float top,
                       const float* leads, int count, float approachT, float s,
                       float cr, float cg, float cb, float op) {
    if (!n || count <= 0 || approachT <= 0.01f) return;
    const float DX0 = 112.f * s;      // exactly where the main pair starts, so handover is seamless
    const float SPREAD = 130.f * s;
    const float DXMAX = 265.f * s;    // past this it is off the sides of the screen
    for (int i = 0; i < count; i++) {
        float e = leads[i] / approachT;
        if (e <= 1.f) continue;       // the main indicator owns this one
        float dx = DX0 + SPREAD * (e - 1.f);
        if (dx >= DXMAX) continue;
        float a = 0.40f * clmp((DXMAX - dx) / (110.f * s), 0.f, 1.f) * op;
        if (a < 0.02f) continue;
        ccColor4F col{ cr, cg, cb, a };
        for (int sg = -1; sg <= 1; sg += 2)
            drawSegOL(n, ccp(c.x + sg * dx, bot), ccp(c.x + sg * dx, top), 1.3f, col);
    }
}

// Guitar-Hero style single lane. Notes fall toward a fixed strike bar at the bottom and you press
// when a note reaches it; a hold is one note with a tail behind it, so its length is how long to
// stay down. The lane tapers toward the top for depth and carries fret lines so the approach speed
// is readable, which is what makes the real thing legible at speed.
void drawHighway(CCDrawNode* n, CCPoint base, float laneW, float laneH, float span,
                 const HwNote* notes, int count, float phase, bool btnDown,
                 float cr, float cg, float cb, float op, bool guides) {
    if (!n || span <= 0.01f || laneH <= 4.f) return;
    auto fade = [op](ccColor4F c) { c.a *= op; return c; };

    const float taper = 0.34f;
    auto frac = [&](float lead) { return clmp(lead / span, 0.f, 1.f); };
    auto halfW = [&](float f) { return laneW * 0.5f * (1.f - taper * f); };

    auto band = [&](float leadLo, float leadHi, float wScale, ccColor4F col) {
        float f0 = frac(leadLo), f1 = frac(leadHi);
        if (f1 <= f0) return;
        float h0 = halfW(f0) * wScale, h1 = halfW(f1) * wScale;
        float y0 = base.y + laneH * f0, y1 = base.y + laneH * f1;
        CCPoint q[4] = { ccp(base.x - h0, y0), ccp(base.x + h0, y0),
                         ccp(base.x + h1, y1), ccp(base.x - h1, y1) };
        n->drawPolygon(q, 4, col, 0.f, ccColor4F{ 0.f, 0.f, 0.f, 0.f });
    };

    // One colour per meaning, so a glance says what kind of note it is and whether you are doing the
    // right thing. Taps take the guide colour; holds are amber, which is what the release marker
    // already means in the other modes.
    const ccColor4F AMBER = { 1.f, 0.72f, 0.25f, 1.f };
    const ccColor4F GOOD = { 0.30f, 1.f, 0.50f, 1.f };
    const ccColor4F BAD = { 1.f, 0.28f, 0.28f, 1.f };
    auto tint = [&](ccColor4F c, float a) { c.a = a; return fade(c); };

    band(0.f, span, 1.f, fade(ccColor4F{ 0.f, 0.f, 0.f, 0.42f }));

    bool anyArmed = false, riding = false, missed = false, needPress = false;
    float approach = 0.f;
    for (int i = 0; i < count; i++) {
        const HwNote& nt = notes[i];
        bool isHold = nt.hold > 0.02f;
        float tailEnd = nt.lead + (isHold ? nt.hold : 0.f);
        // A tap that was never pressed lingers briefly so it can be shown as missed; one that
        // was pressed, and any hold, clears as soon as it is served.
        float keep = (isHold || nt.done) ? -0.02f : -0.25f;
        if (nt.lead > span || tailEnd < keep) continue;
        if (nt.armed) { anyArmed = true; if (!nt.done) needPress = true; }
        if (isHold && nt.lead <= 0.f && tailEnd > 0.f) riding = true;
        if (!isHold && nt.lead < -0.02f && !nt.done) missed = true;
        const float aw = 0.40f;
        if (nt.lead >= -0.02f && nt.lead < aw) approach = std::max(approach, 1.f - nt.lead / aw);
    }
    // What the input should be right now against what it actually is. This is the feedback.
    bool wrongHold = riding && !btnDown;
    float hot = (anyArmed || riding) ? 1.f : approach;
    float pulse = 0.5f + 0.5f * sinf(phase * 16.f);

    const float hb = halfW(0.f);
    const float zoneSec = span * 0.075f;
    float zf = frac(zoneSec);

    // The window is an outline now rather than a filled block. A fill sat directly under the note at
    // the one place precision matters, which is what turned the crossing into mush.
    band(0.f, zoneSec, 1.f, tint(ccColor4F{ cr, cg, cb, 1.f }, 0.05f + 0.10f * hot));
    drawSegOL(n, ccp(base.x - halfW(zf), base.y + laneH * zf), ccp(base.x + halfW(zf), base.y + laneH * zf),
              1.0f, tint(ccColor4F{ cr, cg, cb, 1.f }, 0.22f + 0.45f * hot));

    // The lane's structure: the two rails and the half-second rhythm ticks. Everything that tells
    // you what to do - notes, chevrons, the hit bar - is drawn regardless.
    if (guides) {
        for (int sg = -1; sg <= 1; sg += 2)
            drawSegOL(n, ccp(base.x + sg * halfW(0.f), base.y), ccp(base.x + sg * halfW(1.f), base.y + laneH),
                      1.4f, tint(ccColor4F{ cr, cg, cb, 1.f }, 0.38f + 0.30f * hot));

        for (float mark = 0.5f; mark < span; mark += 0.5f) {
            float f = frac(mark), h = halfW(f), y = base.y + laneH * f;
            n->drawSegment(ccp(base.x - h, y), ccp(base.x + h, y), 0.9f,
                           tint(ccColor4F{ cr, cg, cb, 1.f }, 0.18f * (1.f - 0.6f * f)));
        }
    }

    if (hot > 0.05f) {
        float cl = 5.f + 9.f * hot;
        ccColor4F ch = wrongHold ? tint(BAD, 0.95f)
                     : (anyArmed || riding) ? tint(GOOD, 0.95f)
                                            : tint(ccColor4F{ cr, cg, cb, 1.f }, 0.35f + 0.6f * hot);
        for (int sg = -1; sg <= 1; sg += 2) {
            float ox = base.x + sg * (hb + 3.f + 4.f * hot);
            drawSegOL(n, ccp(ox + sg * cl, base.y + cl), ccp(ox, base.y), 1.5f + 1.1f * hot, ch);
            drawSegOL(n, ccp(ox + sg * cl, base.y - cl), ccp(ox, base.y), 1.5f + 1.1f * hot, ch);
        }
    }

    for (int i = count - 1; i >= 0; i--) {
        const HwNote& nt = notes[i];
        bool isHold = nt.hold > 0.02f;
        float tailEnd = nt.lead + (isHold ? nt.hold : 0.f);
        float keep = (isHold || nt.done) ? -0.02f : -0.25f;
        if (nt.lead > span) continue;
        if (tailEnd < keep) continue;
        bool rid = isHold && nt.lead <= 0.f && tailEnd > 0.f;
        bool miss = !isHold && nt.lead < -0.02f && !nt.done;

        float f = frac(nt.lead);
        float dim = 1.f - 0.45f * f;
        ccColor4F own = isHold ? AMBER : ccColor4F{ cr, cg, cb, 1.f };

        ccColor4F body = miss              ? tint(BAD, 0.95f)
                       : (rid && !btnDown) ? tint(BAD, 0.95f)
                       : (rid && btnDown)  ? tint(GOOD, 1.f)
                       : nt.done           ? tint(own, 0.40f * dim)
                       : nt.armed          ? tint(GOOD, 1.f)
                                           : tint(own, 0.92f * dim);

        if (isHold) {
            ccColor4F tail = (rid && !btnDown) ? tint(BAD, 0.55f)
                           : rid               ? tint(GOOD, 0.65f)
                           : nt.armed          ? tint(GOOD, 0.55f)
                                               : tint(AMBER, 0.42f * dim);
            band(nt.lead, tailEnd, 0.44f, tail);
        }

        if (!rid || nt.lead > -0.02f) {
            float grow = 1.f + 1.0f * (1.f - f) * (1.f - f);
            float gh = (laneH * 0.020f * (1.f - 0.35f * f) + 2.2f) * grow;
            float half = gh / laneH * span * 0.5f;
            float mid = nt.lead < half ? half : nt.lead;
            band(mid - half, mid + half, nt.armed ? 1.f : 0.86f, body);
            // A hold should read as a hold before its tail is what matters: bright leading edge.
            if (isHold && !nt.done) {
                float ty = base.y + laneH * frac(mid + half);
                float th = halfW(frac(mid + half));
                n->drawSegment(ccp(base.x - th, ty), ccp(base.x + th, ty), 1.3f, tint(AMBER, 0.95f));
            }
        }
    }

    if (riding) {
        float capH = laneH * 0.016f + 2.5f;
        float cw = hb * 0.62f;
        CCPoint cap[4] = { ccp(base.x - cw, base.y), ccp(base.x + cw, base.y),
                           ccp(base.x + cw, base.y + capH), ccp(base.x - cw, base.y + capH) };
        n->drawPolygon(cap, 4, btnDown ? tint(GOOD, 0.9f) : tint(BAD, 0.55f + 0.4f * pulse),
                       0.f, ccColor4F{ 0.f, 0.f, 0.f, 0.f });
    }

    if (anyArmed || riding) {
        ccColor4F g = wrongHold ? BAD : GOOD;
        n->drawDot(ccp(base.x, base.y), hb * (0.5f + 0.4f * pulse), tint(g, 0.26f * (1.f - 0.4f * pulse)));
        if (needPress || wrongHold) n->drawDot(ccp(base.x, base.y), hb * 0.26f, tint(g, 0.8f));
    }

    // The datum. Thin, cool white, and it never takes the note colours - the note moves, the ruler
    // does not. Everything turning green at once is what made the crossing unreadable. Drawn last so
    // it sits over the notes, which is what lets you see one pass under it.
    ccColor4F rule = missed ? tint(BAD, 0.95f) : tint(ccColor4F{ 0.86f, 0.95f, 1.f, 1.f }, 0.95f);
    drawSegOL(n, ccp(base.x - hb - 2.f, base.y), ccp(base.x + hb + 2.f, base.y), 1.5f, rule);
}

// A stroke through many points, built as quads that SHARE their end vertices.
//
// drawSegment gives every line rounded ends, so two segments meeting at a corner overlap by half a
// cap and that overlap composites twice. On a bright, nearly opaque core it is invisible; on the
// black rim passes, which are two to four times wider than the line itself and deliberately
// translucent, it is a dark bead at every corner. A wave route turns on every single click, so the
// path ends up beaded from end to end - which is exactly what "multiple lines connected together"
// looks like. The ring drawing in this file was changed for the same reason.
//
// Sharing the vertices means adjacent quads meet exactly and cover nothing twice. The join is a
// miter, limited so a hairpin cannot throw a spike off the end of it - a wave reverses by 90 degrees
// at big size and about 127 at mini, both of which miter cleanly, but a route with a slide in it can
// produce sharper.
static void strokePass(CCDrawNode* n, const CCPoint* p, int count, float w, ccColor4F col,
                       float border) {
    if (!n || count < 2 || col.a < 0.002f || w < 0.05f) return;
    const float h = w * 0.5f;
    ccColor4F none{ 0.f, 0.f, 0.f, 0.f };
    CCPoint prevL, prevR;
    bool have = false;
    for (int i = 0; i < count; i++) {
        CCPoint d0{ 0.f, 0.f }, d1{ 0.f, 0.f };
        if (i > 0) {
            d0 = ccp(p[i].x - p[i - 1].x, p[i].y - p[i - 1].y);
            float l = std::sqrt(d0.x * d0.x + d0.y * d0.y);
            if (l > 1e-6f) d0 = ccp(d0.x / l, d0.y / l); else d0 = ccp(0.f, 0.f);
        }
        if (i + 1 < count) {
            d1 = ccp(p[i + 1].x - p[i].x, p[i + 1].y - p[i].y);
            float l = std::sqrt(d1.x * d1.x + d1.y * d1.y);
            if (l > 1e-6f) d1 = ccp(d1.x / l, d1.y / l); else d1 = ccp(0.f, 0.f);
        }
        if (d0.x == 0.f && d0.y == 0.f) d0 = d1;
        if (d1.x == 0.f && d1.y == 0.f) d1 = d0;
        if (d0.x == 0.f && d0.y == 0.f) continue;      // a run of identical points
        CCPoint m = ccp(d0.x + d1.x, d0.y + d1.y);
        float ml = std::sqrt(m.x * m.x + m.y * m.y);
        CCPoint nm;
        if (ml < 1e-4f) nm = ccp(-d0.y, d0.x);         // a true reversal: use the segment's own normal
        else {
            m = ccp(m.x / ml, m.y / ml);
            nm = ccp(-m.y, m.x);
            // 1/cos(half angle), which is how far out the miter has to reach to keep the width.
            float c = nm.x * -d0.y + nm.y * d0.x;
            float s = std::fabs(c) > 0.25f ? 1.f / std::fabs(c) : 4.f;
            nm = ccp(nm.x * s, nm.y * s);
        }
        CCPoint L = ccp(p[i].x + nm.x * h, p[i].y + nm.y * h);
        CCPoint R = ccp(p[i].x - nm.x * h, p[i].y - nm.y * h);
        if (have) {
            // Wound so the quad is simple and convex, because CCDrawNode fills from a fan.
            CCPoint q[4] = { prevL, prevR, R, L };
            n->drawPolygon(q, 4, col, border, border > 0.01f ? col : none);
        }
        prevL = L; prevR = R; have = true;
    }
}

void drawPolyOL(CCDrawNode* n, const CCPoint* p, int count, float w, ccColor4F col) {
    if (!n || count < 2) return;
    float k = clmp(g_cueContrast, 0.f, 1.f);
    float halo, edge;
    darkAlpha(col.a, k, halo, edge);
    // The rim passes get no border of their own: they ARE the edge, and a border on a shared seam
    // would be the only place two quads covered the same pixel.
    if (halo > 0.002f) strokePass(n, p, count, haloWidth(w, k), ccColor4F{ 0.f, 0.f, 0.f, halo }, 0.f);
    if (edge > 0.002f) strokePass(n, p, count, edgeWidth(w, k), ccColor4F{ 0.f, 0.f, 0.f, edge }, 0.f);
    // The core carries a hairline border in its own colour, which is what the antialiasing shader
    // needs something to work on - a plain fill has no edge texcoords and comes out stair-stepped.
    strokePass(n, p, count, w, col, 0.45f);
}
