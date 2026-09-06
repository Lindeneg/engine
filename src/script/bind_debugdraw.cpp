#include "script/bind_debugdraw.h"

#include "gfx/debugdraw.h"

namespace bind_debugdraw {

// the optional style table at index; absent is this frame only, depth tested
static debugdraw::Style opt_style(lua_State *L, i32 index) {
    debugdraw::Style s = {};
    if (lua_isnoneornil(L, index))
        return s;

    luaL_checktype(L, index, LUA_TTABLE);
    lua_getfield(L, index, "lifetime");
    s.lifetime = (f32)luaL_optnumber(L, -1, 0.0);
    lua_pop(L, 1);
    lua_getfield(L, index, "x_ray");
    s.x_ray = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return s;
}

static i32 line(lua_State *L) {
    debugdraw::line(bind::check_vec3(L, 1), bind::check_vec3(L, 2), bind::check_vec4(L, 3),
                    opt_style(L, 4));
    return 0;
}

static i32 box(lua_State *L) {
    debugdraw::box(bind::check_vec3(L, 1), bind::check_vec3(L, 2), bind::check_vec4(L, 3),
                   opt_style(L, 4));
    return 0;
}

static i32 sphere(lua_State *L) {
    debugdraw::sphere(bind::check_vec3(L, 1), (f32)luaL_checknumber(L, 2), bind::check_vec4(L, 3),
                      opt_style(L, 4));
    return 0;
}

static i32 cross(lua_State *L) {
    debugdraw::cross(bind::check_vec3(L, 1), (f32)luaL_checknumber(L, 2), bind::check_vec4(L, 3),
                     opt_style(L, 4));
    return 0;
}

static const bind::Function FUNCTIONS[] = {
    {"line", line},
    {"box", box},
    {"sphere", sphere},
    {"cross", cross},
};

void open(lua_State *L, bind::Context *c) {
    lua_createtable(L, 0, ARRAY_COUNT(FUNCTIONS));
    bind::set_functions(L, c, FUNCTIONS, ARRAY_COUNT(FUNCTIONS));
    lua_setfield(L, -2, "debugdraw");
}

} // namespace bind_debugdraw
