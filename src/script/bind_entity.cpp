#include "script/bind_entity.h"

#include <string.h>

#include "core/str.h"

#define COMPONENT_META "engine.component"
#define TRANSFORM_META "engine.transform"

// what one declaration may hold; a script wanting more is declaring the wrong thing
#define MAX_FIELDS 64
#define MAX_ENUM_NAMES 256 // across every enum field of one declaration

namespace bind_entity {

// the FieldType and AssetKind enums by the words a declaration uses, in enum order
static const char *FIELD_TYPE_NAMES[] = {"f32", "i32", "b32", "vec3", "string", "enum", "table"};
static const char *ASSET_NAMES[] = {"none", "model", "clip"};
static_assert(ARRAY_COUNT(FIELD_TYPE_NAMES) == entity::FIELD_TABLE + 1,
              "every FieldType needs a name");
static_assert(ARRAY_COUNT(ASSET_NAMES) == entity::ASSET_CLIP + 1, "every AssetKind needs a name");

// what a proxy userdata holds: the entity and which of its components
struct ComponentRef {
    u32 handle;
    u32 component;
};

struct TransformRef {
    u32 handle;
};

static u32 check_component_index(bind::Context *c, lua_State *L, i32 index) {
    const char *key = luaL_checkstring(L, index);
    i32 found = entity::find_component(c->world, key);
    if (found < 0)
        luaL_argerror(L, index, lua_pushfstring(L, "no component is registered as '%s'", key));
    return (u32)found;
}

static void push_component_ref(lua_State *L, entity::Handle h, u32 component) {
    ComponentRef *ref = (ComponentRef *)lua_newuserdatauv(L, sizeof(ComponentRef), 0);
    ref->handle = h.v;
    ref->component = component;
    luaL_setmetatable(L, COMPONENT_META);
}

// the bytes behind a proxy, or an error: the entity or the component may be gone
static void *resolve(bind::Context *c, lua_State *L, const ComponentRef *ref) {
    entity::Handle h{ref->handle};
    void *comp = entity::component(c->world, h, ref->component);
    if (!comp) {
        const char *key = entity::component_desc(c->world, ref->component)->key;
        if (!entity::alive(c->world, h))
            luaL_error(L, "the entity behind this %s is gone", key);
        luaL_error(L, "the entity no longer has its %s", key);
    }
    return comp;
}

static const entity::FieldDesc *find_field(const entity::ComponentDesc *d, const char *key) {
    for (u32 i = 0; i < d->field_count; i++)
        if (strcmp(d->fields[i].key, key) == 0)
            return &d->fields[i];
    return NULL;
}

static void push_field(lua_State *L, const entity::FieldDesc *f, const void *comp) {
    const u8 *at = (const u8 *)comp + f->offset;
    switch (f->type) {
    case entity::FIELD_F32:
        lua_pushnumber(L, (lua_Number) * (const f32 *)at);
        break;
    case entity::FIELD_I32:
        lua_pushinteger(L, (lua_Integer) * (const i32 *)at);
        break;
    case entity::FIELD_B32:
        lua_pushboolean(L, *(const b32 *)at);
        break;
    case entity::FIELD_VEC3:
        bind::push_vec3(L, *(const linalg::Vec3 *)at);
        break;
    case entity::FIELD_STRING:
        lua_pushstring(L, (const char *)at);
        break;
    case entity::FIELD_ENUM: {
        u32 idx = (u32) * (const i32 *)at;
        lua_pushstring(L, idx < f->name_count ? f->names[idx] : f->names[0]);
        break;
    }
    case entity::FIELD_TABLE: {
        // rows as {name, value} pairs, in order
        const entity::Named *rows = entity::table_rows((void *)comp, f);
        u32 count = *entity::table_count((void *)comp, f);
        lua_createtable(L, (i32)count, 0);
        for (u32 r = 0; r < count; r++) {
            lua_createtable(L, 2, 0);
            lua_pushstring(L, rows[r].name);
            lua_rawseti(L, -2, 1);
            lua_pushstring(L, rows[r].value);
            lua_rawseti(L, -2, 2);
            lua_rawseti(L, -2, (lua_Integer)r + 1);
        }
        break;
    }
    }
}

// a string into a fixed field; longer than the cap is the script's bug
static void set_string(lua_State *L, i32 index, char *dst, u32 cap, const char *what) {
    size_t len = 0;
    const char *s = luaL_checklstring(L, index, &len);
    if (len >= cap)
        luaL_error(L, "%s holds at most %d bytes; got %d", what, (int)cap - 1, (int)len);
    memcpy(dst, s, len + 1);
}

static void set_field(lua_State *L, const entity::FieldDesc *f, void *comp, i32 index) {
    u8 *at = (u8 *)comp + f->offset;
    switch (f->type) {
    case entity::FIELD_F32:
        *(f32 *)at = (f32)luaL_checknumber(L, index);
        break;
    case entity::FIELD_I32:
        *(i32 *)at = (i32)luaL_checkinteger(L, index);
        break;
    case entity::FIELD_B32:
        luaL_checktype(L, index, LUA_TBOOLEAN);
        *(b32 *)at = lua_toboolean(L, index);
        break;
    case entity::FIELD_VEC3:
        *(linalg::Vec3 *)at = bind::check_vec3(L, index);
        break;
    case entity::FIELD_STRING:
        set_string(L, index, (char *)at, f->cap, f->key);
        break;
    case entity::FIELD_ENUM: {
        const char *name = luaL_checkstring(L, index);
        i32 idx = str::name_index(f->names, f->name_count, name);
        if (idx < 0)
            luaL_error(L, "'%s' is not a %s", name, f->key);
        *(i32 *)at = idx;
        break;
    }
    case entity::FIELD_TABLE: {
        index = lua_absindex(L, index);
        luaL_checktype(L, index, LUA_TTABLE);
        u32 n = (u32)lua_rawlen(L, index);
        if (n > f->cap)
            luaL_error(L, "%s holds at most %d rows; got %d", f->key, (int)f->cap, (int)n);

        entity::Named *rows = entity::table_rows(comp, f);
        for (u32 r = 0; r < n; r++) {
            lua_rawgeti(L, index, (lua_Integer)r + 1);
            if (lua_type(L, -1) != LUA_TTABLE)
                luaL_error(L, "%s row %d is not a {name, value} pair", f->key, (int)r + 1);
            lua_rawgeti(L, -1, 1);
            set_string(L, -1, rows[r].name, sizeof(rows[r].name), f->key);
            lua_rawgeti(L, -2, 2);
            if (lua_isnoneornil(L, -1))
                rows[r].value[0] = 0;
            else
                set_string(L, -1, rows[r].value, sizeof(rows[r].value), f->key);
            lua_pop(L, 3);
        }
        *entity::table_count(comp, f) = n;
        break;
    }
    }
}

static i32 component_index(lua_State *L) {
    bind::Context *c = bind::context(L);
    ComponentRef *ref = (ComponentRef *)luaL_checkudata(L, 1, COMPONENT_META);
    const char *key = luaL_checkstring(L, 2);

    void *comp = resolve(c, L, ref);
    const entity::ComponentDesc *d = entity::component_desc(c->world, ref->component);
    const entity::FieldDesc *f = find_field(d, key);
    if (!f)
        luaL_error(L, "%s has no field '%s'", d->key, key);

    push_field(L, f, comp);
    return 1;
}

static i32 component_newindex(lua_State *L) {
    bind::Context *c = bind::context(L);
    ComponentRef *ref = (ComponentRef *)luaL_checkudata(L, 1, COMPONENT_META);
    const char *key = luaL_checkstring(L, 2);

    void *comp = resolve(c, L, ref);
    const entity::ComponentDesc *d = entity::component_desc(c->world, ref->component);
    const entity::FieldDesc *f = find_field(d, key);
    if (!f)
        luaL_error(L, "%s has no field '%s'", d->key, key);

    set_field(L, f, comp, 3);

    // as the inspector does after an edit: a path resolves, a range is checked
    if (d->after_read)
        d->after_read(c->world, c->assets, entity::Handle{ref->handle}, comp, d->user);
    return 0;
}

static i32 component_tostring(lua_State *L) {
    bind::Context *c = bind::context(L);
    ComponentRef *ref = (ComponentRef *)luaL_checkudata(L, 1, COMPONENT_META);
    lua_pushfstring(L, "%s of entity %d", entity::component_desc(c->world, ref->component)->key,
                    (int)ref->handle);
    return 1;
}

static i32 transform_index(lua_State *L) {
    bind::Context *c = bind::context(L);
    TransformRef *ref = (TransformRef *)luaL_checkudata(L, 1, TRANSFORM_META);
    const char *key = luaL_checkstring(L, 2);

    entity::Entity *e = entity::get(c->world, entity::Handle{ref->handle});
    if (!e)
        luaL_error(L, "the entity behind this transform is gone");

    if (strcmp(key, "position") == 0) {
        bind::push_vec3(L, e->local.position);
    } else if (strcmp(key, "rotation") == 0) {
        if (!e->local.use_euler)
            luaL_error(L, "rotation was set as a quaternion; read .quaternion instead");
        bind::push_vec3(L, e->local.euler);
    } else if (strcmp(key, "quaternion") == 0) {
        linalg::Quat q = e->local.rotation;
        bind::push_vec4(L, linalg::Vec4{q.x, q.y, q.z, q.w});
    } else if (strcmp(key, "scale") == 0) {
        bind::push_vec3(L, e->local.scale);
    } else {
        luaL_error(L, "a transform has no field '%s'", key);
    }
    return 1;
}

static i32 transform_newindex(lua_State *L) {
    bind::Context *c = bind::context(L);
    TransformRef *ref = (TransformRef *)luaL_checkudata(L, 1, TRANSFORM_META);
    const char *key = luaL_checkstring(L, 2);

    entity::Entity *e = entity::get(c->world, entity::Handle{ref->handle});
    if (!e)
        luaL_error(L, "the entity behind this transform is gone");

    if (strcmp(key, "position") == 0) {
        e->local.position = bind::check_vec3(L, 3);
    } else if (strcmp(key, "rotation") == 0) {
        entity::set_euler(&e->local, bind::check_vec3(L, 3));
    } else if (strcmp(key, "quaternion") == 0) {
        linalg::Vec4 v = bind::check_vec4(L, 3);
        linalg::Quat q{v.x, v.y, v.z, v.w};
        if (linalg::quat_dot(q, q) < LINALG_ZERO_EPSILON)
            luaL_error(L, "a zero quaternion is not a rotation");
        entity::set_rotation(&e->local, linalg::quat_normalize(q));
    } else if (strcmp(key, "scale") == 0) {
        e->local.scale = bind::check_vec3(L, 3);
    } else {
        luaL_error(L, "a transform has no field '%s'", key);
    }
    return 0;
}

static i32 spawn(lua_State *L) {
    bind::Context *c = bind::context(L);
    bind::push_handle(L, entity::spawn(c->world, luaL_checkstring(L, 1)));
    return 1;
}

// nil when the file could not be read or spawned; the log says why
static i32 spawn_definition(lua_State *L) {
    bind::Context *c = bind::context(L);
    entity::HandleResult r = entity::spawn_definition(c->world, c->assets, luaL_checkstring(L, 1));
    bind::push_handle(L, r.err == result::OK ? r.val : entity::NONE);
    return 1;
}

static i32 destroy(lua_State *L) {
    bind::Context *c = bind::context(L);
    entity::destroy(c->world, bind::check_handle(L, 1));
    return 0;
}

static i32 alive(lua_State *L) {
    bind::Context *c = bind::context(L);
    lua_pushboolean(L, entity::alive(c->world, bind::check_handle(L, 1)));
    return 1;
}

static i32 name(lua_State *L) {
    lua_pushstring(L, bind::check_entity(bind::context(L), L, 1)->name);
    return 1;
}

static i32 set_name(lua_State *L) {
    entity::Entity *e = bind::check_entity(bind::context(L), L, 1);
    set_string(L, 2, e->name, sizeof(e->name), "a name");
    return 0;
}

static i32 active(lua_State *L) {
    lua_pushboolean(L, bind::check_entity(bind::context(L), L, 1)->active);
    return 1;
}

static i32 set_active(lua_State *L) {
    entity::Entity *e = bind::check_entity(bind::context(L), L, 1);
    luaL_checktype(L, 2, LUA_TBOOLEAN);
    e->active = lua_toboolean(L, 2);
    return 0;
}

static i32 id_of(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)bind::check_entity(bind::context(L), L, 1)->id);
    return 1;
}

