// The Lua model.*: glTF models, their nodes and clips, animators and bones.
// Part of the Lua host (engine/script.cpp); lua_api.h has the helpers and state the binding files share.
#include "lua_api.h"

#include <SDL3/SDL.h>

#include <cstring>
#include <vector>

#include "gfx.h"
#include "model.h"

namespace script {
namespace api {

// ---- model ----

// Turns the custom properties Blender wrote (JSON text) into Lua values.
void skip_space(const std::string& s, size_t& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\t' || s[i] == '\r' || s[i] == ',' || s[i] == ':')) i++;
}

void push_json(lua_State* l, const std::string& s, size_t& i, int depth) {
    skip_space(s, i);
    if (i >= s.size() || depth > 16) { lua_pushnil(l); return; }
    char c = s[i];
    if (c == '{' || c == '[') {
        char close = c == '{' ? '}' : ']';
        i++;
        lua_newtable(l);
        lua_Integer n = 0;
        for (;;) {
            skip_space(s, i);
            if (i >= s.size()) break;
            if (s[i] == close) { i++; break; }
            if (c == '{') {
                push_json(l, s, i, depth + 1);
                push_json(l, s, i, depth + 1);
                if (lua_isnil(l, -2)) { lua_pop(l, 2); break; }
                lua_settable(l, -3);
            } else {
                push_json(l, s, i, depth + 1);
                lua_rawseti(l, -2, ++n);
            }
        }
    } else if (c == '"') {
        std::string text;
        for (i++; i < s.size() && s[i] != '"'; i++) {
            if (s[i] == '\\' && i + 1 < s.size()) {
                char e = s[++i];
                text += e == 'n' ? '\n' : e == 't' ? '\t' : e;
            } else {
                text += s[i];
            }
        }
        i++;
        lua_pushlstring(l, text.data(), text.size());
    } else if (s.compare(i, 4, "true") == 0) {
        i += 4;
        lua_pushboolean(l, 1);
    } else if (s.compare(i, 5, "false") == 0) {
        i += 5;
        lua_pushboolean(l, 0);
    } else if (s.compare(i, 4, "null") == 0) {
        i += 4;
        lua_pushboolean(l, 0);
    } else {
        char* end = nullptr;
        double value = SDL_strtod(s.c_str() + i, &end);
        size_t used = (size_t)(end - (s.c_str() + i));
        if (used == 0) { i = s.size(); lua_pushnil(l); return; }
        i += used;
        if (value == (double)(lua_Integer)value) lua_pushinteger(l, (lua_Integer)value);   // 3, not 3.0
        else lua_pushnumber(l, value);
    }
}

const std::vector<model::Node>& checked_nodes(lua_State* l, uint32_t m, const char* what) {
    static const std::vector<model::Node> none;
    if (m == 0) return none;   // the stand-in for a file that could not be loaded
    const std::vector<model::Node>* nodes = model::nodes(m);
    if (!nodes) luaL_error(l, "%s: that model is gone (loaded before the game reloaded?)", what);
    return *nodes;
}

// A node given by name or by its number in model.nodes().
int node_arg(lua_State* l, uint32_t m, int index, const char* what) {
    if (lua_type(l, index) == LUA_TSTRING) {
        int found = model::find_node(m, lua_tostring(l, index));
        if (found < 0) luaL_error(l, "%s: the model has no node called '%s'", what, lua_tostring(l, index));
        return found;
    }
    return (int)luaL_checkinteger(l, index) - 1;
}

int l_model_load(lua_State* l) {
    const char* name = luaL_checkstring(l, 1);
    uint32_t made = model::load(name);
    if (!made) report("warn", std::string("missing or unreadable model '") + name + "' (drawn as a pink cube)");
    lua_pushinteger(l, made);
    return 1;
}

// model.nodes(m) -> a list of everything in the file: name, parent, where it is, and what Blender attached to it.
int l_model_nodes(lua_State* l) {
    const std::vector<model::Node>& nodes = checked_nodes(l, handle(l, 1), "model.nodes");
    lua_createtable(l, (int)nodes.size(), 0);
    for (size_t i = 0; i < nodes.size(); i++) {
        const model::Node& n = nodes[i];
        lua_newtable(l);
        lua_pushstring(l, n.name.c_str());
        lua_setfield(l, -2, "name");
        if (n.parent >= 0) set_number(l, "parent", n.parent + 1);
        float t[3], r[4], s[3];
        model::to_trs(n.world, t, r, s);
        set_number(l, "x", t[0]); set_number(l, "y", t[1]); set_number(l, "z", t[2]);
        set_number(l, "qx", r[0]); set_number(l, "qy", r[1]); set_number(l, "qz", r[2]); set_number(l, "qw", r[3]);
        set_number(l, "sx", s[0]); set_number(l, "sy", s[1]); set_number(l, "sz", s[2]);
        lua_pushboolean(l, n.group >= 0);
        lua_setfield(l, -2, "mesh");
        if (n.light) {
            const char* kinds[] = {"", "sun", "point", "spot"};
            lua_pushstring(l, kinds[n.light]);
            lua_setfield(l, -2, "light");
            set_number(l, "r", n.lightColor[0]); set_number(l, "g", n.lightColor[1]); set_number(l, "b", n.lightColor[2]);
            set_number(l, "range", n.lightRange);
            set_number(l, "angle", n.lightAngle);
        }
        if (n.cameraFov > 0) set_number(l, "fov", n.cameraFov);
        size_t at = 0;
        if (n.extras.empty()) lua_newtable(l);
        else push_json(l, n.extras, at, 0);
        if (!lua_istable(l, -1)) {
            lua_pop(l, 1);
            lua_newtable(l);
        }
        lua_setfield(l, -2, "extras");
        lua_rawseti(l, -2, (lua_Integer)i + 1);
    }
    return 1;
}

int l_model_clips(lua_State* l) {
    std::vector<model::ClipInfo> clips = model::clips(handle(l, 1));
    lua_createtable(l, (int)clips.size(), 0);
    for (size_t i = 0; i < clips.size(); i++) {
        lua_newtable(l);
        lua_pushstring(l, clips[i].name.c_str());
        lua_setfield(l, -2, "name");
        set_number(l, "duration", clips[i].duration);
        lua_rawseti(l, -2, (lua_Integer)i + 1);
    }
    return 1;
}

// model.draw(m,  x, y, z,  qx, qy, qz, qw,  sx, sy, sz,  r, g, b, a [, animator [, node]])
int l_model_draw(lua_State* l) {
    uint32_t m = handle(l, 1);
    float world[16], color[4];
    read_matrix(l, 2, world);
    read_color(l, 12, color);
    if (m == 0) {
        const float pink[4] = {1, 0, 1, 1};
        gfx::unlit(true);
        gfx::draw(gfx::builtin_mesh(gfx::MESH_CUBE), world, pink);
        gfx::unlit(false);
        return 0;
    }
    checked_nodes(l, m, "model.draw");
    int only = lua_isnoneornil(l, 17) ? -1 : node_arg(l, m, 17, "model.draw");
    model::draw(m, handle(l, 16), world, color, only);
    return 0;
}

int l_model_animator(lua_State* l) {
    uint32_t m = handle(l, 1);
    if (m) checked_nodes(l, m, "model.animator");
    lua_pushinteger(l, model::new_animator(m));
    return 1;
}

int clip_arg(lua_State* l, uint32_t m, int index) {
    if (lua_type(l, index) == LUA_TSTRING) return model::find_clip(m, lua_tostring(l, index));
    return (int)luaL_checkinteger(l, index) - 1;
}

// model.pose(m, animator,  clip, time, weight [, clip, time, weight ...]): a mix of animations.
int l_model_pose(lua_State* l) {
    uint32_t m = handle(l, 1), animator = handle(l, 2);
    int clip[8], count = 0;
    float time[8], weight[8];
    for (int arg = 3; arg + 1 <= lua_gettop(l) && count < 8; arg += 3, count++) {
        clip[count] = clip_arg(l, m, arg);
        time[count] = num(l, arg + 1, 0);
        weight[count] = num(l, arg + 2, 1);
    }
    model::pose(animator, clip, time, weight, count);
    return 0;
}

// model.joint(m, animator, node) -> where that node is inside the model now: x, y, z, qx, qy, qz, qw, sx, sy, sz.
int l_model_joint(lua_State* l) {
    uint32_t m = handle(l, 1);
    float world[16];
    if (!model::node_world(m, handle(l, 2), node_arg(l, m, 3, "model.joint"), world)) return 0;
    return push_trs(l, world);
}

// model.local(m, animator, node) reads a node's position in its parent. With ten more numbers it sets it,
// which is how a game turns a head to look at something after the animation has been applied.
int l_model_local(lua_State* l) {
    uint32_t m = handle(l, 1), animator = handle(l, 2);
    int node = node_arg(l, m, 3, "model.local");
    float t[3], r[4], s[3];
    if (!model::get_local(animator, node, t, r, s)) return 0;
    if (lua_gettop(l) > 3) {
        for (int i = 0; i < 3; i++) t[i] = num(l, 4 + i, t[i]);
        for (int i = 0; i < 4; i++) r[i] = num(l, 7 + i, r[i]);
        for (int i = 0; i < 3; i++) s[i] = num(l, 11 + i, s[i]);
        model::set_local(animator, node, t, r, s);
        return 0;
    }
    for (float v : t) lua_pushnumber(l, v);
    for (float v : r) lua_pushnumber(l, v);
    for (float v : s) lua_pushnumber(l, v);
    return 10;
}

// model.sample(m, clip, node, time) -> the node's position in its parent at that moment of the clip.
int l_model_sample(lua_State* l) {
    uint32_t m = handle(l, 1);
    float t[3], r[4], s[3];
    if (!model::sample(m, clip_arg(l, m, 2), node_arg(l, m, 3, "model.sample"), num(l, 4, 0), t, r, s)) return 0;
    for (float v : t) lua_pushnumber(l, v);
    for (float v : r) lua_pushnumber(l, v);
    for (float v : s) lua_pushnumber(l, v);
    return 10;
}

void register_model() {
    const luaL_Reg modelFunctions[] = {{"load", l_model_load}, {"nodes", l_model_nodes}, {"clips", l_model_clips}, {"draw", l_model_draw},
                                       {"animator", l_model_animator}, {"pose", l_model_pose}, {"joint", l_model_joint},
                                       {"local", l_model_local}, {"bone_local", l_model_local}, {"sample", l_model_sample}, {nullptr, nullptr}};
    register_table("model", modelFunctions);
}

}  // namespace api
}  // namespace script
