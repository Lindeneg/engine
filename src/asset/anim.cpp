#include "asset/anim.h"

#include <math.h>
#include <string.h>

namespace anim {

#define MIN_KEY_SPAN 1e-8f

struct Trs {
    linalg::Vec3 t;
    linalg::Quat r;
    linalg::Vec3 s;
};

static u32 lower_key(const f32 *times, u32 count, f32 t) {
    u32 lo = 0;
    u32 hi = count - 1;
    while (lo < hi) {
        u32 mid = (lo + hi + 1) / 2;
        if (times[mid] <= t)
            lo = mid;
        else
            hi = mid - 1;
    }
    return lo;
}

static void sample_channel(const asset::Channel *ch, f32 t, Trs *local) {
    if (ch->key_count == 0)
        return;

    u32 comps = ch->path == asset::PATH_ROTATION ? 4 : 3;
    u32 i0 = lower_key(ch->times, ch->key_count, t);
    u32 i1 = MIN(i0 + 1, ch->key_count - 1);

    f32 span = ch->times[i1] - ch->times[i0];
    f32 a = span > MIN_KEY_SPAN ? (t - ch->times[i0]) / span : 0.0f;
    a = MAX(0.0f, MIN(1.0f, a));
    if (ch->interp == asset::INTERP_STEP)
        a = 0.0f;

    const f32 *v0 = &ch->values[(u64)i0 * comps];
    const f32 *v1 = &ch->values[(u64)i1 * comps];

    switch (ch->path) {
    case asset::PATH_TRANSLATION:
        local->t = linalg::vec3_lerp(linalg::Vec3{v0[0], v0[1], v0[2]},
                                     linalg::Vec3{v1[0], v1[1], v1[2]}, a);
        break;
    case asset::PATH_SCALE:
        local->s = linalg::vec3_lerp(linalg::Vec3{v0[0], v0[1], v0[2]},
                                     linalg::Vec3{v1[0], v1[1], v1[2]}, a);
        break;
    case asset::PATH_ROTATION:
        local->r = linalg::quat_nlerp(linalg::Quat{v0[0], v0[1], v0[2], v0[3]},
                                      linalg::Quat{v1[0], v1[1], v1[2], v1[3]}, a);
        break;
    }
}

static void resolve(const asset::Skeleton *skel, const Trs *locals, linalg::Mat4 *global, b8 *done,
                    i32 j) {
    if (done[j])
        return;
    done[j] = 1;

    linalg::Mat4 local = linalg::mat4_from_trs(locals[j].t, locals[j].r, locals[j].s);
    i32 p = skel->joints[j].parent;

    if (p < 0 || p >= (i32)skel->joint_count) {
        // a parentless joint still sits under the armature the inverse binds were baked against
        global[j] = skel->root * local;
        return;
    }

    resolve(skel, locals, global, done, p);
    global[j] = global[p] * local;
}

void sample(memory::Arena *scratch, const asset::Skeleton *skel, const asset::Clip *clip, f32 time,
            linalg::Mat4 *out_skin, linalg::Mat4 *out_global) {
    ASSERT(scratch && skel && out_skin);

    u32 n = skel->joint_count;
    if (n == 0)
        return;

    memory::ArenaTemp tmp = memory::temp_begin(scratch);

    Trs *locals = push_array(scratch, Trs, n);
    linalg::Mat4 *global = push_array(scratch, linalg::Mat4, n);
    b8 *done = push_array(scratch, b8, n);

    for (u32 j = 0; j < n; j++)
        locals[j] = Trs{skel->joints[j].bind_t, skel->joints[j].bind_r, skel->joints[j].bind_s};

    if (clip && clip->duration > 0.0f) {
        f32 t = fmodf(time, clip->duration);
        if (t < 0.0f)
            t += clip->duration;

        for (u32 c = 0; c < clip->channel_count; c++) {
            const asset::Channel *ch = &clip->channels[c];
            if (ch->joint < 0 || ch->joint >= (i32)n)
                continue; // this rig has no such joint
            if (clip->rotation_only && ch->path != asset::PATH_ROTATION)
                continue; // retargeted: keep this skeleton's bone lengths
            sample_channel(ch, t, &locals[ch->joint]);
        }
    }

    for (u32 j = 0; j < n; j++)
        resolve(skel, locals, global, done, (i32)j);

    for (u32 j = 0; j < n; j++)
        out_skin[j] = global[j] * skel->joints[j].inverse_bind;

    if (out_global)
        memcpy(out_global, global, sizeof(linalg::Mat4) * n);

    memory::temp_end(tmp);
}

void bind_pose(memory::Arena *scratch, const asset::Skeleton *skel, linalg::Mat4 *out) {
    sample(scratch, skel, NULL, 0.0f, out, NULL);
}

} // namespace anim
