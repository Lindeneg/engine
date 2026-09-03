#pragma once

// Conventions match glTF and OpenGL exactly so data moves disk -> RAM -> GPU untouched:
//   - Mat4 is column-major, m[col * 4 + row]; upload with glUniformMatrix4fv(..., GL_FALSE, ...)
//   - column vectors, v' = M * v, composition reads right-to-left: proj * view * model
//   - right-handed, +Y up, camera looks down -Z, clip-space depth -1..1
//   - Quat is {x, y, z, w}, same component order as glTF rotation accessors
//   - angles are radians everywhere; DEG2RAD only at call sites

#include <math.h> // IWYU pragma: keep

#include "core/common.h"

#define PI_F32 3.14159265358979323846f
#define DEG2RAD(d) ((d) * (PI_F32 / 180.0f))
#define RAD2DEG(r) ((r) * (180.0f / PI_F32))

// Three thresholds for three questions:
//   ZERO      is this length zero? normalize refuses below it
//   PARALLEL  do these directions coincide? a cross product's length is
//             sin(angle), and a side axis built from it is garbage long before ZERO
//   DET       is this 3x3 singular? a determinant is a product of three scales,
//             so a model imported at 0.001 (det ~1e-9) must not count as flat
#define LINALG_ZERO_EPSILON 1e-8f
#define LINALG_PARALLEL_EPSILON 1e-4f
#define LINALG_DET_EPSILON 1e-12f

