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
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "gfx.h"
#include "input.h"
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
float gameTime = 0;

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

void start_game(const std::string& name) {
    current = name;
    gameTime = 0;
    std::string dir = game_dir(name);
    gfx::set_game_dir(dir);
    script::load(dir, savesDir + "/" + name + ".txt");
    watchStamp = folder_stamp(dir);
}

void show_list() {
    script::unload();
    gfx::set_game_dir("");
    current.clear();
    refresh_games();
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
    const float panel[4] = {0.10f, 0.02f, 0.03f, 0.92f}, red[4] = {1.0f, 0.45f, 0.42f, 1}, white[4] = {1, 1, 1, 0.92f};
    float w = gfx::view_width();
    gfx::rect(0, 0, w, gfx::view_height(), panel);
    gfx::text(title, 40, 60, 28, red);
    gfx::text(wrap(detail, (size_t)((w - 80) / 16)), 40, 120, 16, white);
}

void draw_list() {
    const float title[4] = {1, 1, 1, 1}, dim[4] = {1, 1, 1, 0.55f}, row[4] = {1, 1, 1, 0.07f}, accent[4] = {0.36f, 0.82f, 0.63f, 1};
    float w = gfx::view_width(), h = gfx::view_height();
    gfx::clear(0.06f, 0.07f, 0.10f);
    gfx::text("GAME LAB", 48, 48, 40, title);
    gfx::text(std::to_string(games.size()) + (games.size() == 1 ? " GAME" : " GAMES"), 48, 98, 16, dim);

    float tapX = 0, tapY = 0;
    bool tapped = input::tap(tapX, tapY);
    float y = 130;
    for (const std::string& name : games) {
        gfx::rect(40, y, w - 80, 72, row);
        gfx::rect(40, y, 6, 72, accent);
        gfx::text(name, 72, y + 24, 24, title);
        if (tapped && tapY >= y && tapY < y + 72 && tapX >= 40 && tapX <= w - 40) {
            start_game(name);
            return;
        }
        y += 84;
    }
    if (games.empty()) gfx::text("No games here yet.", 48, y + 10, 22, dim);

    std::string status;
#if defined(LOADER_SYNC)
    // The PC is holding a build made from newer C++ than this app. iOS doesn't let an app replace
    // itself, so these hand the file to LiveContainer (where this app is installed), or to Safari to download it.
    std::string offered = devsync::available_build();
    if (!offered.empty() && offered != BUILD_FINGERPRINT) {
        const float button[4] = {0.36f, 0.82f, 0.63f, 1}, plain[4] = {1, 1, 1, 0.14f}, dark[4] = {0.04f, 0.10f, 0.08f, 1};
        float half = (w - 96) / 2;
        gfx::text(wrap("NEW APP BUILD " + offered + " IS READY ON YOUR PC", (size_t)((w - 96) / 16)), 48, h - 172, 16, title);
        gfx::rect(40, h - 142, half, 60, button);
        gfx::text("INSTALL UPDATE", 40 + (half - gfx::text_width("INSTALL UPDATE", 20)) / 2, h - 122, 20, dark);
        gfx::rect(56 + half, h - 142, half, 60, plain);
        gfx::text("DOWNLOAD FILE", 56 + half + (half - gfx::text_width("DOWNLOAD FILE", 20)) / 2, h - 122, 20, title);
        if (tapped && tapY >= h - 142 && tapY < h - 82) {
            std::string url = devsync::update_url();
            if (tapX < 48 + half) url = "livecontainer://install?url=" + url;
            SDL_OpenURL(url.c_str());
        }
    }
    status = devsync::status();
#else
    status = "Running from the repo. Edit a game's files and it reloads.";
#endif
    gfx::text(wrap(status, (size_t)((w - 96) / 16)), 48, h - 60, 16, dim);
    std::string build = std::string("build ") + BUILD_FINGERPRINT;
    gfx::text(build, w - 48 - gfx::text_width(build, 12), 56, 12, dim);
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
};
const Phone phones[] = {
    {"iPhone 11 / XR", 414, 896, 41.5f, false, 230, 33, 0},
    {"iPhone 11 Pro", 375, 812, 39, false, 209, 30, 0},
    {"iPhone 11 Pro Max", 414, 896, 39, false, 209, 30, 0},
    {"iPhone 12 mini / 13 mini", 375, 812, 44, false, 211, 32, 0},   // the 12 mini's notch, the wider of the two
    {"iPhone 12 / 12 Pro", 390, 844, 47.3f, false, 211, 32, 0},
    {"iPhone 12 Pro Max", 428, 926, 53.3f, false, 211, 32, 0},
    {"iPhone 13 / 13 Pro / 14", 390, 844, 47.3f, false, 162, 34, 0},
    {"iPhone 13 Pro Max / 14 Plus", 428, 926, 53.3f, false, 162, 34, 0},
    {"iPhone 14 Pro / 15 / 15 Pro / 16", 393, 852, 55, true, 126, 37, 11},
    {"iPhone 14 Pro Max / 15 Plus / 15 Pro Max / 16 Plus", 430, 932, 55, true, 126, 37, 11},
    {"iPhone 16 Pro / 17", 402, 874, 62, true, 126, 37, 14},
    {"iPhone 16 Pro Max / 17 Pro Max", 440, 956, 62, true, 126, 37, 14},
};
const int phoneCount = (int)(sizeof(phones) / sizeof(phones[0]));
int phoneIndex = -1;          // -1: no phone frame
bool phoneSideways = true;

