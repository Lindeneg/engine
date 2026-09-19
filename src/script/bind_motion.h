#pragma once

#include "script/bind.h"

// engine.motion, over scene/motion.h: move_and_slide(body, motion, ignore),
// where body is {position=, radius=, height=, step_height=} by the header's
// names, and ray_cast(origin, dir, max_distance, ignore); each returns its
// result as a table with the header's field names.

namespace bind_motion {

void open(lua_State *L, bind::Context *c);

} // namespace bind_motion
