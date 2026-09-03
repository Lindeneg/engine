#include "platform/dialog.h"

#include "core/logger.h"

#if PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <commdlg.h>
#include <shobjidl.h>
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

// The folder picker is the newer shell dialog, which speaks COM and wide
// strings; the result is brought back to UTF-8 with forward slashes.
b32 choose_folder(const char *title, char *out, u32 cap) {
    ASSERT(title && out && cap);
    out[0] = 0;

    HRESULT init = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    b32 we_initialized = SUCCEEDED(init);
    if (init == RPC_E_CHANGED_MODE)
        we_initialized = false; // already up in another mode; use it as is

    IFileOpenDialog *dlg = NULL;
    b32 ok = false;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&dlg)))) {
        WCHAR wtitle[FILTER_CAP];
        MultiByteToWideChar(CP_UTF8, 0, title, -1, wtitle, FILTER_CAP);
        dlg->SetTitle(wtitle);

        DWORD options = 0;
        dlg->GetOptions(&options);
        dlg->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR);

        if (SUCCEEDED(dlg->Show(NULL))) {
            IShellItem *item = NULL;
            if (SUCCEEDED(dlg->GetResult(&item))) {
                PWSTR wpath = NULL;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &wpath))) {
                    ok = WideCharToMultiByte(CP_UTF8, 0, wpath, -1, out, (i32)cap, NULL, NULL) > 0;
                    CoTaskMemFree(wpath);
                }
                item->Release();
            }
        }
        dlg->Release();
    } else {
        LOG_WARN("dialog: the folder picker could not be created");
    }

    if (we_initialized)
        CoUninitialize();

    if (ok)
        for (char *c = out; *c; c++)
            if (*c == '\\')
                *c = '/';
    return ok;
}

#else

b32 open_file(const char *, const char *, const char *, const char *, char *out, u32 cap) {
    ASSERT(out && cap);
    out[0] = 0;
    LOG_WARN_ONCE("dialog: no file picker on this platform yet; type the path instead");
    return false;
}

b32 choose_folder(const char *, char *out, u32 cap) {
    ASSERT(out && cap);
    out[0] = 0;
    LOG_WARN_ONCE("dialog: no folder picker on this platform yet; pass the project as an argument");
    return false;
}

#endif

} // namespace dialog
