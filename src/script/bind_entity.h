#pragma once

#include "script/bind.h"

// engine.entity, over scene/entity.h, plus the two things a script reaches
// that C++ reaches through a struct:
//
//   engine.entity.transform(h)        a proxy with position, rotation (Euler
//                                     degrees), quaternion and scale
//   engine.entity.component(h, key)   a proxy over the component's field table,
//                                     every field by its key in the file
//
// and the declaration a script makes at project open:
//
//   engine.entity.register_component{
//       key = "health",
//       fields = {
//           {"hp", "f32", 100},
//           {"faction", "string", "none", cap = 64},
//           {"style", "enum", "line", names = {"line", "beam"}},
//           {"clips", "table", cap = 4, asset = "clip"},
//       },
//   }
//
// A field is name, type, default, then cap (bytes for a string, rows for a
// table), names (enum), asset ("model" or "clip") and choices_from by key.

namespace bind_entity {

void open(lua_State *L, bind::Context *c);

} // namespace bind_entity
