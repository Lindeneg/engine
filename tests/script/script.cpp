#include "script/script.h"

#include <string.h>

#include "suites.h"

// one manager for the suite: scope arenas never return to the reserve
static asset::Manager *shared_assets(memory::Arena *scratch) {
    static asset::Manager *m = NULL;
    if (m)
        return m;

    static memory::Arena arena;
    arena = memory::create(MiB(4));
    m = MUST(asset::init(&arena, scratch,
                         asset::Config{
                             .root = CL_TEST_FIXTURE,
                             .default_shader = shader::Handle{0},
                             .max_models = 16,
                             .max_meshes = 128,
                             .max_materials = 128,
                             .max_skeletons = 8,
                             .max_clips = 16,
                             .scope_bytes = {MiB(32), MiB(4)},
                         }));
    return m;
}

// a world with the fixture's declarations made, the way main opens a project
struct Fixture {
    asset::Manager *m;
    entity::World *w;
    project::Project *proj; // in the arena: the script state keeps the pointer
    script::State *s;
};

static Fixture open_fixture(memory::Arena *a) {
    Fixture f;
    f.m = shared_assets(a);
    f.w = MUST(entity::init(a, entity::Config{.max_entities = 64, .pool_default = 16}));
    f.proj = push_struct(a, project::Project);
    MUST(project::open(a, CL_TEST_FIXTURE, f.proj));
    f.s = MUST(script::init(a, a, f.w, f.m, NULL, f.proj));
    MUST(script::declare(f.s));
    return f;
}

static const entity::FieldDesc *field_of(const entity::ComponentDesc *d, const char *key) {
    for (u32 i = 0; i < d->field_count; i++)
        if (strcmp(d->fields[i].key, key) == 0)
            return &d->fields[i];
    CHECK(false);
    return &d->fields[0];
}

static void *health_field(Fixture *f, entity::Handle h, const char *key) {
    u32 c = (u32)entity::find_component(f->w, "health");
    const entity::ComponentDesc *d = entity::component_desc(f->w, c);
    void *comp = entity::component(f->w, h, c);
    CHECK(comp != NULL);
    return (u8 *)comp + field_of(d, key)->offset;
}

static f32 hp(Fixture *f, entity::Handle h) { return *(f32 *)health_field(f, h, "hp"); }

// an entity with a health and a script, from text, as a level would spawn it
static entity::Handle spawn_probe(memory::Arena *a, Fixture *f, const char *name,
                                  const char *path) {
    char *text = memory::push_fmt(a,
                                  "entity \"%s\" {\n"
                                  "    health {\n"
                                  "        hp 5\n"
                                  "    }\n"
                                  "    script {\n"
                                  "        path %s\n"
                                  "    }\n"
                                  "}\n",
                                  name, path);
    config::Doc *doc = MUST(config::parse(a, text, "test"));
    entity::Handle h = entity::spawn_from_config(f->w, f->m, doc->first, entity::NONE);
    CHECK(h.v != 0);
    return h;
}

static void the_main_script_declares_components_at_open(memory::Arena *a) {
    Fixture f = open_fixture(a);
    CHECK(!script::running(f.s));

    i32 health = entity::find_component(f.w, "health");
    CHECK(health >= 0);
    CHECK(entity::find_component(f.w, SCRIPT_COMPONENT_KEY) == (i32)script::component(f.s));

    // the defaults the declaration gave, by every type
    entity::Handle h = entity::spawn(f.w, "x");
    CHECK(entity::add_component(f.w, h, (u32)health) != NULL);
    CHECK(hp(&f, h) == 100.0f);
    CHECK(strcmp((const char *)health_field(&f, h, "faction"), "none") == 0);
    CHECK(*(i32 *)health_field(&f, h, "style") == 1);
    CHECK(((linalg::Vec3 *)health_field(&f, h, "tint"))->y == 0.5f);

    const entity::ComponentDesc *d = entity::component_desc(f.w, (u32)health);
    const entity::FieldDesc *clips = field_of(d, "clips");
    CHECK(clips->type == entity::FIELD_TABLE && clips->cap == 4 &&
          clips->asset == entity::ASSET_CLIP);
}

static void hooks_run_on_both_clocks_and_despawn_at_stop(memory::Arena *a) {
    Fixture f = open_fixture(a);
    entity::Handle h = spawn_probe(a, &f, "probe", "scripts/probe.lua");
    CHECK(hp(&f, h) == 5.0f);

    CHECK(script::start(f.s) == result::OK);
    CHECK(script::running(f.s));
    CHECK(script::bytes(f.s) > 0);

    // on_spawn ran at start: its marks are there before any clock ticks
    CHECK(hp(&f, h) == 1.0f);
    CHECK(strcmp((const char *)health_field(&f, h, "faction"), "red") == 0);
    CHECK(*(i32 *)health_field(&f, h, "style") == 0);
    CHECK(entity::get(f.w, h)->local.position.z == 3.0f);

    const entity::ComponentDesc *d =
        entity::component_desc(f.w, (u32)entity::find_component(f.w, "health"));
    const entity::FieldDesc *clips = field_of(d, "clips");
    void *comp = entity::component(f.w, h, (u32)entity::find_component(f.w, "health"));
    CHECK(*entity::table_count(comp, clips) == 2);
    CHECK(strcmp(entity::table_rows(comp, clips)[0].value, "animations/pistol_idle.glb") == 0);
    CHECK(entity::table_rows(comp, clips)[1].value[0] == 0);

    // two fixed steps: +1 from the probe and +10 from main each; one frame: +100
    script::fixed_update(f.s, 0.5f);
    script::fixed_update(f.s, 0.5f);
    script::update(f.s, 0.25f);
    CHECK(hp(&f, h) == 123.0f);
    linalg::Vec3 tint = *(linalg::Vec3 *)health_field(&f, h, "tint");
    CHECK(tint.x == 2.0f && tint.y == 0.25f);

    // an entity given a script during play is spawned on the next tick
    entity::Handle late = spawn_probe(a, &f, "late", "scripts/probe.lua");
    CHECK(hp(&f, late) == 5.0f);
    script::update(f.s, 0.0f);
    CHECK(hp(&f, late) == 101.0f);

    // destroying one runs its on_despawn while it still stands
    entity::destroy(f.w, late);
    CHECK(!entity::alive(f.w, late));

    script::stop(f.s);
    CHECK(!script::running(f.s));
    CHECK(hp(&f, h) == -1.0f);
    CHECK(script::bytes(f.s) == 0);

    // nothing runs outside play
    script::update(f.s, 1.0f);
    CHECK(hp(&f, h) == -1.0f);
}

