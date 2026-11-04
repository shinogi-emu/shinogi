#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "settings.h"
#include "resolution.h"

static int child_path(char *out, size_t size, const char *base, const char *name)
{
    int n = snprintf(out, size, "%s\\%s", base, name);
    return n >= 0 && (size_t)n < size;
}

void settings_resolution(Settings *s)
{
    char path[MAX_PATH];
    FILE *f;
    s->width = 1024;
    s->height = 768;
    if (!child_path(path, sizeof(path), s->drivec, "SHINOGI.INI")) return;
    f = fopen(path, "r");
    if (!f) return;
    shinogi_read_resolution(f, &s->width, &s->height);
    fclose(f);
}

int settings_load(Settings *s, const char *program_dir)
{
    char base[MAX_PATH], fallback[MAX_PATH];
    const char *profile = getenv("USERPROFILE");
    ZeroMemory(s, sizeof(*s));
    if (FAILED(SHGetFolderPathA(NULL, CSIDL_APPDATA | CSIDL_FLAG_CREATE,
                              NULL, 0, base)))
        return 0;
    if (!child_path(s->file, sizeof(s->file), base, "shinogi")) return 0;
    if (!CreateDirectoryA(s->file, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        return 0;
    if (!child_path(s->file, sizeof(s->file), base, "shinogi\\settings.ini") ||
        !child_path(fallback, sizeof(fallback),
                    profile && *profile ? profile : program_dir, "shinogi-drive-c"))
        return 0;
    GetPrivateProfileStringA("Drives", "C", fallback,
                             s->drivec, sizeof(s->drivec), s->file);
    s->fullscreen = GetPrivateProfileIntA("Display", "Fullscreen", 0, s->file) != 0;
    settings_resolution(s);
    return 1;
}

int settings_drive_valid(const char *path)
{
    DWORD attrs;
    if (!*path || strlen(path) > DRIVE_PATH_MAX || strchr(path, '"')) return 0;
    if (!((strlen(path) >= 3 && path[1] == ':' &&
           (path[2] == '\\' || path[2] == '/')) ||
          (path[0] == '\\' && path[1] == '\\'))) return 0;
    attrs = GetFileAttributesA(path);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

/* Keep guest settings and comments; replace only the res line. Commit the
 * new file by rename, so a failed write cannot truncate the existing one. */
static int save_resolution(const Settings *s)
{
    char path[MAX_PATH], temp[MAX_PATH];
    FILE *in, *out;
    int ok = 1;
    if (!child_path(path, sizeof(path), s->drivec, "SHINOGI.INI")) return 0;
    if (!GetTempFileNameA(s->drivec, "shn", 0, temp)) return 0;
    out = fopen(temp, "wb");
    if (!out) { DeleteFileA(temp); return 0; }
    in = fopen(path, "rb");
    if (!in && GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) ok = 0;
    if (ok) ok = shinogi_write_resolution(in, out, s->width, s->height);
    if (in) fclose(in);
    if (fclose(out)) ok = 0;
    if (ok) ok = MoveFileExA(temp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok) DeleteFileA(temp);
    return ok;
}

static int CALLBACK browse_initial(HWND window, UINT message, LPARAM param, LPARAM data)
{
    (void)param;
    if (message == BFFM_INITIALIZED)
        SendMessageA(window, BFFM_SETSELECTIONA, TRUE, data);
    return 0;
}

static void browse(HWND dialog)
{
    char path[MAX_PATH];
    BROWSEINFOA info;
    PIDLIST_ABSOLUTE item;
    GetDlgItemTextA(dialog, IDC_DRIVEC, path, sizeof(path));
    ZeroMemory(&info, sizeof(info));
    info.hwndOwner = dialog;
    info.lpszTitle = "Choose the folder used as Atari drive C:";
    info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    info.lpfn = browse_initial;
    info.lParam = (LPARAM)path;
    item = SHBrowseForFolderA(&info);
    if (item) {
        if (SHGetPathFromIDListA(item, path)) {
            SetDlgItemTextA(dialog, IDC_DRIVEC, path);
            /* A newly selected drive may have its own resolution. */
            Settings other;
            char mode[32];
            ZeroMemory(&other, sizeof(other));
            lstrcpynA(other.drivec, path, sizeof(other.drivec));
            settings_resolution(&other);
            snprintf(mode, sizeof(mode), "%dx%d", other.width, other.height);
            SetDlgItemTextA(dialog, IDC_RESOLUTION, mode);
        }
        CoTaskMemFree(item);
    }
}

static INT_PTR CALLBACK dialog_proc(HWND dialog, UINT message, WPARAM wp, LPARAM lp)
{
    Settings *s = (Settings *)GetWindowLongPtrA(dialog, DWLP_USER);
    if (message == WM_INITDIALOG) {
        static const char *modes[] = {"640x480", "800x600", "1024x768",
                                     "1280x720", "1280x1024", "1920x1080"};
        char text[32];
        size_t i;
        s = (Settings *)lp;
        SetWindowLongPtrA(dialog, DWLP_USER, lp);
        SendDlgItemMessageA(dialog, IDC_DRIVEC, EM_SETLIMITTEXT, DRIVE_PATH_MAX, 0);
        SendDlgItemMessageA(dialog, IDC_RESOLUTION, CB_LIMITTEXT, 32, 0);
        SetDlgItemTextA(dialog, IDC_DRIVEC, s->drivec);
        for (i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i)
            SendDlgItemMessageA(dialog, IDC_RESOLUTION, CB_ADDSTRING, 0, (LPARAM)modes[i]);
        snprintf(text, sizeof(text), "%dx%d", s->width, s->height);
        SetDlgItemTextA(dialog, IDC_RESOLUTION, text);
        CheckDlgButton(dialog, IDC_FULLSCREEN, s->fullscreen ? BST_CHECKED : BST_UNCHECKED);
        return TRUE;
    }
    if (message == WM_CLOSE || (message == WM_COMMAND && LOWORD(wp) == IDCANCEL)) {
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
    if (message != WM_COMMAND) return FALSE;
    if (LOWORD(wp) == IDC_BROWSE) { browse(dialog); return TRUE; }
    if (LOWORD(wp) == IDOK) {
        Settings next = *s;
        char text[64], absolute[MAX_PATH];
        DWORD n;
        GetDlgItemTextA(dialog, IDC_DRIVEC, next.drivec, sizeof(next.drivec));
        if (!settings_drive_valid(next.drivec)) {
            MessageBoxA(dialog, "Choose an existing drive C folder using its full path.",
                        "Shinogi Settings", MB_ICONWARNING);
            return TRUE;
        }
        n = GetFullPathNameA(next.drivec, sizeof(absolute), absolute, NULL);
        if (!n || n > DRIVE_PATH_MAX) {
            MessageBoxA(dialog, "The full folder path is too long. Choose a shorter path.",
                        "Shinogi Settings", MB_ICONWARNING);
            return TRUE;
        }
        lstrcpynA(next.drivec, absolute, sizeof(next.drivec));
        GetDlgItemTextA(dialog, IDC_RESOLUTION, text, sizeof(text));
        if (!shinogi_parse_resolution(text, &next.width, &next.height)) {
            MessageBoxA(dialog, "Enter a size from 320x200 to 1920x1080.\n"
                        "The width must be a multiple of 8.",
                        "Shinogi Settings", MB_ICONWARNING);
            return TRUE;
        }
        next.fullscreen = IsDlgButtonChecked(dialog, IDC_FULLSCREEN) == BST_CHECKED;
        if (!save_resolution(&next) ||
            !WritePrivateProfileStringA("Display", "Fullscreen", next.fullscreen ? "1" : "0", next.file) ||
            !WritePrivateProfileStringA("Drives", "C", next.drivec, next.file)) {
            MessageBoxA(dialog, "Could not save settings. Check that the drive C folder\n"
                        "and your settings folder are writable, then try again.",
                        "Shinogi Settings", MB_ICONERROR);
            return TRUE;
        }
        *s = next;
        EndDialog(dialog, IDOK);
        return TRUE;
    }
    return FALSE;
}

int settings_dialog(HINSTANCE instance, Settings *s)
{
    INT_PTR result;
    HRESULT initialized = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    result = DialogBoxParamA(instance, MAKEINTRESOURCEA(IDD_SETTINGS), NULL,
                             dialog_proc, (LPARAM)s);
    if (SUCCEEDED(initialized)) CoUninitialize();
    if (result == -1)
        MessageBoxA(NULL, "Could not open Shinogi Settings.", "shinogi", MB_ICONERROR);
    return result == IDOK;
}
