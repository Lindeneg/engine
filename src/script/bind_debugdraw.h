#pragma once

#include "script/bind.h"

// engine.debugdraw, over gfx/debugdraw.h: line, box (axis-aligned), sphere and
// cross, each with an optional last argument {lifetime=, x_ray=} for the Style.

namespace bind_debugdraw {

void open(lua_State *L, bind::Context *c);

} // namespace bind_debugdraw
