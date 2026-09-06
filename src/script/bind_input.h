#pragma once

#include "script/bind.h"

// engine.input, over platform/input.h. find(name) once, then down/pressed/
// released at frame rate and consume_pressed on the fixed clock; the wrong
// clock is an error, since the header says why it would be wrong.

namespace bind_input {

void open(lua_State *L, bind::Context *c);

} // namespace bind_input
