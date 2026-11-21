#include "script.h"
#include "lua_api.h"

#include <SDL3/SDL.h>

#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

#include "audio.h"
#include "phys.h"
#include "gfx.h"
#include "input.h"
#include "mocap.h"
#include "model.h"
#include "net.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

namespace script {
namespace api {

lua_State* L = nullptr;
std::string gameDir, savePath, problem;
void (*logSink)(const char*, const std::string&) = nullptr;

const float FIXED_STEP = 1.0f / 60.0f;
float timeScale = 1, fixedDebt = 0, gameSeconds = 0, lastDt = 0;
// Lua instructions run, counted by a hook every OPS_STEP of them: this frame so far, and the whole last frame.
const int OPS_STEP = 100;
long long luaOps = 0, lastFrameOps = 0;
void count_ops(lua_State*, lua_Debug*) { luaOps += OPS_STEP; }
bool stepOnce = false, wantHud = false, listenerSet = false;

std::map<std::string, Saved> saves;

void report(const char* level, const std::string& message) {
    SDL_Log("[%s] %s", level, message.c_str());
    if (logSink) logSink(level, message);
}

bool read_file(const std::string& path, std::string& out) {
    size_t size = 0;
    void* data = SDL_LoadFile(path.c_str(), &size);
    if (!data) return false;
    out.assign((const char*)data, size);
    SDL_free(data);
    return true;
}

// ---- saves: a small text file of key/value pairs per game ----

void load_saves() {
    saves.clear();
    std::string text;
    if (!read_file(savePath, text)) return;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        start = end + 1;
        size_t tab = line.find('\t', 2);
        if (line.size() < 3 || line[1] != '\t' || tab == std::string::npos) continue;
        saves[line.substr(2, tab - 2)] = {line[0] == 'n', line.substr(tab + 1)};
    }
}

void write_saves() {
    std::string text;
    for (const auto& entry : saves) text += std::string(entry.second.isNumber ? "n" : "s") + "\t" + entry.first + "\t" + entry.second.text + "\n";
    // Written beside the old file, then swapped in, so an app killed mid-write keeps the last good save.
    std::string part = savePath + ".part";
    if (SDL_SaveFile(part.c_str(), text.data(), text.size())) SDL_RenamePath(part.c_str(), savePath.c_str());
}

std::string clean(const char* s) {
    std::string out = s ? s : "";
    for (char& c : out)
        if (c == '\n' || c == '\t' || c == '\r') c = ' ';
    return out;
}

// ---- reading arguments ----

float num(lua_State* l, int index, float fallback) { return (float)luaL_optnumber(l, index, fallback); }
uint32_t handle(lua_State* l, int index) { return (uint32_t)luaL_optinteger(l, index, 0); }
bool flag(lua_State* l, int index, bool fallback) { return lua_isnoneornil(l, index) ? fallback : lua_toboolean(l, index) != 0; }

// Ten numbers starting at `first`: position, rotation as a quaternion, scale. A single scale number sizes all three ways.
void read_matrix(lua_State* l, int first, float out[16]) {
    float t[3] = {num(l, first, 0), num(l, first + 1, 0), num(l, first + 2, 0)};
    float r[4] = {num(l, first + 3, 0), num(l, first + 4, 0), num(l, first + 5, 0), num(l, first + 6, 1)};
    float sx = num(l, first + 7, 1);
    float s[3] = {sx, num(l, first + 8, sx), num(l, first + 9, sx)};
    gfx::mat_from_trs(t, r, s, out);
}

void read_color(lua_State* l, int first, float out[4]) {
    for (int i = 0; i < 4; i++) out[i] = num(l, first + i, 1);
}

int push_trs(lua_State* l, const float m[16]) {
    float t[3], r[4], s[3];
    model::to_trs(m, t, r, s);
    for (float v : t) lua_pushnumber(l, v);
    for (float v : r) lua_pushnumber(l, v);
    for (float v : s) lua_pushnumber(l, v);
    return 10;
}

void set_number(lua_State* l, const char* key, double value) {
    lua_pushnumber(l, value);
    lua_setfield(l, -2, key);
}

std::string field(lua_State* l, const char* name) {
    lua_getfield(l, -1, name);
    std::string value = lua_isstring(l, -1) ? lua_tostring(l, -1) : "";
    lua_pop(l, 1);
    return value;
}

float number_field(lua_State* l, const char* name, float fallback) {
    lua_getfield(l, -1, name);
    float value = lua_isnumber(l, -1) ? (float)lua_tonumber(l, -1) : fallback;
    lua_pop(l, 1);
    return value;
}

// "nearest clamp" and the like, for textures.
int texture_flags(lua_State* l, int index) {
    std::string words = luaL_optstring(l, index, "");
    int flags = 0;
    if (words.find("nearest") != std::string::npos) flags |= gfx::TEX_NEAREST;
    if (words.find("clamp") != std::string::npos) flags |= gfx::TEX_CLAMP;
    if (words.find("nomip") != std::string::npos) flags |= gfx::TEX_NOMIP;
    if (words.find("data") != std::string::npos) flags |= gfx::TEX_DATA;
    return flags;
}


int l_print(lua_State* l) {
    std::string line;
    for (int i = 1; i <= lua_gettop(l); i++) {
        if (i > 1) line += " ";
        line += luaL_tolstring(l, i, nullptr);
        lua_pop(l, 1);
    }
    report("log", line);
    return 0;
}

// include("enemies.lua") runs another file from the game's folder.
int l_include(lua_State* l) {
    std::string name = luaL_checkstring(l, 1), source;
    std::string path = gfx::game_path(name);
    if (path.empty() || !read_file(path, source)) return luaL_error(l, "include: can't read '%s'", name.c_str());
    if (luaL_loadbuffer(l, source.data(), source.size(), ("@" + name).c_str()) != LUA_OK) return lua_error(l);
    lua_call(l, 0, LUA_MULTRET);
    return lua_gettop(l) - 1;
}

int traceback(lua_State* l) {
    luaL_traceback(l, l, lua_tostring(l, 1), 1);
    return 1;
}

// Calls a global function if the game defined it. Any error stops the game and is kept for display.
void call(const char* name, int args) {
    if (!L || !problem.empty()) {
        if (L) lua_pop(L, args);
        return;
    }
    int base = lua_gettop(L) - args;
    lua_pushcfunction(L, traceback);
    lua_insert(L, base + 1);
    // Looked up directly, so a game that has made unknown globals an error can still leave callbacks out.
    lua_pushglobaltable(L);
    lua_pushstring(L, name);
    lua_rawget(L, -2);
    lua_remove(L, -2);
    if (!lua_isfunction(L, -1)) {
        lua_settop(L, base);
        return;
    }
    lua_insert(L, base + 2);
    if (lua_pcall(L, args, 0, base + 1) != LUA_OK) {
        problem = lua_tostring(L, -1) ? lua_tostring(L, -1) : "unknown error";
        report("error", problem);
    }
    lua_settop(L, base);
}

void call_with(const char* name, float value) {
    if (!L) return;
    lua_pushnumber(L, value);
    call(name, 1);
}

void register_table(const char* name, const luaL_Reg* functions) {
    lua_newtable(L);
    luaL_setfuncs(L, functions, 0);
    lua_setglobal(L, name);
}

}  // namespace api

