#include "script/bind_log.h"

#include "core/logger.h"

// what print joins its arguments into
#define PRINT_CAP 1024

namespace bind_log {

// The calling script's file and line, so the log points at the Lua, not the
// binding. Copied out: lua_Debug holds the name inline and dies with the call.
static i32 where(lua_State *L, char *file, u64 cap) {
    lua_Debug ar;
    if (lua_getstack(L, 1, &ar) && lua_getinfo(L, "Sl", &ar)) {
        snprintf(file, cap, "%s", ar.short_src);
        return ar.currentline;
    }
    snprintf(file, cap, "%s", "<script>");
    return 0;
}

static i32 write(lua_State *L, logger::Level level) {
    const char *msg = luaL_checkstring(L, 1);
    char file[LUA_IDSIZE];
    i32 line = where(L, file, sizeof(file));
    logger::write(level, file, line, "%s", msg);
    return 0;
}

static i32 trace(lua_State *L) { return write(L, logger::TRACE); }
static i32 debug(lua_State *L) { return write(L, logger::DEBUG); }
static i32 info(lua_State *L) { return write(L, logger::INFO); }
static i32 warn(lua_State *L) { return write(L, logger::WARN); }
static i32 error(lua_State *L) { return write(L, logger::ERR); }

// every argument through tostring, tab-separated, as Lua's own print does
static i32 print(lua_State *L) {
    char out[PRINT_CAP];
    u32 used = 0;

    i32 n = lua_gettop(L);
    for (i32 i = 1; i <= n && used < sizeof(out); i++) {
        const char *s = luaL_tolstring(L, i, NULL);
        i32 wrote = snprintf(out + used, sizeof(out) - used, "%s%s", i > 1 ? "\t" : "", s);
        lua_pop(L, 1);
        if (wrote > 0)
            used += (u32)wrote;
    }

    char file[LUA_IDSIZE];
    i32 line = where(L, file, sizeof(file));
    logger::write(logger::INFO, file, line, "%s", out);
    return 0;
}

static const bind::Function FUNCTIONS[] = {
    {"trace", trace}, {"debug", debug}, {"info", info}, {"warn", warn}, {"error", error},
};

void open(lua_State *L, bind::Context *c) {
    lua_createtable(L, 0, ARRAY_COUNT(FUNCTIONS));
    bind::set_functions(L, c, FUNCTIONS, ARRAY_COUNT(FUNCTIONS));
    lua_setfield(L, -2, "log");

    // print is a global by convention; it lands in the log like everything else
    lua_pushlightuserdata(L, c);
    lua_pushcclosure(L, print, 1);
    lua_setglobal(L, "print");
}

} // namespace bind_log
