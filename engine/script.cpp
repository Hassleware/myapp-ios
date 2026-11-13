#include "script.h"

#include <SDL3/SDL.h>

#include <cstring>
#include <map>
#include <vector>

#include "audio.h"
#include "collide.h"
#include "gfx.h"
#include "input.h"
#include "model.h"

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

const float FIXED_STEP = 1.0f / 60.0f;
float timeScale = 1, fixedDebt = 0, gameSeconds = 0, lastDt = 0;
bool stepOnce = false, wantHud = false, listenerSet = false;

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

// ---- gfx: the scene ----

int l_clear(lua_State* l) {
    gfx::clear(num(l, 1, 0), num(l, 2, 0), num(l, 3, 0));
    return 0;
}

// gfx.camera(ex, ey, ez,  tx, ty, tz [, fov [, near, far [, upx, upy, upz]]])
int l_camera(lua_State* l) {
    float eye[3] = {num(l, 1, 0), num(l, 2, 3), num(l, 3, 8)}, target[3] = {num(l, 4, 0), num(l, 5, 0), num(l, 6, 0)};
    float up[3] = {num(l, 10, 0), num(l, 11, 1), num(l, 12, 0)};
    gfx::camera(eye, target, num(l, 7, 60), num(l, 8, 0.1f), num(l, 9, 500), up);
    return 0;
}

int l_light(lua_State* l) {
    gfx::light(num(l, 1, -0.4f), num(l, 2, -1), num(l, 3, -0.3f), num(l, 4, 0.65f), num(l, 5, 0.65f), num(l, 6, 0.65f));
    return 0;
}

int l_ambient(lua_State* l) {
    gfx::ambient(num(l, 1, 0.35f), num(l, 2, 0.35f), num(l, 3, 0.35f));
    return 0;
}

// gfx.point_light(x, y, z, radius, r, g, b)
int l_point_light(lua_State* l) {
    float pos[3] = {num(l, 1, 0), num(l, 2, 0), num(l, 3, 0)}, rgb[3] = {num(l, 5, 1), num(l, 6, 1), num(l, 7, 1)};
    gfx::point_light(pos, num(l, 4, 5), rgb);
    return 0;
}

// gfx.spot_light(x, y, z,  dx, dy, dz,  radius, angle, r, g, b)
int l_spot_light(lua_State* l) {
    float pos[3] = {num(l, 1, 0), num(l, 2, 0), num(l, 3, 0)}, dir[3] = {num(l, 4, 0), num(l, 5, -1), num(l, 6, 0)}, rgb[3] = {num(l, 9, 1), num(l, 10, 1), num(l, 11, 1)};
    gfx::spot_light(pos, dir, num(l, 7, 10), num(l, 8, 40), rgb);
    return 0;
}

// gfx.fog(r, g, b, start, end). With no arguments, fog is off.
int l_fog(lua_State* l) {
    float rgb[3] = {num(l, 1, 0), num(l, 2, 0), num(l, 3, 0)};
    gfx::fog(rgb, num(l, 4, 0), num(l, 5, 0));
    return 0;
}

// gfx.shadows(cx, cy, cz, radius [, strength]): the sun casts shadows inside this ball of the world.
int l_shadows(lua_State* l) {
    float center[3] = {num(l, 1, 0), num(l, 2, 0), num(l, 3, 0)};
    gfx::shadows(center, num(l, 4, 0), num(l, 5, 0.6f));
    return 0;
}

int l_frame_param(lua_State* l) {
    float v[4] = {num(l, 2, 0), num(l, 3, 0), num(l, 4, 0), num(l, 5, 0)};
    gfx::frame_param((int)luaL_checkinteger(l, 1) - 1, v);
    return 0;
}

int l_target(lua_State* l) {
    gfx::target(handle(l, 1));
    return 0;
}

// ---- gfx: how the next things are drawn ----

int l_shader(lua_State* l) {
    gfx::shader(luaL_optstring(l, 1, ""));
    return 0;
}

