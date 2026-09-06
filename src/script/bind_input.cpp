#include "script/bind_input.h"

namespace bind_input {

// a state made without devices, as a test's is, has nothing to read
static input::State *check_input(lua_State *L, bind::Context *c) {
    if (!c->input)
        luaL_error(L, "this state has no input to read");
    return c->input;
}

// ACTION_NONE is nil, and reads as never held, as the header promises
static input::Action check_action(lua_State *L, i32 index) {
    if (lua_isnoneornil(L, index))
        return input::ACTION_NONE;
    return input::Action{(u32)luaL_checkinteger(L, index)};
}

static void push_action(lua_State *L, input::Action a) {
    if (a.v)
        lua_pushinteger(L, (lua_Integer)a.v);
    else
        lua_pushnil(L);
}

static i32 find(lua_State *L) {
    bind::Context *c = bind::context(L);
    push_action(L, input::find(check_input(L, c), luaL_checkstring(L, 1)));
    return 1;
}

static i32 down(lua_State *L) {
    bind::Context *c = bind::context(L);
    lua_pushboolean(L, input::down(check_input(L, c), check_action(L, 1)));
    return 1;
}

static i32 pressed(lua_State *L) {
    bind::Context *c = bind::context(L);
    bind::require_phase(L, c, bind::PHASE_FRAME, "input.pressed");
    lua_pushboolean(L, input::pressed(check_input(L, c), check_action(L, 1)));
    return 1;
}

static i32 released(lua_State *L) {
    bind::Context *c = bind::context(L);
    bind::require_phase(L, c, bind::PHASE_FRAME, "input.released");
    lua_pushboolean(L, input::released(check_input(L, c), check_action(L, 1)));
    return 1;
}

static i32 consume_pressed(lua_State *L) {
    bind::Context *c = bind::context(L);
    bind::require_phase(L, c, bind::PHASE_FIXED, "input.consume_pressed");
    lua_pushboolean(L, input::consume_pressed(check_input(L, c), check_action(L, 1)));
    return 1;
}

static i32 cursor(lua_State *L) {
    bind::Context *c = bind::context(L);
    bind::push_vec2(L, input::cursor(check_input(L, c)));
    return 1;
}

static i32 cursor_delta(lua_State *L) {
    bind::Context *c = bind::context(L);
    bind::push_vec2(L, input::cursor_delta(check_input(L, c)));
    return 1;
}

static i32 scroll(lua_State *L) {
    bind::Context *c = bind::context(L);
    lua_pushnumber(L, (lua_Number)input::scroll(check_input(L, c)));
    return 1;
}

static i32 mouse_look(lua_State *L) {
    bind::Context *c = bind::context(L);
    lua_pushboolean(L, input::mouse_look(check_input(L, c)));
    return 1;
}

static const bind::Function FUNCTIONS[] = {
    {"find", find},
    {"down", down},
    {"pressed", pressed},
    {"released", released},
    {"consume_pressed", consume_pressed},
    {"cursor", cursor},
    {"cursor_delta", cursor_delta},
    {"scroll", scroll},
    {"mouse_look", mouse_look},
};

void open(lua_State *L, bind::Context *c) {
    lua_createtable(L, 0, ARRAY_COUNT(FUNCTIONS));
    bind::set_functions(L, c, FUNCTIONS, ARRAY_COUNT(FUNCTIONS));
    lua_setfield(L, -2, "input");
}

} // namespace bind_input