static i32 find_by_id(lua_State *L) {
    bind::Context *c = bind::context(L);
    bind::push_handle(L, entity::find_by_id(c->world, (u32)luaL_checkinteger(L, 1)));
    return 1;
}

static i32 find_by_name(lua_State *L) {
    bind::Context *c = bind::context(L);
    bind::push_handle(L, entity::find_by_name(c->world, luaL_checkstring(L, 1)));
    return 1;
}

static i32 parent(lua_State *L) {
    bind::push_handle(L, bind::check_entity(bind::context(L), L, 1)->parent);
    return 1;
}

static i32 first_child(lua_State *L) {
    bind::push_handle(L, bind::check_entity(bind::context(L), L, 1)->first_child);
    return 1;
}

static i32 next_sibling(lua_State *L) {
    bind::push_handle(L, bind::check_entity(bind::context(L), L, 1)->next_sibling);
    return 1;
}

static i32 set_parent(lua_State *L) {
    bind::Context *c = bind::context(L);
    entity::Handle child = bind::check_handle(L, 1);
    entity::Handle to = bind::check_handle(L, 2);
    const char *joint = luaL_optstring(L, 3, NULL);
    lua_pushboolean(L, entity::set_parent(c->world, c->assets, child, to, joint));
    return 1;
}

