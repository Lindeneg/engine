#pragma once

#include "core/common.h"

// The operating system's own file picker, for the editor. Modal: the frame
// waits while it is open, which is what a picker is for.

namespace dialog {

// One extension, without the dot, and what to call it in the picker. The
// absolute path chosen lands in out; false when the user cancelled, the path
// would not fit, or this platform has no picker.
b32 open_file(const char *title, const char *filter_label, const char *extension,
              const char *start_dir, char *out, u32 cap);

} // namespace dialog
