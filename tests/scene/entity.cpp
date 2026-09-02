#include "scene/entity.h"

#include <string.h>

#include "suites.h"

// one manager for the suite: scope arenas never return to the reserve, and the real soldier needs
// room
static asset::Manager *shared_assets(memory::Arena *scratch) {
    static asset::Manager *m = NULL;
    if (m)
        return m;

    static memory::Arena arena;
    arena = memory::create(MiB(4));
    m = MUST(asset::init(&arena, scratch,
                         asset::Config{
                             .root = CL_ASSET_ROOT,
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

static entity::World *make_world(memory::Arena *a) {
    return MUST(entity::init(a, entity::Config{
                                    .max_entities = 64,
                                    .max_renderables = 16,
                                    .max_animators = 8,
                                    .max_cameras = 4,
                                    .max_lights = 4,
                                }));
}

// The first block named key in text, parsed into a.
static config::Node *block(memory::Arena *a, const char *text, const char *key) {
    config::Doc *d = MUST(config::parse(a, memory::push_str(a, text), "test"));
    config::Node *n = config::find(d->first, key);
    CHECK(n != NULL);
    return n;
}

static const char *written(memory::Arena *a, entity::World *w, entity::Handle h, const char *key,
                           b32 expand) {
    config::Doc *doc = config::create(a, "test");
    entity::write_entity(a, doc, NULL, w, h, key, expand);
    return config::write(a, doc);
}

static const char *PROBE = "entity \"probe\" {\n"
                           "    transform {\n"
                           "        position 1 2 3\n"
                           "        rotation 0 90 0\n"
                           "        scale 2\n"
                           "    }\n"
                           "    camera {\n"
                           "        fov 45\n"
                           "        near 0.1\n"
                           "        far 100\n"
                           "        active false\n"
                           "    }\n"
                           "    light {\n"
                           "        kind directional\n"
                           "        color 1 0.5 0.25\n"
                           "        intensity 2\n"
                           "        ambient 0.1\n"
                           "    }\n"
                           "}\n";

static void defaults_come_from_the_table(memory::Arena *a) {
    entity::World *w = make_world(a);
    entity::Handle h = entity::spawn(w, "x");

    entity::Camera *c = entity::add_camera(w, h);
    CHECK(c->fov_y == 60.0f && c->z_near == 0.05f && c->z_far == 500.0f && c->active);
    CHECK(c->owner.v == h.v);

    entity::Light *l = entity::add_light(w, h);
    CHECK(l->kind == entity::LIGHT_DIRECTIONAL && l->intensity == 0.72f && l->ambient == 0.28f);
    CHECK(l->color.x == 1.0f && l->color.y == 1.0f && l->color.z == 1.0f && l->active);

    CHECK(entity::add_renderable(w, h)->visible);
    entity::Animator *an = entity::add_animator(w, h);
    CHECK(an->speed == 1.0f && an->playing);

    // adding again returns the existing component, values intact
    c->fov_y = 30.0f;
    CHECK(entity::add_camera(w, h) == c);
    CHECK(c->fov_y == 30.0f);
}

static void fields_read_every_type(memory::Arena *a) {
    entity::World *w = make_world(a);
    asset::Manager *m = shared_assets(a);

    entity::Handle h = entity::spawn_from_config(w, m, block(a, PROBE, "entity"), entity::NONE);
    entity::Entity *e = entity::get(w, h);
    CHECK(e != NULL);
    CHECK(strcmp(e->name, "probe") == 0);
    CHECK(e->definition[0] == 0);

    CHECK(e->local.position.x == 1.0f && e->local.position.y == 2.0f &&
          e->local.position.z == 3.0f);
    CHECK(e->local.scale.x == 2.0f && e->local.scale.y == 2.0f && e->local.scale.z == 2.0f);
    CHECK(e->local.use_euler && e->local.euler.y == 90.0f);

    entity::Camera *c = entity::camera(w, h);
    CHECK(c->fov_y == 45.0f && c->z_near == 0.1f && c->z_far == 100.0f && !c->active);

    entity::Light *l = entity::light(w, h);
    CHECK(l->kind == entity::LIGHT_DIRECTIONAL);
    CHECK(l->color.x == 1.0f && l->color.y == 0.5f && l->color.z == 0.25f);
    CHECK(l->intensity == 2.0f && l->ambient == 0.1f && l->active); // active kept its default
}

static void bad_values_warn_and_keep_defaults(memory::Arena *a) {
    entity::World *w = make_world(a);
    asset::Manager *m = shared_assets(a);

    const char *text = "entity \"bad\" {\n"
                       "    camera {\n"
                       "        near -1\n"
                       "    }\n"
                       "    light {\n"
                       "        kind spotlight\n"
                       "    }\n"
                       "}\n";

    test::mute();
    entity::Handle h = entity::spawn_from_config(w, m, block(a, text, "entity"), entity::NONE);
    CHECK(test::unmute() == 2);

    CHECK(entity::camera(w, h)->z_near == 0.05f);
    CHECK(entity::light(w, h)->kind == entity::LIGHT_DIRECTIONAL);
}

static void inline_entity_round_trips(memory::Arena *a) {
    entity::World *w = make_world(a);
    asset::Manager *m = shared_assets(a);

    const char *expected = "entity probe {\n"
                           "    transform {\n"
                           "        position 1 2 3\n"
                           "        rotation 0 90 0\n"
                           "        scale 2\n"
                           "    }\n"
                           "    camera {\n"
                           "        fov 45\n"
                           "        near 0.1\n"
                           "        far 100\n"
                           "        active false\n"
                           "    }\n"
                           "    light {\n"
                           "        kind directional\n"
                           "        color 1 0.5 0.25\n"
                           "        intensity 2\n"
                           "        ambient 0.1\n"
                           "        active true\n"
                           "    }\n"
                           "}\n";

    entity::Handle h1 = entity::spawn_from_config(w, m, block(a, PROBE, "entity"), entity::NONE);
    const char *w1 = written(a, w, h1, "entity", false);
    CHECK(strcmp(w1, expected) == 0);

    entity::Handle h2 = entity::spawn_from_config(w, m, block(a, w1, "entity"), entity::NONE);
    const char *w2 = written(a, w, h2, "entity", false);
    CHECK(strcmp(w1, w2) == 0);
}

static void quaternion_rotation_is_written_as_four_values(memory::Arena *a) {
    entity::World *w = make_world(a);
    entity::Handle h = entity::spawn(w, "q");

    entity::set_rotation(&entity::get(w, h)->local,
                         linalg::quat_from_euler(linalg::Vec3{0.0f, 90.0f, 0.0f}));

    const char *text = written(a, w, h, "entity", false);
    CHECK(strstr(text, "rotation 0 0.707107 0 0.707107") != NULL);

    // and what it read back is unit length
    entity::set_euler(&entity::get(w, h)->local, linalg::Vec3{0.0f, 0.0f, 0.0f});
    CHECK(strstr(written(a, w, h, "entity", false), "rotation 0 0 0") != NULL);
}

// a reference to a real file keeps its values except where the block restates them
static void reference_applies_overrides(memory::Arena *a) {
    entity::World *w = make_world(a);
    asset::Manager *m = shared_assets(a);

    const char *text = "instance \"cam\" {\n"
                       "    definition templates/camera3d\n"
                       "    transform {\n"
                       "        position 0 1 3\n"
                       "    }\n"
                       "    camera {\n"
                       "        fov 90\n"
                       "    }\n"
                       "}\n";

    entity::Handle h = entity::spawn_from_config(w, m, block(a, text, "instance"), entity::NONE);
    entity::Entity *e = entity::get(w, h);
    CHECK(e != NULL);
    CHECK(strcmp(e->name, "cam") == 0);
    CHECK(strcmp(e->definition, "templates/camera3d") == 0);
    CHECK(e->overridden == (1u << entity::COMPONENT_CAMERA));
    CHECK(e->local.position.y == 1.0f && e->local.position.z == 3.0f);

    entity::Camera *c = entity::camera(w, h);
    CHECK(c->fov_y == 90.0f && c->z_near == 0.05f && c->z_far == 500.0f);

    // written as a reference: definition, transform, and only the overridden component
    const char *expected = "instance cam {\n"
                           "    definition templates/camera3d\n"
                           "    transform {\n"
                           "        position 0 1 3\n"
                           "        rotation 0 0 0\n"
                           "        scale 1\n"
                           "    }\n"
                           "    camera {\n"
                           "        fov 90\n"
                           "        near 0.05\n"
                           "        far 500\n"
                           "        active true\n"
                           "    }\n"
                           "}\n";
    CHECK(strcmp(written(a, w, h, "instance", false), expected) == 0);
}

// soldier.entity is real: model, clip, and a pistol attached to a joint. The
// instance write restates none of it; the expanded write restates all of it.
static void definition_children_are_owned(memory::Arena *a) {
    entity::World *w = make_world(a);
    asset::Manager *m = shared_assets(a);

    entity::Handle h = entity::spawn_from_config(
        w, m, block(a, "instance \"player\" {\n    definition soldier\n}\n", "instance"),
        entity::NONE);
    entity::Entity *e = entity::get(w, h);
    CHECK(e != NULL);
    CHECK(strcmp(e->definition, "soldier") == 0);
    CHECK(entity::renderable(w, h)->model.v != 0); // the real model loaded
    CHECK(entity::animator(w, h)->clip.v != 0);

    entity::Entity *weapon = entity::get(w, e->first_child);
    CHECK(weapon != NULL);
    CHECK(strcmp(weapon->name, "weapon") == 0);
    CHECK(weapon->owned_by_definition);
    CHECK(weapon->attach_joint_index >= 0);
    CHECK(strcmp(weapon->definition, "pistol") == 0);

    const char *as_instance = written(a, w, h, "instance", false);
    CHECK(strcmp(as_instance, "instance player {\n"
                              "    definition soldier\n"
                              "    transform {\n"
                              "        position 0 0 0\n"
                              "        rotation 0 0 0\n"
                              "        scale 1\n"
                              "    }\n"
                              "}\n") == 0);

    const char *expanded = written(a, w, h, "entity", true);
    CHECK(strstr(expanded, "entity player {") != NULL);
    CHECK(strstr(expanded, "model characters/soldier.glb") != NULL);
    CHECK(strstr(expanded, "clip animations/pistol_idle.glb") != NULL);
    CHECK(strstr(expanded, "child weapon {") != NULL);
    CHECK(strstr(expanded, "definition pistol") != NULL);
    CHECK(strstr(expanded, "attach_joint mixamorig_RightHand") != NULL);
    CHECK(strstr(expanded, "rotation 180 0 90") != NULL);
    CHECK(strstr(expanded, "weapons/pistol.glb") == NULL); // the child is still a reference

    // a child the level adds is not owned, and is written
    entity::Handle extra = entity::spawn(w, "extra");
    entity::set_parent(w, m, extra, h, NULL);
    CHECK(strstr(written(a, w, h, "instance", false), "child extra {") != NULL);
}

static void missing_model_falls_back_and_is_cached(memory::Arena *a) {
    entity::World *w = make_world(a);
    asset::Manager *m = shared_assets(a);

    const char *text = "entity \"crate\" {\n"
                       "    renderable {\n"
                       "        model nope/missing.glb\n"
                       "    }\n"
                       "}\n";

    test::mute();
    entity::Handle h1 = entity::spawn_from_config(w, m, block(a, text, "entity"), entity::NONE);
    CHECK(test::unmute() >= 1);
    CHECK(entity::renderable(w, h1)->model.v == 0);

    // the second reference hits the failure cache: only OR_ELSE's warning, nothing from the loader
    test::mute();
    entity::spawn_from_config(w, m, block(a, text, "entity"), entity::NONE);
    CHECK(test::unmute() == 1);

    // and the path survives for a writer
    CHECK(strstr(written(a, w, h1, "entity", false), "model nope/missing.glb") != NULL);
}

test::Suite entity_suite() {
    static const test::Case cases[] = {
        CASE(defaults_come_from_the_table),
        CASE(fields_read_every_type),
        CASE(bad_values_warn_and_keep_defaults),
        CASE(inline_entity_round_trips),
        CASE(quaternion_rotation_is_written_as_four_values),
        CASE(reference_applies_overrides),
        CASE(definition_children_are_owned),
        CASE(missing_model_falls_back_and_is_cached),
    };
    return test::Suite{.name = "entity", .cases = cases, .count = ARRAY_COUNT(cases)};
}
