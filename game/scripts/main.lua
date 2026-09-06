
-- The project's main script. Its top level runs when the project opens and is
-- where components are declared; it runs again each time play starts, and may
-- return hooks for the level as a whole. An entity gets its own hooks from the
-- file its `script` component names.
--
-- engine.entity.register_component{
--     key = "health",
--     fields = {
--         {"hp", "f32", 100},
--         {"faction", "string", "none", cap = 64},
--     },
-- }

local M = {}

function M.on_spawn(self) end

function M.on_fixed_update(self, dt) end

function M.on_update(self, dt) 
    engine.log.info("tick")
end

function M.on_despawn(self) end

return M
