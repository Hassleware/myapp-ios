// The Lua net.*: HTTPS requests and the game's own backend (the safety rules are in engine/net.cpp).
// Part of the Lua host (engine/script.cpp); lua_api.h has the helpers and state the binding files share.
#include "lua_api.h"

#include <SDL3/SDL.h>

#include <cstring>
#include <vector>

#include "net.h"

namespace script {
namespace api {

std::map<int, int> netCallbacks;   // request id -> the Lua function waiting for its answer

// net.request({url=, method=, headers={}, body=, timeout=}, function(status, body, err) ... end)
// net.backend({path=, ...}, function(status, body, err) ... end): the same, to the game's own backend.
int send_request(lua_State* l, bool backend) {
    luaL_checktype(l, 1, LUA_TTABLE);
    luaL_checktype(l, 2, LUA_TFUNCTION);
    net::Request request;
    request.backend = backend;
    size_t size = 0;
    lua_getfield(l, 1, backend ? "path" : "url");
    request.url = luaL_checkstring(l, -1);
    lua_getfield(l, 1, "method");
    request.method = luaL_optstring(l, -1, "GET");
    lua_getfield(l, 1, "body");
    const char* body = luaL_optlstring(l, -1, "", &size);
    request.body.assign(body, size);
    lua_getfield(l, 1, "timeout");
    request.timeout = SDL_clamp((float)luaL_optnumber(l, -1, 15), 1.0f, 120.0f);
    lua_getfield(l, 1, "headers");
    if (lua_istable(l, -1)) {
        lua_pushnil(l);
        while (lua_next(l, -2)) {
            if (lua_type(l, -2) == LUA_TSTRING && lua_type(l, -1) == LUA_TSTRING) request.headers.push_back({lua_tostring(l, -2), lua_tostring(l, -1)});
            lua_pop(l, 1);
        }
    }
    int id = net::send(request);
    lua_pushvalue(l, 2);
    netCallbacks[id] = luaL_ref(l, LUA_REGISTRYINDEX);
    return 0;
}

int l_net_request(lua_State* l) { return send_request(l, false); }
int l_net_backend(lua_State* l) { return send_request(l, true); }

void register_net() {
    const luaL_Reg netFunctions[] = {{"request", l_net_request}, {"backend", l_net_backend}, {nullptr, nullptr}};
    register_table("net", netFunctions);
}

}  // namespace api
}  // namespace script
