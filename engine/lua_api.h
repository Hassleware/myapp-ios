// What the Lua host (script.cpp) and the binding files (lua_<table>.cpp, one per Lua table) share. Internal to the
// engine's Lua layer: the rest of the engine talks to it through script.h.
//
// Adding a Lua function: write l_<name> in the lua_<table>.cpp of its table, add it to that file's luaL_Reg list
// in register_<table>(), and describe it in docs/api/gamelab.lua (the handbook build checks the two agree).
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "model.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

namespace script {
namespace api {

// The running game (script.cpp).
extern lua_State* L;
extern std::string gameDir, savePath, problem;
extern float timeScale, fixedDebt, gameSeconds, lastDt;
extern long long luaOps, lastFrameOps;   // Lua instructions: this frame so far, and the whole last frame
extern bool stepOnce, wantHud, listenerSet;

struct Saved {
    bool isNumber;
    std::string text;
};
extern std::map<std::string, Saved> saves;   // save.*: loaded with the game, written on every set
void write_saves();

void report(const char* level, const std::string& message);   // to the log, dev.py and the screen
bool read_file(const std::string& path, std::string& out);
std::string clean(const char* s);                             // one line of printable text
void register_table(const char* name, const luaL_Reg* functions);

// Reading arguments and pushing results.
float num(lua_State* l, int index, float fallback);
uint32_t handle(lua_State* l, int index);
bool flag(lua_State* l, int index, bool fallback);
void read_matrix(lua_State* l, int first, float out[16]);     // position, quaternion, scale: ten numbers
void read_color(lua_State* l, int first, float out[4]);
int push_trs(lua_State* l, const float m[16]);
void set_number(lua_State* l, const char* key, double value);
std::string field(lua_State* l, const char* name);
float number_field(lua_State* l, const char* name, float fallback);
int texture_flags(lua_State* l, int index);

// lua_model.cpp: a model's nodes, and a node named or numbered by argument `index` (phys.model uses them too).
const std::vector<model::Node>& checked_nodes(lua_State* l, uint32_t m, const char* what);
int node_arg(lua_State* l, uint32_t m, int index, const char* what);

// State a binding file owns that the host also clears when a game unloads.
extern std::map<int, int> netCallbacks;                       // lua_net.cpp: request id -> waiting Lua function
extern int cameraW, cameraH, cameraSerial;                    // lua_mocap.cpp: the camera picture's texture
extern uint32_t cameraTexture;

// Each table's functions, registered by script::load in this order.
void register_gfx();
void register_model();
void register_phys();
void register_audio();
void register_input();
void register_net();
void register_mocap();
void register_save();
void register_sys();

}  // namespace api
}  // namespace script
