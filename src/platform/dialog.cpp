#include "platform/dialog.h"

#include "core/logger.h"

#if PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <commdlg.h>
#endif

// "glTF binary\0*.glb\0\0": the picker's filter is a run of NUL-separated pairs
#define FILTER_CAP 128

namespace dialog {

#if PLATFORM_WINDOWS

b32 open_file(const char *title, const char *filter_label, const char *extension,
              const char *start_dir, char *out, u32 cap) {
    ASSERT(title && filter_label && extension && out && cap);

    char filter[FILTER_CAP];
    i32 n = snprintf(filter, sizeof(filter), "%s", filter_label);
    if (n < 0 || (u32)n + 1 >= sizeof(filter))
        return false;
    i32 m = snprintf(filter + n + 1, sizeof(filter) - (u32)n - 1, "*.%s", extension);
    if (m < 0 || (u32)(n + 1 + m) + 2 > sizeof(filter))
        return false;
    filter[n + 1 + m + 1] = 0; // the double NUL that ends the list

    out[0] = 0;

    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrTitle = title;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = out;
    ofn.nMaxFile = cap;
    ofn.lpstrInitialDir = start_dir;
    ofn.lpstrDefExt = extension;
    // NOCHANGEDIR: the picker must not move the process's working directory
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;

    if (!GetOpenFileNameA(&ofn)) {
        // 0 is a cancel; anything else is a failure worth hearing about
        DWORD err = CommDlgExtendedError();
        if (err)
            LOG_WARN("dialog: the file picker failed (%lu)", (unsigned long)err);
        return false;
    }

    // the OS spells directories with backslashes; the asset root does not
    for (char *c = out; *c; c++)
        if (*c == '\\')
            *c = '/';
    return true;
}

#else

b32 open_file(const char *, const char *, const char *, const char *, char *out, u32 cap) {
    ASSERT(out && cap);
    out[0] = 0;
    LOG_WARN_ONCE("dialog: no file picker on this platform yet; type the path instead");
    return false;
}

#endif

} // namespace dialog