static i32 first(lua_State *L) {
    bind::push_handle(L, entity::first(bind::context(L)->world));
    return 1;
}

static i32 next(lua_State *L) {
    bind::Context *c = bind::context(L);
    entity::Handle h = bind::check_handle(L, 1);
    bind::push_handle(L, h.v ? entity::next(c->world, h) : entity::NONE);
    return 1;
}

static i32 transform(lua_State *L) {
    entity::Entity *e = bind::check_entity(bind::context(L), L, 1);
    (void)e;
    TransformRef *ref = (TransformRef *)lua_newuserdatauv(L, sizeof(TransformRef), 0);
    ref->handle = (u32)lua_tointeger(L, 1);
    luaL_setmetatable(L, TRANSFORM_META);
    return 1;
}

static i32 world_position(lua_State *L) {
    entity::Entity *e = bind::check_entity(bind::context(L), L, 1);
    bind::push_vec3(L, linalg::mat4_translation(e->world));
    return 1;
}

// the entity's basis in the world, unit length; a flattened axis reads as zero
static i32 world_axis(lua_State *L) {
    entity::Entity *e = bind::check_entity(bind::context(L), L, 1);
    lua_Integer axis = luaL_checkinteger(L, 2);
    if (axis < 0 || axis > 2)
        luaL_argerror(L, 2, "an axis is 0, 1 or 2");
    bind::push_vec3(L, linalg::vec3_normalize_or(linalg::mat4_axis(e->world, (u32)axis),
                                                 linalg::Vec3{0.0f, 0.0f, 0.0f}));
    return 1;
}

