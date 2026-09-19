-- The player: look, walk, fall, fire, and a camera that follows. Every number
-- lives in the `player` component beside this script, declared in main.lua
-- and edited in the inspector. The body must be a root entity: movement is
-- resolved in world space.

local M = {}

-- a vector in the body's yawed frame, as a world direction
local function yawed(self, v)
    local r = math.rad(self.yaw)
    local s, c = math.sin(r), math.cos(r)
    return {x = v.x * c + v.z * s, y = v.y, z = -v.x * s + v.z * c}
end

-- where the eyes are and which way they look: pitch about X, then yaw about Y, on -Z
local function eye(self, p)
    local pos = engine.entity.transform(self.entity).position
    local off = yawed(self, p.eye_offset)
    local origin = {x = pos.x + off.x, y = pos.y + off.y, z = pos.z + off.z}

    local pr, yr = math.rad(self.pitch), math.rad(self.yaw)
    local cp = math.cos(pr)
    local forward = {x = -cp * math.sin(yr), y = math.sin(pr), z = -cp * math.cos(yr)}
    return origin, forward
end

function M.on_spawn(self)
    self.yaw = 0
    self.pitch = 0
    self.velocity = {x = 0, y = 0, z = 0}

    self.move_forward = engine.input.find("move_forward")
    self.move_back = engine.input.find("move_back")
    self.move_left = engine.input.find("move_left")
    self.move_right = engine.input.find("move_right")
    self.fire = engine.input.find("fire")
end

-- aim at frame rate: a mouse delta is per frame by nature
function M.on_update(self, dt)
    local p = engine.entity.component(self.entity, "player")
    if not p or not engine.input.mouse_look() then
        return
    end

    local d = engine.input.cursor_delta()
    self.yaw = (self.yaw - d.x * p.look_sensitivity) % 360
    self.pitch = math.max(p.pitch_min, math.min(p.pitch_max, self.pitch + d.y * p.look_sensitivity))
end

local function fire(self, p)
    local origin, forward = eye(self, p)
    local r = engine.motion.ray_cast(origin, forward, p.range, self.entity)
    local stop = r.hit and r.point or {
        x = origin.x + forward.x * p.range,
        y = origin.y + forward.y * p.range,
        z = origin.z + forward.z * p.range,
    }
    local color = {x = p.tracer_color.x, y = p.tracer_color.y, z = p.tracer_color.z, w = 1}
    engine.debugdraw.line(origin, stop, color, {lifetime = p.tracer_seconds})
end

function M.on_fixed_update(self, dt)
    local p = engine.entity.component(self.entity, "player")
    if not p then
        return
    end

    local t = engine.entity.transform(self.entity)
    local pos = t.position

    -- -Z is forward in the body's own frame; a diagonal is no faster than straight
    local wish = {x = 0, y = 0, z = 0}
    if engine.input.down(self.move_forward) then wish.z = wish.z - 1 end
    if engine.input.down(self.move_back) then wish.z = wish.z + 1 end
    if engine.input.down(self.move_left) then wish.x = wish.x - 1 end
    if engine.input.down(self.move_right) then wish.x = wish.x + 1 end
    local len = math.sqrt(wish.x * wish.x + wish.z * wish.z)
    if len > 0 then
        wish.x, wish.z = wish.x / len, wish.z / len
    end
    local dir = yawed(self, wish)

    local v = self.velocity
    v.x, v.z = dir.x * p.move_speed, dir.z * p.move_speed
    v.y = v.y - p.gravity * dt

    -- the stand-in floor limits the move, not the result
    local motion = {x = v.x * dt, y = v.y * dt, z = v.z * dt}
    local floor = p.ground_height + p.radius
    if pos.y + motion.y < floor then
        motion.y = floor - pos.y
        v.y = 0
    end

    local body = {position = pos, radius = p.radius, height = p.height, step_height = p.step_height}
    local r = engine.motion.move_and_slide(body, motion, self.entity)

    -- cancel the velocity into what stopped it, or gravity builds up against a floor
    if r.hit then
        local n = r.normal
        local along = v.x * n.x + v.y * n.y + v.z * n.z
        v.x, v.y, v.z = v.x - n.x * along, v.y - n.y * along, v.z - n.z * along
    end
    if r.stuck then
        v.x, v.y, v.z = 0, 0, 0
    end

    -- the body carries the yaw; pitch stays on the camera
    t.position = r.position
    t.rotation = {x = 0, y = self.yaw, z = 0}

    if engine.input.consume_pressed(self.fire) then
        fire(self, p)
    end
end

-- the camera follows after the steps, so it sees where the body ended up
function M.on_late_update(self, dt)
    local p = engine.entity.component(self.entity, "player")
    local cam = engine.entity.active_camera()
    if not p or not cam then
        return
    end

    local origin = eye(self, p)
    local ct = engine.entity.transform(cam)
    ct.position = origin
    ct.rotation = {x = self.pitch, y = self.yaw, z = 0}
end

return M