using namespace api;

bool load(const std::string& dir, const std::string& saveFile) {
    unload();
    gameDir = dir;
    savePath = saveFile;
    load_saves();
    input::set_controls({});
    timeScale = 1;
    fixedDebt = gameSeconds = 0;
    stepOnce = wantHud = false;

    L = luaL_newstate();
    lua_sethook(L, count_ops, LUA_MASKCOUNT, OPS_STEP);
    luaOps = lastFrameOps = 0;
    const luaL_Reg libs[] = {{"_G", luaopen_base}, {"table", luaopen_table}, {"string", luaopen_string},
                             {"math", luaopen_math}, {"utf8", luaopen_utf8}, {"coroutine", luaopen_coroutine}};
    for (const luaL_Reg& lib : libs) {
        luaL_requiref(L, lib.name, lib.func, 1);
        lua_pop(L, 1);
    }
    // Games read files through include(), which works the same on the PC and inside the app.
    lua_pushnil(L); lua_setglobal(L, "dofile");
    lua_pushnil(L); lua_setglobal(L, "loadfile");

    register_gfx();
    register_model();
    register_phys();
    register_audio();
    register_input();
    register_net();
    register_mocap();
    register_save();
    register_sys();
    // The built-in shapes, usable with gfx.draw like any other mesh.
    lua_getglobal(L, "gfx");
    lua_pushinteger(L, gfx::builtin_mesh(gfx::MESH_CUBE)); lua_setfield(L, -2, "CUBE");
    lua_pushinteger(L, gfx::builtin_mesh(gfx::MESH_SPHERE)); lua_setfield(L, -2, "SPHERE");
    lua_pushinteger(L, gfx::builtin_mesh(gfx::MESH_PLANE)); lua_setfield(L, -2, "PLANE");
    lua_pop(L, 1);
    lua_register(L, "print", l_print);
    lua_register(L, "include", l_include);

    std::string source;
    if (!read_file(gameDir + "/main.lua", source)) {
        problem = "This game has no main.lua";
        report("error", problem);
        return false;
    }
    lua_pushcfunction(L, traceback);
    if (luaL_loadbuffer(L, source.data(), source.size(), "@main.lua") != LUA_OK || lua_pcall(L, 0, 0, 1) != LUA_OK) {
        problem = lua_tostring(L, -1) ? lua_tostring(L, -1) : "unknown error";
        report("error", problem);
        lua_settop(L, 0);
        return false;
    }
    lua_settop(L, 0);
    call("init", 0);
    return problem.empty();
}

