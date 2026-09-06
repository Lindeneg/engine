#include "script/script.h"

#include <stddef.h>
#include <string.h>

#include "core/logger.h"
#include "platform/file.h"
#include "platform/path.h"
#include "script/bind.h"
#include "script/bind_debugdraw.h"
#include "script/bind_entity.h"
#include "script/bind_input.h"
#include "script/bind_log.h"
#include "script/bind_motion.h"

// the registry table of loaded files: path -> hooks table, or false once one failed
#define CACHE_KEY "engine.scripts"

// a chunk named after its file, so an error reads "scripts/player.lua:12:"
#define CHUNK_NAME_CAP (PROJECT_PATH_CAP + 1)

namespace script {

enum Hook {
    HOOK_SPAWN,
    HOOK_DESPAWN,
    HOOK_UPDATE,
    HOOK_FIXED_UPDATE,
    HOOK_LATE_UPDATE,
    HOOK_COUNT,
};

static const char *HOOK_NAMES[HOOK_COUNT] = {"on_spawn", "on_despawn", "on_update",
                                              "on_fixed_update", "on_late_update"};

// the main script's hooks, run beside the entities' with an empty self
struct Main {
    i32 hooks_ref;
    i32 self_ref;
    u32 failed;
};

struct State {
    bind::Context ctx;
    memory::Arena *scratch;
    project::Project *project;
    u32 component;
    Main main;
    char last_error[SCRIPT_ERROR_CAP];
};

// the log line and the remembered one are the same text
#define SCRIPT_ERR(s, ...)                                                                         \
    do {                                                                                           \
        LOG_ERR(__VA_ARGS__);                                                                      \
        snprintf((s)->last_error, sizeof((s)->last_error), __VA_ARGS__);                          \
    } while (0)

static const Script SCRIPT_DEFAULTS = {.hooks_ref = LUA_NOREF, .self_ref = LUA_NOREF};
static const entity::FieldDesc SCRIPT_FIELDS[] = {
    {.key = "path",
     .type = entity::FIELD_STRING,
     .offset = (u32)offsetof(Script, path),
     .cap = ENTITY_PATH_CAP,
     .asset = entity::ASSET_SCRIPT},
};
static_assert(offsetof(Script, owner) == 0, "Script must start with owner");

static void before_remove(entity::World *w, entity::Handle h, void *comp, void *user);

StateResult init(memory::Arena *a, memory::Arena *scratch, entity::World *w, asset::Manager *m,
                 input::State *in, project::Project *proj) {
    ASSERT(a && scratch && w && m && proj);

    State *s = push_struct(a, State);
    s->ctx.world = w;
    s->ctx.assets = m;
    s->ctx.input = in;
    s->scratch = scratch;
    s->project = proj;
    s->main.hooks_ref = LUA_NOREF;
    s->main.self_ref = LUA_NOREF;

    entity::ComponentDesc desc = {
        .key = SCRIPT_COMPONENT_KEY,
        .size = sizeof(Script),
        .defaults = &SCRIPT_DEFAULTS,
        .fields = SCRIPT_FIELDS,
        .field_count = ARRAY_COUNT(SCRIPT_FIELDS),
        .before_remove = before_remove,
        .user = s,
    };
    i32 c = entity::register_component(w, &desc);
    if (c < 0) {
        LOG_ERR("script: the script component would not register");
        return StateResult{.err = result::ERR_INVALID};
    }
    s->component = (u32)c;
    return StateResult{.val = s};
}

u32 component(const State *s) {
    ASSERT(s);
    return s->component;
}

static i32 traceback(lua_State *L) {
    const char *msg = lua_tostring(L, 1);
    if (!msg)
        msg = "(error object is not a string)";
    luaL_traceback(L, L, msg, 1);
    return 1;
}

// only reachable from outside a protected call, which nothing here does
static i32 panic(lua_State *L) {
    LOG_FATAL("script: unprotected error: %s", lua_tostring(L, -1));
    return 0;
}

// A protected call of the function under nargs arguments. False leaves the
// error, traceback included, on top of the stack for the caller to report.
static b32 call(lua_State *L, i32 nargs, i32 nresults) {
    i32 base = lua_gettop(L) - nargs;
    lua_pushcfunction(L, traceback);
    lua_insert(L, base);
    i32 status = lua_pcall(L, nargs, nresults, base);
    lua_remove(L, base);
    return status == LUA_OK;
}

// the standard libraries a game script gets; no io or os, since scripts are project content
static const luaL_Reg LIBS[] = {
    {LUA_GNAME, luaopen_base},       {LUA_TABLIBNAME, luaopen_table},
    {LUA_STRLIBNAME, luaopen_string}, {LUA_MATHLIBNAME, luaopen_math},
    {LUA_UTF8LIBNAME, luaopen_utf8},
};

static void open_state(State *s) {
    ASSERT(!s->ctx.L);

    lua_State *L = luaL_newstate();
    ASSERT_MSG(L, "script: could not create a Lua state");
    lua_atpanic(L, panic);
    s->ctx.L = L;
    s->ctx.phase = bind::PHASE_NONE;

    for (u32 i = 0; i < ARRAY_COUNT(LIBS); i++) {
        luaL_requiref(L, LIBS[i].name, LIBS[i].func, 1);
        lua_pop(L, 1);
    }

    lua_createtable(L, 0, 5);
    bind_entity::open(L, &s->ctx);
    bind_input::open(L, &s->ctx);
    bind_motion::open(L, &s->ctx);
    bind_debugdraw::open(L, &s->ctx);
    bind_log::open(L, &s->ctx);
    lua_setglobal(L, BIND_GLOBAL);

    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, CACHE_KEY);
}