int l_texture(lua_State* l) {
    int slot = (int)luaL_checkinteger(l, 1);
    uint32_t tex = handle(l, 2);
    int w, h;
    if (slot < 1 || slot > 4) return luaL_error(l, "gfx.texture: the slot is 1 to 4");
    if (tex && !gfx::texture_size(tex, w, h)) return luaL_error(l, "gfx.texture: that texture is gone (freed, or loaded before the game reloaded)");
    gfx::texture(slot - 1, tex);
    return 0;
}

int l_blend(lua_State* l) {
    std::string mode = luaL_optstring(l, 1, "opaque");
    gfx::blend(mode == "add" ? gfx::BLEND_ADD : mode == "alpha" ? gfx::BLEND_ALPHA : gfx::BLEND_OPAQUE);
    return 0;
}

int l_cull(lua_State* l) { gfx::cull(flag(l, 1, true)); return 0; }
int l_unlit(lua_State* l) { gfx::unlit(flag(l, 1, true)); return 0; }
int l_depth(lua_State* l) { gfx::depth(flag(l, 1, true), flag(l, 2, true)); return 0; }
int l_cutoff(lua_State* l) { gfx::cutoff(num(l, 1, 0)); return 0; }
int l_cast_shadows(lua_State* l) { gfx::cast_shadows(flag(l, 1, true)); return 0; }
int l_font(lua_State* l) { gfx::font(handle(l, 1)); return 0; }
int l_reset(lua_State*) { gfx::reset_state(); return 0; }

int l_param(lua_State* l) {
    float v[4] = {num(l, 2, 0), num(l, 3, 0), num(l, 4, 0), num(l, 5, 0)};
    gfx::param((int)luaL_checkinteger(l, 1) - 1, v);
    return 0;
}

// ---- gfx: textures, meshes, fonts ----

int l_load_texture(lua_State* l) {
    const char* name = luaL_checkstring(l, 1);
    uint32_t tex = gfx::load_texture(name, texture_flags(l, 2));
    if (tex == gfx::missing_texture()) report("warn", std::string("missing texture '") + name + "' (drawn as a pink checker)");
    lua_pushinteger(l, tex);
    return 1;
}

// gfx.new_texture(w, h, pixels [, options]): pixels is a string of w*h*4 bytes, red green blue alpha.
int l_new_texture(lua_State* l) {
    int w = (int)luaL_checkinteger(l, 1), h = (int)luaL_checkinteger(l, 2);
    size_t size = 0;
    const char* pixels = luaL_checklstring(l, 3, &size);
    if (w <= 0 || h <= 0 || size != (size_t)w * (size_t)h * 4) return luaL_error(l, "gfx.new_texture: the pixel string must be width * height * 4 bytes long");
    lua_pushinteger(l, gfx::new_texture(w, h, (const uint8_t*)pixels, texture_flags(l, 4), true));
    return 1;
}

int l_new_target(lua_State* l) {
    uint32_t made = gfx::new_target((int)luaL_checkinteger(l, 1), (int)luaL_checkinteger(l, 2));
    if (!made) return luaL_error(l, "gfx.new_target: couldn't make a target that size");
    lua_pushinteger(l, made);
    return 1;
}

int l_texture_size(lua_State* l) {
    int w = 0, h = 0;
    if (!gfx::texture_size(handle(l, 1), w, h)) return 0;
    lua_pushinteger(l, w);
    lua_pushinteger(l, h);
    return 2;
}

int l_free_texture(lua_State* l) { gfx::free_texture(handle(l, 1)); return 0; }
int l_free_mesh(lua_State* l) { gfx::free_mesh(handle(l, 1)); return 0; }

