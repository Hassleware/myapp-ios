// The Lua phys.*: collision bodies and the questions asked of them (engine/phys.cpp).
// Part of the Lua host (engine/script.cpp); lua_api.h has the helpers and state the binding files share.
#include "lua_api.h"

#include <SDL3/SDL.h>

#include <cstring>
#include <vector>

#include "phys.h"
#include "model.h"

namespace script {
namespace api {

// ---- phys ----

uint32_t layer_arg(lua_State* l, int index, uint32_t fallback) { return (uint32_t)luaL_optinteger(l, index, fallback); }

int l_phys_sphere(lua_State* l) {
    float pos[3] = {num(l, 1, 0), num(l, 2, 0), num(l, 3, 0)};
    lua_pushinteger(l, phys::add_sphere(pos, num(l, 4, 0.5f), layer_arg(l, 5, 1)));
    return 1;
}

// phys.capsule(x, y, z, radius, height [, layer]): upright, with x y z at its middle.
int l_phys_capsule(lua_State* l) {
    float pos[3] = {num(l, 1, 0), num(l, 2, 0), num(l, 3, 0)};
    lua_pushinteger(l, phys::add_capsule(pos, num(l, 4, 0.35f), num(l, 5, 1.8f), layer_arg(l, 6, 1)));
    return 1;
}

// phys.box(x, y, z,  sx, sy, sz [, qx, qy, qz, qw [, layer]]): sizes are full widths, like gfx.cube.
int l_phys_box(lua_State* l) {
    float pos[3] = {num(l, 1, 0), num(l, 2, 0), num(l, 3, 0)}, half[3] = {num(l, 4, 1) / 2, num(l, 5, 1) / 2, num(l, 6, 1) / 2};
    float quat[4] = {num(l, 7, 0), num(l, 8, 0), num(l, 9, 0), num(l, 10, 1)};
    lua_pushinteger(l, phys::add_box(pos, quat, half, layer_arg(l, 11, 1)));
    return 1;
}

// phys.mesh(mesh,  x, y, z,  qx, qy, qz, qw,  sx, sy, sz [, layer]): solid scenery that never moves.
int l_phys_mesh(lua_State* l) {
    const gfx::MeshData* data = gfx::mesh_data(handle(l, 1));
    if (!data) return luaL_error(l, "phys.mesh: that mesh is gone");
    float world[16];
    read_matrix(l, 2, world);
    lua_pushinteger(l, phys::add_mesh(*data, world, layer_arg(l, 12, 1)));
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
            uint32_t body = data ? phys::add_mesh(*data, placed, layer) : 0;
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
    if (!phys::place(handle(l, 1), pos, lua_isnoneornil(l, 5) ? nullptr : quat)) return luaL_error(l, "phys.place: that body is gone, or is a mesh (meshes never move)");
    return 0;
}

int l_phys_position(lua_State* l) {
    float pos[3];
    if (!phys::position(handle(l, 1), pos)) return luaL_error(l, "phys.position: that body is gone");
    for (float v : pos) lua_pushnumber(l, v);
    return 3;
}

int l_phys_remove(lua_State* l) { phys::remove(handle(l, 1)); return 0; }
int l_phys_clear(lua_State*) { phys::clear(); return 0; }
int l_phys_debug(lua_State*) { phys::debug_draw(); return 0; }

int push_hit(lua_State* l, const phys::Hit& hit) {
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
    return push_hit(l, phys::raycast(origin, dir, num(l, 7, 1000), layer_arg(l, 8, 0xFFFFFFFFu), handle(l, 9)));
}

// phys.spherecast(ox, oy, oz,  dx, dy, dz,  distance, radius [, mask [, ignore]]): a ball swept along a line.
int l_phys_spherecast(lua_State* l) {
    float origin[3] = {num(l, 1, 0), num(l, 2, 0), num(l, 3, 0)}, dir[3] = {num(l, 4, 0), num(l, 5, -1), num(l, 6, 0)};
    return push_hit(l, phys::spherecast(origin, dir, num(l, 7, 1000), num(l, 8, 0.25f), layer_arg(l, 9, 0xFFFFFFFFu), handle(l, 10)));
}

// phys.overlap(x, y, z, radius [, mask]) -> a list of the bodies that ball touches.
int l_phys_overlap(lua_State* l) {
    float center[3] = {num(l, 1, 0), num(l, 2, 0), num(l, 3, 0)};
    static std::vector<phys::Handle> found;
    phys::overlap(center, num(l, 4, 0.5f), layer_arg(l, 5, 0xFFFFFFFFu), found);
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
    if (!phys::position(body, unused)) return luaL_error(l, "phys.move: that body is gone");
    phys::Moved moved = phys::slide(body, delta, layer_arg(l, 5, 0xFFFFFFFFu), num(l, 6, 0.3f), num(l, 7, 50));
    for (float v : moved.pos) lua_pushnumber(l, v);
    lua_pushboolean(l, moved.grounded);
    lua_pushboolean(l, moved.blocked);
    for (float v : moved.normal) lua_pushnumber(l, v);
    return 8;
}

void register_phys() {
    const luaL_Reg physFunctions[] = {{"sphere", l_phys_sphere}, {"capsule", l_phys_capsule}, {"box", l_phys_box}, {"mesh", l_phys_mesh},
                                      {"model", l_phys_model}, {"place", l_phys_place}, {"position", l_phys_position}, {"remove", l_phys_remove},
                                      {"clear", l_phys_clear}, {"raycast", l_phys_raycast}, {"spherecast", l_phys_spherecast},
                                      {"overlap", l_phys_overlap}, {"move", l_phys_move}, {"debug", l_phys_debug}, {nullptr, nullptr}};
    register_table("phys", physFunctions);
}

}  // namespace api
}  // namespace script