static void close_state(State *s) {
    ASSERT(s->ctx.L);
    lua_close(s->ctx.L);
    s->ctx.L = NULL;
    s->ctx.phase = bind::PHASE_NONE;
    s->main.hooks_ref = LUA_NOREF;
    s->main.self_ref = LUA_NOREF;
    s->main.failed = 0;
}

static b32 main_exists(State *s) {
    memory::ArenaTemp t = memory::temp_begin(s->scratch);
    b32 exists = file::exists(FPATH(s->scratch, asset::root(s->ctx.assets), s->project->script));
    memory::temp_end(t);
    return exists;
}

// Loads and runs the file at rel, under the project folder. True leaves the
// chunk's one result on the stack; false has logged why.
static b32 load_chunk(State *s, const char *rel) {
    lua_State *L = s->ctx.L;

    // The source is scratch only until Lua has compiled it. The chunk then
    // runs outside the scope: what it does may push into any arena, the
    // world's included, and that must outlive this call.
    memory::ArenaTemp t = memory::temp_begin(s->scratch);
    file::BlobResult blob =
        file::read(s->scratch, FPATH(s->scratch, asset::root(s->ctx.assets), rel));
    i32 status = LUA_ERRFILE;
    if (blob.err == result::OK) {
        char chunk[CHUNK_NAME_CAP];
        snprintf(chunk, sizeof(chunk), "@%s", rel);
        status = luaL_loadbuffer(L, (const char *)blob.val.data, blob.val.size, chunk);
    }
    memory::temp_end(t);

    if (blob.err != result::OK) {
        // read said why in the log; the panel gets the short of it
        snprintf(s->last_error, sizeof(s->last_error), "script: %s could not be read", rel);
        return false;
    }

    b32 ok = status == LUA_OK && call(L, 0, 1);
    if (!ok) {
        SCRIPT_ERR(s, "script: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    return ok;
}

// The hooks table of a file, by reference, loaded once per state. LUA_NOREF
// when it does not load or returns no table, logged the first time only.
static i32 hooks_for(State *s, const char *path) {
    lua_State *L = s->ctx.L;
    lua_getfield(L, LUA_REGISTRYINDEX, CACHE_KEY);
    lua_getfield(L, -1, path);

    if (lua_type(L, -1) == LUA_TTABLE) {
        i32 ref = luaL_ref(L, LUA_REGISTRYINDEX);
        lua_pop(L, 1);
        return ref;
    }
    if (lua_type(L, -1) == LUA_TBOOLEAN) {
        lua_pop(L, 2); // failed before; already reported
        return LUA_NOREF;
    }
    lua_pop(L, 1);

    b32 loaded = load_chunk(s, path);
    if (loaded && lua_type(L, -1) != LUA_TTABLE) {
        SCRIPT_ERR(s, "script: %s did not return a table of hooks", path);
        lua_pop(L, 1);
        loaded = false;
    }
    if (!loaded) {
        lua_pushboolean(L, false);
        lua_setfield(L, -2, path);
        lua_pop(L, 1);
        return LUA_NOREF;
    }

    lua_pushvalue(L, -1);
    lua_setfield(L, -3, path);
    i32 ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_pop(L, 1);
    return ref;
}

// One hook of one script, with its self and, on the clocks, dt. True when
// there was nothing to run or it ran; false has logged the error and says the
// hook is off, which the caller records against whoever owns it.
static b32 run_hook(State *s, i32 hooks_ref, i32 self_ref, Hook hook, b32 with_dt, f32 dt) {
    lua_State *L = s->ctx.L;
    if (hooks_ref == LUA_NOREF)
        return true;

    lua_rawgeti(L, LUA_REGISTRYINDEX, hooks_ref);
    lua_getfield(L, -1, HOOK_NAMES[hook]);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 2);
        return true;
    }
    lua_remove(L, -2);

    lua_rawgeti(L, LUA_REGISTRYINDEX, self_ref);
    if (with_dt)
        lua_pushnumber(L, (lua_Number)dt);

    if (call(L, with_dt ? 2 : 1, 0))
        return true;

    SCRIPT_ERR(s, "script: %s; %s is off for that script until the scripts reload",
               lua_tostring(L, -1), HOOK_NAMES[hook]);
    lua_pop(L, 1);
    return false;
}

