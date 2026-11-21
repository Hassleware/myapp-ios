// The Lua save.*: a game's small key/value store.
// Part of the Lua host (engine/script.cpp); lua_api.h has the helpers and state the binding files share.
#include "lua_api.h"

#include <SDL3/SDL.h>

#include <cstring>
#include <vector>


namespace script {
namespace api {

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

void register_save() {
    const luaL_Reg saveFunctions[] = {{"get", l_save_get}, {"set", l_save_set}, {nullptr, nullptr}};
    register_table("save", saveFunctions);
}

}  // namespace api
}  // namespace script
