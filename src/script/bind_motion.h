#pragma once

#include "script/bind.h"

// engine.motion, over scene/motion.h: move_and_slide(position, radius, motion,
// ignore) returns the Result as a table with the header's field names.

namespace bind_motion {

void open(lua_State *L, bind::Context *c);

} // namespace bind_motion