static i32 set_world_position(lua_State *L) {
    bind::Context *c = bind::context(L);
    entity::set_world_position(c->world, bind::check_handle(L, 1), bind::check_vec3(L, 2));
    return 0;
}

static i32 component(lua_State *L) {
    bind::Context *c = bind::context(L);
    entity::Handle h = bind::check_handle(L, 1);
    u32 index = check_component_index(c, L, 2);
    if (entity::component(c->world, h, index))
        push_component_ref(L, h, index);
    else
        lua_pushnil(L);
    return 1;
}

static i32 has_component(lua_State *L) {
    bind::Context *c = bind::context(L);
    entity::Handle h = bind::check_handle(L, 1);
    lua_pushboolean(L, entity::component(c->world, h, check_component_index(c, L, 2)) != NULL);
    return 1;
}

// nil when the entity is dead or the pool is full, which the log reports
static i32 add_component(lua_State *L) {
    bind::Context *c = bind::context(L);
    entity::Handle h = bind::check_handle(L, 1);
    u32 index = check_component_index(c, L, 2);

    void *comp = entity::add_component(c->world, h, index);
    if (!comp) {
        lua_pushnil(L);
        return 1;
    }

    const entity::ComponentDesc *d = entity::component_desc(c->world, index);
    if (d->after_read)
        d->after_read(c->world, c->assets, h, comp, d->user);
    push_component_ref(L, h, index);
    return 1;
}

static i32 remove_component(lua_State *L) {
    bind::Context *c = bind::context(L);
    entity::Handle h = bind::check_handle(L, 1);
    entity::remove_component(c->world, h, check_component_index(c, L, 2));
    return 0;
}

static i32 play(lua_State *L) {
    bind::Context *c = bind::context(L);
    lua_pushboolean(L, entity::play(c->world, bind::check_handle(L, 1), luaL_checkstring(L, 2)));
    return 1;
}

static i32 current_clip(lua_State *L) {
    bind::Context *c = bind::context(L);
    lua_pushstring(L, entity::current_clip(c->world, bind::check_handle(L, 1)));
    return 1;
}

