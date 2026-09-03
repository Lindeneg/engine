#include "asset/anim.h"

#include <math.h>
#include <string.h>

#include "suites.h"

#define NEAR(a, b) (fabsf((a) - (b)) < 1e-4f)

static b32 vec3_near(linalg::Vec3 a, linalg::Vec3 b) {
    return NEAR(a.x, b.x) && NEAR(a.y, b.y) && NEAR(a.z, b.z);
}

static const f32 H = 0.70710678f; // sin/cos 45

static linalg::Quat z_rotation(f32 degrees) {
    return linalg::quat_from_euler(linalg::Vec3{0.0f, 0.0f, degrees});
}

// n joints, all parentless, at the bind pose, with identity inverse binds
static asset::Skeleton skeleton(memory::Arena *a, u32 n) {
    asset::Skeleton s{};
    s.joints = push_array(a, asset::Joint, n);
    s.joint_count = n;
    s.root = linalg::mat4_identity();
    for (u32 i = 0; i < n; i++) {
        s.joints[i].parent = -1;
        s.joints[i].bind_r = linalg::quat_identity();
        s.joints[i].bind_s = linalg::Vec3{1.0f, 1.0f, 1.0f};
        s.joints[i].inverse_bind = linalg::mat4_identity();
    }
    return s;
}

static asset::Channel rotation_channel(memory::Arena *a, i32 joint, const f32 *times,
                                       const linalg::Quat *keys, u32 n, asset::Interp interp) {
    asset::Channel c{};
    c.joint = joint;
    c.path = asset::PATH_ROTATION;
    c.interp = interp;
    c.key_count = n;
    c.times = (f32 *)memory::push_bytes(a, times, sizeof(f32) * n);
    c.values = (f32 *)memory::push_bytes(a, keys, sizeof(linalg::Quat) * n);
    return c;
}

static asset::Channel translation_channel(memory::Arena *a, i32 joint, const f32 *times,
                                          const linalg::Vec3 *keys, u32 n) {
    asset::Channel c{};
    c.joint = joint;
    c.path = asset::PATH_TRANSLATION;
    c.interp = asset::INTERP_LINEAR;
    c.key_count = n;
    c.times = (f32 *)memory::push_bytes(a, times, sizeof(f32) * n);
    c.values = (f32 *)memory::push_bytes(a, keys, sizeof(linalg::Vec3) * n);
    return c;
}

static asset::Clip clip_of(memory::Arena *a, asset::Channel ch, f32 duration) {
    asset::Clip c{};
    c.channels = push_array(a, asset::Channel, 1);
    c.channels[0] = ch;
    c.channel_count = 1;
    c.duration = duration;
    c.loop = true;
    return c;
}

// where joint 0's rotation sends +X, from the global transform
static linalg::Vec3 x_axis_of(memory::Arena *a, const asset::Skeleton *s, const asset::Clip *c,
                              f32 t) {
    linalg::Mat4 skin[4];
    linalg::Mat4 global[4];
    anim::sample(a, s, c, t, skin, global);
    return linalg::mat4_mul_dir(global[0], linalg::Vec3{1.0f, 0.0f, 0.0f});
}

static const f32 TWO_KEYS[] = {0.0f, 1.0f};

static void bind_pose_is_identity_when_bind_agrees(memory::Arena *a) {
    asset::Skeleton s = skeleton(a, 2);
    s.joints[0].bind_t = linalg::Vec3{0.0f, 1.0f, 0.0f};
    s.joints[1].parent = 0;
    s.joints[1].bind_t = linalg::Vec3{0.0f, 1.0f, 0.0f};

    // inverse binds that undo exactly the bind globals
    s.joints[0].inverse_bind = linalg::mat4_from_translation(linalg::Vec3{0.0f, -1.0f, 0.0f});
    s.joints[1].inverse_bind = linalg::mat4_from_translation(linalg::Vec3{0.0f, -2.0f, 0.0f});

    linalg::Mat4 skin[2];
    anim::bind_pose(a, &s, skin);

    linalg::Vec3 p{3.0f, 4.0f, 5.0f};
    CHECK(vec3_near(linalg::mat4_mul_point(skin[0], p), p));
    CHECK(vec3_near(linalg::mat4_mul_point(skin[1], p), p));
}

