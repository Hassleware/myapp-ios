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
#include <cctype>
#include <cmath>
#include <cstring>
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
#include "app.h"

#if defined(LOADER_SYNC)
#include "devsync.h"
#endif

#ifndef BUILD_FINGERPRINT
#define BUILD_FINGERPRINT "dev"
#endif

namespace app {

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
Uint64 game_folder_stamp(const std::string& dir) {
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
    watchStamp = game_folder_stamp(dir);
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
std::string wrap_text(const std::string& text, size_t width) {
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
    gfx::text(wrap_text(detail, (size_t)((w - 80) / 16)), 40, 120, 16, white);
}


void frame() {
#if !defined(SDL_PLATFORM_IOS)
    if (checking && !current.empty()) layout_check_tick();
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
        Uint64 stamp = game_folder_stamp(game_dir(current));
        if (stamp != watchStamp) start_game(current);
    }
    if (current.empty() && now - lastListRefresh > 1000) {
        lastListRefresh = now;
        refresh_games();
    }

    // PC checks: GAMELAB_TURBO=n plays n whole 1/60 s steps (physics and animation alike) for every frame drawn, so
    // a long self-test runs many times faster and plays exactly as it would at 60 frames a second.
    static const int turbo = SDL_getenv("GAMELAB_TURBO") ? SDL_max(1, SDL_atoi(SDL_getenv("GAMELAB_TURBO"))) : 0;
    if (!current.empty() && fatal.empty()) {
        if (turbo) for (int i = 0; i < turbo; i++) script::update(1.0f / 60.0f);
        else script::update(dt);
    }

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
            gfx::text(wrap_text(gfx::shader_error(), (size_t)((gfx::view_width() - 80) / 14)), 80, 28, 14, red);
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
#if defined(GFX_SCREENSHOT)
    // Debugging aid: GAMELAB_SHOT=file.bmp saves the 30th frame and quits.
    static int frames = 0;
    const char* shot = SDL_getenv("GAMELAB_SHOT");
    const char* when = SDL_getenv("GAMELAB_SHOT_FRAME");
    if (shot && ++frames == (when ? SDL_atoi(when) : 30)) {
        gfx::screenshot(shot);   // taken by the end_frame below
        SDL_Event quit = {};
        quit.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&quit);
    }
#endif
    gfx::end_frame(script::game_time());
    input::end_frame();
}

}  // namespace app

using namespace app;

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
    if (SDL_getenv("GAMELAB_HIDDEN")) flags |= SDL_WINDOW_HIDDEN;   // PC checks: render and screenshot without showing a window
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
        apply_phone_frame();
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
            apply_phone_frame();
        }
        if (event->key.key == SDLK_F2 && phoneIndex >= 0) {
            phoneSideways = !phoneSideways;
            apply_phone_frame();
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
