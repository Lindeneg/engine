-- The fixture's main script: declares what the script tests read, and gives
-- the entity named "probe" ten hp per fixed step.

engine.entity.register_component{
    key = "health",
    fields = {
        {"hp", "f32", 100},
        {"faction", "string", "none", cap = 64},
        {"style", "enum", "beam", names = {"line", "beam"}},
        {"tint", "vec3", {x = 1, y = 0.5, z = 0.25}},
        {"clips", "table", cap = 4, asset = "clip"},
    },
}

local M = {}

function M.on_fixed_update(self, dt)
    local probe = engine.entity.find_by_name("probe")
    if probe then
        local h = engine.entity.component(probe, "health")
        h.hp = h.hp + 10
    end
end

return M
