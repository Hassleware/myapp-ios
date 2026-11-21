// The Lua sys.*: game time, time scale, stepping, the HUD, stats, the PS1-style budget, the platform.
// Part of the Lua host (engine/script.cpp); lua_api.h has the helpers and state the binding files share.
#include "lua_api.h"

#include <SDL3/SDL.h>

#include <cstring>
#include <vector>

#include "audio.h"
#include "gfx.h"
#include "model.h"
#include "input.h"

namespace script {
namespace api {

// ---- sys ----

int l_time(lua_State* l) {
    lua_pushnumber(l, gameSeconds);
    return 1;
}

int l_real_time(lua_State* l) {
    lua_pushnumber(l, (double)SDL_GetTicks() / 1000.0);
    return 1;
}

// sys.time_scale([scale]): 1 is normal, 0.5 slow motion, 0 stopped. Returns the scale in use.
int l_time_scale(lua_State* l) {
    if (!lua_isnoneornil(l, 1)) timeScale = SDL_clamp(num(l, 1, 1), 0.0f, 10.0f);
    lua_pushnumber(l, timeScale);
    return 1;
}

int l_step(lua_State*) { stepOnce = true; return 0; }
int l_hud(lua_State* l) { wantHud = flag(l, 1, true); return 0; }

// sys.stats() -> frames a second, draw calls, vertices, sounds playing
int l_stats(lua_State* l) {
    gfx::Stats s = gfx::stats();
    lua_pushnumber(l, lastDt > 0 ? 1.0 / lastDt : 0);
    lua_pushinteger(l, s.draws);
    lua_pushinteger(l, s.vertices);
    lua_pushinteger(l, audio::voices());
    return 4;
}

// sys.budget() -> a table of what the game is using, to hold it to a budget (a PS1's, for example):
// triangles drawn last frame, lua_ops (Lua instructions run last frame; lua_ops_now: so far this one), texels and largest_texture (its own
// textures), mesh_vertices, mesh_triangles, animation_bytes (at 16 bits a number) and lua_kb (its Lua heap).
int l_budget(lua_State* l) {
    gfx::Usage u = gfx::usage();
    lua_createtable(l, 0, 8);
    auto set = [&](const char* k, double v) { lua_pushnumber(l, v); lua_setfield(l, -2, k); };
    set("triangles", gfx::stats().triangles);
    set("lua_ops", (double)lastFrameOps);
    set("lua_ops_now", (double)luaOps);   // so far this frame: for timing one piece of code
    set("texels", u.texels);
    set("largest_texture", u.largestTexture);
    set("mesh_vertices", u.meshVertices);
    set("mesh_triangles", u.meshTriangles);
    set("animation_bytes", model::animation_bytes());
    set("lua_kb", lua_gc(l, LUA_GCCOUNT, 0) + lua_gc(l, LUA_GCCOUNTB, 0) / 1024.0);
    return 1;
}

int l_platform(lua_State* l) {
#if defined(SDL_PLATFORM_IOS)
    lua_pushstring(l, "ios");
#else
    lua_pushstring(l, "pc");
#endif
    return 1;
}

void register_sys() {
    const luaL_Reg sysFunctions[] = {{"time", l_time}, {"real_time", l_real_time}, {"time_scale", l_time_scale}, {"step", l_step},
                                     {"hud", l_hud}, {"stats", l_stats}, {"budget", l_budget}, {"platform", l_platform}, {nullptr, nullptr}};
    register_table("sys", sysFunctions);
}

}  // namespace api
}  // namespace script
