#pragma once

#include "asset.h"
#include "common.h"
#include "linalg.h"
#include "memory.h"

namespace anim {

// Writes skel->joint_count joint matrices into out, ready for the skinning
// palette. time wraps on the clip's duration.
//
// Pass clip == NULL for the bind pose. That case must produce identity
// matrices: it is the check that a skeleton's local TRS and its inverse bind
// matrices actually agree with each other.
void sample(memory::Arena *scratch, const asset::Skeleton *skel, const asset::Clip *clip, f32 time,
            linalg::Mat4 *out);

} // namespace anim
