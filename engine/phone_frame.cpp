// The screen's safe area, and on the PC the iPhone frames drawn over the window (F1, F2) and the layout check
// (`gamelab --check <game>`) that reports anything a corner, notch, island or home bar covers.
#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "app.h"
#include "gfx.h"
#include "script.h"

namespace app {

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
extern const int phoneCount = (int)(sizeof(phones) / sizeof(phones[0]));
int phoneIndex = -1;          // -1: no phone frame
bool phoneSideways = true;

// Resizes the window to the chosen phone's shape, as large as its real size or the monitor allows.
void apply_phone_frame() {
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

Hidden phone_frame_hidden(const Phone& p) {
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
void rounded_rect(const float b[4], const float rgba[4]) {
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
    Hidden z = phone_frame_hidden(p);

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
        rounded_rect(z.cut, black);
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
    rounded_rect(z.bar, white);
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

void layout_check_frame() {
    const Phone& p = phones[phoneIndex];
    float w = gfx::view_width(), h = gfx::view_height();
    Hidden z = phone_frame_hidden(p);
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
void layout_check_tick() {
    if (checkStep >= 0) {
        checkFrames++;
        if (checkFrames == 4) start_game(current);   // restart once the window has its new shape
        if (checkFrames < 12) return;
        layout_check_frame();
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
    apply_phone_frame();
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

}  // namespace app