static i32 clip_finished(lua_State *L) {
    bind::Context *c = bind::context(L);
    lua_pushboolean(L, entity::clip_finished(c->world, bind::check_handle(L, 1)));
    return 1;
}

static i32 fixed_dt(lua_State *L) {
    lua_pushnumber(L, (lua_Number)entity::fixed_dt(bind::context(L)->world));
    return 1;
}

static i32 fixed_alpha(lua_State *L) {
    lua_pushnumber(L, (lua_Number)entity::fixed_alpha(bind::context(L)->world));
    return 1;
}

// an optional string field of the table at index; NULL when absent
static const char *opt_string_field(lua_State *L, i32 index, const char *key) {
    lua_getfield(L, index, key);
    const char *s = NULL;
    if (!lua_isnil(L, -1)) {
        if (lua_type(L, -1) != LUA_TSTRING)
            luaL_error(L, "'%s' must be a string", key);
        s = lua_tostring(L, -1);
    }
    lua_pop(L, 1);
    return s;
}

// a word from a fixed vocabulary, by the field or by position
static i32 check_word(lua_State *L, i32 index, const char *const *names, u32 count,
                      const char *what) {
    const char *s = luaL_checkstring(L, index);
    i32 idx = str::name_index(names, count, s);
    if (idx < 0)
        luaL_error(L, "'%s' is not a %s", s, what);
    return idx;
}

