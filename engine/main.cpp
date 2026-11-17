// The app itself: a window, the game list, and the loop that runs one game at a time.
//
// Three builds come from this file:
//   PC          run and edit games straight from the repo's games/ folder
//   Loader      the phone app that holds every game and syncs with the PC over Wi-Fi
//   Release     one game on its own (SINGLE_GAME), with no game list and no syncing

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "audio.h"
#include "gfx.h"
#include "input.h"
#include "model.h"
#include "net.h"
#include "script.h"

#if defined(LOADER_SYNC)
#include "sync.h"
#endif

#ifndef BUILD_FINGERPRINT
#define BUILD_FINGERPRINT "dev"
#endif

namespace {

SDL_Window* window = nullptr;
std::string bundledDir;    // games that ship inside the app (on the PC: the repo's games/ folder)
std::string syncedDir;     // games downloaded from the PC (loader only)
std::string savesDir;
std::vector<std::string> games;
std::string current;       // the running game, or empty while the game list is showing
std::string fatal;         // a start-up problem to show instead of anything else
bool paused = false;
Uint64 lastTicks = 0, lastWatch = 0, lastListRefresh = 0;
Uint64 watchStamp = 0;
bool showHud = false;
float smoothedDt = 1.0f / 60.0f;
std::string loadedDir;     // the folder whose files the renderer and mixer are holding
std::string orientation;   // the running game's game.cfg choice: "landscape", "portrait" or "" for either
std::string logPath, runningMark, lastCrash;
SDL_IOStream* logFile = nullptr;
SDL_LogOutputFunction defaultLog = nullptr;

bool exists(const std::string& path) { return SDL_GetPathInfo(path.c_str(), nullptr); }

std::string game_dir(const std::string& name) {
    if (!syncedDir.empty() && exists(syncedDir + "/" + name + "/main.lua")) return syncedDir + "/" + name;
    return bundledDir + "/" + name;
}

void add_games_from(const std::string& dir) {
    if (dir.empty()) return;
    int count = 0;
    char** entries = SDL_GlobDirectory(dir.c_str(), "*", 0, &count);
    for (int i = 0; entries && i < count; i++) {
        std::string name = entries[i];
        if (exists(dir + "/" + name + "/main.lua") && std::find(games.begin(), games.end(), name) == games.end()) games.push_back(name);
    }
    SDL_free(entries);
}

void refresh_games() {
    games.clear();
    add_games_from(syncedDir);
    add_games_from(bundledDir);
    std::sort(games.begin(), games.end());
}

// A number that changes whenever any file in the game's folder is added, edited or removed.
Uint64 folder_stamp(const std::string& dir) {
    Uint64 stamp = 1469598103934665603ull;
    int count = 0;
    char** entries = SDL_GlobDirectory(dir.c_str(), nullptr, 0, &count);
    for (int i = 0; entries && i < count; i++) {
        SDL_PathInfo info;
        if (!SDL_GetPathInfo((dir + "/" + entries[i]).c_str(), &info) || info.type != SDL_PATHTYPE_FILE) continue;
        for (const char* c = entries[i]; *c; c++) stamp = (stamp ^ (unsigned char)*c) * 1099511628211ull;
        stamp = (stamp ^ (Uint64)info.modify_time) * 1099511628211ull;
        stamp = (stamp ^ (Uint64)info.size) * 1099511628211ull;
    }
    SDL_free(entries);
    return stamp;
}

void on_log(const char* level, const std::string& message) {
#if defined(LOADER_SYNC)
    devsync::log(level, current, message);
#else
    (void)level;
    (void)message;
#endif
}

// Everything the engine logs also goes to a file, so a phone that was away from the PC can still say what happened.
void SDLCALL log_to_file(void* userdata, int category, SDL_LogPriority priority, const char* message) {
    if (defaultLog) defaultLog(userdata, category, priority, message);
    if (logFile) {
        SDL_WriteIO(logFile, message, SDL_strlen(message));
        SDL_WriteIO(logFile, "\n", 1);
        SDL_FlushIO(logFile);
    }
}

// A game's optional game.cfg: lines of  key = value.
std::string config_value(const std::string& dir, const std::string& key) {
    size_t size = 0;
    char* data = (char*)SDL_LoadFile((dir + "/game.cfg").c_str(), &size);
    if (!data) return "";
    std::string text(data, size), found;
    SDL_free(data);
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        start = end + 1;
        size_t equals = line.find('=');
        if (equals == std::string::npos || line[0] == '#') continue;
        auto trim = [](std::string s) {
            size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
            return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
        };
        if (trim(line.substr(0, equals)) == key) found = trim(line.substr(equals + 1));
    }
    return found;
}

