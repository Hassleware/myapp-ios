#include "script.h"

#include <SDL3/SDL.h>

#include <map>
#include <vector>

#include "gfx.h"
#include "input.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

namespace script {
namespace {

lua_State* L = nullptr;
std::string gameDir, savePath, problem;
void (*logSink)(const char*, const std::string&) = nullptr;

struct Saved {
    bool isNumber;
    std::string text;
};
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
    SDL_SaveFile(savePath.c_str(), text.data(), text.size());
}

std::string clean(const char* s) {
    std::string out = s ? s : "";
    for (char& c : out)
        if (c == '\n' || c == '\t' || c == '\r') c = ' ';
    return out;
}

// ---- functions the game can call ----

float num(lua_State* l, int index, float fallback) { return (float)luaL_optnumber(l, index, fallback); }

int l_clear(lua_State* l) {
    gfx::clear(num(l, 1, 0), num(l, 2, 0), num(l, 3, 0));
    return 0;
}

int l_camera(lua_State* l) {
    float eye[3] = {num(l, 1, 0), num(l, 2, 3), num(l, 3, 8)}, target[3] = {num(l, 4, 0), num(l, 5, 0), num(l, 6, 0)};
    gfx::camera(eye, target, num(l, 7, 60));
    return 0;
}

int l_light(lua_State* l) {
    gfx::light(num(l, 1, -0.4f), num(l, 2, -1), num(l, 3, -0.3f));
    return 0;
}

int l_shader(lua_State* l) {
    gfx::shader(luaL_optstring(l, 1, ""));
    return 0;
}

// gfx.cube(x, y, z,  sx, sy, sz,  r, g, b [, yaw [, alpha]])
int l_cube(lua_State* l) {
    float pos[3] = {num(l, 1, 0), num(l, 2, 0), num(l, 3, 0)}, size[3] = {num(l, 4, 1), num(l, 5, 1), num(l, 6, 1)};
    float color[4] = {num(l, 7, 1), num(l, 8, 1), num(l, 9, 1), num(l, 11, 1)};
    gfx::mesh(gfx::MESH_CUBE, pos, size, num(l, 10, 0), color);
    return 0;
}

// gfx.sphere(x, y, z,  diameter,  r, g, b)
int l_sphere(lua_State* l) {
    float d = num(l, 4, 1);
    float pos[3] = {num(l, 1, 0), num(l, 2, 0), num(l, 3, 0)}, size[3] = {d, d, d}, color[4] = {num(l, 5, 1), num(l, 6, 1), num(l, 7, 1), 1};
    gfx::mesh(gfx::MESH_SPHERE, pos, size, 0, color);
    return 0;
}

// gfx.plane(x, y, z,  sx, sz,  r, g, b)
int l_plane(lua_State* l) {
    float pos[3] = {num(l, 1, 0), num(l, 2, 0), num(l, 3, 0)}, size[3] = {num(l, 4, 1), 1, num(l, 5, 1)}, color[4] = {num(l, 6, 1), num(l, 7, 1), num(l, 8, 1), 1};
    gfx::mesh(gfx::MESH_PLANE, pos, size, 0, color);
    return 0;
}

int l_rect(lua_State* l) {
    float color[4] = {num(l, 5, 1), num(l, 6, 1), num(l, 7, 1), num(l, 8, 1)};
    gfx::rect(num(l, 1, 0), num(l, 2, 0), num(l, 3, 0), num(l, 4, 0), color);
    return 0;
}

int l_circle(lua_State* l) {
    float color[4] = {num(l, 4, 1), num(l, 5, 1), num(l, 6, 1), num(l, 7, 1)};
    gfx::circle(num(l, 1, 0), num(l, 2, 0), num(l, 3, 10), color);
    return 0;
}

int l_text(lua_State* l) {
    float color[4] = {num(l, 5, 1), num(l, 6, 1), num(l, 7, 1), num(l, 8, 1)};
    gfx::text(luaL_tolstring(l, 1, nullptr), num(l, 2, 0), num(l, 3, 0), num(l, 4, 24), color);
    return 0;
}

int l_text_width(lua_State* l) {
    lua_pushnumber(l, gfx::text_width(luaL_tolstring(l, 1, nullptr), num(l, 2, 24)));
    return 1;
}

int l_size(lua_State* l) {
    lua_pushnumber(l, gfx::view_width());
    lua_pushnumber(l, gfx::view_height());
    return 2;
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

// input.controls{ {type="stick", id="move", x=0.14, y=0.74, size=0.13}, {type="button", id="jump", ...} }
int l_controls(lua_State* l) {
    luaL_checktype(l, 1, LUA_TTABLE);
    std::vector<input::Control> controls;
    lua_Integer count = luaL_len(l, 1);
    for (lua_Integer i = 1; i <= count; i++) {
        lua_geti(l, 1, i);
        if (lua_istable(l, -1)) {
            input::Control c;
            c.isStick = field(l, "type") == "stick";
            c.id = field(l, "id");
            c.label = field(l, "label");
            c.key = field(l, "key");
            c.x = number_field(l, "x", 0.5f);
            c.y = number_field(l, "y", 0.5f);
            c.size = number_field(l, "size", c.isStick ? 0.13f : 0.09f);
            controls.push_back(c);
        }
        lua_pop(l, 1);
    }
    input::set_controls(controls);
    return 0;
}

int l_stick(lua_State* l) {
    float x, y;
    input::stick(luaL_checkstring(l, 1), x, y);
    lua_pushnumber(l, x);
    lua_pushnumber(l, y);
    return 2;
}

int l_down(lua_State* l) {
    lua_pushboolean(l, input::down(luaL_checkstring(l, 1)));
    return 1;
}

int l_pressed(lua_State* l) {
    lua_pushboolean(l, input::pressed(luaL_checkstring(l, 1)));
    return 1;
}

int l_save_get(lua_State* l) {
    auto found = saves.find(luaL_checkstring(l, 1));
    if (found == saves.end()) {
        lua_settop(l, 2);
        return 1;
    }
    if (found->second.isNumber) lua_pushnumber(l, SDL_strtod(found->second.text.c_str(), nullptr));
    else lua_pushstring(l, found->second.text.c_str());
    return 1;
}

int l_save_set(lua_State* l) {
    std::string key = clean(luaL_checkstring(l, 1));
    if (lua_isnoneornil(l, 2)) saves.erase(key);
    else saves[key] = {lua_type(l, 2) == LUA_TNUMBER, clean(luaL_tolstring(l, 2, nullptr))};
    write_saves();
    return 0;
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
    if (name.find("..") != std::string::npos || !read_file(gameDir + "/" + name, source)) return luaL_error(l, "include: can't read '%s'", name.c_str());
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
    lua_getglobal(L, name);
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

void register_table(const char* name, const luaL_Reg* functions) {
    lua_newtable(L);
    luaL_setfuncs(L, functions, 0);
    lua_setglobal(L, name);
}

}  // namespace

bool load(const std::string& dir, const std::string& saveFile) {
    unload();
    gameDir = dir;
    savePath = saveFile;
    load_saves();
    input::set_controls({});

    L = luaL_newstate();
    const luaL_Reg libs[] = {{"_G", luaopen_base}, {"table", luaopen_table}, {"string", luaopen_string},
                             {"math", luaopen_math}, {"utf8", luaopen_utf8}, {"coroutine", luaopen_coroutine}};
    for (const luaL_Reg& lib : libs) {
        luaL_requiref(L, lib.name, lib.func, 1);
        lua_pop(L, 1);
    }
    // Games read files through include(), which works the same on the PC and inside the app.
    lua_pushnil(L); lua_setglobal(L, "dofile");
    lua_pushnil(L); lua_setglobal(L, "loadfile");

    const luaL_Reg gfxFunctions[] = {{"clear", l_clear}, {"camera", l_camera}, {"light", l_light}, {"shader", l_shader},
                                     {"cube", l_cube}, {"sphere", l_sphere}, {"plane", l_plane}, {"rect", l_rect},
                                     {"circle", l_circle}, {"text", l_text}, {"text_width", l_text_width}, {"size", l_size}, {nullptr, nullptr}};
    const luaL_Reg inputFunctions[] = {{"controls", l_controls}, {"stick", l_stick}, {"down", l_down}, {"pressed", l_pressed}, {nullptr, nullptr}};
    const luaL_Reg saveFunctions[] = {{"get", l_save_get}, {"set", l_save_set}, {nullptr, nullptr}};
    register_table("gfx", gfxFunctions);
    register_table("input", inputFunctions);
    register_table("save", saveFunctions);
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
    input::set_controls({});
}

void update(float dt) {
    if (!L) return;
    lua_pushnumber(L, dt);
    call("update", 1);
}

void draw() {
    if (!L) return;
    call("draw", 0);
}

const std::string& error() { return problem; }

void set_log_sink(void (*sink)(const char*, const std::string&)) { logSink = sink; }

}  // namespace script
