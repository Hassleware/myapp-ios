// The Lua gfx.*: the scene, how things are drawn, textures, meshes and fonts, 3D and 2D drawing.
// Part of the Lua host (engine/script.cpp); lua_api.h has the helpers and state the binding files share.
#include "lua_api.h"

#include <SDL3/SDL.h>

#include <cstring>
#include <vector>

#include "gfx.h"

namespace script {
namespace api {

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

void register_gfx() {
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
    register_table("gfx", gfxFunctions);
}

}  // namespace api
}  // namespace script
