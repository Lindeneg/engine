#pragma once

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include "asset/asset.h"
#include "core/common.h"
#include "math/linalg.h"
#include "platform/input.h"
#include "scene/entity.h"

// What every binding file shares: the context a C function reaches through its
// closure, and how values cross the boundary. The rule for the surface itself
// is one sub-table of the `engine` global per bound header, the function names
// copied from the header minus the world and manager arguments, so the headers
// stay the documentation:
//
//     engine.entity.play(h, "walk")           -- entity::play(w, h, "walk")
//     engine.input.consume_pressed(jump)      -- input::consume_pressed(in, jump)
//
// Handles and action ids are integers, and nil stands for NONE both ways.
// Vectors are tables with named fields, {x=, y=, z=} and {x=, y=, z=, w=}.

#define BIND_GLOBAL "engine"

namespace bind {

// which hook is running, so an edge is read on the clock it belongs to
enum Phase { PHASE_NONE, PHASE_FIXED, PHASE_FRAME };

struct Context {
    lua_State *L;
    entity::World *world;
    asset::Manager *assets;
    input::State *input; // may be NULL where nothing is played, as in a test

    Phase phase;

    // main.lua at project open: register_component registers. Later, in a
    // play state, it only checks that the declaration is the one registered.
    b32 declaring;
};

// the context a bound function was given as its upvalue
Context *context(lua_State *L);

struct Function {
    const char *name;
    lua_CFunction fn;
};

// fills the table on top of the stack, each function with c as its upvalue
void set_functions(lua_State *L, Context *c, const Function *fns, u32 count);

void push_vec2(lua_State *L, linalg::Vec2 v);
void push_vec3(lua_State *L, linalg::Vec3 v);
void push_vec4(lua_State *L, linalg::Vec4 v);
linalg::Vec3 check_vec3(lua_State *L, i32 index);
linalg::Vec4 check_vec4(lua_State *L, i32 index);

// NONE is nil
void push_handle(lua_State *L, entity::Handle h);
entity::Handle check_handle(lua_State *L, i32 index);

// the entity behind the argument, or an error naming the argument
entity::Entity *check_entity(Context *c, lua_State *L, i32 index);

// raises unless the running hook is on that clock
void require_phase(lua_State *L, Context *c, Phase phase, const char *what);

} // namespace bind