static void a_hook_that_raises_is_reported_once_and_switched_off(memory::Arena *a) {
    Fixture f = open_fixture(a);
    entity::Handle h = spawn_probe(a, &f, "broken", "scripts/broken.lua");
    CHECK(script::start(f.s) == result::OK);

    test::mute();
    script::update(f.s, 0.1f);
    script::update(f.s, 0.1f);
    CHECK(test::unmute() == 1);

    // the rest of the level plays on
    CHECK(script::running(f.s));
    CHECK(hp(&f, h) == 5.0f);

    // a reload gives it another chance, and it fails the same way once more
    test::mute();
    CHECK(script::reload(f.s) == result::OK);
    script::update(f.s, 0.1f);
    CHECK(test::unmute() == 1);
    script::stop(f.s);
}

static void a_changed_declaration_refuses_play(memory::Arena *a) {
    Fixture f = open_fixture(a);

    snprintf(f.proj->script, sizeof(f.proj->script), "%s", "scripts/main_changed.lua");
    test::mute();
    CHECK(script::start(f.s) == result::ERR_PARSE);
    CHECK(test::unmute() >= 1);
    CHECK(!script::running(f.s));

    // no main script at all is a level with no level-wide hooks, not an error
    snprintf(f.proj->script, sizeof(f.proj->script), "%s", "scripts/missing.lua");
    CHECK(script::start(f.s) == result::OK);
    CHECK(script::running(f.s));
    script::stop(f.s);
}

static void a_script_that_does_not_load_leaves_its_entity_without_hooks(memory::Arena *a) {
    Fixture f = open_fixture(a);
    entity::Handle h = spawn_probe(a, &f, "nope", "scripts/nope.lua");

    test::mute();
    CHECK(script::start(f.s) == result::OK);
    script::update(f.s, 0.1f);
    script::update(f.s, 0.1f);
    CHECK(test::unmute() == 1); // the missing file, once
    CHECK(hp(&f, h) == 5.0f);
    script::stop(f.s);
}

static void the_bindings_refuse_what_the_engine_would_not_take(memory::Arena *a) {
    Fixture f = open_fixture(a);
    entity::Handle h = spawn_probe(a, &f, "probe", "scripts/probe.lua");
    CHECK(script::start(f.s) == result::OK);

    // each raises, and the log says so: the error, plus the registry's own
    // word where a declaration is refused
    struct Case {
        const char *code;
        u32 logged;
    };
    static const Case CASES[] = {
        {"engine.entity.add_component(engine.entity.find_by_name('probe'), 'nope')", 1},
        {"engine.entity.component(engine.entity.find_by_name('probe'), 'health').hp = 'x'", 1},
        {"engine.entity.component(engine.entity.find_by_name('probe'), 'health').style = 'z'", 1},
        {"engine.entity.component(engine.entity.find_by_name('probe'), 'health').faction = "
         "string.rep('a', 64)",
         1},
        {"engine.entity.transform(engine.entity.find_by_name('probe')).position = {x = 1}", 1},
        {"engine.entity.register_component{key = 'late', fields = {}}", 2},
        {"engine.input.consume_pressed(nil)", 1},
    };
    for (u32 i = 0; i < ARRAY_COUNT(CASES); i++) {
        test::mute();
        CHECK(script::run(f.s, CASES[i].code, "case") == result::ERR_INVALID);
        CHECK(test::unmute() == CASES[i].logged);
    }
    CHECK(hp(&f, h) == 1.0f);

    // and what is fine is fine
    CHECK(script::run(f.s, "engine.entity.component(engine.entity.find_by_name('probe'), "
                            "'health').hp = 7",
                      "ok") == result::OK);
    CHECK(hp(&f, h) == 7.0f);

    script::stop(f.s);
    test::mute();
    CHECK(script::run(f.s, "x = 1", "idle") == result::ERR_INVALID);
    CHECK(test::unmute() == 1);
}

test::Suite script_suite() {
    static const test::Case cases[] = {
        CASE(the_main_script_declares_components_at_open),
        CASE(hooks_run_on_both_clocks_and_despawn_at_stop),
        CASE(a_hook_that_raises_is_reported_once_and_switched_off),
        CASE(a_changed_declaration_refuses_play),
        CASE(a_script_that_does_not_load_leaves_its_entity_without_hooks),
        CASE(the_bindings_refuse_what_the_engine_would_not_take),
    };
    return test::Suite{.name = "script", .cases = cases, .count = ARRAY_COUNT(cases)};
}