// One {name, type, default, ...} row of a declaration, at the top of the
// stack, into a FieldDesc. Strings point into the spec table, which the
// registry copies from before the call returns. Enum names go into the pool.
static void read_field_spec(lua_State *L, entity::FieldDesc *f, const char **name_pool,
                            u32 *name_used) {
    i32 spec = lua_gettop(L);
    luaL_checktype(L, spec, LUA_TTABLE);

    lua_rawgeti(L, spec, 1);
    if (lua_type(L, -1) != LUA_TSTRING)
        luaL_error(L, "a field's first entry is its name");
    f->key = lua_tostring(L, -1);
    lua_pop(L, 1);

    lua_rawgeti(L, spec, 2);
    f->type = (entity::FieldType)check_word(L, -1, FIELD_TYPE_NAMES,
                                            ARRAY_COUNT(FIELD_TYPE_NAMES), "field type");
    lua_pop(L, 1);

    lua_getfield(L, spec, "cap");
    f->cap = (u32)luaL_optinteger(L, -1, 0);
    lua_pop(L, 1);

    lua_getfield(L, spec, "asset");
    f->asset = lua_isnil(L, -1) ? entity::ASSET_NONE
                                : (entity::AssetKind)check_word(L, -1, ASSET_NAMES,
                                                                ARRAY_COUNT(ASSET_NAMES),
                                                                "asset kind");
    lua_pop(L, 1);

    f->choices_from = opt_string_field(L, spec, "choices_from");

    if (f->type == entity::FIELD_ENUM) {
        lua_getfield(L, spec, "names");
        if (lua_type(L, -1) != LUA_TTABLE)
            luaL_error(L, "enum '%s' needs a 'names' table", f->key);
        u32 n = (u32)lua_rawlen(L, -1);
        if (*name_used + n > MAX_ENUM_NAMES)
            luaL_error(L, "a declaration holds at most %d enum names in all", MAX_ENUM_NAMES);
        f->names = name_pool + *name_used;
        f->name_count = n;
        for (u32 i = 0; i < n; i++) {
            lua_rawgeti(L, -1, (lua_Integer)i + 1);
            if (lua_type(L, -1) != LUA_TSTRING)
                luaL_error(L, "enum '%s' name %d is not a string", f->key, (int)i + 1);
            name_pool[(*name_used)++] = lua_tostring(L, -1);
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }

    // the default, by type; absent is zero, empty, or the first name
    lua_rawgeti(L, spec, 3);
    if (!lua_isnil(L, -1)) {
        switch (f->type) {
        case entity::FIELD_F32:
            f->def.as_f32 = (f32)luaL_checknumber(L, -1);
            break;
        case entity::FIELD_I32:
            f->def.as_i32 = (i32)luaL_checkinteger(L, -1);
            break;
        case entity::FIELD_B32:
            luaL_checktype(L, -1, LUA_TBOOLEAN);
            f->def.as_i32 = lua_toboolean(L, -1);
            break;
        case entity::FIELD_VEC3:
            f->def.as_vec3 = bind::check_vec3(L, -1);
            break;
        case entity::FIELD_STRING:
            if (lua_type(L, -1) != LUA_TSTRING)
                luaL_error(L, "the default of string '%s' must be a string", f->key);
            f->def.as_str = lua_tostring(L, -1);
            break;
        case entity::FIELD_ENUM:
            f->def.as_i32 = check_word(L, -1, f->names, f->name_count, "name of the enum");
            break;
        case entity::FIELD_TABLE:
            luaL_error(L, "table '%s' starts empty and takes no default", f->key);
            break;
        }
    }
    lua_pop(L, 1);
}

// At project open this registers. In a play state the world is populated and
// a layout cannot change, so the declaration must be the one already made.
static i32 register_component(lua_State *L) {
    bind::Context *c = bind::context(L);
    luaL_checktype(L, 1, LUA_TTABLE);

    const char *key = opt_string_field(L, 1, "key");
    if (!key || !key[0])
        luaL_error(L, "register_component needs a 'key'");

    lua_getfield(L, 1, "fields");
    if (lua_type(L, -1) != LUA_TTABLE)
        luaL_error(L, "component '%s' needs a 'fields' list", key);
    u32 count = (u32)lua_rawlen(L, -1);
    if (count > MAX_FIELDS)
        luaL_error(L, "component '%s' declares %d fields; at most %d", key, (int)count, MAX_FIELDS);

    entity::FieldDesc fields[MAX_FIELDS] = {};
    const char *names[MAX_ENUM_NAMES];
    u32 names_used = 0;
    for (u32 i = 0; i < count; i++) {
        lua_rawgeti(L, -1, (lua_Integer)i + 1);
        read_field_spec(L, &fields[i], names, &names_used);
        lua_pop(L, 1);
    }

    entity::ComponentDesc desc = {.key = key, .fields = fields, .field_count = count};

    if (c->declaring) {
        if (entity::register_component(c->world, &desc) < 0)
            luaL_error(L, "component '%s' was refused; the log says why", key);
        return 0;
    }

    if (!entity::declaration_matches(c->world, &desc))
        luaL_error(L,
                   "component '%s' is not declared as it was when the project opened; a "
                   "declaration belongs in the main script, and a changed one needs the project "
                   "reopened",
                   key);
    return 0;
}

static const bind::Function FUNCTIONS[] = {
    {"spawn", spawn},
    {"spawn_definition", spawn_definition},
    {"destroy", destroy},
    {"alive", alive},
    {"name", name},
    {"set_name", set_name},
    {"active", active},
    {"set_active", set_active},
    {"id_of", id_of},
    {"find_by_id", find_by_id},
    {"find_by_name", find_by_name},
    {"parent", parent},
    {"first_child", first_child},
    {"next_sibling", next_sibling},
    {"set_parent", set_parent},
    {"first", first},
    {"next", next},
    {"transform", transform},
    {"world_position", world_position},
    {"world_axis", world_axis},
    {"set_world_position", set_world_position},
    {"component", component},
    {"has_component", has_component},
    {"add_component", add_component},
    {"remove_component", remove_component},
    {"play", play},
    {"current_clip", current_clip},
    {"clip_finished", clip_finished},
    {"fixed_dt", fixed_dt},
    {"fixed_alpha", fixed_alpha},
    {"register_component", register_component},
};

static const bind::Function COMPONENT_META_FUNCTIONS[] = {
    {"__index", component_index},
    {"__newindex", component_newindex},
    {"__tostring", component_tostring},
};

static const bind::Function TRANSFORM_META_FUNCTIONS[] = {
    {"__index", transform_index},
    {"__newindex", transform_newindex},
};

void open(lua_State *L, bind::Context *c) {
    luaL_newmetatable(L, COMPONENT_META);
    bind::set_functions(L, c, COMPONENT_META_FUNCTIONS, ARRAY_COUNT(COMPONENT_META_FUNCTIONS));
    lua_pop(L, 1);

    luaL_newmetatable(L, TRANSFORM_META);
    bind::set_functions(L, c, TRANSFORM_META_FUNCTIONS, ARRAY_COUNT(TRANSFORM_META_FUNCTIONS));
    lua_pop(L, 1);

    lua_createtable(L, 0, ARRAY_COUNT(FUNCTIONS));
    bind::set_functions(L, c, FUNCTIONS, ARRAY_COUNT(FUNCTIONS));
    lua_setfield(L, -2, "entity");
}

} // namespace bind_entity
