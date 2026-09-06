#include "script/bind.h"

namespace bind {

Context *context(lua_State *L) {
    Context *c = (Context *)lua_touserdata(L, lua_upvalueindex(1));
    ASSERT(c);
    return c;
}

void set_functions(lua_State *L, Context *c, const Function *fns, u32 count) {
    for (u32 i = 0; i < count; i++) {
        lua_pushlightuserdata(L, c);
        lua_pushcclosure(L, fns[i].fn, 1);
        lua_setfield(L, -2, fns[i].name);
    }
}

static void set_number(lua_State *L, const char *key, f32 v) {
    lua_pushnumber(L, (lua_Number)v);
    lua_setfield(L, -2, key);
}

// a named component of the table at index, which must be a number
static f32 check_component(lua_State *L, i32 index, const char *key) {
    lua_getfield(L, index, key);
    if (!lua_isnumber(L, -1)) {
        lua_pop(L, 1);
        luaL_error(L, "vector field '%s' is missing or not a number", key);
    }
    f32 v = (f32)lua_tonumber(L, -1);
    lua_pop(L, 1);
    return v;
}

void push_vec2(lua_State *L, linalg::Vec2 v) {
    lua_createtable(L, 0, 2);
    set_number(L, "x", v.x);
    set_number(L, "y", v.y);
}

void push_vec3(lua_State *L, linalg::Vec3 v) {
    lua_createtable(L, 0, 3);
    set_number(L, "x", v.x);
    set_number(L, "y", v.y);
    set_number(L, "z", v.z);
}

void push_vec4(lua_State *L, linalg::Vec4 v) {
    lua_createtable(L, 0, 4);
    set_number(L, "x", v.x);
    set_number(L, "y", v.y);
    set_number(L, "z", v.z);
    set_number(L, "w", v.w);
}

linalg::Vec3 check_vec3(lua_State *L, i32 index) {
    index = lua_absindex(L, index);
    luaL_checktype(L, index, LUA_TTABLE);
    return linalg::Vec3{check_component(L, index, "x"), check_component(L, index, "y"),
                        check_component(L, index, "z")};
}

linalg::Vec4 check_vec4(lua_State *L, i32 index) {
    index = lua_absindex(L, index);
    luaL_checktype(L, index, LUA_TTABLE);
    return linalg::Vec4{check_component(L, index, "x"), check_component(L, index, "y"),
                        check_component(L, index, "z"), check_component(L, index, "w")};
}

void push_handle(lua_State *L, entity::Handle h) {
    if (h.v)
        lua_pushinteger(L, (lua_Integer)h.v);
    else
        lua_pushnil(L);
}

entity::Handle check_handle(lua_State *L, i32 index) {
    if (lua_isnoneornil(L, index))
        return entity::NONE;
    return entity::Handle{(u32)luaL_checkinteger(L, index)};
}

entity::Entity *check_entity(Context *c, lua_State *L, i32 index) {
    entity::Handle h = check_handle(L, index);
    entity::Entity *e = entity::get(c->world, h);
    if (!e)
        luaL_argerror(L, index, "entity is not alive");
    return e;
}

void require_phase(lua_State *L, Context *c, Phase phase, const char *what) {
    if (c->phase == phase)
        return;
    if (phase == PHASE_FIXED)
        luaL_error(L, "%s reads an edge on the fixed clock: call it from on_fixed_update", what);
    else
        luaL_error(L, "%s reads an edge at frame rate: call it from on_update", what);
}

} // namespace bind
