#include "common.h"
#include "logger.h"
#include "memory.h"
#include "strmap.h"

static i32 g_checks = 0;
static i32 g_fails = 0;

#define CHECK(expr)                                                                                \
    do {                                                                                           \
        g_checks++;                                                                                \
        if (!(expr)) {                                                                             \
            if (++g_fails <= 20)                                                                   \
                LOG_ERR("FAIL %s:%d  %s", __FILE__, __LINE__, #expr);                              \
        }                                                                                          \
    } while (0)

static const char *mk_key(memory::Arena *a, u64 i) {
    char *buf = push_array(a, char, 32);
    snprintf(buf, 32, "assets/mesh_%llu.glb", (unsigned long long)i);
    return buf;
}

static void test_basic(memory::Arena *a) {
    memory::ArenaTemp t = memory::temp_begin(a);
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
    CHECK(strmap::find(&m, "pisto") == NULL);
    CHECK(strmap::find(&m, "pistol_") == NULL);

    CHECK(strmap::remove(&m, "pistol"));
    CHECK(m.count == 2);
    CHECK(strmap::find(&m, "pistol") == NULL);
    CHECK(strmap::find(&m, "pistol_idle") != NULL);
    CHECK(strmap::find(&m, "soldier") != NULL);
    CHECK(!strmap::remove(&m, "pistol"));

    memory::temp_end(t);
}

static void test_overwrite(memory::Arena *a) {
    memory::ArenaTemp t = memory::temp_begin(a);
    strmap::Map m = strmap::create(a, 16);

    CHECK(strmap::insert(&m, "soldier", 7));
    CHECK(!strmap::insert(&m, "soldier", 9)); // false == overwrote
    CHECK(m.count == 1);

    u32 *v = strmap::find(&m, "soldier");
    CHECK(v && *v == 9);

    *v = 11;
    u32 *v2 = strmap::find(&m, "soldier");
    CHECK(v2 && *v2 == 11);

    memory::temp_end(t);
}

static void test_clear(memory::Arena *a) {
    memory::ArenaTemp t = memory::temp_begin(a);
    strmap::Map m = strmap::create(a, 16);

    for (u64 i = 0; i < 10; i++)
        strmap::insert(&m, mk_key(a, i), (u32)i);
    CHECK(m.count == 10);

    strmap::clear(&m);
    CHECK(m.count == 0);
    for (u64 i = 0; i < 10; i++)
        CHECK(strmap::find(&m, mk_key(a, i)) == NULL);

    memory::temp_end(t);
}

// The one that matters: after every single removal, sweep the whole key set and
// confirm nothing living became unreachable. A broken backward-shift orphans
// entries whose probe chain crossed the hole, and only a full sweep sees it.
static void test_delete_integrity(memory::Arena *a, u64 n) {
    ASSERT(n % 37 != 0); // stride below must be coprime with n

    memory::ArenaTemp t = memory::temp_begin(a);
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

    LOG_INFO("strmap: delete integrity n=%llu ok (cap %llu)", (unsigned long long)n,
             (unsigned long long)m.cap);
    memory::temp_end(t);
}

int main() {
    logger::init(NULL);
    logger::set_level(logger::TRACE);

    memory::init(MiB(64));
    memory::Arena arena = memory::create(MiB(16));

    test_basic(&arena);
    test_overwrite(&arena);
    test_clear(&arena);

    // sizes chosen to sit just under a power-of-two bucket, so the table runs
    // at ~70% load and clusters are long
    test_delete_integrity(&arena, 5);   // cap 8
    test_delete_integrity(&arena, 178); // cap 256
    test_delete_integrity(&arena, 716); // cap 1024

#if CL_MEM_STATS
    memory::log_stats(&arena, "test");
#endif

    if (g_fails)
        LOG_ERR("strmap: %d/%d checks FAILED", g_fails, g_checks);
    else
        LOG_INFO("strmap: all %d checks passed", g_checks);

    logger::shutdown();
    return g_fails ? EXIT_FAILURE : EXIT_SUCCESS;
}
