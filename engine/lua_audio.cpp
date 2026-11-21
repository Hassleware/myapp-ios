// The Lua audio.*: sounds, music, 3D sound and groups.
// Part of the Lua host (engine/script.cpp); lua_api.h has the helpers and state the binding files share.
#include "lua_api.h"

#include <SDL3/SDL.h>

#include <cstring>
#include <vector>

#include "audio.h"
#include "gfx.h"

namespace script {
namespace api {

// ---- audio ----

int l_audio_load(lua_State* l) {
    const char* name = luaL_checkstring(l, 1);
    uint32_t made = audio::load(name, flag(l, 2, false));
    if (!made) report("warn", std::string("missing or unreadable sound '") + name + "' (it will be silent)");
    lua_pushinteger(l, made);
    return 1;
}

// audio.play(sound [, volume_db [, pitch [, loop [, group]]]]) -> a voice, for changing the sound while it plays
int l_audio_play(lua_State* l) {
    lua_pushinteger(l, audio::play(handle(l, 1), num(l, 2, 0), num(l, 3, 1), flag(l, 4, false), (int)luaL_optinteger(l, 5, 0)));
    return 1;
}

int l_audio_stop(lua_State* l) { audio::stop(handle(l, 1), num(l, 2, 0)); return 0; }
int l_audio_volume(lua_State* l) { audio::set_volume(handle(l, 1), num(l, 2, 0)); return 0; }
int l_audio_pitch(lua_State* l) { audio::set_pitch(handle(l, 1), num(l, 2, 1)); return 0; }
int l_audio_pan(lua_State* l) { audio::set_pan(handle(l, 1), num(l, 2, 0)); return 0; }
int l_audio_lowpass(lua_State* l) { audio::set_lowpass(handle(l, 1), num(l, 2, 0)); return 0; }
int l_audio_group(lua_State* l) { audio::group_volume((int)luaL_checkinteger(l, 1), num(l, 2, 0), num(l, 3, 0)); return 0; }
int l_audio_group_pause(lua_State* l) { audio::group_pause((int)luaL_checkinteger(l, 1), flag(l, 2, true)); return 0; }

int l_audio_playing(lua_State* l) {
    lua_pushboolean(l, audio::playing(handle(l, 1)));
    return 1;
}

// audio.position(voice, x, y, z [, min_radius, max_radius])
int l_audio_position(lua_State* l) {
    float pos[3] = {num(l, 2, 0), num(l, 3, 0), num(l, 4, 0)};
    audio::set_position(handle(l, 1), pos, num(l, 5, 2), num(l, 6, 40));
    return 0;
}

// audio.listener(x, y, z,  rightx, righty, rightz). Without it, the ears follow the camera.
int l_audio_listener(lua_State* l) {
    float pos[3] = {num(l, 1, 0), num(l, 2, 0), num(l, 3, 0)}, right[3] = {num(l, 4, 1), num(l, 5, 0), num(l, 6, 0)};
    audio::listener(pos, right);
    listenerSet = true;
    return 0;
}

void register_audio() {
    const luaL_Reg audioFunctions[] = {{"load", l_audio_load}, {"play", l_audio_play}, {"stop", l_audio_stop}, {"playing", l_audio_playing},
                                       {"volume", l_audio_volume}, {"pitch", l_audio_pitch}, {"pan", l_audio_pan}, {"lowpass", l_audio_lowpass},
                                       {"position", l_audio_position}, {"listener", l_audio_listener}, {"group", l_audio_group},
                                       {"group_pause", l_audio_group_pause}, {nullptr, nullptr}};
    register_table("audio", audioFunctions);
}

}  // namespace api
}  // namespace script
