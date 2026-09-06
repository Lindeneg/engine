#include "project/project.h"

#include <string.h>

#include "platform/file.h"
#include "platform/path.h"
#include "suites.h"

// made under the engine folder, and removed again; it doubles as the "engine
// root" for the recent list so that file lands inside it too
#define TEST_PROJECT "zz_project_test"

static const char *test_dir(memory::Arena *a) { return FPATH(a, CL_ENGINE_ROOT, TEST_PROJECT); }

static void a_project_is_created_from_the_template(memory::Arena *a) {
    const char *dir = test_dir(a);
    CHECK(!project::is_project(dir));

    project::Project p;
    CHECK(project::create(a, a, CL_ENGINE_ROOT, CL_ENGINE_ROOT, TEST_PROJECT, &p) == result::OK);
    CHECK(project::is_project(dir));
    CHECK(strcmp(p.name, TEST_PROJECT) == 0);
    CHECK(strcmp(p.title, TEST_PROJECT) == 0);
    CHECK(strcmp(p.level, "main") == 0);
    CHECK(config::find(p.doc->first, "world") != NULL);

    // the template's files and the empty folders a project keeps
    CHECK(file::exists(FPATH(a, dir, "levels", "main.level")));
    CHECK(file::exists(FPATH(a, dir, "entities", "templates", "camera3d.entity")));
    CHECK(file::exists(FPATH(a, dir, PROJECT_INPUT_CFG)));

    file::Entry entries[16];
    u32 n = file::list(dir, entries, 16);
    CHECK(n == 7);
    CHECK(strcmp(entries[0].name, "animations") == 0 && entries[0].is_directory);
    CHECK(strcmp(entries[1].name, "entities") == 0);
    CHECK(strcmp(entries[2].name, "input.cfg") == 0 && !entries[2].is_directory);
    CHECK(strcmp(entries[5].name, "project.cfg") == 0);
    CHECK(strcmp(entries[6].name, "scripts") == 0 && entries[6].is_directory);
    CHECK(strcmp(p.script, PROJECT_DEFAULT_SCRIPT) == 0);
    CHECK(file::exists(FPATH(a, dir, p.script)));

    // a second create over it is refused
    test::mute();
    project::Project again;
    CHECK(project::create(a, a, CL_ENGINE_ROOT, CL_ENGINE_ROOT, TEST_PROJECT, &again) ==
          result::ERR_INVALID);
    CHECK(test::unmute() == 1);

    // a change saved is a change read back, and the blocks stay
    snprintf(p.title, sizeof(p.title), "A Title");
    snprintf(p.level, sizeof(p.level), "arena");
    CHECK(project::save(a, &p) == result::OK);

    project::Project back;
    CHECK(project::open(a, dir, &back) == result::OK);
    CHECK(strcmp(back.title, "A Title") == 0);
    CHECK(strcmp(back.level, "arena") == 0);
    CHECK(config::find(back.doc->first, "world") != NULL);

    CHECK(file::remove_directory(dir));
    CHECK(!project::is_project(dir));
}

static void names_that_are_paths_are_refused(memory::Arena *a) {
    project::Project p;
    test::mute();
    CHECK(project::create(a, a, CL_ENGINE_ROOT, CL_ENGINE_ROOT, "bad/name", &p) ==
          result::ERR_INVALID);
    CHECK(project::create(a, a, CL_ENGINE_ROOT, CL_ENGINE_ROOT, "", &p) == result::ERR_INVALID);
    CHECK(test::unmute() == 2);

    CHECK(!project::is_project(FPATH(a, CL_ENGINE_ROOT, "fonts")));
    test::mute();
    CHECK(project::open(a, FPATH(a, CL_ENGINE_ROOT, "fonts"), &p) != result::OK);
    CHECK(test::unmute() >= 1);
}

static void recent_projects_keep_order_and_cap(memory::Arena *a) {
    const char *dir = test_dir(a);
    CHECK(file::make_directory(dir));

    const char *found[4];
    CHECK(project::recent(a, dir, found, 4) == 0);

    project::remember(a, dir, "D:/one", 3);
    project::remember(a, dir, "D:/two", 3);
    project::remember(a, dir, "D:/three", 3);
    CHECK(project::recent(a, dir, found, 4) == 3);
    CHECK(strcmp(found[0], "D:/three") == 0 && strcmp(found[2], "D:/one") == 0);

    // reopening moves to the front without a duplicate; a fourth drops the oldest
    project::remember(a, dir, "D:/one", 3);
    CHECK(project::recent(a, dir, found, 4) == 3);
    CHECK(strcmp(found[0], "D:/one") == 0 && strcmp(found[1], "D:/three") == 0);

    project::remember(a, dir, "D:/four", 3);
    CHECK(project::recent(a, dir, found, 4) == 3);
    CHECK(strcmp(found[2], "D:/three") == 0);

    // a folder that stopped being a project is dropped; the others keep their order
    project::forget(a, dir, "D:/one", 3);
    CHECK(project::recent(a, dir, found, 4) == 2);
    CHECK(strcmp(found[0], "D:/four") == 0 && strcmp(found[1], "D:/three") == 0);

    project::forget(a, dir, "D:/never-listed", 3);
    CHECK(project::recent(a, dir, found, 4) == 2);

    CHECK(file::remove_directory(dir));
}

test::Suite project_suite() {
    static const test::Case cases[] = {
        CASE(a_project_is_created_from_the_template),
        CASE(names_that_are_paths_are_refused),
        CASE(recent_projects_keep_order_and_cap),
    };
    return test::Suite{.name = "project", .cases = cases, .count = ARRAY_COUNT(cases)};
}
