#pragma once

#include "common.h"
#include "linalg.h"

namespace render {

// Sets the GL state this engine assumes everywhere: depth testing on, back
// faces culled, counter-clockwise front faces (glTF's winding).
void init(i32 fb_width, i32 fb_height);

void resize(i32 fb_width, i32 fb_height);

void begin_frame(linalg::Vec4 clear_color);

} // namespace render
