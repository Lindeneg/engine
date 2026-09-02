#include <math.h>

#include "linalg.h"
#include "suites.h"

#define NEAR(a, b) (fabsf((a) - (b)) < 1e-4f)

static b32 vec3_near(linalg::Vec3 a, linalg::Vec3 b) {
    return NEAR(a.x, b.x) && NEAR(a.y, b.y) && NEAR(a.z, b.z);
}

static linalg::Vec3 rotate(linalg::Quat q, linalg::Vec3 v) {
    return linalg::mat4_mul_point(linalg::mat4_from_quat(q), v);
}

static void euler_is_degrees_about_xyz(memory::Arena *) {
    linalg::Vec3 x{1.0f, 0.0f, 0.0f};
    linalg::Vec3 y{0.0f, 1.0f, 0.0f};
    linalg::Vec3 z{0.0f, 0.0f, 1.0f};

    CHECK(rotate(linalg::quat_from_euler(linalg::Vec3{0, 0, 0}), x).x == 1.0f);

    // right-handed, +Y up, camera down -Z: 90 about X sends +Y to +Z
    CHECK(vec3_near(rotate(linalg::quat_from_euler(linalg::Vec3{90, 0, 0}), y), z));

    // 90 about Y sends +Z to +X
    CHECK(vec3_near(rotate(linalg::quat_from_euler(linalg::Vec3{0, 90, 0}), z), x));

    // 90 about Z sends +X to +Y
    CHECK(vec3_near(rotate(linalg::quat_from_euler(linalg::Vec3{0, 0, 90}), x), y));
}

// the order is X, then Y, then Z -- swapping it is invisible until something
// points the wrong way, and then it looks like a broken asset
static void euler_applies_x_then_y_then_z(memory::Arena *) {
    linalg::Quat q = linalg::quat_from_euler(linalg::Vec3{90, 90, 0});

    // +Z --X90--> -Y --Y90--> -Y. The Y rotation leaves the Y axis alone, so
    // the giveaway is what happens to +Y instead.
    CHECK(vec3_near(rotate(q, linalg::Vec3{0, 0, 1}), linalg::Vec3{0, -1, 0}));

    // +Y --X90--> +Z --Y90--> +X
    CHECK(vec3_near(rotate(q, linalg::Vec3{0, 1, 0}), linalg::Vec3{1, 0, 0}));
}

static void euler_produces_unit_quaternions(memory::Arena *) {
    const f32 angles[] = {-540.0f, -90.0f, 0.0f, 37.0f, 180.0f, 720.0f};

    for (u32 i = 0; i < ARRAY_COUNT(angles); i++) {
        for (u32 j = 0; j < ARRAY_COUNT(angles); j++) {
            linalg::Quat q =
                linalg::quat_from_euler(linalg::Vec3{angles[i], angles[j], angles[(i + j) % 6]});
            CHECK(NEAR(sqrtf(linalg::quat_dot(q, q)), 1.0f));
        }
    }
}

// Why the config layer normalizes. mat4_from_quat simplifies using
// w^2+x^2+y^2+z^2 == 1, so a non-unit quaternion is not a scaled rotation --
// it is a non-orthogonal matrix that stretches each axis differently and
// shears. This is what "rotation 2 0 0 1" did to a pistol.
static void non_unit_quaternions_are_not_rotations(memory::Arena *) {
    linalg::Quat bad{2.0f, 0.0f, 0.0f, 1.0f};

    // the rotation axis survives untouched, which is why it does not read as a
    // uniform scale
    CHECK(NEAR(linalg::vec3_length(rotate(bad, linalg::Vec3{1, 0, 0})), 1.0f));

    // while the other two axes blow out, by the same factor as each other but
    // not as X -- non-uniform, and sheared off their own axes
    f32 ly = linalg::vec3_length(rotate(bad, linalg::Vec3{0, 1, 0}));
    f32 lz = linalg::vec3_length(rotate(bad, linalg::Vec3{0, 0, 1}));
    CHECK(NEAR(ly, sqrtf(65.0f))); // (0, -7, 4)
    CHECK(NEAR(lz, sqrtf(65.0f))); // (0, -4, -7)

    // normalizing puts all three back to unit length
    linalg::Quat good = linalg::quat_normalize(bad);
    CHECK(NEAR(linalg::vec3_length(rotate(good, linalg::Vec3{1, 0, 0})), 1.0f));
    CHECK(NEAR(linalg::vec3_length(rotate(good, linalg::Vec3{0, 1, 0})), 1.0f));
    CHECK(NEAR(linalg::vec3_length(rotate(good, linalg::Vec3{0, 0, 1})), 1.0f));
}