static void keys_interpolate_linearly(memory::Arena *a) {
    asset::Skeleton s = skeleton(a, 1);
    linalg::Quat keys[] = {linalg::quat_identity(), z_rotation(90.0f)};

    asset::Clip linear =
        clip_of(a, rotation_channel(a, 0, TWO_KEYS, keys, 2, asset::INTERP_LINEAR), 1.0f);
    CHECK(vec3_near(x_axis_of(a, &s, &linear, 0.0f), linalg::Vec3{1.0f, 0.0f, 0.0f}));
    CHECK(vec3_near(x_axis_of(a, &s, &linear, 0.5f), linalg::Vec3{H, H, 0.0f}));

    // STEP holds the earlier key until the next one arrives
    asset::Clip step =
        clip_of(a, rotation_channel(a, 0, TWO_KEYS, keys, 2, asset::INTERP_STEP), 1.0f);
    CHECK(vec3_near(x_axis_of(a, &s, &step, 0.5f), linalg::Vec3{1.0f, 0.0f, 0.0f}));
    CHECK(vec3_near(x_axis_of(a, &s, &step, 0.999f), linalg::Vec3{1.0f, 0.0f, 0.0f}));
}

static void time_wraps_on_duration(memory::Arena *a) {
    asset::Skeleton s = skeleton(a, 1);
    linalg::Quat keys[] = {linalg::quat_identity(), z_rotation(90.0f)};
    asset::Clip c =
        clip_of(a, rotation_channel(a, 0, TWO_KEYS, keys, 2, asset::INTERP_LINEAR), 1.0f);

    CHECK(vec3_near(x_axis_of(a, &s, &c, 1.5f), x_axis_of(a, &s, &c, 0.5f)));
    CHECK(vec3_near(x_axis_of(a, &s, &c, 1.0f), x_axis_of(a, &s, &c, 0.0f)));

    // negative time wraps forward: -0.25 is 0.75. Compared against the positive
    // sample, since nlerp is not constant velocity.
    CHECK(vec3_near(x_axis_of(a, &s, &c, -0.25f), x_axis_of(a, &s, &c, 0.75f)));
    CHECK(!vec3_near(x_axis_of(a, &s, &c, 0.75f), x_axis_of(a, &s, &c, 0.25f)));

    // without looping, time holds at either end instead
    c.loop = false;
    CHECK(vec3_near(x_axis_of(a, &s, &c, 1.5f), x_axis_of(a, &s, &c, 1.0f)));
    CHECK(vec3_near(x_axis_of(a, &s, &c, 1.0f), linalg::Vec3{0.0f, 1.0f, 0.0f}));
    CHECK(vec3_near(x_axis_of(a, &s, &c, -0.25f), x_axis_of(a, &s, &c, 0.0f)));
}

// q and -q are the same rotation; blending toward the far representation
// must not swing the long way round
static void rotation_keys_take_the_short_path(memory::Arena *a) {
    asset::Skeleton s = skeleton(a, 1);
    linalg::Quat keys[] = {linalg::quat_identity(), linalg::quat_neg(z_rotation(90.0f))};
    asset::Clip c =
        clip_of(a, rotation_channel(a, 0, TWO_KEYS, keys, 2, asset::INTERP_LINEAR), 1.0f);

    CHECK(vec3_near(x_axis_of(a, &s, &c, 0.5f), linalg::Vec3{H, H, 0.0f}));
}