static void run_main_hook(State *s, Hook hook, b32 with_dt, f32 dt) {
    u32 bit = 1u << hook;
    if (s->main.failed & bit)
        return;
    if (!run_hook(s, s->main.hooks_ref, s->main.self_ref, hook, with_dt, dt))
        s->main.failed |= bit;
}

// Marks the hook failed on the component in that slot, unless the hook itself
// destroyed the entity and the slot has since gone to another.
static void mark_failed(State *s, u32 slot, u32 owner, Hook hook) {
    Script *sc = (Script *)entity::component_at(s->ctx.world, s->component, slot);
    if (sc && sc->owner.v == owner)
        sc->failed |= 1u << hook;
}

// gives every unspawned script component its self and its on_spawn
static void spawn_pending(State *s) {
    lua_State *L = s->ctx.L;
    entity::World *w = s->ctx.world;

    for (u32 slot = entity::component_first(w, s->component); slot;
         slot = entity::component_next(w, s->component, slot)) {
        Script *sc = (Script *)entity::component_at(w, s->component, slot);
        if (sc->spawned)
            continue;

        sc->spawned = true;
        sc->failed = 0;
        sc->hooks_ref = sc->path[0] ? hooks_for(s, sc->path) : LUA_NOREF;

        lua_createtable(L, 0, 1);
        bind::push_handle(L, sc->owner);
        lua_setfield(L, -2, "entity");
        sc->self_ref = luaL_ref(L, LUA_REGISTRYINDEX);

        u32 owner = sc->owner.v;
        i32 hooks = sc->hooks_ref;
        i32 self = sc->self_ref;
        bind::Phase was = s->ctx.phase;
        s->ctx.phase = bind::PHASE_NONE;
        if (!run_hook(s, hooks, self, HOOK_SPAWN, false, 0.0f))
            mark_failed(s, slot, owner, HOOK_SPAWN);
        s->ctx.phase = was;
    }
}

// on_despawn and the references let go; the entity is still there for the hook
static void despawn(State *s, Script *sc) {
    if (!sc->spawned)
        return;

    bind::Phase was = s->ctx.phase;
    s->ctx.phase = bind::PHASE_NONE;
    if (!(sc->failed & (1u << HOOK_DESPAWN)))
        run_hook(s, sc->hooks_ref, sc->self_ref, HOOK_DESPAWN, false, 0.0f);
    s->ctx.phase = was;

    luaL_unref(s->ctx.L, LUA_REGISTRYINDEX, sc->self_ref);
    luaL_unref(s->ctx.L, LUA_REGISTRYINDEX, sc->hooks_ref);
    sc->self_ref = LUA_NOREF;
    sc->hooks_ref = LUA_NOREF;
    sc->spawned = false;
}

static void before_remove(entity::World *, entity::Handle, void *comp, void *user) {
    State *s = (State *)user;
    if (s->ctx.L)
        despawn(s, (Script *)comp);
}

static void run_all(State *s, Hook hook, bind::Phase phase, f32 dt) {
    entity::World *w = s->ctx.world;
    u32 bit = 1u << hook;

    s->ctx.phase = phase;
    run_main_hook(s, hook, true, dt);

    for (u32 slot = entity::component_first(w, s->component); slot;
         slot = entity::component_next(w, s->component, slot)) {
        Script *sc = (Script *)entity::component_at(w, s->component, slot);
        if (!sc->spawned || sc->hooks_ref == LUA_NOREF || (sc->failed & bit))
            continue;

        // the hook may destroy its own entity, so nothing of sc is touched after the call
        u32 owner = sc->owner.v;
        if (!run_hook(s, sc->hooks_ref, sc->self_ref, hook, true, dt))
            mark_failed(s, slot, owner, hook);
    }
    s->ctx.phase = bind::PHASE_NONE;
}