// Which ways up the phone may be held. iOS asks again each time the phone is turned.
void apply_orientation(const std::string& wanted) {
    orientation = wanted == "landscape" || wanted == "portrait" ? wanted : "";
    SDL_SetHint(SDL_HINT_ORIENTATIONS, orientation == "landscape" ? "LandscapeLeft LandscapeRight"
                                     : orientation == "portrait" ? "Portrait" : "LandscapeLeft LandscapeRight Portrait");
}

// Lets go of the last game's files. On a reload of the same game, files that have not changed are kept.
void release_assets(const std::string& nextDir) {
    bool everything = nextDir != loadedDir;
    script::unload();
    model::release(everything);
    audio::release(everything);
    gfx::set_game_dir(nextDir);
    loadedDir = nextDir;
}

void start_game(const std::string& name) {
    current = name;
    std::string dir = game_dir(name);
    release_assets(dir);
    apply_orientation(config_value(dir, "orientation"));
    net::set_hosts(config_value(dir, "hosts"));
    net::set_backend(name, config_value(dir, "backend"));
    script::load(dir, savesDir + "/" + name + ".txt");
    watchStamp = folder_stamp(dir);
}

void show_list() {
    release_assets("");
    apply_orientation("");
    current.clear();
    refresh_games();
}

// Game Lab's own text is in MyDotFont (engine/fonts). Games keep their own fonts (the 8x8 one unless they pick one).
gfx::Handle uiFont = 0;

// Frame time, drawing load and memory, in a corner. F3 on the PC, sys.hud(true) from a game.
void draw_hud() {
    gfx::font(uiFont);
    gfx::Stats s = gfx::stats();
    char line[160];
    SDL_snprintf(line, sizeof(line), "%3.0f FPS %4.1f MS\n%d DRAWS %dK VERTS\n%d TEX %d MESH\nLUA %d KB  %d SOUNDS", 1.0f / smoothedDt, smoothedDt * 1000.0f,
                 s.draws, s.vertices / 1000, s.textures, s.meshes, script::memory_kb(), audio::voices());
    const float back[4] = {0, 0, 0, 0.55f}, ink[4] = {0.6f, 1.0f, 0.7f, 1};
    const float* safe = gfx::safe_area();
    float x = gfx::view_width() - safe[2] - 300, y = safe[1] + 12;
    gfx::rect(x - 8, y - 8, 296, 76, back);
    gfx::text(line, x, y, 12, ink);
}

// Breaks long text into lines that fit the screen, since errors can be long.
std::string wrap(const std::string& text, size_t width) {
    std::string out;
    size_t column = 0;
    for (char c : text) {
        if (c == '\t') c = ' ';
        if (c == '\n') column = 0;
        else if (++column > width) {
            out += '\n';
            column = 1;
        }
        out += c;
    }
    return out;
}

void draw_problem(const std::string& title, const std::string& detail) {
    gfx::font(uiFont);
    const float panel[4] = {0.10f, 0.02f, 0.03f, 0.92f}, red[4] = {1.0f, 0.45f, 0.42f, 1}, white[4] = {1, 1, 1, 0.92f};
    float w = gfx::view_width();
    gfx::rect(0, 0, w, gfx::view_height(), panel);
    gfx::text(title, 40, 60, 28, red);
    gfx::text(wrap(detail, (size_t)((w - 80) / 16)), 40, 120, 16, white);
}

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
void gold_at(float u, float v, float out[3]) {
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
            gold_at(u, v, c);
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
    const float title[4] = {1, 1, 1, 1}, dim[4] = {1, 1, 1, 0.8f}, row[4] = {1, 1, 1, 0.2f}, accent[4] = {1, 1, 1, 0.9f};
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
    // Rows grow to HOVER_SCALE under the mouse or a finger and sink PRESS_SHIFT of their height while pressed,
    // both eased; a game starts when the press is let go over the row it began on.
    const float HOVER_SCALE = 1.1f, PRESS_SHIFT = 0.05f, EASE = 14;   // EASE: how fast they follow, per second
    static std::vector<float> grow, sink;
    static int pressedRow = -1;
    static Uint64 lastDraw = 0;
    Uint64 nowTicks = SDL_GetTicks();
    float dt = lastDraw ? std::min((nowTicks - lastDraw) / 1000.0f, 0.1f) : 0;
    lastDraw = nowTicks;
    float follow = 1 - std::exp(-EASE * dt);
    grow.resize(games.size(), 1);
    sink.resize(games.size(), 0);
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
        sink[i] += ((over && down && pressedRow == (int)i ? PRESS_SHIFT * 72 : 0) - sink[i]) * follow;
        // The whole button scaled about its centre, then moved down while pressed.
        float s = grow[i], cx = (left + right) / 2, cy = y + 36 + sink[i];
        float bw = (right - left) * s, bh = 72 * s, bx = cx - bw / 2, by = cy - bh / 2;
        gfx::rect(bx, by, bw, bh, row);
        gfx::rect(bx, by, 6 * s, bh, accent);
        gfx::text(name, bx + 34 * s, by + 26 * s, 24 * s, textShadow);
        gfx::text(name, bx + 32 * s, by + 24 * s, 24 * s, title);
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
        gfx::text(wrap("NEW APP BUILD " + offered + " IS READY. DOWNLOAD IT, THEN IN LIVECONTAINER TAP + AND PICK GAMELAB.IPA", columns),
                  left + 8, bottom - 216, 16, ink);
        gfx::rect(left, bottom - 142, right - left, 60, button);
        gfx::text("DOWNLOAD UPDATE", left + (right - left - gfx::text_width("DOWNLOAD UPDATE", 20)) / 2, bottom - 122, 20, dark);
        if (tapped && tapY >= bottom - 142 && tapY < bottom - 82 && tapX >= left && tapX <= right) SDL_OpenURL(devsync::update_url().c_str());
    }
    status = devsync::status();
