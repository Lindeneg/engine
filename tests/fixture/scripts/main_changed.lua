-- main.lua with one default changed: a declaration play must refuse

engine.entity.register_component{
    key = "health",
    fields = {
        {"hp", "f32", 50},
        {"faction", "string", "none", cap = 64},
        {"style", "enum", "beam", names = {"line", "beam"}},
        {"tint", "vec3", {x = 1, y = 0.5, z = 0.25}},
        {"clips", "table", cap = 4, asset = "clip"},
    },
}
