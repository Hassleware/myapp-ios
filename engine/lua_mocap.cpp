// The Lua mocap.*: ARKit body tracking (iPhone only).
// Part of the Lua host (engine/script.cpp); lua_api.h has the helpers and state the binding files share.
#include "lua_api.h"

#include <SDL3/SDL.h>

#include <cstring>
#include <vector>

#include "mocap.h"
#include "gfx.h"

namespace script {
namespace api {

// ---- mocap: ARKit body tracking (iPhone only) ----

// mocap.start() -> true, or false and why not
int l_mocap_start(lua_State* l) {
    char error[256] = "";
    bool ok = gamelab_mocap_start(error, sizeof(error)) != 0;
    lua_pushboolean(l, ok);
    if (ok) return 1;
    lua_pushstring(l, error);
    return 2;
}

int l_mocap_stop(lua_State*) {
    gamelab_mocap_stop();
    return 0;
}

// mocap.pose() -> tracked, {x, y, z, x, y, z, ...}, {parent, ...} (counting from 1; 0 for the root), error
int l_mocap_pose(lua_State* l) {
    static float positions[256 * 3];
    static int parents[256];
    int count = 0;
    char error[256] = "";
    bool tracked = gamelab_mocap_pose(positions, parents, 256, &count, error, sizeof(error)) != 0;
    lua_pushboolean(l, tracked);
    lua_createtable(l, count * 3, 0);
    for (int i = 0; i < count * 3; i++) {
        lua_pushnumber(l, positions[i]);
        lua_rawseti(l, -2, i + 1);
    }
    lua_createtable(l, count, 0);
    for (int i = 0; i < count; i++) {
        lua_pushinteger(l, parents[i] + 1);
        lua_rawseti(l, -2, i + 1);
    }
    if (error[0]) lua_pushstring(l, error);
    else lua_pushnil(l);
    return 4;
}

int l_mocap_record(lua_State* l) {
    gamelab_mocap_record(lua_toboolean(l, 1));
    return 0;
}

int l_mocap_frames(lua_State* l) {
    lua_pushinteger(l, gamelab_mocap_frames());
    return 1;
}

// mocap.take() -> the recording as text (send it with net.backend), or nil
// mocap.camera() -> texture, width, height: the back camera's picture, upright, refreshed each frame. Nothing without one.
uint32_t cameraTexture = 0;
int cameraW = 0, cameraH = 0, cameraSerial = -1;

int l_mocap_camera(lua_State* l) {
    static std::vector<uint8_t> pixels(512 * 512 * 4);
    int w = 0, h = 0, serial = 0;
    if (!gamelab_mocap_camera(pixels.data(), (int)pixels.size(), &w, &h, &serial)) return 0;
    if (serial != cameraSerial) {
        cameraSerial = serial;
        if (!cameraTexture || w != cameraW || h != cameraH || !gfx::update_texture(cameraTexture, pixels.data())) {
            if (cameraTexture) gfx::free_texture(cameraTexture);
            cameraTexture = gfx::new_texture(w, h, pixels.data(), gfx::TEX_NOMIP | gfx::TEX_CLAMP, true);
            cameraW = w;
            cameraH = h;
        }
    }
    lua_pushinteger(l, cameraTexture);
    lua_pushinteger(l, cameraW);
    lua_pushinteger(l, cameraH);
    return 3;
}

// mocap.screen() -> {x, y, x, y, ...}: each joint on the camera picture, 0 to 1 across and down. Nothing while untracked.
int l_mocap_screen(lua_State* l) {
    static float points[256 * 2];
    int count = gamelab_mocap_screen(points, 256);
    if (count <= 0) return 0;
    lua_createtable(l, count * 2, 0);
    for (int i = 0; i < count * 2; i++) {
        lua_pushnumber(l, points[i]);
        lua_rawseti(l, -2, i + 1);
    }
    return 1;
}

// mocap.video() -> the take's video (.mp4 bytes), or nil until it has finished writing (a moment after STOP)
int l_mocap_video(lua_State* l) {
    size_t size = 0;
    char* bytes = gamelab_mocap_video(&size);
    if (!bytes) return 0;
    lua_pushlstring(l, bytes, size);
    free(bytes);
    return 1;
}

int l_mocap_take(lua_State* l) {
    size_t size = 0;
    char* text = gamelab_mocap_take(&size);
    if (!text) return 0;
    lua_pushlstring(l, text, size);
    free(text);
    return 1;
}

void register_mocap() {
    const luaL_Reg mocapFunctions[] = {{"start", l_mocap_start}, {"stop", l_mocap_stop}, {"pose", l_mocap_pose}, {"record", l_mocap_record},
                                       {"frames", l_mocap_frames}, {"take", l_mocap_take}, {"camera", l_mocap_camera},
                                       {"screen", l_mocap_screen}, {"video", l_mocap_video}, {nullptr, nullptr}};
    register_table("mocap", mocapFunctions);
}

}  // namespace api
}  // namespace script