// gfx.new_mesh(numbers [, indices [, layout]]). layout says what each vertex holds, in order:
// p position (3 numbers), n normal (3), u texture coordinate (2), c colour (4). The default is "pnu".
// indices count from 1. Leave them out when every three vertices are one triangle.
int l_new_mesh(lua_State* l) {
    luaL_checktype(l, 1, LUA_TTABLE);
    std::string layout = luaL_optstring(l, 3, "pnu");
    int stride = 0;
    bool hasNormal = false;
    for (char c : layout) {
        if (c == 'p' || c == 'n') stride += 3;
        else if (c == 'u') stride += 2;
        else if (c == 'c') stride += 4;
        else return luaL_error(l, "gfx.new_mesh: the layout uses only the letters p, n, u and c");
        hasNormal |= c == 'n';
    }
    lua_Integer count = luaL_len(l, 1);
    if (stride == 0 || layout.find('p') == std::string::npos || count % stride != 0) return luaL_error(l, "gfx.new_mesh: expected %d numbers for every vertex", stride);
    std::vector<gfx::Vertex> vertices((size_t)(count / stride));
    lua_Integer at = 1;
    auto next = [&]() {
        lua_rawgeti(l, 1, at++);
        float v = (float)lua_tonumber(l, -1);
        lua_pop(l, 1);
        return v;
    };
    for (gfx::Vertex& v : vertices) {
        memset(&v, 0, sizeof(v));
        v.color[0] = v.color[1] = v.color[2] = v.color[3] = 1;
        for (char c : layout) {
            float* into = c == 'p' ? v.pos : c == 'n' ? v.normal : c == 'u' ? v.uv : v.color;
            int n = c == 'u' ? 2 : c == 'c' ? 4 : 3;
            for (int i = 0; i < n; i++) into[i] = next();
        }
    }
    std::vector<uint32_t> indices;
    if (lua_istable(l, 2)) {
        lua_Integer n = luaL_len(l, 2);
        indices.resize((size_t)n);
        for (lua_Integer i = 1; i <= n; i++) {
            lua_rawgeti(l, 2, i);
            lua_Integer index = lua_tointeger(l, -1);
            lua_pop(l, 1);
            if (index < 1 || index > (lua_Integer)vertices.size()) return luaL_error(l, "gfx.new_mesh: index %d points at no vertex", (int)index);
            indices[(size_t)(i - 1)] = (uint32_t)(index - 1);
        }
    } else {
        indices.resize(vertices.size());
        for (size_t i = 0; i < indices.size(); i++) indices[i] = (uint32_t)i;
    }
    if (indices.empty() || indices.size() % 3 != 0) return luaL_error(l, "gfx.new_mesh: triangles need three corners each");
    gfx::finish_vertices(vertices, indices, !hasNormal);
    lua_pushinteger(l, gfx::new_mesh(vertices, indices, true));
    return 1;
}

int l_load_font(lua_State* l) {
    const char* name = luaL_checkstring(l, 1);
    uint32_t made = gfx::load_font(name, num(l, 2, 48));
    if (!made) report("warn", std::string("missing font '") + name + "' (the built-in font is used)");
    lua_pushinteger(l, made);
    return 1;
}

// ---- gfx: 3D drawing ----

// gfx.draw(mesh,  x, y, z,  qx, qy, qz, qw,  sx, sy, sz,  r, g, b, a)
int l_draw(lua_State* l) {
    uint32_t mesh = handle(l, 1);
    if (!gfx::mesh_data(mesh)) return luaL_error(l, "gfx.draw: that mesh is gone (freed, or made before the game reloaded)");
    float m[16], color[4];
    read_matrix(l, 2, m);
    read_color(l, 12, color);
    gfx::draw(mesh, m, color);
    return 0;
}

// gfx.cube(x, y, z,  sx, sy, sz,  r, g, b [, yaw [, alpha]])
int l_cube(lua_State* l) {
    float pos[3] = {num(l, 1, 0), num(l, 2, 0), num(l, 3, 0)}, size[3] = {num(l, 4, 1), num(l, 5, 1), num(l, 6, 1)};
    float color[4] = {num(l, 7, 1), num(l, 8, 1), num(l, 9, 1), num(l, 11, 1)};
    gfx::mesh(gfx::MESH_CUBE, pos, size, num(l, 10, 0), color);
    return 0;
}

// gfx.sphere(x, y, z,  diameter,  r, g, b [, alpha])
int l_sphere(lua_State* l) {
    float d = num(l, 4, 1);
    float pos[3] = {num(l, 1, 0), num(l, 2, 0), num(l, 3, 0)}, size[3] = {d, d, d}, color[4] = {num(l, 5, 1), num(l, 6, 1), num(l, 7, 1), num(l, 8, 1)};
    gfx::mesh(gfx::MESH_SPHERE, pos, size, 0, color);
    return 0;
}

