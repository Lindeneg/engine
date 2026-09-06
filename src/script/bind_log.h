#pragma once

#include "script/bind.h"

// engine.log: trace, debug, info, warn, error, each one string, reported from
// the script's own file and line. `print` goes to info the same way.

namespace bind_log {

void open(lua_State *L, bind::Context *c);

} // namespace bind_log