// the main script's top level; true with its hooks, if it returned any, held
static b32 run_main(State *s) {
    lua_State *L = s->ctx.L;

    if (!main_exists(s)) {
        LOG_INFO("script: no main script at %s", s->project->script);
    } else {
        if (!load_chunk(s, s->project->script))
            return false;
        if (lua_type(L, -1) == LUA_TTABLE)
            s->main.hooks_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        else
            lua_pop(L, 1);
    }

    lua_newtable(L);
    s->main.self_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    s->main.failed = 0;
    return true;
}

result::Error declare(State *s) {
    ASSERT(s);
    ASSERT_MSG(!s->ctx.L, "script: declare while a play state exists");

    open_state(s);
    s->ctx.declaring = true;

    b32 ok = true;
    if (main_exists(s)) {
        ok = load_chunk(s, s->project->script);
        if (ok)
            lua_pop(s->ctx.L, 1); // whatever it returned; only its declarations matter here
    } else {
        LOG_INFO("script: no main script at %s; nothing declared", s->project->script);
    }

    s->ctx.declaring = false;
    close_state(s);

    if (!ok)
        LOG_ERR("script: %s did not load at project open; play refuses until it does",
                s->project->script);
    return ok ? result::OK : result::ERR_PARSE;
}

result::Error start(State *s) {
    ASSERT(s);
    if (s->ctx.L)
        stop(s);

    s->last_error[0] = 0;
    open_state(s);
    if (!run_main(s)) {
        close_state(s);
        LOG_ERR("script: play refused; %s did not load", s->project->script);
        return result::ERR_PARSE;
    }

    run_main_hook(s, HOOK_SPAWN, false, 0.0f);
    spawn_pending(s);

    LOG_INFO("script: playing, main script %s", s->project->script);
    return result::OK;
}

void stop(State *s) {
    ASSERT(s);
    if (!s->ctx.L)
        return;

    entity::World *w = s->ctx.world;
    for (u32 slot = entity::component_first(w, s->component); slot;
         slot = entity::component_next(w, s->component, slot))
        despawn(s, (Script *)entity::component_at(w, s->component, slot));

    run_main_hook(s, HOOK_DESPAWN, false, 0.0f);

    close_state(s);
    LOG_INFO("script: stopped");
}

b32 running(const State *s) {
    ASSERT(s);
    return s->ctx.L != NULL;
}

result::Error reload(State *s) {
    ASSERT(s);
    stop(s);
    return start(s);
}

void fixed_update(State *s, f32 dt) {
    ASSERT(s);
    if (!s->ctx.L)
        return;
    spawn_pending(s);
    run_all(s, HOOK_FIXED_UPDATE, bind::PHASE_FIXED, dt);
}

void update(State *s, f32 dt) {
    ASSERT(s);
    if (!s->ctx.L)
        return;
    spawn_pending(s);
    run_all(s, HOOK_UPDATE, bind::PHASE_FRAME, dt);
}

void late_update(State *s, f32 dt) {
    ASSERT(s);
    if (!s->ctx.L)
        return;
    spawn_pending(s);
    run_all(s, HOOK_LATE_UPDATE, bind::PHASE_FRAME, dt);
}

result::Error run(State *s, const char *code, const char *name) {
    ASSERT(s && code && name);
    if (!s->ctx.L) {
        LOG_WARN("script: nothing is playing to run '%s' in", name);
        return result::ERR_INVALID;
    }

    lua_State *L = s->ctx.L;
    b32 ok = luaL_loadbuffer(L, code, strlen(code), name) == LUA_OK && call(L, 0, 0);
    if (!ok) {
        SCRIPT_ERR(s, "script: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    return ok ? result::OK : result::ERR_INVALID;
}

u64 bytes(const State *s) {
    ASSERT(s);
    if (!s->ctx.L)
        return 0;
    return (u64)lua_gc(s->ctx.L, LUA_GCCOUNT) * 1024 + (u64)lua_gc(s->ctx.L, LUA_GCCOUNTB);
}

const char *last_error(const State *s) {
    ASSERT(s);
    return s->last_error;
}

} // namespace script
