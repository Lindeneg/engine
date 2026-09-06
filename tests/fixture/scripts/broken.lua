-- a hook that raises, to see it reported once and switched off

local M = {}

function M.on_update(self, dt)
    error("boom")
end

return M