#else
    status = "Running from the repo. Edit a game's files and it reloads.";
#endif
    gfx::text(wrap(status, columns), left + 8, bottom - 60, 16, ink);
    std::string build = std::string("build ") + BUILD_FINGERPRINT;
    gfx::text(build, right - 8 - gfx::text_width(build, 12), top + 56, 12, dim);
}

#if !defined(SDL_PLATFORM_IOS)
// ---- PC only: an iPhone's rounded corners, notch or Dynamic Island and home bar, drawn over the window,
// so a layout can be checked against the parts of the screen a real phone hides.
// F1 picks the phone, F2 turns it. GAMELAB_DEVICE=3 starts with the third one in this list.
// Sizes are in iPhone points with the phone upright. They are close, not exact.
struct Phone {
    const char* name;
    float w, h, corner;
    bool island;                  // a Dynamic Island (floating pill) rather than a notch
    float cutW, cutH, cutTop;     // the notch or island: its size, and its gap from the top edge
    float body;                   // how far the phone's black body reaches past the screen on every side
};
const Phone phones[] = {
    {"iPhone 11 / XR", 414, 896, 41.5f, false, 230, 33, 0, 36},
    {"iPhone 11 Pro", 375, 812, 39, false, 209, 30, 0, 27},
    {"iPhone 11 Pro Max", 414, 896, 39, false, 209, 30, 0, 27},
    {"iPhone 12 mini / 13 mini", 375, 812, 44, false, 211, 32, 0, 21},   // the 12 mini's notch, the wider of the two
    {"iPhone 12 / 12 Pro", 390, 844, 47.3f, false, 211, 32, 0, 21},
    {"iPhone 12 Pro Max", 428, 926, 53.3f, false, 211, 32, 0, 21},
    {"iPhone 13 / 13 Pro / 14", 390, 844, 47.3f, false, 162, 34, 0, 21},
    {"iPhone 13 Pro Max / 14 Plus", 428, 926, 53.3f, false, 162, 34, 0, 21},
    {"iPhone 14 Pro / 15 / 15 Pro / 16", 393, 852, 55, true, 126, 37, 11, 19},
    {"iPhone 14 Pro Max / 15 Plus / 15 Pro Max / 16 Plus", 430, 932, 55, true, 126, 37, 11, 18},
    {"iPhone 16 Pro / 17", 402, 874, 62, true, 126, 37, 14, 14},
    {"iPhone 16 Pro Max / 17 Pro Max", 440, 956, 62, true, 126, 37, 14, 14},
};
const int phoneCount = (int)(sizeof(phones) / sizeof(phones[0]));
int phoneIndex = -1;          // -1: no phone frame
bool phoneSideways = true;

// Resizes the window to the chosen phone's shape, as large as its real size or the monitor allows.
void apply_phone() {
    SDL_RestoreWindow(window);
    if (phoneIndex < 0) {
        gfx::set_bezel(0);
        SDL_SetWindowSize(window, 1280, 720);
        SDL_SetWindowTitle(window, "Game Lab   (F1: iPhone frame)");
    } else {
        // The window is the whole phone: the screen plus the black body around it.
        const Phone& p = phones[phoneIndex];
        float w = (phoneSideways ? p.h : p.w) + 2 * p.body, h = (phoneSideways ? p.w : p.h) + 2 * p.body;
        float scale = SDL_GetWindowDisplayScale(window);
        SDL_Rect usable;
        if (SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(window), &usable))
            scale = SDL_min(scale, SDL_min((usable.w - 40) / w, (usable.h - 80) / h));
        if (scale <= 0) scale = 1;
        gfx::set_bezel((int)(p.body * scale * SDL_GetWindowPixelDensity(window)));
        SDL_SetWindowSize(window, (int)(w * scale), (int)(h * scale));
        SDL_SetWindowTitle(window, (std::string("Game Lab - ") + p.name + "   (F1: next phone, F2: turn)").c_str());
    }
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
}

