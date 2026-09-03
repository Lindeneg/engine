#pragma once

#include "asset/asset.h"
#include "core/common.h"
#include "math/linalg.h"
#include "scene/entity.h"

// Moving a body through the world without moving through the world. This is the
// engine's half of a character controller and deliberately only that half:
// nothing here decides a speed, a gravity, or what a jump is worth. Say where
// you are, how big you are and where you would like to be; it says where you
// ended up and what stopped you.
//
// The body is a sphere. A capsule is the obvious next shape and is only worth
// it alongside step handling, which is its own decision; a sphere already
// answers walls and floors and reports enough for a caller to cancel its own
// velocity against them.
//
// The world is its collider components, each swept in its own rigid frame, so
// a rotated box is exact and a sphere stays a sphere. A level with no
// colliders has no floor: a model is something to look at, not something to
// stand on, and the two are declared separately on purpose.

#define MOTION_MAX_SLIDES 4

// Pushes out of overlapping geometry before each move. More than one because
// leaving one box can enter another; a body that is still inside something
// after this many is wedged rather than merely overlapping.
#define MOTION_MAX_DEPENETRATIONS 4

// Held off every surface, so the next sweep starts outside what it just touched
// rather than exactly on it, where rounding decides the answer.
#define MOTION_SKIN 1e-3f

// A contact steeper than this is a wall to slide along, not a floor to stand on.
#define MOTION_GROUND_NORMAL_Y 0.7f

namespace motion {

struct Result {
    linalg::Vec3 position;

    // stood on something with a shallow enough contact this move
    b32 grounded;

    // the last surface that stopped it, for a caller cancelling its velocity
    b32 hit;
    linalg::Vec3 normal;

    // it began inside geometry and was pushed clear before moving. Normal, not
    // an error: a body ends up overlapping through no fault of its own.
    b32 depenetrated;

    // still inside something after every attempt to push it out -- wedged
    // between two surfaces closer together than the body is wide. The position
    // is the best that could be reached; a caller that cares has to decide what
    // that means, and killing its velocity is usually the least of it.
    b32 stuck;
};

// Sweeps `motion` from `position`, sliding along what it meets, up to
// MOTION_MAX_SLIDES surfaces. `ignore` and everything parented below it are not
// colliders, which is how a body misses its own collider and the gun in its
// hand; entity::NONE collides with everything.
Result move_and_slide(entity::World *w, asset::Manager *m, linalg::Vec3 position, f32 radius,
                      linalg::Vec3 motion, entity::Handle ignore);

} // namespace motion
