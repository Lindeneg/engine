-- Every hook leaves a mark in the entity's health, so a test can count them.

local M = {}

function M.on_spawn(self)
    local h = engine.entity.component(self.entity, "health")
    h.hp = 1
    h.faction = "red"
    h.style = "line"
    h.clips = {{"idle", "animations/pistol_idle.glb"}, {"walk"}}
    engine.entity.transform(self.entity).position = {x = 1, y = 2, z = 3}
    self.steps = 0
end

function M.on_fixed_update(self, dt)
    local h = engine.entity.component(self.entity, "health")
    h.hp = h.hp + 1
    self.steps = self.steps + 1
end

function M.on_update(self, dt)
    local h = engine.entity.component(self.entity, "health")
    h.hp = h.hp + 100
    h.tint = {x = self.steps, y = dt, z = 0}
end

function M.on_despawn(self)
    engine.entity.component(self.entity, "health").hp = -1
end

return M