// Where the current phone hides the screen, in 2D units: {x, y, width, height} of the notch or island and
// of the home bar, and the radius of the rounded corners. The drawing and the layout check both use this.
struct Hidden {
    float corner, cut[4], bar[4];
};

Hidden phone_hidden(const Phone& p) {
    float w = gfx::view_width(), h = gfx::view_height();
    float k = 720.0f / p.w;   // 2D units per iPhone point: the screen's shorter side is always 720 units
    Hidden z;
    z.corner = p.corner * k;
    // The notch or island sits in the middle of the phone's top edge, which is the left edge when held sideways.
    float along = p.cutW * k, into = p.cutH * k, gap = p.cutTop * k;
    if (w > h) { z.cut[0] = gap; z.cut[1] = (h - along) / 2; z.cut[2] = into; z.cut[3] = along; }
    else { z.cut[0] = (w - along) / 2; z.cut[1] = gap; z.cut[2] = along; z.cut[3] = into; }
    // The home bar runs along the bottom, whichever way the phone is held.
    z.bar[2] = (w > h ? 0.26f : 0.36f) * w;
    z.bar[3] = 5 * k;
    z.bar[0] = (w - z.bar[2]) / 2;
    z.bar[1] = h - 8 * k - z.bar[3];
    return z;
}

// A lozenge filling the given box: a rectangle with fully rounded ends.
void pill(const float b[4], const float rgba[4]) {
    float r = SDL_min(b[2], b[3]) / 2;
    if (b[2] > b[3]) {
        gfx::rect(b[0] + r, b[1], b[2] - 2 * r, b[3], rgba);
        gfx::circle(b[0] + r, b[1] + r, r, rgba);
        gfx::circle(b[0] + b[2] - r, b[1] + r, r, rgba);
    } else {
        gfx::rect(b[0], b[1] + r, b[2], b[3] - 2 * r, rgba);
        gfx::circle(b[0] + r, b[1] + r, r, rgba);
        gfx::circle(b[0] + r, b[1] + b[3] - r, r, rgba);
    }
}

void draw_phone_frame() {
    gfx::ui_log(false);   // the frame itself is not part of the game's layout
    if (phoneIndex < 0) return;
    const Phone& p = phones[phoneIndex];
    const float black[4] = {0, 0, 0, 1}, white[4] = {1, 1, 1, 0.75f};
    float w = gfx::view_width(), h = gfx::view_height();
    Hidden z = phone_hidden(p);

    // Rounded corners, built from thin strips that get wider towards each corner.
    float r = z.corner;
    const int strips = 40;
    for (int i = 0; i < strips; i++) {
        float step = r / strips, y = step * i, dy = r - (y + step / 2);
        float inset = r - SDL_sqrtf(r * r - dy * dy);
        gfx::rect(0, y, inset, step, black);
        gfx::rect(w - inset, y, inset, step, black);
        gfx::rect(0, h - y - step, inset, step, black);
        gfx::rect(w - inset, h - y - step, inset, step, black);
    }

    if (p.island) {
        pill(z.cut, black);
    } else {
        // A notch is square against the phone's edge and rounded on the side that reaches into the screen.
        const float* c = z.cut;
        float round = 20 * 720.0f / p.w;
        if (w > h) {
            gfx::rect(c[0], c[1], c[2] - round, c[3], black);
            gfx::rect(c[0] + c[2] - round, c[1] + round, round, c[3] - 2 * round, black);
            gfx::circle(c[0] + c[2] - round, c[1] + round, round, black);
            gfx::circle(c[0] + c[2] - round, c[1] + c[3] - round, round, black);
        } else {
            gfx::rect(c[0], c[1], c[2], c[3] - round, black);
            gfx::rect(c[0] + round, c[1] + c[3] - round, c[2] - 2 * round, round, black);
            gfx::circle(c[0] + round, c[1] + c[3] - round, round, black);
            gfx::circle(c[0] + c[2] - round, c[1] + c[3] - round, round, black);
        }
    }
    pill(z.bar, white);
}

