#pragma once

#include "asset.h"
#include "common.h"
#include "linalg.h"
#include "memory.h"

namespace anim {

// skel->joint_count matrices; time wraps on the clip's duration. clip == NULL
// gives the bind pose, which must come out identity. out_skin receives
// global * inverse_bind, what the shader wants; out_global, optional, the
// joints' model-space transforms, what an attachment wants.
void sample(memory::Arena *scratch, const asset::Skeleton *skel, const asset::Clip *clip, f32 time,
            linalg::Mat4 *out_skin, linalg::Mat4 *out_global);

// The palette with no clip; what bounds are measured against, since raw skinned
// positions are pre-skin.
void bind_pose(memory::Arena *scratch, const asset::Skeleton *skel, linalg::Mat4 *out);

} // namespace anim
