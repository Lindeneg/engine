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
// The body is an upright capsule: a radius, a height, and a step it may walk
// up. A height of twice the radius is a sphere. Its position is the bottom
// sphere's centre, so a body of any height stands the same way on a floor.
//
// The world is its collider components, each swept in its own rigid frame, so
// a rotated box is exact and a capsule stays a capsule: in that frame a moving
// capsule against a box is a moving sphere against the box extruded along the
// capsule's axis, which intersect answers exactly. A level with no colliders
// has no floor: a model is something to look at, not something to stand on,
// and the two are declared separately on purpose.
//
// A step is a retry, not a shape. When a wall stops the body, it is tried
// again lifted by up to step_height, moved the same way, and put down; if
// that got further and landed on something to stand on, that is the move.

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

struct Body {
    linalg::Vec3 position; // the bottom sphere's centre
    f32 radius;
    f32 height;      // feet to crown; less than twice the radius is taken as a sphere
    f32 step_height; // the most a wall may rise and still be walked up; 0 walks up nothing
};

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

// Sweeps `motion` from the body's position, sliding along what it meets, up
// to MOTION_MAX_SLIDES surfaces, stepping up where the body allows. `ignore`
// and everything parented below it are not colliders, which is how a body
// misses its own collider and the gun in its hand; entity::NONE collides with
// everything.
Result move_and_slide(entity::World *w, asset::Manager *m, Body body, linalg::Vec3 motion,
                      entity::Handle ignore);

struct RayResult {
    b32 hit;
    entity::Handle entity; // whose collider, when hit
    linalg::Vec3 point;
    linalg::Vec3 normal;
    f32 distance; // metres along the ray
};

// A bullet: the nearest collider along the ray within max_distance, tested
// exactly in each collider's own frame. `dir` need not be unit length; a zero
// direction hits nothing and warns once. `ignore` is as for move_and_slide. A
// ray starting inside a collider hits it at distance 0 with a zero normal,
// which is the intersect convention for "there is no surface to report".
RayResult ray_cast(entity::World *w, linalg::Vec3 origin, linalg::Vec3 dir, f32 max_distance,
                   entity::Handle ignore);

} // namespace motion