// ---- the layout check: `gamelab --check <game>` (or `python tools/dev.py check <game>`) ----
// Runs the game inside every phone above, sideways and upright, and prints each piece of 2D drawing or
// touch control that a corner, notch, island or home bar would cover. It only sees what the game draws in
// its first moments, so screens reached later (menus, game over) are not checked.
bool checking = false;
int checkStep = -1, checkFrames = 0;
std::map<std::string, std::vector<std::string>> checkFound;   // problem -> phones it happens on

bool overlaps(const gfx::UiBox& b, const float z[4]) {
    if (!b.round) return b.x < z[0] + z[2] && b.x + b.w > z[0] && b.y < z[1] + z[3] && b.y + b.h > z[1];
    float cx = b.x + b.w / 2, cy = b.y + b.h / 2;
    float dx = cx - SDL_clamp(cx, z[0], z[0] + z[2]), dy = cy - SDL_clamp(cy, z[1], z[1] + z[3]);
    return dx * dx + dy * dy < b.w * b.w / 4;
}

// True if the box reaches past the rounded corner nearest to it.
bool cut_by_corner(const gfx::UiBox& b, float r, float w, float h, std::string& which) {
    for (int corner = 0; corner < 4; corner++) {
        bool right = (corner & 1) != 0, bottom = (corner & 2) != 0;
        // Measure from this corner, so every case looks like the top-left one.
        float x = right ? w - (b.x + b.w) : b.x, y = bottom ? h - (b.y + b.h) : b.y;
        bool cut;
        if (b.round) {
            float cx = x + b.w / 2, cy = y + b.h / 2, dx = r - cx, dy = r - cy;
            cut = cx < r && cy < r && SDL_sqrtf(dx * dx + dy * dy) + b.w / 2 > r;
        } else {
            cut = x < r && y < r && (r - x) * (r - x) + (r - y) * (r - y) > r * r;
        }
        if (cut) {
            which = std::string(bottom ? "bottom-" : "top-") + (right ? "right" : "left") + " corner";
            return true;
        }
    }
    return false;
}

void check_frame() {
    const Phone& p = phones[phoneIndex];
    float w = gfx::view_width(), h = gfx::view_height();
    Hidden z = phone_hidden(p);
    std::string way = w > h ? "SIDEWAYS" : "UPRIGHT";
    if (!script::error().empty()) checkFound[way + "\tthe game stopped with an error: " + script::error()].push_back(p.name);
    for (const gfx::UiBox& b : gfx::ui_boxes()) {
        if (b.w <= 0 || b.h <= 0 || b.w * b.h > 0.6f * w * h) continue;   // nothing, or a background
        std::string what;
        if (!b.text.empty()) {
            what = "text \"" + b.text + "\"";
        } else {
            float cx = b.x + b.w / 2, cy = b.y + b.h / 2;
            what = std::string(b.round ? "circle " : "rect ") + std::to_string((int)b.w) + "x" + std::to_string((int)b.h) + " in the " +
                   (cy < h / 3 ? "top " : cy > h * 2 / 3 ? "bottom " : "middle ") + (cx < w / 3 ? "left" : cx > w * 2 / 3 ? "right" : "centre");
        }
        std::string where;
        std::vector<std::string> hits;
        if (overlaps(b, z.cut)) hits.push_back(p.island ? "Dynamic Island" : "notch");
        if (overlaps(b, z.bar)) hits.push_back("home bar");
        if (cut_by_corner(b, z.corner, w, h, where)) hits.push_back(where);
        for (const std::string& hit : hits) {
            std::vector<std::string>& list = checkFound[way + "\t" + what + "  ->  " + hit];
            if (list.empty() || list.back() != p.name) list.push_back(p.name);
        }
    }
}

// Called at the start of every frame while checking: a few frames per phone, then the report.
void check_tick() {
    if (checkStep >= 0) {
        checkFrames++;
        if (checkFrames == 4) start_game(current);   // restart once the window has its new shape
        if (checkFrames < 12) return;
        check_frame();
    }
    // A game that only ever runs one way up is only checked that way up.
    ++checkStep;
    while (checkStep < phoneCount * 2 && ((checkStep % 2 == 0 && orientation == "portrait") || (checkStep % 2 == 1 && orientation == "landscape"))) ++checkStep;
    if (checkStep >= phoneCount * 2) {
        printf("\nLayout check for '%s': %d iPhones, %s.\n", current.c_str(), phoneCount,
               orientation == "portrait" ? "upright" : orientation == "landscape" ? "sideways" : "sideways and upright");
        if (checkFound.empty()) printf("Nothing is covered on any of them.\n");
        std::string way;
        for (const auto& found : checkFound) {
            size_t tab = found.first.find('\t');
            if (found.first.substr(0, tab) != way) {
                way = found.first.substr(0, tab);
                printf("\n%s\n", way.c_str());
            }
            printf("  %s\n", found.first.substr(tab + 1).c_str());
            if ((int)found.second.size() == phoneCount) printf("      on every phone\n");
            else for (const std::string& name : found.second) printf("      on %s\n", name.c_str());
        }
        printf("\nOnly what the game draws when it starts was checked. Phone sizes are approximate.\n");
        fflush(stdout);
        checking = false;
        SDL_Event quit = {};
        quit.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&quit);
        return;
    }
    phoneIndex = checkStep / 2;
    phoneSideways = checkStep % 2 == 0;
    apply_phone();
    checkFrames = 0;
}
#else
void draw_phone_frame() {}
#endif

