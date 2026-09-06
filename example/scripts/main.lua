-- The project's main script. Its top level runs when the project opens and is
-- where components are declared; it runs again each time play starts, and may
-- return hooks for the level as a whole. An entity gets its own hooks from the
-- file its `script` component names.

-- what scripts/player.lua reads; every entity carrying one is a player
engine.entity.register_component{
    key = "player",
    fields = {
        {"move_speed", "f32", 4},          -- metres per second on the ground
        {"look_sensitivity", "f32", 0.12}, -- degrees per pixel of mouse movement
        {"pitch_min", "f32", -89},         -- degrees; negative looks up
        {"pitch_max", "f32", 89},
        {"radius", "f32", 0.35},           -- the body is swept as a sphere
        {"gravity", "f32", 9.81},          -- metres per second squared, downward
        {"ground_height", "f32", -10},     -- a floor of last resort under any collider
        {"eye_offset", "vec3", {x = 0, y = 1.65, z = 0}}, -- the camera in the body's yawed frame
        {"range", "f32", 200},             -- how far a shot reaches, metres
        {"tracer_seconds", "f32", 1.5},    -- how long a shot stays drawn
        {"tracer_color", "vec3", {x = 1, y = 0.9, z = 0.3}},
    },
}

local M = {}

function M.on_spawn(self) end
function M.on_update(self, dt) end
function M.on_fixed_update(self, dt) end
function M.on_late_update(self, dt) end
function M.on_despawn(self) end

return M