namespace linalg {

struct Vec2 {
    f32 x, y;
};

struct Vec3 {
    f32 x, y, z;
};

struct Vec4 {
    f32 x, y, z, w;
};

struct Quat {
    f32 x, y, z, w;
};

struct Mat3 {
    f32 m[9]; // column-major, m[col * 3 + row], matching Mat4
};

struct Mat4 {
    f32 m[16];
};

static inline f32 f32_lerp(f32 a, f32 b, f32 t) { return a + (b - a) * t; }

static inline Vec2 vec2_add(Vec2 a, Vec2 b) { return Vec2{a.x + b.x, a.y + b.y}; }
static inline Vec2 vec2_sub(Vec2 a, Vec2 b) { return Vec2{a.x - b.x, a.y - b.y}; }
static inline Vec2 vec2_scale(Vec2 v, f32 s) { return Vec2{v.x * s, v.y * s}; }

static inline Vec3 vec3_add(Vec3 a, Vec3 b) { return Vec3{a.x + b.x, a.y + b.y, a.z + b.z}; }
static inline Vec3 vec3_sub(Vec3 a, Vec3 b) { return Vec3{a.x - b.x, a.y - b.y, a.z - b.z}; }
static inline Vec3 vec3_scale(Vec3 v, f32 s) { return Vec3{v.x * s, v.y * s, v.z * s}; }
static inline Vec3 vec3_neg(Vec3 v) { return Vec3{-v.x, -v.y, -v.z}; }

static inline f32 vec3_dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

static inline Vec3 vec3_cross(Vec3 a, Vec3 b) {
    return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

static inline f32 vec3_length_sq(Vec3 v) { return vec3_dot(v, v); }
static inline f32 vec3_length(Vec3 v) { return sqrtf(vec3_length_sq(v)); }

static inline Vec3 vec3_normalize(Vec3 v) {
    f32 len = vec3_length(v);
    ASSERT_MSG(len > LINALG_ZERO_EPSILON, "normalizing near-zero vector");
    return vec3_scale(v, 1.0f / len);
}

// for data that may legitimately be zero (a velocity); vec3_normalize asserts instead
static inline Vec3 vec3_normalize_or(Vec3 v, Vec3 fallback) {
    f32 len = vec3_length(v);
    return len > LINALG_ZERO_EPSILON ? vec3_scale(v, 1.0f / len) : fallback;
}

static inline Vec3 vec3_lerp(Vec3 a, Vec3 b, f32 t) {
    return Vec3{f32_lerp(a.x, b.x, t), f32_lerp(a.y, b.y, t), f32_lerp(a.z, b.z, t)};
}

static inline Vec4 vec4_add(Vec4 a, Vec4 b) {
    return Vec4{a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
}

static inline Vec4 vec4_sub(Vec4 a, Vec4 b) {
    return Vec4{a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w};
}

static inline Vec4 vec4_scale(Vec4 v, f32 s) { return Vec4{v.x * s, v.y * s, v.z * s, v.w * s}; }

static inline f32 vec4_dot(Vec4 a, Vec4 b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }

static inline Vec4 vec4_from(Vec3 v, f32 w) { return Vec4{v.x, v.y, v.z, w}; }
static inline Vec3 vec3_from(Vec4 v) { return Vec3{v.x, v.y, v.z}; }

static inline Quat quat_identity() { return Quat{0.0f, 0.0f, 0.0f, 1.0f}; }

static inline f32 quat_dot(Quat a, Quat b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }

static inline Quat quat_neg(Quat q) { return Quat{-q.x, -q.y, -q.z, -q.w}; }

// a * b applies b first, matching mat4_mul composition order
static inline Quat quat_mul(Quat a, Quat b) {
    return Quat{
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

static inline Quat quat_normalize(Quat q) {
    f32 len = sqrtf(quat_dot(q, q));
    ASSERT_MSG(len > LINALG_ZERO_EPSILON, "normalizing near-zero quaternion");
    f32 inv = 1.0f / len;
    return Quat{q.x * inv, q.y * inv, q.z * inv, q.w * inv};
}

// about X, then Y, then Z, in degrees
static inline Quat quat_from_euler(Vec3 degrees) {
    f32 hx = DEG2RAD(degrees.x) * 0.5f;
    f32 hy = DEG2RAD(degrees.y) * 0.5f;
    f32 hz = DEG2RAD(degrees.z) * 0.5f;

    Quat qx{sinf(hx), 0.0f, 0.0f, cosf(hx)};
    Quat qy{0.0f, sinf(hy), 0.0f, cosf(hy)};
    Quat qz{0.0f, 0.0f, sinf(hz), cosf(hz)};

    // quat_mul applies its right operand first, so this is X then Y then Z
    return quat_mul(quat_mul(qz, qy), qx);
}

// v + 2 * cross(q.xyz, cross(q.xyz, v) + q.w * v); cheaper than building a
// matrix for one vector, and it does not care whether q is exactly unit.
static inline Vec3 quat_rotate(Quat q, Vec3 v) {
    Vec3 u{q.x, q.y, q.z};
    Vec3 t = vec3_add(vec3_cross(u, v), vec3_scale(v, q.w));
    return vec3_add(v, vec3_scale(vec3_cross(u, t), 2.0f));
}

// right-handed (side, up, forward) basis along forward. When forward is parallel
// to up (a camera pitched straight up), the hint is replaced by the world axis
// furthest from forward.
static inline void look_basis(Vec3 forward, Vec3 up, Vec3 *out_s, Vec3 *out_u, Vec3 *out_f) {
    Vec3 f = vec3_normalize(forward);
    Vec3 s = vec3_cross(f, up);
    if (vec3_length_sq(s) < LINALG_PARALLEL_EPSILON * LINALG_PARALLEL_EPSILON) {
        Vec3 alt = fabsf(f.y) < fabsf(f.z) ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{0.0f, 0.0f, 1.0f};
        s = vec3_cross(f, alt);
    }
    s = vec3_normalize(s);
    *out_s = s;
    *out_u = vec3_cross(s, f);
    *out_f = f;
}

// rotation whose -Z is forward, matching mat4_look_at. Shepperd's method:
// branch on the largest component, since dividing by w loses precision near 180 degrees.
static inline Quat quat_look_rotation(Vec3 forward, Vec3 up) {
    Vec3 s, u, f;
    look_basis(forward, up, &s, &u, &f);

    // basis columns are s, u, -f; m[row][col]
    f32 m00 = s.x, m01 = u.x, m02 = -f.x;
    f32 m10 = s.y, m11 = u.y, m12 = -f.y;
    f32 m20 = s.z, m21 = u.z, m22 = -f.z;

    f32 trace = m00 + m11 + m22;
    if (trace > 0.0f) {
        f32 k = sqrtf(trace + 1.0f) * 2.0f;
        return Quat{(m21 - m12) / k, (m02 - m20) / k, (m10 - m01) / k, 0.25f * k};
    }
    if (m00 > m11 && m00 > m22) {
        f32 k = sqrtf(1.0f + m00 - m11 - m22) * 2.0f;
        return Quat{0.25f * k, (m01 + m10) / k, (m02 + m20) / k, (m21 - m12) / k};
    }
    if (m11 > m22) {
        f32 k = sqrtf(1.0f + m11 - m00 - m22) * 2.0f;
        return Quat{(m01 + m10) / k, 0.25f * k, (m12 + m21) / k, (m02 - m20) / k};
    }
    f32 k = sqrtf(1.0f + m22 - m00 - m11) * 2.0f;
    return Quat{(m02 + m20) / k, (m12 + m21) / k, 0.25f * k, (m10 - m01) / k};
}

// q and -q are the same rotation; lerp through the nearer neighborhood or joints flip mid-blend
static inline Quat quat_nlerp(Quat a, Quat b, f32 t) {
    if (quat_dot(a, b) < 0.0f)
        b = quat_neg(b);
    Quat q{
        f32_lerp(a.x, b.x, t),
        f32_lerp(a.y, b.y, t),
        f32_lerp(a.z, b.z, t),
        f32_lerp(a.w, b.w, t),
    };
    return quat_normalize(q);
}

static inline Mat4 mat4_identity() {
    Mat4 out{};
    out.m[0] = out.m[5] = out.m[10] = out.m[15] = 1.0f;
    return out;
}

static inline Mat4 mat4_mul(Mat4 a, Mat4 b) {
    Mat4 out{};
    for (i32 c = 0; c < 4; ++c) {
        for (i32 r = 0; r < 4; ++r) {
            f32 sum = 0.0f;
            for (i32 k = 0; k < 4; ++k)
                sum += a.m[k * 4 + r] * b.m[c * 4 + k];
            out.m[c * 4 + r] = sum;
        }
    }
    return out;
}

static inline Vec4 mat4_mul_vec4(Mat4 a, Vec4 v) {
    return Vec4{
        a.m[0] * v.x + a.m[4] * v.y + a.m[8] * v.z + a.m[12] * v.w,
        a.m[1] * v.x + a.m[5] * v.y + a.m[9] * v.z + a.m[13] * v.w,
        a.m[2] * v.x + a.m[6] * v.y + a.m[10] * v.z + a.m[14] * v.w,
        a.m[3] * v.x + a.m[7] * v.y + a.m[11] * v.z + a.m[15] * v.w,
    };
}

// Point (w = 1) and direction (w = 0) transforms. Directions come back
// unnormalized: a scaled matrix scales them.
static inline Vec3 mat4_mul_point(Mat4 m, Vec3 p) {
    return vec3_from(mat4_mul_vec4(m, vec4_from(p, 1.0f)));
}
static inline Vec3 mat4_mul_dir(Mat4 m, Vec3 d) {
    return vec3_from(mat4_mul_vec4(m, vec4_from(d, 0.0f)));
}

static inline Vec3 mat4_translation(Mat4 m) { return Vec3{m.m[12], m.m[13], m.m[14]}; }

// Basis column 0..2: the matrix's local X, Y, Z in the parent space, scale included.
static inline Vec3 mat4_axis(Mat4 m, u32 col) {
    ASSERT(col < 3);
    return Vec3{m.m[col * 4 + 0], m.m[col * 4 + 1], m.m[col * 4 + 2]};
}

static inline Mat4 mat4_transpose(Mat4 a) {
    Mat4 out;
    for (i32 c = 0; c < 4; ++c)
        for (i32 r = 0; r < 4; ++r)
            out.m[r * 4 + c] = a.m[c * 4 + r];
    return out;
}

static inline Mat3 mat3_identity() {
    Mat3 out{};
    out.m[0] = out.m[4] = out.m[8] = 1.0f;
    return out;
}

static inline Mat3 mat3_from_mat4(Mat4 a) {
    return Mat3{
        a.m[0], a.m[1], a.m[2],  //
        a.m[4], a.m[5], a.m[6],  //
        a.m[8], a.m[9], a.m[10], //
    };
}

// transpose(inverse(upper 3x3)): plain mat3(model) shears normals under
// non-uniform scale. inverse = adjugate/det and adjugate is the transposed
// cofactor matrix, so this is cofactor(M)/det(M).
static inline Mat3 mat3_normal_matrix(Mat4 src) {
    Mat3 u = mat3_from_mat4(src);

    f32 a = u.m[0], d = u.m[1], g = u.m[2];
    f32 b = u.m[3], e = u.m[4], h = u.m[5];
    f32 c = u.m[6], f = u.m[7], i = u.m[8];

    f32 c00 = e * i - f * h;
    f32 c01 = -(d * i - f * g);
    f32 c02 = d * h - e * g;
    f32 c10 = -(b * i - c * h);
    f32 c11 = a * i - c * g;
    f32 c12 = -(a * h - b * g);
    f32 c20 = b * f - c * e;
    f32 c21 = -(a * f - c * d);
    f32 c22 = a * e - b * d;

    f32 det = a * c00 + b * c01 + c * c02;
    if (det > -LINALG_DET_EPSILON && det < LINALG_DET_EPSILON)
        return u; // degenerate (zero scale); the unmodified basis beats NaNs

    // column-major, so column 0 is (c00, c10, c20); row-wise would store the
    // inverse. |det| is dropped (the shader normalizes) but its sign is not: a
    // mirrored scale would point every normal inward.
    f32 sign = det < 0.0f ? -1.0f : 1.0f;
    return Mat3{
        c00 * sign, c10 * sign, c20 * sign, //
        c01 * sign, c11 * sign, c21 * sign, //
        c02 * sign, c12 * sign, c22 * sign, //
    };
}

static inline Mat4 mat4_from_translation(Vec3 t) {
    Mat4 out = mat4_identity();
    out.m[12] = t.x;
    out.m[13] = t.y;
    out.m[14] = t.z;
    return out;
}

// expects a unit quaternion
static inline Mat4 mat4_from_quat(Quat q) {
    f32 xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    f32 xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    f32 wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;

    Mat4 out{};
    out.m[0] = 1.0f - 2.0f * (yy + zz);
    out.m[1] = 2.0f * (xy + wz);
    out.m[2] = 2.0f * (xz - wy);

    out.m[4] = 2.0f * (xy - wz);
    out.m[5] = 1.0f - 2.0f * (xx + zz);
    out.m[6] = 2.0f * (yz + wx);

    out.m[8] = 2.0f * (xz + wy);
    out.m[9] = 2.0f * (yz - wx);
    out.m[10] = 1.0f - 2.0f * (xx + yy);

    out.m[15] = 1.0f;
    return out;
}

// Inverse of a transform whose bottom row is (0, 0, 0, 1): any TRS, scale
// included. A singular upper 3x3 (a zero scale) gives identity, which beats
// NaNs downstream.
static inline Mat4 mat4_inverse_affine(Mat4 src) {
    f32 a = src.m[0], d = src.m[1], g = src.m[2];
    f32 b = src.m[4], e = src.m[5], h = src.m[6];
    f32 c = src.m[8], f = src.m[9], i = src.m[10];

    f32 c00 = e * i - f * h;
    f32 c01 = -(d * i - f * g);
    f32 c02 = d * h - e * g;
    f32 c10 = -(b * i - c * h);
    f32 c11 = a * i - c * g;
    f32 c12 = -(a * h - b * g);
    f32 c20 = b * f - c * e;
    f32 c21 = -(a * f - c * d);
    f32 c22 = a * e - b * d;

    f32 det = a * c00 + b * c01 + c * c02;
    if (det > -LINALG_DET_EPSILON && det < LINALG_DET_EPSILON)
        return mat4_identity();
    f32 inv = 1.0f / det;

    // the inverse 3x3 is the transposed cofactors over det: column-major, so
    // column 0 holds row 0 of the cofactors
    Mat4 out{};
    out.m[0] = c00 * inv;
    out.m[1] = c01 * inv;
    out.m[2] = c02 * inv;
    out.m[4] = c10 * inv;
    out.m[5] = c11 * inv;
    out.m[6] = c12 * inv;
    out.m[8] = c20 * inv;
    out.m[9] = c21 * inv;
    out.m[10] = c22 * inv;

    Vec3 t = mat4_translation(src);
    out.m[12] = -(out.m[0] * t.x + out.m[4] * t.y + out.m[8] * t.z);
    out.m[13] = -(out.m[1] * t.x + out.m[5] * t.y + out.m[9] * t.z);
    out.m[14] = -(out.m[2] * t.x + out.m[6] * t.y + out.m[10] * t.z);
    out.m[15] = 1.0f;
    return out;
}

// T * R * S — glTF node transform order (scale, then rotate, then translate)
static inline Mat4 mat4_from_trs(Vec3 t, Quat r, Vec3 s) {
    Mat4 out = mat4_from_quat(r);
    out.m[0] *= s.x;
    out.m[1] *= s.x;
    out.m[2] *= s.x;
    out.m[4] *= s.y;
    out.m[5] *= s.y;
    out.m[6] *= s.y;
    out.m[8] *= s.z;
    out.m[9] *= s.z;
    out.m[10] *= s.z;
    out.m[12] = t.x;
    out.m[13] = t.y;
    out.m[14] = t.z;
    return out;
}

// znear/zfar, not near/far — windows.h #defines those away
static inline Mat4 mat4_perspective(f32 fovy, f32 aspect, f32 znear, f32 zfar) {
    ASSERT(aspect > 0.0f);
    ASSERT(zfar > znear && znear > 0.0f);
    f32 f = 1.0f / tanf(fovy * 0.5f);
    Mat4 out{};
    out.m[0] = f / aspect;
    out.m[5] = f;
    out.m[10] = (zfar + znear) / (znear - zfar);
    out.m[11] = -1.0f;
    out.m[14] = (2.0f * zfar * znear) / (znear - zfar);
    return out;
}

// maps the box to clip space; a canvas passes (0, width, height, 0) to put the
// origin at the top left
static inline Mat4 mat4_orthographic(f32 left, f32 right, f32 bottom, f32 top, f32 znear,
                                     f32 zfar) {
    ASSERT(right != left && top != bottom && zfar != znear);
    Mat4 out{};
    out.m[0] = 2.0f / (right - left);
    out.m[5] = 2.0f / (top - bottom);
    out.m[10] = -2.0f / (zfar - znear);
    out.m[12] = -(right + left) / (right - left);
    out.m[13] = -(top + bottom) / (top - bottom);
    out.m[14] = -(zfar + znear) / (zfar - znear);
    out.m[15] = 1.0f;
    return out;
}

// inverse of a camera's world transform, with scale normalized away: a camera
// under a scaled parent would otherwise resize the whole world
static inline Mat4 mat4_view_from_world(Mat4 world) {
    Vec3 x = mat4_axis(world, 0);
    Vec3 y = mat4_axis(world, 1);
    Vec3 z = mat4_axis(world, 2);
    Vec3 t = mat4_translation(world);

    f32 lx = vec3_length(x);
    f32 ly = vec3_length(y);
    f32 lz = vec3_length(z);
    if (lx < LINALG_ZERO_EPSILON || ly < LINALG_ZERO_EPSILON || lz < LINALG_ZERO_EPSILON)
        return mat4_identity(); // degenerate basis; identity beats NaNs on screen

    x = vec3_scale(x, 1.0f / lx);
    y = vec3_scale(y, 1.0f / ly);
    z = vec3_scale(z, 1.0f / lz);

    // the inverse of an orthonormal basis is its transpose
    Mat4 out{};
    out.m[0] = x.x;
    out.m[4] = x.y;
    out.m[8] = x.z;
    out.m[1] = y.x;
    out.m[5] = y.y;
    out.m[9] = y.z;
    out.m[2] = z.x;
    out.m[6] = z.y;
    out.m[10] = z.z;
    out.m[12] = -vec3_dot(x, t);
    out.m[13] = -vec3_dot(y, t);
    out.m[14] = -vec3_dot(z, t);
    out.m[15] = 1.0f;
    return out;
}

static inline Mat4 mat4_look_at(Vec3 eye, Vec3 target, Vec3 up) {
    Vec3 s, u, f;
    look_basis(vec3_sub(target, eye), up, &s, &u, &f);

    Mat4 out{};
    out.m[0] = s.x;
    out.m[4] = s.y;
    out.m[8] = s.z;
    out.m[1] = u.x;
    out.m[5] = u.y;
    out.m[9] = u.z;
    out.m[2] = -f.x;
    out.m[6] = -f.y;
    out.m[10] = -f.z;
    out.m[12] = -vec3_dot(s, eye);
    out.m[13] = -vec3_dot(u, eye);
    out.m[14] = vec3_dot(f, eye);
    out.m[15] = 1.0f;
    return out;
}

static inline Vec2 operator+(Vec2 a, Vec2 b) { return vec2_add(a, b); }
static inline Vec2 operator-(Vec2 a, Vec2 b) { return vec2_sub(a, b); }
static inline Vec2 operator*(Vec2 v, f32 s) { return vec2_scale(v, s); }
static inline Vec2 operator*(f32 s, Vec2 v) { return vec2_scale(v, s); }

static inline Vec3 operator+(Vec3 a, Vec3 b) { return vec3_add(a, b); }
static inline Vec3 operator-(Vec3 a, Vec3 b) { return vec3_sub(a, b); }
static inline Vec3 operator-(Vec3 v) { return vec3_neg(v); }
static inline Vec3 operator*(Vec3 v, f32 s) { return vec3_scale(v, s); }
static inline Vec3 operator*(f32 s, Vec3 v) { return vec3_scale(v, s); }

static inline Vec4 operator+(Vec4 a, Vec4 b) { return vec4_add(a, b); }
static inline Vec4 operator-(Vec4 a, Vec4 b) { return vec4_sub(a, b); }
static inline Vec4 operator*(Vec4 v, f32 s) { return vec4_scale(v, s); }
static inline Vec4 operator*(f32 s, Vec4 v) { return vec4_scale(v, s); }

static inline Quat operator*(Quat a, Quat b) { return quat_mul(a, b); }

static inline Mat4 operator*(Mat4 a, Mat4 b) { return mat4_mul(a, b); }
static inline Vec4 operator*(Mat4 a, Vec4 v) { return mat4_mul_vec4(a, v); }

} // namespace linalg