void unload() {
    if (L) lua_close(L);
    L = nullptr;
    problem.clear();
    saves.clear();
    netCallbacks.clear();   // answers still on their way are dropped when they arrive
    gamelab_mocap_stop();   // the camera goes off when the game does
    cameraTexture = 0;      // freed with the game's other textures
    cameraSerial = -1;
    input::keyboard(false);
    input::set_controls({});
    phys::clear();
}

void update(float dt) {
    if (!L) return;
    lastDt = dt;
    lastFrameOps = luaOps;   // update, then draw, is one frame
    luaOps = 0;
    // What the player typed, then the answers to web requests, before the game's own update.
    std::string text;
    bool isKey = false;
    while (input::take_typed(text, isKey)) {
        lua_pushstring(L, text.c_str());
        call(isKey ? "on_key" : "on_text", 1);
    }
    net::Response response;
    while (net::poll(response)) {
        auto found = netCallbacks.find(response.id);
        if (found == netCallbacks.end()) continue;   // asked for by a game that has since been reloaded
        int ref = found->second;
        netCallbacks.erase(found);
        if (!problem.empty()) continue;
        lua_pushcfunction(L, traceback);
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
        if (response.status) {
            lua_pushinteger(L, response.status);
            lua_pushlstring(L, response.body.data(), response.body.size());
            lua_pushnil(L);
        } else {
            lua_pushnil(L);
            lua_pushnil(L);
            lua_pushstring(L, response.error.c_str());
        }
        if (lua_pcall(L, 3, 0, -5) != LUA_OK) {
            problem = lua_tostring(L, -1) ? lua_tostring(L, -1) : "unknown error";
            report("error", problem);
        }
        lua_settop(L, 0);
    }
    float scaled = dt * timeScale;
    if (stepOnce) {
        // Asked for from the PC while stopped: move on by exactly one tick.
        stepOnce = false;
        if (timeScale == 0) scaled = FIXED_STEP;
    }
    fixedDebt += scaled;
    int ticks = 0;
    while (fixedDebt >= FIXED_STEP && ticks++ < 5) {   // never try to catch up a long stall all at once
        fixedDebt -= FIXED_STEP;
        call_with("fixed_update", FIXED_STEP);
    }
    if (fixedDebt > FIXED_STEP) fixedDebt = 0;
    gameSeconds += scaled;
    call_with("update", scaled);
    call_with("late_update", scaled);
}

void draw() {
    if (!L) return;
    listenerSet = false;
    call("draw", 0);
    // Sounds are heard from where the camera ended up, unless the game placed the ears itself.
    if (!listenerSet) audio::listener(gfx::camera_position(), gfx::camera_right());
}

void pause() { if (L) call("on_pause", 0); }
void resume() { if (L) call("on_resume", 0); }

void eval(const std::string& code) {
    if (!L) {
        report("log", "No game is running.");
        return;
    }
    int base = lua_gettop(L);
    // Try it as an expression first, so typing  score  prints the score.
    if (luaL_loadstring(L, ("return " + code).c_str()) != LUA_OK) {
        lua_settop(L, base);
        if (luaL_loadstring(L, code.c_str()) != LUA_OK) {
            report("log", std::string("lua: ") + lua_tostring(L, -1));
            lua_settop(L, base);
            return;
        }
    }
    if (lua_pcall(L, 0, LUA_MULTRET, 0) != LUA_OK) {
        report("log", std::string("lua: ") + (lua_tostring(L, -1) ? lua_tostring(L, -1) : "error"));
    } else {
        std::string line;
        for (int i = base + 1; i <= lua_gettop(L); i++) {
            line += (line.empty() ? "" : "  ") + std::string(luaL_tolstring(L, i, nullptr));
            lua_pop(L, 1);
        }
        report("log", "lua: " + (line.empty() ? std::string("ok") : line));
    }
    lua_settop(L, base);
}

const std::string& error() { return problem; }
float game_time() { return gameSeconds; }
bool hud_wanted() { return wantHud; }
int memory_kb() { return L ? lua_gc(L, LUA_GCCOUNT, 0) : 0; }

void set_log_sink(void (*sink)(const char*, const std::string&)) { logSink = sink; }

}  // namespace script
