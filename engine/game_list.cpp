// The game list: its gold, starry, striped background and a button per game. Pressing a button and letting go
// over it starts that game. Drawn by main.cpp's frame() while no game is running.
#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "app.h"
#include "gfx.h"
#include "input.h"

#if defined(LOADER_SYNC)
#include "devsync.h"
#endif

#ifndef BUILD_FINGERPRINT
#define BUILD_FINGERPRINT "dev"
#endif

namespace app {

// ---- The game list's background: gold with tilted stars down to just past the list, a sawtooth edge with a
// soft shadow, and grey and white diagonal stripes below. Stars and stripes drift slowly right. Every picture
// is made here once, as small repeating textures, so the app needs no image files for it.
const float STAR_TILT = 15.0f * 3.14159265f / 180.0f;   // the star rows slope down to the right, and each star leans
const int STAR_W = 448, STAR_H = 480;                    // one repeat: 4 columns of stars, 4 rows (row step = STAR_W * tan 15)
const float STAR_SPEED = 8, STRIPE_SPEED = 10;           // 2D units a second
const float STRIPE_TILE = 72, TOOTH_W = 30, TOOTH_H = 14;
const float TOOTH_SHADOW = 4, SHADOW_BLUR = 4, SHADOW_H = 32;   // the teeth's shadow: drop, softness, strip height
gfx::Handle goldTex = 0, fadeTex = 0, starTex = 0, stripeTex = 0, shadowTex = 0, bandTex = 0;

float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

// The gold at (u, v) across the gold area, in sRGB: deep gold, lighter toward the middle right.
void background_gold_at(float u, float v, float out[3]) {
    float d = std::sqrt((u - 0.8f) * (u - 0.8f) + (v - 0.6f) * (v - 0.6f) * 0.5f);
    float t = clamp01(1.0f - d / 0.9f);
    const float dark[3] = {0.78f, 0.51f, 0.09f}, light[3] = {0.94f, 0.67f, 0.17f};
    for (int i = 0; i < 3; i++) out[i] = dark[i] + (light[i] - dark[i]) * t;
}

// Signed distance to a five-pointed star of radius r pointing up (y up), negative inside (Inigo Quilez's).
float star_distance(float px, float py, float r, float inner) {
    const float k1x = 0.809016994f, k1y = -0.587785252f, k2x = -k1x, k2y = k1y;
    px = std::fabs(px);
    float d = std::max(k1x * px + k1y * py, 0.0f) * 2;
    px -= d * k1x; py -= d * k1y;
    d = std::max(k2x * px + k2y * py, 0.0f) * 2;
    px -= d * k2x; py -= d * k2y;
    px = std::fabs(px);
    py -= r;
    float bax = -inner * k1y, bay = inner * k1x - 1;
    float h = std::min(std::max((px * bax + py * bay) / (bax * bax + bay * bay), 0.0f), r);
    float ex = px - bax * h, ey = py - bay * h;
    return std::sqrt(ex * ex + ey * ey) * ((py * bax - px * bay) < 0 ? -1.0f : 1.0f);
}

void make_background() {
    const int GW = 32, GH = 64;
    std::vector<uint8_t> gold(GW * GH * 4), fade(GW * GH * 4);
    for (int y = 0; y < GH; y++)
        for (int x = 0; x < GW; x++) {
            float u = (x + 0.5f) / GW, v = (y + 0.5f) / GH, c[3];
            background_gold_at(u, v, c);
            // The fade sits over the stars, so they seem to come out of and go into the sides, top and teeth.
            float edge = std::max({1 - clamp01(u / 0.2f), 1 - clamp01((1 - u) / 0.2f), 1 - clamp01(v / 0.15f),
                                   1 - clamp01((1 - v) / 0.1f)});
            uint8_t* g = &gold[(y * GW + x) * 4];
            uint8_t* f = &fade[(y * GW + x) * 4];
            for (int i = 0; i < 3; i++) g[i] = f[i] = (uint8_t)(c[i] * 255);
            g[3] = 255;
            f[3] = (uint8_t)(edge * edge * 230);
        }
    goldTex = gfx::engine_texture(GW, GH, gold.data(), gfx::TEX_CLAMP);
    fadeTex = gfx::engine_texture(GW, GH, fade.data(), gfx::TEX_CLAMP);

    // Stars: rounded, leaning STAR_TILT, on a grid whose rows slope by the same angle. The repeat is 4 columns
    // wide, and across it the rows drop exactly one row, so it joins up with itself on every side.
    std::vector<uint8_t> stars(STAR_W * STAR_H * 4);
    const float col = STAR_W / 4.0f, rowStep = STAR_H / 4.0f, slope = rowStep / STAR_W;
    const float cs = std::cos(STAR_TILT), sn = std::sin(STAR_TILT);
    for (int y = 0; y < STAR_H; y++)
        for (int x = 0; x < STAR_W; x++) {
            float best = 1e9f;
            for (int i = 0; i < 4; i++)
                for (int j = 0; j < 4; j++) {
                    float cx = (i + 0.5f) * col, cy = (j + 0.5f) * rowStep + cx * slope;
                    float dx = std::remainder(x + 0.5f - cx, (float)STAR_W), dy = std::remainder(y + 0.5f - cy, (float)STAR_H);
                    if (dx * dx + dy * dy > 50 * 50) continue;
                    float sx = cs * dx + sn * dy, sy = -(-sn * dx + cs * dy);   // into the star's own turned frame, y up
                    best = std::min(best, star_distance(sx, sy, 21, 0.55f) - 4);   // the 4 rounds its corners
                }
            uint8_t* p = &stars[(y * STAR_W + x) * 4];
            p[0] = p[1] = p[2] = 255;
            p[3] = (uint8_t)(clamp01(0.5f - best) * 255);
        }
    starTex = gfx::engine_texture(STAR_W, STAR_H, stars.data(), 0);

    // Stripes running up to the right, white and light grey, two to a repeat, edges smoothed.
    const int S = 64;
    std::vector<uint8_t> stripes(S * S * 4);
    for (int y = 0; y < S; y++)
        for (int x = 0; x < S; x++) {
            float p = std::fmod(x + y + 1.0f, S / 2.0f);                            // 0..32 across one white and one grey
            float white = clamp01(std::min(p, 16 - p) + 0.5f) * clamp01(16.5f - p);   // 1 in the white half, eased edges
            float shade = 0.885f + (0.965f - 0.885f) * white;
            uint8_t* q = &stripes[(y * S + x) * 4];
            q[0] = q[1] = q[2] = (uint8_t)(shade * 255);
            q[3] = 255;
        }
    stripeTex = gfx::engine_texture(S, S, stripes.data(), 0);

    // The teeth's shadow as one repeating strip: the sawtooth's outline moved down TOOTH_SHADOW and blurred.
    // One repeat is one tooth wide (2 texels a unit, so it stays smooth) and SHADOW_H tall; it is clear at
    // the bottom, so nothing builds up where neighbouring teeth meet.
    const int SW = (int)(TOOTH_W * 2), SH = (int)(SHADOW_H * 2);
    std::vector<uint8_t> shade(SW * SH * 4);
    const float edgeSlope = TOOTH_H / (TOOTH_W / 2), along = 1 / std::sqrt(1 + edgeSlope * edgeSlope);
    for (int y = 0; y < SH; y++)
        for (int x = 0; x < SW; x++) {
            float ux = (x + 0.5f) / 2, uy = (y + 0.5f) / 2;                         // in 2D units, y down from the edge
            float lowest = TOOTH_SHADOW + TOOTH_H - std::fabs(ux - TOOTH_W / 2) * edgeSlope;   // the outline's y here
            float d = (uy - lowest) * along;                                          // + below the outline
            uint8_t* q = &shade[(y * SW + x) * 4];
            q[0] = q[1] = q[2] = 255;
            q[3] = (uint8_t)(clamp01(0.5f - d / SHADOW_BLUR) * clamp01((SHADOW_H - 1 - uy) / 3) * 255);
        }
    shadowTex = gfx::engine_texture(SW, SH, shade.data(), gfx::TEX_NOMIP);
    uint8_t band[32 * 4];
    for (int y = 0; y < 32; y++) {
        band[y * 4] = band[y * 4 + 1] = band[y * 4 + 2] = 255;
        band[y * 4 + 3] = (uint8_t)((1 - y / 31.0f) * (1 - y / 31.0f) * 255);
    }
    bandTex = gfx::engine_texture(1, 32, band, gfx::TEX_CLAMP);
}

// goldBottom: where the teeth start. The gold, its stars and its fade are each drawn once over the whole
// shape (a rectangle down to goldBottom plus a triangle per tooth) with one texture mapping, so the colour
// and the stars run straight on into the points with no seam.
void draw_background(float goldBottom) {
    if (!goldTex) make_background();
    float w = gfx::view_width(), h = gfx::view_height(), t = SDL_GetTicks() / 1000.0f, all = goldBottom + TOOTH_H;
    const float white[4] = {1, 1, 1, 1}, shadow[4] = {0, 0, 0, 0.16f}, band[4] = {0, 0, 0, 0.07f}, starTint[4] = {1, 1, 1, 0.11f};
    // ponytail: the pan wraps by fmod so the float never grows; a session of weeks would still be smooth
    float su = std::fmod(t * STRIPE_SPEED / STRIPE_TILE, 1.0f), tu = std::fmod(t * STAR_SPEED / STAR_W, 1.0f);
    const float stripeUv[4] = {-su, goldBottom / STRIPE_TILE, w / STRIPE_TILE - su, h / STRIPE_TILE};
    gfx::image(stripeTex, 0, goldBottom, w, h - goldBottom, white, stripeUv);
    const float bandUv[4] = {0, 0, 1, 1};
    gfx::image(bandTex, 0, goldBottom, w, 60, band, bandUv);
    const float half = 0.5f / (SHADOW_H * 2);   // half a texel: keeps the strip's clear bottom from wrapping to its top
    const float shadowUv[4] = {0, half, w / TOOTH_W, 1 - half};
    gfx::image(shadowTex, 0, goldBottom, w, SHADOW_H, shadow, shadowUv);

    // One layer over the gold shape, its texture spanning u0..u1 across the screen and v0..v1 from 0 to `all`.
    auto layer = [&](gfx::Handle tex, const float tint[4], float u0, float v0, float u1, float v1) {
        auto U = [&](float x) { return u0 + (u1 - u0) * x / w; };
        auto V = [&](float y) { return v0 + (v1 - v0) * y / all; };
        const float rectUv[4] = {u0, v0, u1, V(goldBottom)};
        gfx::image(tex, 0, 0, w, goldBottom, tint, rectUv);
        for (float x = 0; x < w; x += TOOTH_W) {
            float mid = x + TOOTH_W / 2, end = x + TOOTH_W;
            const float xy[6] = {x, goldBottom, mid, all, end, goldBottom};
            const float uv[6] = {U(x), V(goldBottom), U(mid), V(all), U(end), V(goldBottom)};
            gfx::triangle(tex, xy, uv, tint);
        }
    };
    layer(goldTex, white, 0, 0, 1, 1);
    layer(starTex, starTint, -tu, 0, w / STAR_W - tu, all / STAR_H);
    layer(fadeTex, white, 0, 0, 1, 1);
}

void draw_list() {
    gfx::font(uiFont);
    const float title[4] = {1, 1, 1, 1}, dim[4] = {1, 1, 1, 0.8f};
    const float ink[4] = {0.32f, 0.32f, 0.35f, 1}, textShadow[4] = {0.45f, 0.27f, 0.03f, 0.45f};   // ink: text on the stripes
    // Everything sits inside the part of the screen the phone leaves free.
    const float* safe = gfx::safe_area();
    float left = safe[0] + 40, top = safe[1], right = gfx::view_width() - safe[2] - 40, bottom = gfx::view_height() - safe[3];
    size_t columns = (size_t)((right - left - 16) / 16);   // letters of size 16 that fit on a line
    gfx::clear(0.9f, 0.9f, 0.9f);
    float listEnd = top + 130 + (games.empty() ? 60 : games.size() * 84.0f - 12);
    draw_background(std::min(listEnd + 36, gfx::view_height() - 40));   // the gold reaches a little past the list
    gfx::text("GAME LAB", left + 11, top + 51, 40, textShadow);
    gfx::text("GAME LAB", left + 8, top + 48, 40, title);
    gfx::text(std::to_string(games.size()) + (games.size() == 1 ? " GAME" : " GAMES"), left + 8, top + 98, 16, dim);

    float tapX = 0, tapY = 0;
    bool tapped = input::tap(tapX, tapY);
    // Rows grow to HOVER_SCALE under the mouse or a finger. Behind each is the same button with its colours
    // inverted; pressing pulls the face in by PRESS_INSET on every edge (the same distance on each, not a scale),
    // so the inverted copy shows round it as a rim. Both eased; a game starts when the press is let go over the
    // row it began on.
    const float HOVER_SCALE = 1.1f, PRESS_INSET = 6, EASE = 14;   // EASE: how fast they follow, per second
    const float PRESS_DARKEN = 0.2f, DARKEN_TIME = 0.05f;   // the pressed face fades this much darker over this many seconds
    static std::vector<float> grow, sink, shade;   // sink: how far the face is pulled in; shade: 0 to 1 of PRESS_DARKEN
    static int pressedRow = -1;
    static Uint64 lastDraw = 0;
    Uint64 nowTicks = SDL_GetTicks();
    float dt = lastDraw ? std::min((nowTicks - lastDraw) / 1000.0f, 0.1f) : 0;
    lastDraw = nowTicks;
    float follow = 1 - std::exp(-EASE * dt);
    grow.resize(games.size(), 1);
    sink.resize(games.size(), 0);
    shade.resize(games.size(), 0);
    float px = 0, py = 0;
    bool down = false, present = input::pointer(px, py, down);
    float y = top + 130;
    for (size_t i = 0; i < games.size(); i++) {
        const std::string& name = games[i];
        bool over = present && py >= y && py < y + 72 && px >= left && px <= right;
        if (over && down && tapped) pressedRow = (int)i;
        if (over && tapped && !down) pressedRow = (int)i;   // touched and let go within one frame
        if (pressedRow == (int)i && !down) {
            pressedRow = -1;
            if (over) {
                start_game(name);
                return;
            }
        }
        grow[i] += ((over ? HOVER_SCALE : 1) - grow[i]) * follow;
        bool pressing = over && down && pressedRow == (int)i;
        sink[i] += ((pressing ? PRESS_INSET : 0) - sink[i]) * follow;
        shade[i] = std::clamp(shade[i] + (pressing ? 1 : -1) * dt / DARKEN_TIME, 0.0f, 1.0f);
        // The whole button scaled about its centre: the inverted copy, then the face pulled in by `in` per edge.
        float s = grow[i], cx = (left + right) / 2, cy = y + 36, in = sink[i];
        float bw = (right - left) * s, bh = 72 * s, bx = cx - bw / 2, by = cy - bh / 2;
        // Styled like a desktop OK button: a white outer ring, a dark ring, a light grey face, rounded corners
        // and black centred text. The copy behind has every colour inverted.
        // Styled like a desktop OK button: a white outer ring, a see-through gap, a light grey face, rounded
        // corners and black centred text. The copy behind has every colour inverted.
        auto button = [&](float x, float y0, float w, float h, float k, bool invert) {
            const float RING = 3 * s, RADIUS = 14 * s, ringShade = 0.97f, faceShade = 0.94f;
            const float INVERT = 0.5f;   // how far the copy behind goes toward inverted: 1 fully, 0 not at all
            auto tone = [&](float c) { return invert ? c + (1 - 2 * c) * INVERT : c; };
            float v = tone(ringShade) * k, c[4] = {v, v, v, 1};
            // The ring: corner arcs joined into one closed outline, each step a quad between outer and inner edge.
            static gfx::Handle white = 0;
            if (!white) { const uint8_t px[4] = {255, 255, 255, 255}; white = gfx::engine_texture(1, 1, px, 0); }
            const int STEPS = 8;
            float prev[4] = {}, uv[6] = {};
            for (int j = 0; j <= 4 * (STEPS + 1); j++) {
                int corner = (j / (STEPS + 1)) % 4, step = j % (STEPS + 1);   // corners clockwise from top left
                float a = 3.14159265f * (1 + corner * 0.5f + step * 0.5f / STEPS);
                float ccx = corner == 1 || corner == 2 ? x + w - RADIUS : x + RADIUS;
                float ccy = corner >= 2 ? y0 + h - RADIUS : y0 + RADIUS;
                float cur[4] = {ccx + std::cos(a) * RADIUS, ccy + std::sin(a) * RADIUS,
                                ccx + std::cos(a) * (RADIUS - RING), ccy + std::sin(a) * (RADIUS - RING)};
                if (j) {
                    const float t1[6] = {prev[0], prev[1], cur[0], cur[1], cur[2], cur[3]};
                    const float t2[6] = {prev[0], prev[1], cur[2], cur[3], prev[2], prev[3]};
                    gfx::triangle(white, t1, uv, c);
                    gfx::triangle(white, t2, uv, c);
                }
                memcpy(prev, cur, sizeof prev);
            }
            v = tone(faceShade) * k;
            const float f[4] = {v, v, v, 1};
            float o = 2 * RING, r = RADIUS - o, lx = x + o, ly = y0 + o, lw = w - 2 * o, lh = h - 2 * o;
            gfx::rect(lx + r, ly, lw - 2 * r, lh, f);
            gfx::rect(lx, ly + r, lw, lh - 2 * r, f);
            for (int corner = 0; corner < 4; corner++)
                gfx::circle(corner & 1 ? lx + lw - r : lx + r, corner & 2 ? ly + lh - r : ly + r, r, f);
        };
        button(bx, by, bw, bh, 1, true);
        float k = 1 - PRESS_DARKEN * shade[i];
        button(bx + in, by + in, bw - 2 * in, bh - 2 * in, k, false);
        const float label[4] = {0.05f * k, 0.05f * k, 0.05f * k, 1};
        std::string shown = name;   // "dream-table" is shown as "Dream table"
        std::replace(shown.begin(), shown.end(), '-', ' ');
        if (!shown.empty()) shown[0] = (char)std::toupper((unsigned char)shown[0]);
        gfx::text(shown, cx - gfx::text_width(shown, 24 * s) / 2, cy - 12 * s, 24 * s, label);
        y += 84;
    }
    if (games.empty()) gfx::text("No games here yet.", left + 8, y + 10, 22, dim);

    std::string status;
#if defined(LOADER_SYNC)
    // The PC is holding a build made from newer C++ than this app. iOS doesn't let an app replace itself,
    // and LiveContainer won't install anything while an app is running inside it, so the button only
    // downloads the file through Safari. Installing it is done by hand in LiveContainer.
    std::string offered = devsync::available_build();
    if (!offered.empty() && offered != BUILD_FINGERPRINT) {
        const float button[4] = {0.36f, 0.82f, 0.63f, 1}, dark[4] = {0.04f, 0.10f, 0.08f, 1};
        gfx::text(wrap_text("NEW APP BUILD " + offered + " IS READY. DOWNLOAD IT, THEN IN LIVECONTAINER TAP + AND PICK GAMELAB.IPA", columns),
                  left + 8, bottom - 216, 16, ink);
        gfx::rect(left, bottom - 142, right - left, 60, button);
        gfx::text("DOWNLOAD UPDATE", left + (right - left - gfx::text_width("DOWNLOAD UPDATE", 20)) / 2, bottom - 122, 20, dark);
        if (tapped && tapY >= bottom - 142 && tapY < bottom - 82 && tapX >= left && tapX <= right) SDL_OpenURL(devsync::update_url().c_str());
    }
    status = devsync::status();
#else
    status = "Running from the repo. Edit a game's files and it reloads.";
#endif
    gfx::text(wrap_text(status, columns), left + 8, bottom - 60, 16, ink);
    std::string build = std::string("build ") + BUILD_FINGERPRINT;
    gfx::text(build, right - 8 - gfx::text_width(build, 12), top + 56, 12, dim);
}

}  // namespace app