// Tells the renderer which edges of the screen the phone keeps for itself, so the game list and the games
// can stay clear of them. On the iPhone this is what iOS reports. On the PC it comes from the phone frame
// being shown, if there is one.
void update_safe_area() {
    float safe[4] = {0, 0, 0, 0};
    float w = gfx::view_width(), h = gfx::view_height();
#if defined(SDL_PLATFORM_IOS)
    SDL_Rect area;
    int pointsW = 0, pointsH = 0;
    if (SDL_GetWindowSafeArea(window, &area) && SDL_GetWindowSize(window, &pointsW, &pointsH) && pointsW > 0 && pointsH > 0) {
        float kx = w / pointsW, ky = h / pointsH;
        safe[0] = area.x * kx;
        safe[1] = area.y * ky;
        safe[2] = (pointsW - area.x - area.w) * kx;
        safe[3] = (pointsH - area.y - area.h) * ky;
    }
#else
    if (phoneIndex >= 0) {
        const Phone& p = phones[phoneIndex];
        float k = 720.0f / p.w;
        // iOS keeps a strip a little deeper than the notch or island, on both ends when the phone is sideways,
        // and a thinner one above the home bar.
        float cut = (p.cutTop + p.cutH + (p.island ? 11 : 14)) * k;
        if (w > h) { safe[0] = safe[2] = cut; safe[3] = 21 * k; }
        else { safe[1] = cut; safe[3] = 34 * k; }
    }
#endif
    gfx::set_safe_area(safe);
}

void frame() {
#if !defined(SDL_PLATFORM_IOS)
    if (checking && !current.empty()) check_tick();
#endif
    Uint64 now = SDL_GetTicks();
    float dt = SDL_min((now - lastTicks) / 1000.0f, 0.05f);
    lastTicks = now;
    smoothedDt += (dt - smoothedDt) * 0.1f;

#if defined(LOADER_SYNC)
    devsync::Command command;
    while (devsync::next_command(command)) {
        refresh_games();
        if (command.action == "play" && std::find(games.begin(), games.end(), command.game) != games.end()) start_game(command.game);
        else if (command.action == "menu" && !current.empty()) show_list();
        else if (command.action == "reload" && !current.empty()) start_game(current);
        else if (command.action == "lua") script::eval(command.game);
    }
    if (!lastCrash.empty()) {
        devsync::log("error", "", lastCrash);   // held until the PC is found
        lastCrash.clear();
    }
    bool syncing = devsync::busy();
#else
    bool syncing = false;
#endif

    // Reload the running game when its files change, on the PC and on the phone alike.
    if (!current.empty() && !syncing && now - lastWatch > 400) {
        lastWatch = now;
        Uint64 stamp = folder_stamp(game_dir(current));
        if (stamp != watchStamp) start_game(current);
    }
    if (current.empty() && now - lastListRefresh > 1000) {
        lastListRefresh = now;
        refresh_games();
    }

    if (!current.empty() && fatal.empty()) script::update(dt);

    if (!gfx::begin_frame()) {
        input::end_frame();
        return;
    }
    update_safe_area();
    if (!fatal.empty()) {
        draw_problem("CAN'T START", fatal);
    } else if (current.empty()) {
        draw_list();
    } else {
#if !defined(SDL_PLATFORM_IOS)
        gfx::ui_log(checking);
#endif
        gfx::font(0);   // a game starts each frame on its own font, not Game Lab's
        script::draw();
        // The engine's own drawing must not inherit a shader, texture or target the game left set.
        gfx::target(0);
        gfx::reset_state();
        input::draw();
        gfx::font(uiFont);
        if (showHud || script::hud_wanted()) draw_hud();
#if !defined(SINGLE_GAME)
        // A small button to get back to the game list, tucked into the screen's top-left corner. It sits on the
        // diagonal out of the corner, the same distance from the curve all the way round. iOS doesn't say how round
        // the corners are, so the curve is taken to be nearly as deep as the strip kept for the notch or island.
        const float back[4] = {0, 0, 0, 0.4f}, ink[4] = {1, 1, 1, 0.9f};
        const float* safe = gfx::safe_area();
        float curve = 0.95f * SDL_max(safe[0], safe[1]), gap = 14, backX = 44;
        if (curve > 20 + gap) backX = curve - (curve - 20 - gap) * 0.7071f;
        float backY = backX;
        gfx::circle(backX, backY, 20, back);
        gfx::text("X", backX - gfx::text_width("X", 16) / 2, backY - 8, 16, ink);
        float tapX = 0, tapY = 0;
        bool leave = input::tap(tapX, tapY) && tapX < backX + 40 && tapY < backY + 40;
#else
        bool leave = false;
#endif
        if (!script::error().empty()) draw_problem("ERROR IN " + current, script::error());
        else if (!gfx::shader_error().empty()) {
            const float red[4] = {1.0f, 0.45f, 0.42f, 1};
            gfx::text(wrap(gfx::shader_error(), (size_t)((gfx::view_width() - 80) / 14)), 80, 28, 14, red);
        }
        if (leave) {
            draw_phone_frame();
            gfx::end_frame(script::game_time());
            input::end_frame();
            show_list();
            return;
        }
    }
    draw_phone_frame();
    gfx::end_frame(script::game_time());
    input::end_frame();

#if defined(GFX_SCREENSHOT)
    // Debugging aid: GAMELAB_SHOT=file.bmp saves the 30th frame and quits.
    static int frames = 0;
    const char* shot = SDL_getenv("GAMELAB_SHOT");
    const char* when = SDL_getenv("GAMELAB_SHOT_FRAME");
    if (shot && ++frames == (when ? SDL_atoi(when) : 30)) {
        gfx::screenshot(shot);
        SDL_Event quit = {};
        quit.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&quit);
    }
