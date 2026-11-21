// The Lua input.*: touch controls, raw touches, keys, tilt, gamepad, haptics, the keyboard and the clipboard.
// Part of the Lua host (engine/script.cpp); lua_api.h has the helpers and state the binding files share.
#include "lua_api.h"

#include <SDL3/SDL.h>

#include <cstring>
#include <vector>

#include "input.h"
#include "gfx.h"

namespace script {
namespace api {

// ---- input ----

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

// input.touches() -> a list of fingers: {id, x, y, dx, dy, phase = "down" | "move" | "up", claimed}
int l_touches(lua_State* l) {
    const std::vector<input::Touch>& touches = input::touches();
    lua_createtable(l, (int)touches.size(), 0);
    const char* phases[] = {"down", "move", "up"};
    for (size_t i = 0; i < touches.size(); i++) {
        const input::Touch& t = touches[i];
        lua_createtable(l, 0, 7);
        set_number(l, "id", (double)t.id);
        set_number(l, "x", t.x); set_number(l, "y", t.y);
        set_number(l, "dx", t.dx); set_number(l, "dy", t.dy);
        lua_pushstring(l, phases[SDL_clamp(t.phase, 0, 2)]);
        lua_setfield(l, -2, "phase");
        lua_pushboolean(l, t.claimed);
        lua_setfield(l, -2, "claimed");
        lua_rawseti(l, -2, (lua_Integer)i + 1);
    }
    return 1;
}

int l_key(lua_State* l) {
    lua_pushboolean(l, input::key(luaL_checkstring(l, 1)));
    return 1;
}

int l_tilt(lua_State* l) {
    float v[3];
    input::tilt(v);
    for (float x : v) lua_pushnumber(l, x);
    return 3;
}

int l_gyro(lua_State* l) {
    float v[3];
    input::gyro(v);
    for (float x : v) lua_pushnumber(l, x);
    return 3;
}

int l_pad(lua_State* l) {
    lua_pushnumber(l, input::pad(luaL_checkstring(l, 1)));
    return 1;
}

int l_haptic(lua_State* l) {
    input::haptic(luaL_optstring(l, 1, "medium"));
    return 0;
}

// input.keyboard(show, kind) shows or hides the keyboard; with no arguments it says whether it is showing.
int l_keyboard(lua_State* l) {
    if (lua_gettop(l) > 0) input::keyboard(lua_toboolean(l, 1), luaL_optstring(l, 2, "text"));
    lua_pushboolean(l, input::keyboard_shown());
    return 1;
}

int l_paste(lua_State* l) {
    lua_pushstring(l, input::clipboard().c_str());
    return 1;
}

int l_copy(lua_State* l) {
    input::set_clipboard(luaL_checkstring(l, 1));
    return 0;
}

void register_input() {
    const luaL_Reg inputFunctions[] = {{"controls", l_controls}, {"stick", l_stick}, {"down", l_down}, {"pressed", l_pressed},
                                       {"touches", l_touches}, {"key", l_key}, {"tilt", l_tilt}, {"gyro", l_gyro}, {"pad", l_pad},
                                       {"haptic", l_haptic}, {"keyboard", l_keyboard}, {"paste", l_paste}, {"copy", l_copy},
                                       {nullptr, nullptr}};
    register_table("input", inputFunctions);
}

}  // namespace api
}  // namespace script
