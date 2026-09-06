#pragma once

#include "script/bind.h"

// engine.motion, over scene/motion.h: move_and_slide(position, radius, motion,
// ignore) and ray_cast(origin, dir, max_distance, ignore), each returning its
// result as a table with the header's field names.

namespace bind_motion {

void open(lua_State *L, bind::Context *c);

} // namespace bind_motion
