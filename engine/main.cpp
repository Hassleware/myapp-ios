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
    gfx::rect(0, 0, w, 720, panel);
    gfx::text(title, 40, 60, 28, red);
    gfx::text(wrap(detail, (size_t)((w - 80) / 16)), 40, 120, 16, white);
}

void draw_list() {
    const float title[4] = {1, 1, 1, 1}, dim[4] = {1, 1, 1, 0.55f}, row[4] = {1, 1, 1, 0.07f}, accent[4] = {0.36f, 0.82f, 0.63f, 1};
    float w = gfx::view_width();
    gfx::clear(0.06f, 0.07f, 0.10f);
    gfx::text("GAME LAB", 48, 48, 40, title);

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
    status = devsync::status();
#else
    status = "Running from the repo. Edit a game's files and it reloads.";
#endif
    gfx::text(wrap(status, (size_t)((w - 96) / 16)), 48, 660, 16, dim);
    std::string build = std::string("build ") + BUILD_FINGERPRINT;
    gfx::text(build, w - 48 - gfx::text_width(build, 12), 56, 12, dim);
}

void frame() {
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
        script::draw();
        input::draw();
#if !defined(SINGLE_GAME)
        // A small button to get back to the game list.
        const float back[4] = {0, 0, 0, 0.4f}, ink[4] = {1, 1, 1, 0.9f};
        gfx::circle(36, 36, 20, back);
        gfx::text("x", 28, 27, 16, ink);
        float tapX = 0, tapY = 0;
        bool leave = input::tap(tapX, tapY) && tapX < 64 && tapY < 64;
#else
        bool leave = false;
#endif
        if (!script::error().empty()) draw_problem("ERROR IN " + current, script::error());
        else if (!gfx::shader_error().empty()) {
            const float red[4] = {1.0f, 0.45f, 0.42f, 1};
            gfx::text(wrap(gfx::shader_error(), (size_t)((gfx::view_width() - 80) / 14)), 80, 28, 14, red);
        }
        if (leave) {
            gfx::end_frame(gameTime);
            input::end_frame();
            show_list();
            return;
        }
    }
    gfx::end_frame(gameTime);
    input::end_frame();

#if defined(GFX_SCREENSHOT)
    // Debugging aid: GAMELAB_SHOT=file.bmp saves the 30th frame and quits.
    static int frames = 0;
    const char* shot = SDL_getenv("GAMELAB_SHOT");
    if (shot && ++frames == 30) {
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
    window = SDL_CreateWindow("Game Lab", 1280, 720, flags);
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
