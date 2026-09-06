#pragma once

#include "asset/asset.h"
#include "core/common.h"
#include "core/memory.h"
#include "core/result.h"
#include "platform/input.h"
#include "project/project.h"
#include "scene/entity.h"

// Game code, in Lua. The engine offers services; scripts decide policy. Two
// kinds of script, both files in the project folder:
//
//   the main script (project.cfg `script`, scripts/main.lua by default): its
//   top level runs when the project opens, in a state thrown away afterwards,
//   and is where components are declared. It runs again in every play state,
//   where a declaration must be the one made at open. It may return a table of
//   hooks for the level as a whole.
//
//   an entity's script (the `script` component, a path): the file returns a
//   table of hooks, and each entity carrying it gets its own `self` with
//   `self.entity` holding the handle:
//
//       local M = {}
//       function M.on_spawn(self) self.speed = 4 end
//       function M.on_update(self, dt) ... end         -- frame rate, before the fixed steps
//       function M.on_fixed_update(self, dt) ... end   -- the simulation clock
//       function M.on_late_update(self, dt) ... end    -- frame rate, after them
//       function M.on_despawn(self) end
//       return M
//
// on_update is where a frame's input is read, a mouse delta say, so the steps
// that follow move by it; on_late_update is where a camera follows what the
// steps did.
//
// A play state lives from start to stop and everything a script made goes with
// it; edit mode has no state at all. The main script failing to load refuses
// play, since the level cannot mean what it says without it. An entity's
// script that fails to load is logged once and that entity runs without hooks.
// A hook that raises is logged once with its traceback and switched off for
// that entity until the scripts reload. Lua's memory is its own, through the
// C runtime; bytes() reports it.

// the component's key in a file, and what the inspector lists it as
#define SCRIPT_COMPONENT_KEY "script"

// what last_error holds of a message; a traceback past this is cut, the log has it whole
#define SCRIPT_ERROR_CAP 1024

namespace script {

// The engine component this module registers: a path, and the play state's
// bookkeeping, which is empty outside play.
struct Script {
    entity::Handle owner;
    char path[ENTITY_PATH_CAP]; // relative to the project folder

    b32 spawned;
    i32 hooks_ref; // the file's hooks table, in the Lua registry
    i32 self_ref;  // this entity's self
    u32 failed;    // a bit per hook that raised: reported once, then skipped
};

struct State;

RESULT_TYPE(State *, StateResult);

// Registers the script component: between entity::init and the first spawn,
// before declare. `in` may be NULL where nothing is played, as in a test.
StateResult init(memory::Arena *a, memory::Arena *scratch, entity::World *w, asset::Manager *m,
                 input::State *in, project::Project *proj);

// the registered index of the script component
u32 component(const State *s);

// The main script's top level, at project open, for its declarations. A
// project without one is fine. One that will not load is an error, logged;
// the project still opens, and play refuses until it loads.
result::Error declare(State *s);

// Play: a fresh state, the main script run, and on_spawn for every entity
// with a script component. stop runs on_despawn for each and closes the
// state. reload is stop and start, with the world left alone.
result::Error start(State *s);
void stop(State *s);
b32 running(const State *s);
result::Error reload(State *s);

// The two clocks, the frame's in two halves around the fixed steps. Entities
// that gained a script component since the last call are spawned first, so a
// script that spawns an entity sees its hooks run.
void update(State *s, f32 dt);
void fixed_update(State *s, f32 dt);
void late_update(State *s, f32 dt);

// A chunk of Lua in the play state, for a test or a console. Its error, if
// any, is logged.
result::Error run(State *s, const char *code, const char *name);

// What the play state holds, in bytes; 0 outside play.
u64 bytes(const State *s);

// The most recent failure, as the log had it: a file that would not load, a
// hook that raised, a chunk run by hand. "" when the last start or reload has
// seen none; it stays through stop, so an editor can show what ended play.
const char *last_error(const State *s);

} // namespace script
