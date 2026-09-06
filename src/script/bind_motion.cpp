#include "script/bind_motion.h"

#include "scene/motion.h"

namespace bind_motion {

static void set_bool(lua_State *L, const char *key, b32 v) {
    lua_pushboolean(L, v);
    lua_setfield(L, -2, key);
}

static i32 move_and_slide(lua_State *L) {
    bind::Context *c = bind::context(L);
    linalg::Vec3 position = bind::check_vec3(L, 1);
    f32 radius = (f32)luaL_checknumber(L, 2);
    linalg::Vec3 motion = bind::check_vec3(L, 3);
    entity::Handle ignore = bind::check_handle(L, 4);

    motion::Result r = motion::move_and_slide(c->world, c->assets, position, radius, motion, ignore);

    lua_createtable(L, 0, 6);
    bind::push_vec3(L, r.position);
    lua_setfield(L, -2, "position");
    set_bool(L, "grounded", r.grounded);
    set_bool(L, "hit", r.hit);
    bind::push_vec3(L, r.normal);
    lua_setfield(L, -2, "normal");
    set_bool(L, "depenetrated", r.depenetrated);
    set_bool(L, "stuck", r.stuck);
    return 1;
}

static i32 ray_cast(lua_State *L) {
    bind::Context *c = bind::context(L);
    linalg::Vec3 origin = bind::check_vec3(L, 1);
    linalg::Vec3 dir = bind::check_vec3(L, 2);
    f32 max_distance = (f32)luaL_checknumber(L, 3);
    entity::Handle ignore = bind::check_handle(L, 4);

    motion::RayResult r = motion::ray_cast(c->world, origin, dir, max_distance, ignore);

    lua_createtable(L, 0, 5);
    set_bool(L, "hit", r.hit);
    bind::push_handle(L, r.hit ? r.entity : entity::NONE);
    lua_setfield(L, -2, "entity");
    bind::push_vec3(L, r.point);
    lua_setfield(L, -2, "point");
    bind::push_vec3(L, r.normal);
    lua_setfield(L, -2, "normal");
    lua_pushnumber(L, (lua_Number)r.distance);
    lua_setfield(L, -2, "distance");
    return 1;
}

static const bind::Function FUNCTIONS[] = {
    {"move_and_slide", move_and_slide},
    {"ray_cast", ray_cast},
};

void open(lua_State *L, bind::Context *c) {
    lua_createtable(L, 0, ARRAY_COUNT(FUNCTIONS));
    bind::set_functions(L, c, FUNCTIONS, ARRAY_COUNT(FUNCTIONS));
    lua_setfield(L, -2, "motion");
}

} // namespace bind_motion