// gfx.plane(x, y, z,  sx, sz,  r, g, b [, alpha])
int l_plane(lua_State* l) {
    float pos[3] = {num(l, 1, 0), num(l, 2, 0), num(l, 3, 0)}, size[3] = {num(l, 4, 1), 1, num(l, 5, 1)}, color[4] = {num(l, 6, 1), num(l, 7, 1), num(l, 8, 1), num(l, 9, 1)};
    gfx::mesh(gfx::MESH_PLANE, pos, size, 0, color);
    return 0;
}

int l_sky(lua_State* l) {
    float color[4] = {num(l, 2, 1), num(l, 3, 1), num(l, 4, 1), 1};
    gfx::sky(handle(l, 1), color);
    return 0;
}

// gfx.particles(numbers [, count]): eight numbers each: x, y, z, size, r, g, b, a.
int l_particles(lua_State* l) {
    luaL_checktype(l, 1, LUA_TTABLE);
    lua_Integer count = luaL_optinteger(l, 2, luaL_len(l, 1) / 8);
    if (count <= 0) return 0;
    static std::vector<float> data;
    data.resize((size_t)count * 8);
    for (lua_Integer i = 0; i < count * 8; i++) {
        lua_rawgeti(l, 1, i + 1);
        data[(size_t)i] = (float)lua_tonumber(l, -1);
        lua_pop(l, 1);
    }
    gfx::particles(data.data(), (int)count);
    return 0;
}

// gfx.line(x1, y1, z1,  x2, y2, z2,  r, g, b, a [, width])
int l_line(lua_State* l) {
    float a[3] = {num(l, 1, 0), num(l, 2, 0), num(l, 3, 0)}, b[3] = {num(l, 4, 0), num(l, 5, 0), num(l, 6, 0)}, color[4];
    read_color(l, 7, color);
    gfx::line(a, b, color, num(l, 11, 0.03f));
    return 0;
}

// gfx.project(x, y, z) -> the 2D screen position, or nothing when the point is behind the camera.
int l_project(lua_State* l) {
    float world[3] = {num(l, 1, 0), num(l, 2, 0), num(l, 3, 0)}, x = 0, y = 0;
    if (!gfx::project(world, x, y)) return 0;
    lua_pushnumber(l, x);
    lua_pushnumber(l, y);
    return 2;
}

// gfx.unproject(sx, sy) -> a ray into the world from that point on the screen: ox, oy, oz, dx, dy, dz.
int l_unproject(lua_State* l) {
    float origin[3], dir[3];
    gfx::unproject(num(l, 1, 0), num(l, 2, 0), origin, dir);
    for (float v : origin) lua_pushnumber(l, v);
    for (float v : dir) lua_pushnumber(l, v);
    return 6;
}

// ---- gfx: 2D drawing ----

int l_rect(lua_State* l) {
    float color[4];
    read_color(l, 5, color);
    gfx::rect(num(l, 1, 0), num(l, 2, 0), num(l, 3, 0), num(l, 4, 0), color);
    return 0;
}

int l_circle(lua_State* l) {
    float color[4];
    read_color(l, 4, color);
    gfx::circle(num(l, 1, 0), num(l, 2, 0), num(l, 3, 10), color);
    return 0;
}

// gfx.image(texture, x, y, w, h [, r, g, b, a [, u0, v0, u1, v1]])
int l_image(lua_State* l) {
    float color[4], uv[4] = {num(l, 10, 0), num(l, 11, 0), num(l, 12, 1), num(l, 13, 1)};
    read_color(l, 6, color);
    gfx::image(handle(l, 1), num(l, 2, 0), num(l, 3, 0), num(l, 4, 0), num(l, 5, 0), color, uv);
    return 0;
}

