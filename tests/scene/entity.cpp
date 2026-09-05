#include "scene/entity.h"

#include <string.h>

#include "platform/file.h"
#include "platform/path.h"
#include "suites.h"

// written into the fixture's entities/ and removed again
#define TEST_DEFINITION "zz_reload_test"

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

static entity::World *make_world(memory::Arena *a) {
    return MUST(entity::init(a, entity::Config{
                                    .max_entities = 64,
                                    .pool_default = 16,
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
                           b32 as_definition) {
    config::Doc *doc = config::create(a, "test");
    entity::write_entity(a, doc, NULL, w, h, key, as_definition);
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
    CHECK(an->speed == 1.0f && an->autoplay && an->clip_count == 0 && an->current == -1);

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
                           "    id 1\n"
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

    // read back into a fresh world, where its id is free to be the same
    entity::World *w2_world = make_world(a);
    entity::Handle h2 =
        entity::spawn_from_config(w2_world, m, block(a, w1, "entity"), entity::NONE);
    const char *w2 = written(a, w2_world, h2, "entity", false);
    CHECK(strcmp(w1, w2) == 0);
}

static void ids_are_unique_and_kept_by_the_file(memory::Arena *a) {
    entity::World *w = make_world(a);
    asset::Manager *m = shared_assets(a);

    entity::Handle first = entity::spawn(w, "first");
    entity::Handle second = entity::spawn(w, "second");
    CHECK(entity::id_of(w, first) == 1 && entity::id_of(w, second) == 2);

    // a freed id is never handed out again
    entity::destroy(w, first);
    CHECK(entity::id_of(w, entity::spawn(w, "third")) == 3);
    CHECK(entity::find_by_id(w, 2).v == second.v);
    CHECK(entity::find_by_id(w, 1).v == 0);

    // a file's id is taken, and the counter moves past it
    entity::Handle from_file = entity::spawn_from_config(
        w, m, block(a, "entity seven {\n    id 7\n}\n", "entity"), entity::NONE);
    CHECK(entity::id_of(w, from_file) == 7);
    CHECK(entity::id_of(w, entity::spawn(w, "eighth")) == 8);

    // an id already alive is refused, loudly, and a fresh one stands in
    test::mute();
    entity::Handle clash = entity::spawn_with_id(w, "clash", 2);
    CHECK(test::unmute() == 1);
    CHECK(entity::id_of(w, clash) == 9);
    CHECK(entity::find_by_id(w, 2).v == second.v);
}

// the children a definition spawns are not written, so the instance names their ids
static void owned_ids_come_back_the_same(memory::Arena *a) {
    entity::World *w = make_world(a);
    asset::Manager *m = shared_assets(a);

    entity::Handle h = entity::spawn_from_config(
        w, m, block(a, "instance \"player\" {\n    definition soldier\n}\n", "instance"),
        entity::NONE);
    entity::Handle weapon = entity::get(w, h)->first_child;
    u32 weapon_id = entity::id_of(w, weapon);
    CHECK(weapon_id != 0 && weapon_id != entity::id_of(w, h));

    const char *text = written(a, w, h, "instance", false);
    CHECK(strstr(text, "owned_ids ") != NULL);

    entity::World *w2 = make_world(a);
    entity::Handle h2 = entity::spawn_from_config(w2, m, block(a, text, "instance"), entity::NONE);
    CHECK(entity::id_of(w2, h2) == entity::id_of(w, h));
    CHECK(entity::id_of(w2, entity::get(w2, h2)->first_child) == weapon_id);
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
                           "    id 1\n"
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
    CHECK(entity::animator(w, h)->handles[0].v != 0);
    CHECK(entity::animator(w, h)->current == 0 && entity::animator(w, h)->playing);

    entity::Entity *weapon = entity::get(w, e->first_child);
    CHECK(weapon != NULL);
    CHECK(strcmp(weapon->name, "weapon") == 0);
    CHECK(weapon->owned_by_definition);
    CHECK(weapon->attach_joint_index >= 0);
    CHECK(strcmp(weapon->definition, "pistol") == 0);

    // the reference, its id, the ids its definition spawned, and the transform; no child blocks
    const char *as_instance = written(a, w, h, "instance", false);
    CHECK(strstr(as_instance, "instance player {\n    id 1\n    definition soldier\n") != NULL);
    CHECK(strstr(as_instance, "owned_ids ") != NULL);
    CHECK(strstr(as_instance, "transform {") != NULL);
    CHECK(strstr(as_instance, "child") == NULL);
    CHECK(strstr(as_instance, "renderable") == NULL);

    // as a definition: still a reference to soldier, with no ids and nothing of soldier's restated
    const char *as_definition = written(a, w, h, "entity", true);
    CHECK(strstr(as_definition, "entity player {") != NULL);
    CHECK(strstr(as_definition, "definition soldier") != NULL);
    CHECK(strstr(as_definition, "    id ") == NULL);
    CHECK(strstr(as_definition, "owned_ids") == NULL);
    CHECK(strstr(as_definition, "model characters/soldier.glb") == NULL);
    CHECK(strstr(as_definition, "child weapon {") == NULL);

    // detached, it is its own entity and spells everything out
    entity::detach_definition(w, h);
    const char *detached = written(a, w, h, "entity", true);
    CHECK(strstr(detached, "definition soldier") == NULL);
    CHECK(strstr(detached, "model characters/soldier.glb") != NULL);
    CHECK(strstr(detached, "idle animations/pistol_idle.glb") != NULL);
    CHECK(strstr(detached, "child weapon {") != NULL);
    CHECK(strstr(detached, "definition pistol") != NULL);
    CHECK(strstr(detached, "attach_joint mixamorigRightHand") != NULL); // written canonical
    CHECK(strstr(detached, "rotation 180 0 90") != NULL);
    CHECK(strstr(detached, "weapons/pistol.glb") == NULL); // the child is still a reference

    // a child the level adds is not owned, and is written
    entity::attach_definition(w, h, "soldier");
    entity::Handle extra = entity::spawn(w, "extra");
    entity::set_parent(w, m, extra, h, NULL);
    CHECK(strstr(written(a, w, h, "instance", false), "child extra {") != NULL);
    CHECK(strstr(written(a, w, h, "instance", false), "renderable") == NULL);
}

static void the_project_folder_is_listed(memory::Arena *a) {
    asset::Manager *m = shared_assets(a);

    entity::Name names[8];
    CHECK(entity::list_levels(m, names, 8) == 1);
    CHECK(strcmp(names[0].v, "main") == 0);

    // sorted, and the templates folder is not a definition
    CHECK(entity::list_definitions(m, NULL, names, 8) == 2);
    CHECK(strcmp(names[0].v, "pistol") == 0 && strcmp(names[1].v, "soldier") == 0);

    CHECK(entity::list_definitions(m, ENTITY_TEMPLATE_SUBDIR, names, 8) == 2);
    CHECK(strcmp(names[0].v, "templates/camera3d") == 0);

    // the count is the truth even when the room is not there
    CHECK(entity::list_definitions(m, NULL, names, 1) == 2);
    CHECK(entity::list_definitions(m, NULL, NULL, 0) == 2);
}

// a definition saved from the editor becomes a file, the entity an instance of
// it, and every instance follows the file when it changes
static void a_saved_definition_is_reloaded_into_its_instances(memory::Arena *a) {
    entity::World *w = make_world(a);
    asset::Manager *m = shared_assets(a);

    // a template copied: a plain entity writing its camera in full
    entity::Handle cam = MUST(entity::spawn_definition(w, m, "templates/camera3d"));
    entity::detach_definition(w, cam);
    CHECK(entity::get(w, cam)->definition[0] == 0);
    CHECK(strstr(written(a, w, cam, "instance", false), "fov 60") != NULL);

    entity::camera(w, cam)->fov_y = 33.0f;
    u32 cam_id = entity::id_of(w, cam);
    CHECK(entity::save_definition(w, m, cam, TEST_DEFINITION) == result::OK);
    entity::attach_definition(w, cam, TEST_DEFINITION);
    CHECK(strstr(written(a, w, cam, "instance", false), "definition " TEST_DEFINITION) != NULL);
    CHECK(strstr(written(a, w, cam, "instance", false), "fov") == NULL);

    entity::Handle other = MUST(entity::spawn_definition(w, m, TEST_DEFINITION));
    u32 other_id = entity::id_of(w, other);
    CHECK(entity::camera(w, other)->fov_y == 33.0f);

    // saved over its own file it is spelled out, not made to reference itself
    entity::camera(w, cam)->fov_y = 44.0f;
    CHECK(entity::save_definition(w, m, cam, TEST_DEFINITION) == result::OK);
    CHECK(strcmp(entity::get(w, cam)->definition, TEST_DEFINITION) == 0);

    // the file changed; both instances take it, keeping their ids
    entity::camera(w, other)->fov_y = 1.0f;
    CHECK(entity::reload_definition(w, m, TEST_DEFINITION) == 2);
    entity::Handle cam_back = entity::find_by_id(w, cam_id);
    entity::Handle other_back = entity::find_by_id(w, other_id);
    CHECK(cam_back.v && other_back.v);
    CHECK(entity::camera(w, cam_back)->fov_y == 44.0f);
    CHECK(entity::camera(w, other_back)->fov_y == 44.0f);
    CHECK(strcmp(entity::get(w, other_back)->definition, TEST_DEFINITION) == 0);

    CHECK(file::remove(FPATH(a, CL_TEST_FIXTURE, "entities", TEST_DEFINITION ".entity")));
    CHECK(entity::list_definitions(m, NULL, NULL, 0) == 2);
}

static const char *ANIMATED = "entity \"animated\" {\n"
                              "    renderable {\n"
                              "        model characters/soldier.glb\n"
                              "    }\n"
                              "    animator {\n"
                              "        clips {\n"
                              "            idle animations/pistol_idle.glb\n"
                              "            fire animations/pistol_idle.glb\n"
                              "        }\n"
                              "        start fire\n"
                              "        autoplay false\n"
                              "    }\n"
                              "}\n";

static void the_clip_table_plays_by_name(memory::Arena *a) {
    entity::World *w = make_world(a);
    asset::Manager *m = shared_assets(a);

    entity::Handle h = entity::spawn_from_config(w, m, block(a, ANIMATED, "entity"), entity::NONE);
    entity::Animator *an = entity::animator(w, h);
    CHECK(an != NULL);
    CHECK(an->clip_count == 2 && strcmp(an->clips[1].name, "fire") == 0);
    CHECK(an->current == 1 && !an->playing); // starts on fire, held until told
    CHECK(an->handles[0].v != 0 && an->handles[1].v == an->handles[0].v); // one file, one clip
    CHECK(strcmp(entity::current_clip(w, h), "fire") == 0);

    test::mute();
    CHECK(!entity::play(w, h, "reload"));
    CHECK(test::unmute() == 1);
    CHECK(entity::play(w, h, "idle"));
    CHECK(an->current == 0 && an->playing && an->time == 0.0f && !an->finished);

    // a clip that does not loop stops at its end and says so
    asset::Clip *clip = asset::get(m, an->handles[0]);
    b32 loop_before = clip->loop;
    clip->loop = false;
    entity::end_frame(w, m, a, clip->duration * 2.0f);
    CHECK(!an->playing && an->finished && an->time == clip->duration);
    CHECK(entity::clip_finished(w, h));
    clip->loop = loop_before;

    // written back as the table it was read from
    const char *text = written(a, w, h, "entity", true);
    CHECK(strstr(text, "clips {") != NULL);
    CHECK(strstr(text, "idle animations/pistol_idle.glb") != NULL);
    CHECK(strstr(text, "fire animations/pistol_idle.glb") != NULL);
    CHECK(strstr(text, "start fire") != NULL);
    CHECK(strstr(text, "autoplay false") != NULL);
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

// A component with no C++ struct behind it: what a script will declare. Its
// layout, its defaults and its place in a file all come out of this table.
static const char *TRACER_STYLE_NAMES[] = {"line", "beam"};

static const entity::FieldDesc TRACER_FIELDS[] = {
    {.key = "speed", .type = entity::FIELD_F32, .def = {.as_f32 = 900.0f}},
    {.key = "visible", .type = entity::FIELD_B32, .def = {.as_i32 = true}},
    {.key = "model",
     .type = entity::FIELD_STRING,
     .cap = ENTITY_PATH_CAP,
     .asset = entity::ASSET_MODEL},
    {.key = "style",
     .type = entity::FIELD_ENUM,
     .names = TRACER_STYLE_NAMES,
     .name_count = ARRAY_COUNT(TRACER_STYLE_NAMES),
     .def = {.as_i32 = 1}},
    {.key = "clips", .type = entity::FIELD_TABLE, .cap = 4, .asset = entity::ASSET_CLIP},
    {.key = "tint", .type = entity::FIELD_VEC3, .def = {.as_vec3 = {1.0f, 0.5f, 0.25f}}},
};

static const entity::ComponentDesc TRACER = {
    .key = "tracer",
    .fields = TRACER_FIELDS,
    .field_count = ARRAY_COUNT(TRACER_FIELDS),
};

static const char *SHOT = "entity \"shot\" {\n"
                          "    tracer {\n"
                          "        speed 1200\n"
                          "        visible false\n"
                          "        model models/pistol.glb\n"
                          "        style line\n"
                          "        clips {\n"
                          "            fly animations/pistol_idle.glb\n"
                          "        }\n"
                          "        tint 0 1 0\n"
                          "    }\n"
                          "}\n";

// a data component has no struct, so its fields are reached through the registry
static const entity::FieldDesc *field_of(const entity::ComponentDesc *d, const char *key) {
    for (u32 i = 0; i < d->field_count; i++)
        if (strcmp(d->fields[i].key, key) == 0)
            return &d->fields[i];
    CHECK(false);
    return &d->fields[0];
}

static void *field_at(const entity::ComponentDesc *d, void *comp, const char *key) {
    return (u8 *)comp + field_of(d, key)->offset;
}

static void a_declared_component_is_laid_out_and_registered(memory::Arena *a) {
    entity::World *w = make_world(a);

    i32 tracer = entity::register_component(w, &TRACER);
    CHECK(tracer == (i32)entity::COMPONENT_ENGINE_COUNT);
    CHECK(entity::component_count(w) == (u32)entity::COMPONENT_ENGINE_COUNT + 1);
    CHECK(entity::find_component(w, "tracer") == tracer);
    CHECK(entity::find_component(w, "nothing") == -1);

    // the engine's own registered first and kept their indices
    CHECK(entity::find_component(w, "renderable") == (i32)entity::COMPONENT_RENDERABLE);
    CHECK(entity::find_component(w, "collider") == (i32)entity::COMPONENT_COLLIDER);

    // a key can only be claimed once, and the declaration is copied, not held
    test::mute();
    CHECK(entity::register_component(w, &TRACER) == -1);
    CHECK(test::unmute() == 1);

    const entity::ComponentDesc *d = entity::component_desc(w, (u32)tracer);
    CHECK(d->key != TRACER.key && strcmp(d->key, "tracer") == 0);
    CHECK(d->fields != TRACER_FIELDS);
    CHECK(d->after_read == NULL);

    // owner first, then the fields in declaration order, none overlapping
    CHECK(d->fields[0].offset == sizeof(entity::Handle));
    for (u32 i = 1; i < d->field_count; i++)
        CHECK(d->fields[i].offset >=
              d->fields[i - 1].offset + entity::field_size(&d->fields[i - 1]));

    const entity::FieldDesc *last = &d->fields[d->field_count - 1];
    CHECK(d->size >= last->offset + entity::field_size(last));
    CHECK(d->size <= ENTITY_MAX_COMPONENT_BYTES);
}

static void a_data_component_reads_writes_and_defaults(memory::Arena *a) {
    entity::World *w = make_world(a);
    asset::Manager *m = shared_assets(a);

    u32 tracer = (u32)entity::register_component(w, &TRACER);
    const entity::ComponentDesc *d = entity::component_desc(w, tracer);

    // a fresh component is what the field table said
    entity::Handle blank = entity::spawn(w, "blank");
    void *c = entity::add_component(w, blank, tracer);
    CHECK(c != NULL);
    CHECK(((entity::Handle *)c)->v == blank.v);
    CHECK(*(f32 *)field_at(d, c, "speed") == 900.0f);
    CHECK(*(b32 *)field_at(d, c, "visible"));
    CHECK(*(i32 *)field_at(d, c, "style") == 1);
    CHECK(((const char *)field_at(d, c, "model"))[0] == 0);
    CHECK(((linalg::Vec3 *)field_at(d, c, "tint"))->y == 0.5f);

    const entity::FieldDesc *clips = field_of(d, "clips");
    CHECK(*entity::table_count(c, clips) == 0);

    // and a file says otherwise for every type
    entity::Handle h = entity::spawn_from_config(w, m, block(a, SHOT, "entity"), entity::NONE);
    void *shot = entity::component(w, h, tracer);
    CHECK(shot != NULL);
    CHECK(*(f32 *)field_at(d, shot, "speed") == 1200.0f);
    CHECK(!*(b32 *)field_at(d, shot, "visible"));
    CHECK(*(i32 *)field_at(d, shot, "style") == 0);
    CHECK(strcmp((const char *)field_at(d, shot, "model"), "models/pistol.glb") == 0);
    CHECK(((linalg::Vec3 *)field_at(d, shot, "tint"))->y == 1.0f);
    CHECK(*entity::table_count(shot, clips) == 1);
    CHECK(strcmp(entity::table_rows(shot, clips)[0].name, "fly") == 0);

    // written back as the block it was read from
    const char *text = written(a, w, h, "entity", true);
    CHECK(strstr(text, "tracer {") != NULL);
    CHECK(strstr(text, "speed 1200") != NULL);
    CHECK(strstr(text, "style line") != NULL);
    CHECK(strstr(text, "fly animations/pistol_idle.glb") != NULL);

    // and the level round trips through text with the bytes intact
    u8 before[ENTITY_MAX_COMPONENT_BYTES];
    memcpy(before, shot, d->size);
    u32 id = entity::id_of(w, h);

    char *level = entity::write_level(a, w);
    entity::clear(w);
    CHECK(entity::load_level_text(w, m, level, "<test>") == 2);

    void *back = entity::component(w, entity::find_by_id(w, id), tracer);
    CHECK(back != NULL);
    CHECK(memcmp((u8 *)back + sizeof(entity::Handle), before + sizeof(entity::Handle),
                 d->size - sizeof(entity::Handle)) == 0);
}

// a declaration the layout cannot be built from is refused, not half-registered
static void a_bad_declaration_is_refused(memory::Arena *a) {
    entity::World *w = make_world(a);
    u32 before = entity::component_count(w);

    // the keys an entity block reads for itself
    entity::ComponentDesc transform = TRACER;
    transform.key = "transform";
    entity::ComponentDesc child = TRACER;
    child.key = "child";

    // an enum with a count but no names, and one whose default is not a name
    const entity::FieldDesc nameless[] = {
        {.key = "style", .type = entity::FIELD_ENUM, .name_count = 2},
    };
    const entity::FieldDesc off_the_end[] = {
        {.key = "style",
         .type = entity::FIELD_ENUM,
         .names = TRACER_STYLE_NAMES,
         .name_count = ARRAY_COUNT(TRACER_STYLE_NAMES),
         .def = {.as_i32 = 2}},
    };
    const entity::ComponentDesc no_names = {.key = "a", .fields = nameless, .field_count = 1};
    const entity::ComponentDesc bad_default = {.key = "b", .fields = off_the_end, .field_count = 1};

    test::mute();
    CHECK(entity::register_component(w, &transform) == -1);
    CHECK(entity::register_component(w, &child) == -1);
    CHECK(entity::register_component(w, &no_names) == -1);
    CHECK(entity::register_component(w, &bad_default) == -1);
    CHECK(test::unmute() == 4);

    CHECK(entity::component_count(w) == before);
    CHECK(entity::find_component(w, "transform") == -1);
}

// a string with a default can be emptied, and the file says so
static void an_emptied_string_default_round_trips(memory::Arena *a) {
    entity::World *w = make_world(a);
    asset::Manager *m = shared_assets(a);

    const entity::FieldDesc fields[] = {
        {.key = "team", .type = entity::FIELD_STRING, .cap = ENTITY_NAME_CAP,
         .def = {.as_str = "red"}},
    };
    const entity::ComponentDesc desc = {.key = "side", .fields = fields, .field_count = 1};
    u32 side = (u32)entity::register_component(w, &desc);
    const entity::ComponentDesc *d = entity::component_desc(w, side);

    entity::Handle h = entity::spawn(w, "flag");
    char *team = (char *)field_at(d, entity::add_component(w, h, side), "team");
    CHECK(strcmp(team, "red") == 0);
    team[0] = 0;

    const char *text = written(a, w, h, "entity", true);
    entity::Handle back = entity::spawn_from_config(w, m, block(a, text, "entity"), entity::NONE);
    CHECK(((const char *)field_at(d, entity::component(w, back, side), "team"))[0] == 0);
}

// a component the pools block does not name takes pool_default, which make_world sets to 16
static void an_unnamed_component_takes_the_default_capacity(memory::Arena *a) {
    entity::World *w = make_world(a);
    u32 tracer = (u32)entity::register_component(w, &TRACER);

    for (u32 i = 0; i < 16; i++)
        CHECK(entity::add_component(w, entity::spawn(w, "shot"), tracer) != NULL);

    test::mute();
    CHECK(entity::add_component(w, entity::spawn(w, "one too many"), tracer) == NULL);
    CHECK(test::unmute() == 1);
}

test::Suite entity_suite() {
    static const test::Case cases[] = {
        CASE(defaults_come_from_the_table),
        CASE(fields_read_every_type),
        CASE(bad_values_warn_and_keep_defaults),
        CASE(inline_entity_round_trips),
        CASE(ids_are_unique_and_kept_by_the_file),
        CASE(owned_ids_come_back_the_same),
        CASE(quaternion_rotation_is_written_as_four_values),
        CASE(reference_applies_overrides),
        CASE(definition_children_are_owned),
        CASE(the_project_folder_is_listed),
        CASE(a_saved_definition_is_reloaded_into_its_instances),
        CASE(the_clip_table_plays_by_name),
        CASE(missing_model_falls_back_and_is_cached),
        CASE(a_declared_component_is_laid_out_and_registered),
        CASE(a_data_component_reads_writes_and_defaults),
        CASE(a_bad_declaration_is_refused),
        CASE(an_emptied_string_default_round_trips),
        CASE(an_unnamed_component_takes_the_default_capacity),
    };
    return test::Suite{.name = "entity", .cases = cases, .count = ARRAY_COUNT(cases)};
}