#endif
}

}  // namespace

SDL_AppResult SDL_AppInit(void** appstate, int argc, char** argv) {
    (void)appstate;
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight Portrait");
    SDL_SetAppMetadata("Game Lab", "1.0", "com.starter.gamelab");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE;
#if defined(SDL_PLATFORM_IOS)
    flags |= SDL_WINDOW_FULLSCREEN | SDL_WINDOW_BORDERLESS;
#endif
    int width = 1280, height = 720;
    if (const char* size = SDL_getenv("GAMELAB_WINDOW")) SDL_sscanf(size, "%dx%d", &width, &height);   // e.g. 414x896 to try a phone shape
    window = SDL_CreateWindow("Game Lab", width, height, flags);
    if (!window) {
        SDL_Log("Couldn't open a window: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    SDL_DisableScreenSaver();   // keeps the phone awake while it sits beside the PC

    // Where things live. On the PC the paths point into the repo; in the app they point inside the bundle.
    std::string engineShaders;
    std::string startGame;
    const char* base = SDL_GetBasePath();
    std::string appDir = base ? base : "";
#if defined(DEV_REPO_DIR)
    bundledDir = std::string(DEV_REPO_DIR) + "/games";
    engineShaders = std::string(DEV_REPO_DIR) + "/engine/shaders";
    std::string engineFonts = std::string(DEV_REPO_DIR) + "/engine/fonts";
#else
    bundledDir = appDir + "games";
    engineShaders = appDir + "shaders";
    std::string engineFonts = appDir + "fonts";
#endif
    char* pref = SDL_GetPrefPath("starter", "gamelab");
    std::string prefDir = pref ? pref : "";
    SDL_free(pref);
    savesDir = prefDir + "saves";
    SDL_CreateDirectory(savesDir.c_str());

    // The log of this run. If the last run never said goodbye, keep its final lines to show the PC.
    logPath = prefDir + "log.txt";
    runningMark = prefDir + "running";
    if (SDL_GetPathInfo(runningMark.c_str(), nullptr)) {
        size_t size = 0;
        char* old = (char*)SDL_LoadFile(logPath.c_str(), &size);
        if (old) {
            std::string tail(old + (size > 1500 ? size - 1500 : 0), size > 1500 ? 1500 : size);
            for (char& c : tail)
                if (c == '\n' || c == '\t' || c == '\r') c = ' ';
            lastCrash = "The app stopped unexpectedly last time. Its log ended: " + tail;
        }
        SDL_free(old);
    }
    logFile = SDL_IOFromFile(logPath.c_str(), "w");
    defaultLog = SDL_GetDefaultLogOutputFunction();
    SDL_SetLogOutputFunction(log_to_file, nullptr);
    SDL_SaveFile(runningMark.c_str(), "1", 1);
    if (!lastCrash.empty()) SDL_Log("%s", lastCrash.c_str());

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
#if !defined(SDL_PLATFORM_IOS)
        if (arg == "--check") {
            checking = true;
            continue;
        }
#endif
        if (arg == "--phone" && i + 1 < argc) {
            // Behave like the phone: ignore the repo and use games synced into this folder.
            syncedDir = argv[++i];
            bundledDir.clear();
        } else if (!arg.empty() && arg[0] != '-') {
            startGame = arg;
        }
    }
#if defined(SDL_PLATFORM_IOS) && defined(LOADER_SYNC)
    // The Documents folder shows up in the Files app, so synced games and server.txt can be seen there.
    const char* documents = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS);
    syncedDir = (documents ? std::string(documents) : prefDir) + "games";
#endif
#if defined(SINGLE_GAME)
    startGame = SINGLE_GAME;
#endif

    script::set_log_sink(on_log);
    if (!gfx::init(window, engineShaders, fatal)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Game Lab can't start", fatal.c_str(), window);
        return SDL_APP_FAILURE;
    }
    uiFont = gfx::load_engine_font(engineFonts + "/MyDotFont.ttf", 64);
    audio::init();   // a game still runs, silently, if this fails
    input::init();

#if defined(LOADER_SYNC)
    if (!syncedDir.empty()) {
        std::string settings = syncedDir.substr(0, syncedDir.find_last_of('/'));
        devsync::start(syncedDir, settings.empty() ? "." : settings, BUILD_FINGERPRINT);
    }
#endif

#if !defined(SDL_PLATFORM_IOS)
    if (const char* device = SDL_getenv("GAMELAB_DEVICE")) {
        phoneIndex = SDL_clamp(SDL_atoi(device), 1, phoneCount) - 1;
        phoneSideways = !SDL_strchr(device, 'u');   // "1u" starts upright
        apply_phone();
    }
#endif

    refresh_games();
    lastTicks = SDL_GetTicks();
    if (!startGame.empty()) {
        if (std::find(games.begin(), games.end(), startGame) != games.end()) start_game(startGame);
        else SDL_Log("No game called '%s' in %s", startGame.c_str(), bundledDir.c_str());
    }
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void* appstate, SDL_Event* event) {
    (void)appstate;
    switch (event->type) {
    case SDL_EVENT_QUIT: return SDL_APP_SUCCESS;
    case SDL_EVENT_DID_ENTER_BACKGROUND:
        if (!paused) {
            paused = true;
            script::pause();
            audio::suspend(true);
            SDL_RemovePath(runningMark.c_str());   // being closed from here on is not a crash
        }
        break;
    case SDL_EVENT_WILL_ENTER_FOREGROUND:
    case SDL_EVENT_DID_ENTER_FOREGROUND:
        if (paused) {
            paused = false;
            audio::suspend(false);
            script::resume();
            SDL_SaveFile(runningMark.c_str(), "1", 1);
        }
        lastTicks = SDL_GetTicks();
        break;
    case SDL_EVENT_KEY_DOWN:
        if (event->key.repeat) break;
#if !defined(SINGLE_GAME)
        if (event->key.key == SDLK_ESCAPE) {
            if (current.empty()) return SDL_APP_SUCCESS;
            show_list();
            return SDL_APP_CONTINUE;
        }
#endif
        if (event->key.key == SDLK_F5 && !current.empty()) start_game(current);
        if (event->key.key == SDLK_F3) showHud = !showHud;
#if !defined(SDL_PLATFORM_IOS)
        if (event->key.key == SDLK_F1) {
            if (++phoneIndex >= phoneCount) phoneIndex = -1;
            apply_phone();
        }
        if (event->key.key == SDLK_F2 && phoneIndex >= 0) {
            phoneSideways = !phoneSideways;
            apply_phone();
        }
#endif
        break;
    default: break;
    }
    input::handle_event(*event);
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void* appstate) {
    (void)appstate;
    if (paused) {
        SDL_Delay(50);
        return SDL_APP_CONTINUE;
    }
    frame();
    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void* appstate, SDL_AppResult result) {
    (void)appstate;
    (void)result;
#if defined(LOADER_SYNC)
    devsync::stop();
#endif
    script::unload();
    model::release(true);
    audio::shutdown();
    gfx::shutdown();
    SDL_RemovePath(runningMark.c_str());
    SDL_SetLogOutputFunction(defaultLog, nullptr);
    if (logFile) SDL_CloseIO(logFile);
}