// Resizes the window to the chosen phone's shape, as large as its real size or the monitor allows.
void apply_phone() {
    SDL_RestoreWindow(window);
    if (phoneIndex < 0) {
        SDL_SetWindowSize(window, 1280, 720);
        SDL_SetWindowTitle(window, "Game Lab   (F1: iPhone frame)");
    } else {
        const Phone& p = phones[phoneIndex];
        float w = phoneSideways ? p.h : p.w, h = phoneSideways ? p.w : p.h;
        float scale = SDL_GetWindowDisplayScale(window);
        SDL_Rect usable;
        if (SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(window), &usable))
            scale = SDL_min(scale, SDL_min((usable.w - 40) / w, (usable.h - 80) / h));
        if (scale <= 0) scale = 1;
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
    if (++checkStep == phoneCount * 2) {
        printf("\nLayout check for '%s': %d iPhones, sideways and upright.\n", current.c_str(), phoneCount);
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

void frame() {
#if !defined(SDL_PLATFORM_IOS)
    if (checking && !current.empty()) check_tick();
#endif
    Uint64 now = SDL_GetTicks();
    float dt = SDL_min((now - lastTicks) / 1000.0f, 0.05f);
    lastTicks = now;

#if defined(LOADER_SYNC)
    devsync::Command command;
    while (devsync::next_command(command)) {
        refresh_games();
        if (command.action == "play" && std::find(games.begin(), games.end(), command.game) != games.end()) start_game(command.game);
        else if (command.action == "menu" && !current.empty()) show_list();
        else if (command.action == "reload" && !current.empty()) start_game(current);
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

    if (!current.empty() && fatal.empty()) {
        gameTime += dt;
        script::update(dt);
    }

    if (!gfx::begin_frame()) {
        input::end_frame();
        return;
    }
    if (!fatal.empty()) {
        draw_problem("CAN'T START", fatal);
    } else if (current.empty()) {
        draw_list();
    } else {
#if !defined(SDL_PLATFORM_IOS)
        gfx::ui_log(checking);
#endif
        script::draw();
        input::draw();
#if !defined(SINGLE_GAME)
        // A small button to get back to the game list, far enough in to clear a phone's rounded corner.
        const float back[4] = {0, 0, 0, 0.4f}, ink[4] = {1, 1, 1, 0.9f};
        gfx::circle(68, 68, 20, back);
        gfx::text("x", 60, 59, 16, ink);
        float tapX = 0, tapY = 0;
        bool leave = input::tap(tapX, tapY) && tapX < 100 && tapY < 100;
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
            gfx::end_frame(gameTime);
            input::end_frame();
            show_list();
            return;
        }
    }
    draw_phone_frame();
    gfx::end_frame(gameTime);
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
#else
    bundledDir = appDir + "games";
    engineShaders = appDir + "shaders";
#endif
    char* pref = SDL_GetPrefPath("starter", "gamelab");
    std::string prefDir = pref ? pref : "";
    SDL_free(pref);
    savesDir = prefDir + "saves";
    SDL_CreateDirectory(savesDir.c_str());

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

#if defined(LOADER_SYNC)
    if (!syncedDir.empty()) {
        std::string settings = syncedDir.substr(0, syncedDir.find_last_of('/'));
        devsync::start(syncedDir, settings.empty() ? "." : settings, BUILD_FINGERPRINT);
    }
#endif

#if !defined(SDL_PLATFORM_IOS)
    if (const char* device = SDL_getenv("GAMELAB_DEVICE")) {
        phoneIndex = SDL_clamp(SDL_atoi(device), 1, phoneCount) - 1;
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
    case SDL_EVENT_DID_ENTER_BACKGROUND: paused = true; break;
    case SDL_EVENT_WILL_ENTER_FOREGROUND:
    case SDL_EVENT_DID_ENTER_FOREGROUND:
        paused = false;
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
    gfx::shutdown();
}