// rotation_only keeps our bone lengths against a clip carrying another rig's
static void rotation_only_keeps_the_skeletons_bones(memory::Arena *a) {
    asset::Skeleton s = skeleton(a, 1);
    s.joints[0].bind_t = linalg::Vec3{0.0f, 1.0f, 0.0f};

    linalg::Vec3 far_away[] = {{5.0f, 5.0f, 5.0f}, {5.0f, 5.0f, 5.0f}};
    asset::Clip c = clip_of(a, translation_channel(a, 0, TWO_KEYS, far_away, 2), 1.0f);

    linalg::Mat4 skin[1];
    linalg::Mat4 global[1];

    c.rotation_only = false;
    anim::sample(a, &s, &c, 0.5f, skin, global);
    CHECK(vec3_near(linalg::mat4_translation(global[0]), linalg::Vec3{5.0f, 5.0f, 5.0f}));

    c.rotation_only = true;
    anim::sample(a, &s, &c, 0.5f, skin, global);
    CHECK(vec3_near(linalg::mat4_translation(global[0]), linalg::Vec3{0.0f, 1.0f, 0.0f}));
}

// glTF does not order joints parent-first; our soldier lists Spine2 before Spine1
static void parents_resolve_in_any_order(memory::Arena *a) {
    asset::Skeleton s = skeleton(a, 2);
    s.joints[0].parent = 1; // the child comes first
    s.joints[0].bind_t = linalg::Vec3{1.0f, 0.0f, 0.0f};
    s.joints[1].bind_t = linalg::Vec3{0.0f, 1.0f, 0.0f};

    linalg::Mat4 skin[2];
    linalg::Mat4 global[2];
    anim::sample(a, &s, NULL, 0.0f, skin, global);

    CHECK(vec3_near(linalg::mat4_translation(global[1]), linalg::Vec3{0.0f, 1.0f, 0.0f}));
    CHECK(vec3_near(linalg::mat4_translation(global[0]), linalg::Vec3{1.0f, 1.0f, 0.0f}));
}

// the armature node above the skin, which the inverse binds were baked against
static void skeleton_root_sits_above_parentless_joints(memory::Arena *a) {
    asset::Skeleton s = skeleton(a, 1);
    s.root = linalg::mat4_from_translation(linalg::Vec3{0.0f, 10.0f, 0.0f});
    s.joints[0].bind_t = linalg::Vec3{0.0f, 1.0f, 0.0f};

    linalg::Mat4 skin[1];
    linalg::Mat4 global[1];
    anim::sample(a, &s, NULL, 0.0f, skin, global);
    CHECK(vec3_near(linalg::mat4_translation(global[0]), linalg::Vec3{0.0f, 11.0f, 0.0f}));
}

static void zero_duration_and_unbound_channels_play_bind_pose(memory::Arena *a) {
    asset::Skeleton s = skeleton(a, 1);
    linalg::Quat keys[] = {z_rotation(90.0f), z_rotation(90.0f)};

    asset::Clip empty =
        clip_of(a, rotation_channel(a, 0, TWO_KEYS, keys, 2, asset::INTERP_LINEAR), 0.0f);
    CHECK(vec3_near(x_axis_of(a, &s, &empty, 0.5f), linalg::Vec3{1.0f, 0.0f, 0.0f}));

    asset::Clip unbound =
        clip_of(a, rotation_channel(a, -1, TWO_KEYS, keys, 2, asset::INTERP_LINEAR), 1.0f);
    CHECK(vec3_near(x_axis_of(a, &s, &unbound, 0.5f), linalg::Vec3{1.0f, 0.0f, 0.0f}));
}

test::Suite anim_suite() {
    static const test::Case cases[] = {
        CASE(bind_pose_is_identity_when_bind_agrees),
        CASE(keys_interpolate_linearly),
        CASE(time_wraps_on_duration),
        CASE(rotation_keys_take_the_short_path),
        CASE(rotation_only_keeps_the_skeletons_bones),
        CASE(parents_resolve_in_any_order),
        CASE(skeleton_root_sits_above_parentless_joints),
        CASE(zero_duration_and_unbound_channels_play_bind_pose),
    };
    return test::Suite{.name = "anim", .cases = cases, .count = ARRAY_COUNT(cases)};
}
