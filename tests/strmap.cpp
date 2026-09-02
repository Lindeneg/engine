#include "strmap.h"
#include "suites.h"

static const char *mk_key(memory::Arena *a, u64 i) {
    return memory::push_fmt(a, "assets/mesh_%llu.glb", (unsigned long long)i);
}

static void basic(memory::Arena *a) {
    strmap::Map m = strmap::create(a, 16);

    CHECK(m.count == 0);
    CHECK(strmap::find(&m, "nothing") == NULL);
    CHECK(!strmap::remove(&m, "nothing"));

    CHECK(strmap::insert(&m, "soldier", 1));
    CHECK(strmap::insert(&m, "pistol", 2));
    CHECK(strmap::insert(&m, "pistol_idle", 3));
    CHECK(m.count == 3);

    u32 *v = strmap::find(&m, "pistol");
    CHECK(v && *v == 2);

    // a prefix of a stored key is not that key
    CHECK(strmap::find(&m, "pisto") == NULL);
    CHECK(strmap::find(&m, "pistol_") == NULL);

    CHECK(strmap::remove(&m, "pistol"));
    CHECK(m.count == 2);
    CHECK(strmap::find(&m, "pistol") == NULL);
    CHECK(strmap::find(&m, "pistol_idle") != NULL);
    CHECK(strmap::find(&m, "soldier") != NULL);
    CHECK(!strmap::remove(&m, "pistol"));
}

static void overwrite(memory::Arena *a) {
    strmap::Map m = strmap::create(a, 16);

    CHECK(strmap::insert(&m, "soldier", 7));
    CHECK(!strmap::insert(&m, "soldier", 9)); // false == overwrote
    CHECK(m.count == 1);

    u32 *v = strmap::find(&m, "soldier");
    CHECK(v && *v == 9);

    *v = 11;
    u32 *v2 = strmap::find(&m, "soldier");
    CHECK(v2 && *v2 == 11);
}

static void clear(memory::Arena *a) {
    strmap::Map m = strmap::create(a, 16);

    for (u64 i = 0; i < 10; i++)
        strmap::insert(&m, mk_key(a, i), (u32)i);
    CHECK(m.count == 10);

    strmap::clear(&m);
    CHECK(m.count == 0);
    for (u64 i = 0; i < 10; i++)
        CHECK(strmap::find(&m, mk_key(a, i)) == NULL);
}

// after every removal, sweep the whole key set: a broken backward-shift orphans
// entries whose probe chain crossed the hole, and only a full sweep sees it
static void delete_integrity(memory::Arena *a, u64 n) {
    ASSERT(n % 37 != 0); // stride below must be coprime with n

    strmap::Map m = strmap::create(a, n);
    const char **keys = push_array(a, const char *, n);
    b8 *gone = push_array(a, b8, n);

    for (u64 i = 0; i < n; i++) {
        keys[i] = mk_key(a, i);
        CHECK(strmap::insert(&m, keys[i], (u32)i));
    }
    CHECK(m.count == n);

    for (u64 step = 0; step < n; step++) {
        u64 idx = (step * 37) % n; // scramble so holes land mid-cluster
        CHECK(strmap::remove(&m, keys[idx]));
        gone[idx] = 1;
        CHECK(m.count == n - step - 1);

        for (u64 i = 0; i < n; i++) {
            u32 *v = strmap::find(&m, keys[i]);
            if (gone[i])
                CHECK(v == NULL);
            else
                CHECK(v && *v == (u32)i);
        }
    }

    CHECK(m.count == 0);

    // a table emptied by removal must be as good as new
    for (u64 i = 0; i < n; i++)
        CHECK(strmap::insert(&m, keys[i], (u32)(i + 1000)));
    for (u64 i = 0; i < n; i++) {
        u32 *v = strmap::find(&m, keys[i]);
        CHECK(v && *v == (u32)(i + 1000));
    }
}

// sizes just under a power-of-two bucket run at ~70% load with long clusters
static void delete_integrity_5(memory::Arena *a) { delete_integrity(a, 5); }
static void delete_integrity_178(memory::Arena *a) { delete_integrity(a, 178); }
static void delete_integrity_716(memory::Arena *a) { delete_integrity(a, 716); }

test::Suite strmap_suite() {
    static const test::Case cases[] = {
        CASE(basic),
        CASE(overwrite),
        CASE(clear),
        CASE(delete_integrity_5),
        CASE(delete_integrity_178),
        CASE(delete_integrity_716),
    };
    return test::Suite{.name = "strmap", .cases = cases, .count = ARRAY_COUNT(cases)};
}
