#pragma once

#include "core/common.h"
#include "math/intersect.h"
#include "math/linalg.h"

// The translate gizmo's geometry and maths, with no state of its own: which
// handle a mouse ray is over, and how far a drag has moved. The editor owns
// the drag, draws the handles, and applies the result. Everything here is in
// world units, so a test needs no camera; the editor turns pixels into units
// at the gizmo's distance.

namespace gizmo {

enum Handle {
    HANDLE_NONE = -1,
    HANDLE_X,
    HANDLE_Y,
    HANDLE_Z,
    HANDLE_YZ, // the plane whose normal is X
    HANDLE_XZ,
    HANDLE_XY,
    HANDLE_COUNT,
};

// where the handles are: the origin, three unit axes, and the sizes
struct Frame {
    linalg::Vec3 origin;
    linalg::Vec3 axis[3];
    f32 length;       // of an axis line
    f32 plane_offset; // from the origin to a plane square's near corner
    f32 plane_size;   // a square's side
};

// A plane square wins over an axis line where they overlap; otherwise the
// nearest axis within tolerance.
Handle hit(const Frame *f, intersect::Ray ray, f32 tolerance);

// A drag remembers the frame it started in and the point it grabbed, so the
// handles may move with the thing they move without the measure drifting.
struct Drag {
    Frame frame;
    Handle handle;
    linalg::Vec3 grab; // on the axis line or the plane
    b32 valid;         // false when the ray was parallel to the handle
};

Drag begin(const Frame *f, Handle h, intersect::Ray ray);

// The world offset the ray asks for relative to the grab, along the handle's
// axis or in its plane. A positive snap rounds each moved axis to that step.
linalg::Vec3 delta(const Drag *d, intersect::Ray ray, f32 snap);

// The axis a handle constrains to, or the plane's normal for a plane handle.
u32 handle_axis(Handle h);
b32 handle_is_plane(Handle h);

} // namespace gizmo