// mat3(model) is only correct under uniform scale; the whole reason the
// renderer carries a separate normal matrix
static void normal_matrix_survives_non_uniform_scale(memory::Arena *) {
    linalg::Mat4 m = linalg::mat4_from_trs(linalg::Vec3{0, 0, 0}, linalg::quat_identity(),
                                           linalg::Vec3{1.0f, 4.0f, 1.0f});
    linalg::Mat3 n = linalg::mat3_normal_matrix(m);

    // a surface stretched 4x in Y has its normal squashed 4x in Y, not stretched
    CHECK(NEAR(n.m[0], 4.0f)); // column 0, x
    CHECK(NEAR(n.m[4], 1.0f)); // column 1, y
    CHECK(NEAR(n.m[8], 4.0f)); // column 2, z

    // a singular matrix returns the plain basis rather than NaNs
    linalg::Mat4 flat = linalg::mat4_from_trs(linalg::Vec3{0, 0, 0}, linalg::quat_identity(),
                                              linalg::Vec3{1.0f, 0.0f, 1.0f});
    linalg::Mat3 degenerate = linalg::mat3_normal_matrix(flat);
    CHECK(degenerate.m[0] == 1.0f);
    CHECK(degenerate.m[4] == 0.0f);
}

// cofactor(M) = det(M) * inverse-transpose. Without the sign of det, a model
// mirrored in one axis -- a left-hand weapon -- lights from the inside.
static void normal_matrix_survives_mirrored_scale(memory::Arena *) {
    linalg::Mat4 m = linalg::mat4_from_trs(linalg::Vec3{0, 0, 0}, linalg::quat_identity(),
                                           linalg::Vec3{-1.0f, 1.0f, 1.0f});
    linalg::Mat3 n = linalg::mat3_normal_matrix(m);

    // a normal along +X on a model mirrored in X points along -X, and the
    // other two axes are untouched
    CHECK(NEAR(n.m[0], -1.0f));
    CHECK(NEAR(n.m[4], 1.0f));
    CHECK(NEAR(n.m[8], 1.0f));
}

// A camera pitched straight up or down is gameplay, not a bug: forward
// parallel to up must produce a rotation, not an assert.
static void look_along_up_does_not_degenerate(memory::Arena *) {
    linalg::Vec3 up{0, 1, 0};
    linalg::Vec3 fwd{0, 0, -1}; // what -Z of a rotated frame is

    linalg::Quat q = linalg::quat_look_rotation(up, up);
    CHECK(NEAR(sqrtf(linalg::quat_dot(q, q)), 1.0f));
    CHECK(vec3_near(rotate(q, fwd), up));

    linalg::Quat d = linalg::quat_look_rotation(linalg::vec3_neg(up), up);
    CHECK(vec3_near(rotate(d, fwd), linalg::vec3_neg(up)));

    // mat4_look_at shares the basis: a target straight above lands down -Z
    linalg::Mat4 v = linalg::mat4_look_at(linalg::Vec3{0, 0, 0}, linalg::Vec3{0, 5, 0}, up);
    linalg::Vec4 t = v * linalg::Vec4{0, 5, 0, 1};
    CHECK(vec3_near(linalg::Vec3{t.x, t.y, t.z}, linalg::Vec3{0, 0, -5}));
}

test::Suite linalg_suite() {
    static const test::Case cases[] = {
        CASE(euler_is_degrees_about_xyz),
        CASE(euler_applies_x_then_y_then_z),
        CASE(euler_produces_unit_quaternions),
        CASE(non_unit_quaternions_are_not_rotations),
        CASE(normal_matrix_survives_non_uniform_scale),
        CASE(normal_matrix_survives_mirrored_scale),
        CASE(look_along_up_does_not_degenerate),
    };
    return test::Suite{.name = "linalg", .cases = cases, .count = ARRAY_COUNT(cases)};
}
