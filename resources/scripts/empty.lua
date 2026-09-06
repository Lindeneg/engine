-- An entity's script: the file returns a table of hooks, and every entity
-- whose `script` component names this file gets its own `self`, with
-- `self.entity` holding the handle. Keep state in `self`.

local M = {}

function M.on_spawn(self) end

-- frame rate, before the fixed steps: read the frame's input here
function M.on_update(self, dt) end

-- the simulation clock: move things here
function M.on_fixed_update(self, dt) end

-- frame rate, after the fixed steps: follow what moved, a camera say
function M.on_late_update(self, dt) end

function M.on_despawn(self) end

return M