int l_text(lua_State* l) {
    float color[4];
    read_color(l, 5, color);
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

// gfx.safe() -> left, top, right, bottom: how far to stay in from each edge to clear the notch and home bar.
int l_safe(lua_State* l) {
    for (int i = 0; i < 4; i++) lua_pushnumber(l, gfx::safe_area()[i]);
    return 4;
}

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

// ---- phys ----

uint32_t layer_arg(lua_State* l, int index, uint32_t fallback) { return (uint32_t)luaL_optinteger(l, index, fallback); }

int l_phys_sphere(lua_State* l) {
    float pos[3] = {num(l, 1, 0), num(l, 2, 0), num(l, 3, 0)};
    lua_pushinteger(l, collide::add_sphere(pos, num(l, 4, 0.5f), layer_arg(l, 5, 1)));
    return 1;
}

// phys.capsule(x, y, z, radius, height [, layer]): upright, with x y z at its middle.
int l_phys_capsule(lua_State* l) {
    float pos[3] = {num(l, 1, 0), num(l, 2, 0), num(l, 3, 0)};
    lua_pushinteger(l, collide::add_capsule(pos, num(l, 4, 0.35f), num(l, 5, 1.8f), layer_arg(l, 6, 1)));
    return 1;
}

// phys.box(x, y, z,  sx, sy, sz [, qx, qy, qz, qw [, layer]]): sizes are full widths, like gfx.cube.
int l_phys_box(lua_State* l) {
    float pos[3] = {num(l, 1, 0), num(l, 2, 0), num(l, 3, 0)}, half[3] = {num(l, 4, 1) / 2, num(l, 5, 1) / 2, num(l, 6, 1) / 2};
    float quat[4] = {num(l, 7, 0), num(l, 8, 0), num(l, 9, 0), num(l, 10, 1)};
    lua_pushinteger(l, collide::add_box(pos, quat, half, layer_arg(l, 11, 1)));
    return 1;
}

// phys.mesh(mesh,  x, y, z,  qx, qy, qz, qw,  sx, sy, sz [, layer]): solid scenery that never moves.
int l_phys_mesh(lua_State* l) {
    const gfx::MeshData* data = gfx::mesh_data(handle(l, 1));
    if (!data) return luaL_error(l, "phys.mesh: that mesh is gone");
    float world[16];
    read_matrix(l, 2, world);
    lua_pushinteger(l, collide::add_mesh(*data, world, layer_arg(l, 12, 1)));
    return 1;
}

// phys.model(m,  x, y, z,  qx, qy, qz, qw,  sx, sy, sz [, layer [, node]]) -> a list of bodies, one per mesh in the model.
int l_phys_model(lua_State* l) {
    uint32_t m = handle(l, 1);
    const std::vector<model::Node>& nodes = checked_nodes(l, m, "phys.model");
    float world[16];
    read_matrix(l, 2, world);
    uint32_t layer = layer_arg(l, 12, 1);
    int only = lua_isnoneornil(l, 13) ? -1 : node_arg(l, m, 13, "phys.model");
    lua_newtable(l);
    lua_Integer made = 0;
    for (size_t i = 0; i < nodes.size(); i++) {
        if (only >= 0 && (int)i != only) continue;
        float placed[16];
        gfx::mat_multiply(world, nodes[i].world, placed);
        for (gfx::Handle mesh : model::node_meshes(m, (int)i)) {
            const gfx::MeshData* data = gfx::mesh_data(mesh);
            uint32_t body = data ? collide::add_mesh(*data, placed, layer) : 0;
            if (!body) continue;
            lua_pushinteger(l, body);
            lua_rawseti(l, -2, ++made);
        }
    }
    return 1;
}

// phys.place(body, x, y, z [, qx, qy, qz, qw])
int l_phys_place(lua_State* l) {
    float pos[3] = {num(l, 2, 0), num(l, 3, 0), num(l, 4, 0)}, quat[4] = {num(l, 5, 0), num(l, 6, 0), num(l, 7, 0), num(l, 8, 1)};
    if (!collide::place(handle(l, 1), pos, lua_isnoneornil(l, 5) ? nullptr : quat)) return luaL_error(l, "phys.place: that body is gone, or is a mesh (meshes never move)");
    return 0;
}

int l_phys_position(lua_State* l) {
    float pos[3];
    if (!collide::position(handle(l, 1), pos)) return luaL_error(l, "phys.position: that body is gone");
    for (float v : pos) lua_pushnumber(l, v);
    return 3;
}

int l_phys_remove(lua_State* l) { collide::remove(handle(l, 1)); return 0; }
int l_phys_clear(lua_State*) { collide::clear(); return 0; }
int l_phys_debug(lua_State*) { collide::debug_draw(); return 0; }

int push_hit(lua_State* l, const collide::Hit& hit) {
    if (!hit.hit) return 0;
    lua_pushnumber(l, hit.distance);
    for (float v : hit.point) lua_pushnumber(l, v);
    for (float v : hit.normal) lua_pushnumber(l, v);
    lua_pushinteger(l, hit.body);
    return 8;
}

// phys.raycast(ox, oy, oz,  dx, dy, dz,  distance [, mask [, ignore]]) -> nothing, or: distance, x, y, z, nx, ny, nz, body
int l_phys_raycast(lua_State* l) {
    float origin[3] = {num(l, 1, 0), num(l, 2, 0), num(l, 3, 0)}, dir[3] = {num(l, 4, 0), num(l, 5, -1), num(l, 6, 0)};
    return push_hit(l, collide::raycast(origin, dir, num(l, 7, 1000), layer_arg(l, 8, 0xFFFFFFFFu), handle(l, 9)));
}

// phys.spherecast(ox, oy, oz,  dx, dy, dz,  distance, radius [, mask [, ignore]]): a ball swept along a line.
int l_phys_spherecast(lua_State* l) {
    float origin[3] = {num(l, 1, 0), num(l, 2, 0), num(l, 3, 0)}, dir[3] = {num(l, 4, 0), num(l, 5, -1), num(l, 6, 0)};
    return push_hit(l, collide::spherecast(origin, dir, num(l, 7, 1000), num(l, 8, 0.25f), layer_arg(l, 9, 0xFFFFFFFFu), handle(l, 10)));
}

// phys.overlap(x, y, z, radius [, mask]) -> a list of the bodies that ball touches.
int l_phys_overlap(lua_State* l) {
    float center[3] = {num(l, 1, 0), num(l, 2, 0), num(l, 3, 0)};
    static std::vector<collide::Handle> found;
    collide::overlap(center, num(l, 4, 0.5f), layer_arg(l, 5, 0xFFFFFFFFu), found);
    lua_createtable(l, (int)found.size(), 0);
    for (size_t i = 0; i < found.size(); i++) {
        lua_pushinteger(l, found[i]);
        lua_rawseti(l, -2, (lua_Integer)i + 1);
    }
    return 1;
}

// phys.move(body,  dx, dy, dz [, mask [, step, slope]]) -> x, y, z, grounded, blocked, nx, ny, nz
// Moves a sphere or capsule, sliding along what it meets. step: the tallest kerb it climbs. slope: the steepest hill, in degrees.
int l_phys_move(lua_State* l) {
    float delta[3] = {num(l, 2, 0), num(l, 3, 0), num(l, 4, 0)}, unused[3];
    uint32_t body = handle(l, 1);
    if (!collide::position(body, unused)) return luaL_error(l, "phys.move: that body is gone");
    collide::Moved moved = collide::slide(body, delta, layer_arg(l, 5, 0xFFFFFFFFu), num(l, 6, 0.3f), num(l, 7, 50));
    for (float v : moved.pos) lua_pushnumber(l, v);
    lua_pushboolean(l, moved.grounded);
    lua_pushboolean(l, moved.blocked);
    for (float v : moved.normal) lua_pushnumber(l, v);
    return 8;
}

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

// ---- save ----

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

int l_platform(lua_State* l) {
#if defined(SDL_PLATFORM_IOS)
    lua_pushstring(l, "ios");
#else
    lua_pushstring(l, "pc");
#endif
    return 1;
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

}  // namespace

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
    const luaL_Reg libs[] = {{"_G", luaopen_base}, {"table", luaopen_table}, {"string", luaopen_string},
                             {"math", luaopen_math}, {"utf8", luaopen_utf8}, {"coroutine", luaopen_coroutine}};
    for (const luaL_Reg& lib : libs) {
        luaL_requiref(L, lib.name, lib.func, 1);
        lua_pop(L, 1);
    }
    // Games read files through include(), which works the same on the PC and inside the app.
    lua_pushnil(L); lua_setglobal(L, "dofile");
    lua_pushnil(L); lua_setglobal(L, "loadfile");

    const luaL_Reg gfxFunctions[] = {
        {"clear", l_clear}, {"camera", l_camera}, {"light", l_light}, {"ambient", l_ambient}, {"point_light", l_point_light},
        {"spot_light", l_spot_light}, {"fog", l_fog}, {"shadows", l_shadows}, {"frame_param", l_frame_param}, {"target", l_target},
        {"shader", l_shader}, {"texture", l_texture}, {"blend", l_blend}, {"cull", l_cull}, {"unlit", l_unlit}, {"depth", l_depth},
        {"cutoff", l_cutoff}, {"cast_shadows", l_cast_shadows}, {"param", l_param}, {"font", l_font}, {"reset", l_reset},
        {"load_texture", l_load_texture}, {"new_texture", l_new_texture}, {"new_target", l_new_target}, {"texture_size", l_texture_size},
        {"free_texture", l_free_texture}, {"new_mesh", l_new_mesh}, {"free_mesh", l_free_mesh}, {"load_font", l_load_font},
        {"draw", l_draw}, {"cube", l_cube}, {"sphere", l_sphere}, {"plane", l_plane}, {"sky", l_sky}, {"particles", l_particles},
        {"line", l_line}, {"project", l_project}, {"unproject", l_unproject},
        {"rect", l_rect}, {"circle", l_circle}, {"image", l_image}, {"text", l_text}, {"text_width", l_text_width},
        {"size", l_size}, {"safe", l_safe}, {nullptr, nullptr}};
    const luaL_Reg modelFunctions[] = {{"load", l_model_load}, {"nodes", l_model_nodes}, {"clips", l_model_clips}, {"draw", l_model_draw},
                                       {"animator", l_model_animator}, {"pose", l_model_pose}, {"joint", l_model_joint},
                                       {"local", l_model_local}, {"sample", l_model_sample}, {nullptr, nullptr}};
    const luaL_Reg physFunctions[] = {{"sphere", l_phys_sphere}, {"capsule", l_phys_capsule}, {"box", l_phys_box}, {"mesh", l_phys_mesh},
                                      {"model", l_phys_model}, {"place", l_phys_place}, {"position", l_phys_position}, {"remove", l_phys_remove},
                                      {"clear", l_phys_clear}, {"raycast", l_phys_raycast}, {"spherecast", l_phys_spherecast},
                                      {"overlap", l_phys_overlap}, {"move", l_phys_move}, {"debug", l_phys_debug}, {nullptr, nullptr}};
    const luaL_Reg audioFunctions[] = {{"load", l_audio_load}, {"play", l_audio_play}, {"stop", l_audio_stop}, {"playing", l_audio_playing},
                                       {"volume", l_audio_volume}, {"pitch", l_audio_pitch}, {"pan", l_audio_pan}, {"lowpass", l_audio_lowpass},
                                       {"position", l_audio_position}, {"listener", l_audio_listener}, {"group", l_audio_group},
                                       {"group_pause", l_audio_group_pause}, {nullptr, nullptr}};
    const luaL_Reg inputFunctions[] = {{"controls", l_controls}, {"stick", l_stick}, {"down", l_down}, {"pressed", l_pressed},
                                       {"touches", l_touches}, {"key", l_key}, {"tilt", l_tilt}, {"gyro", l_gyro}, {"pad", l_pad},
                                       {"haptic", l_haptic}, {nullptr, nullptr}};
    const luaL_Reg saveFunctions[] = {{"get", l_save_get}, {"set", l_save_set}, {nullptr, nullptr}};
    const luaL_Reg sysFunctions[] = {{"time", l_time}, {"real_time", l_real_time}, {"time_scale", l_time_scale}, {"step", l_step},
                                     {"hud", l_hud}, {"stats", l_stats}, {"platform", l_platform}, {nullptr, nullptr}};
    register_table("gfx", gfxFunctions);
    register_table("model", modelFunctions);
    register_table("phys", physFunctions);
    register_table("audio", audioFunctions);
    register_table("input", inputFunctions);
    register_table("save", saveFunctions);
    register_table("sys", sysFunctions);
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
    input::set_controls({});
    collide::clear();
}

void update(float dt) {
    if (!L) return;
    lastDt = dt;
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
