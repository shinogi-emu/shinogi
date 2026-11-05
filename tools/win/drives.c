#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "drives.h"

static void section(int i, char *out, size_t size)
{
    if (i < EXTRA_FOLDERS) snprintf(out, size, "Folder%c", 'D' + i);
    else if (i < EXTRA_FOLDERS + HARD_IMAGES)
        snprintf(out, size, "HardDisk%d", i - EXTRA_FOLDERS + 1);
    else snprintf(out, size, "Floppy%c", 'A' + i - EXTRA_FOLDERS - HARD_IMAGES);
}

int drives_load(DriveSettings *d, const char *file)
{
    int i;
    memset(d, 0, sizeof(*d));
    for (i = 0; i < EXTRA_DRIVES; i++) {
        char name[32];
        DWORD n;
        section(i, name, sizeof(name));
        n = GetPrivateProfileStringA(name, "Path", "", d->mapping[i].path, MAX_PATH, file);
        if (n == MAX_PATH - 1) return 0;
        d->mapping[i].readonly = GetPrivateProfileIntA(name, "ReadOnly", i >= EXTRA_FOLDERS, file) != 0;
    }
    return 1;
}

int drives_save(const DriveSettings *d, const char *file)
{
    int i;
    for (i = 0; i < EXTRA_DRIVES; i++) {
        char name[32];
        section(i, name, sizeof(name));
        if (!WritePrivateProfileStringA(name, "Path", d->mapping[i].path, file) ||
            !WritePrivateProfileStringA(name, "ReadOnly", d->mapping[i].readonly ? "1" : "0", file)) return 0;
    }
    return 1;
}

static int full_path(const char *path)
{
    size_t n = strlen(path);
    if (!n || n > DRIVE_PATH_MAX || strchr(path, '"')) return 0;
    if (path[0] == '\\' && path[1] == '\\') {
        /* Device paths can name volumes/pipes rather than ordinary files. */
        return path[2] != '.' && path[2] != '?';
    }
    return n >= 3 && ((path[0] >= 'A' && path[0] <= 'Z') ||
           (path[0] >= 'a' && path[0] <= 'z')) && path[1] == ':' &&
           (path[2] == '\\' || path[2] == '/');
}

int drives_folder_valid(const char *path)
{
    DWORD a;
    if (!full_path(path)) return 0;
    a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static int identity(const char *path, BY_HANDLE_FILE_INFORMATION *info)
{
    HANDLE h = CreateFileA(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    int ok;
    if (h == INVALID_HANDLE_VALUE) return 0;
    ok = GetFileInformationByHandle(h, info);
    CloseHandle(h);
    return ok;
}

static int same_file(const BY_HANDLE_FILE_INFORMATION *a, const BY_HANDLE_FILE_INFORMATION *b)
{
    return a->dwVolumeSerialNumber == b->dwVolumeSerialNumber &&
           a->nFileIndexHigh == b->nFileIndexHigh && a->nFileIndexLow == b->nFileIndexLow;
}

int drives_valid(const DriveSettings *d, const char *drivec, char *error, size_t size)
{
    BY_HANDLE_FILE_INFORMATION ids[EXTRA_DRIVES], boot;
    int used[EXTRA_DRIVES] = {0}, have_boot = identity(drivec, &boot), i, j;
    for (i = 0; i < EXTRA_DRIVES; i++) {
        const char *path = d->mapping[i].path;
        char name[32];
        ULONGLONG bytes;
        if (!*path) continue;
        section(i, name, sizeof(name));
        if (i == 'U' - 'D') {
            snprintf(error, size, "U: is reserved for FreeMiNT's unified filesystem. Choose another folder letter.");
            return 0;
        }
        if (!full_path(path) || !identity(path, &ids[i])) {
            snprintf(error, size, "%s: choose an existing folder or image using its full path (up to %d characters).", name, DRIVE_PATH_MAX);
            return 0;
        }
        used[i] = 1;
        if ((ids[i].dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            if (i >= EXTRA_FOLDERS) {
                snprintf(error, size, "%s needs an image file.", name); return 0;
            }
        } else if (i < EXTRA_FOLDERS) {
            snprintf(error, size, "%s needs a folder.", name); return 0;
        }
        bytes = ((ULONGLONG)ids[i].nFileSizeHigh << 32) | ids[i].nFileSizeLow;
        if (i >= EXTRA_FOLDERS && (!bytes || bytes % 512)) {
            snprintf(error, size, "%s: raw images need a nonzero multiple of 512 bytes.", name); return 0;
        }
        if (i >= EXTRA_FOLDERS + HARD_IMAGES && bytes != 737280 && bytes != 1474560) {
            snprintf(error, size, "%s: choose a raw 720 KB or 1.44 MB floppy image (.st or .img).", name); return 0;
        }
        if (i >= EXTRA_FOLDERS + HARD_IMAGES) {
            unsigned char sector[512];
            FILE *f = fopen(path, "rb");
            int ok = f && fread(sector, 1, sizeof(sector), f) == sizeof(sector);
            if (f) fclose(f);
            if (!ok || sector[11] != 0 || sector[12] != 2 ||
                sector[24] != (bytes == 737280 ? 9 : 18) || sector[25] != 0 ||
                sector[26] != 2 || sector[27] != 0 ||
                ((unsigned)sector[19] | ((unsigned)sector[20] << 8)) != bytes / 512) {
                snprintf(error, size, "%s: expected a formatted 512-byte-sector FAT floppy with 80 tracks and two sides.", name);
                return 0;
            }
        }
        if (i >= EXTRA_FOLDERS) {
            const char *ext = strrchr(path, '.');
            if (!ext || (lstrcmpiA(ext, ".img") && lstrcmpiA(ext, ".hdf") &&
                         !(i >= EXTRA_FOLDERS + HARD_IMAGES && !lstrcmpiA(ext, ".st")))) {
                snprintf(error, size, "%s: only raw .img/.hdf hard disks and .st/.img floppies are supported.", name); return 0;
            }
        }
        if (have_boot && same_file(&boot, &ids[i])) {
            snprintf(error, size, "%s already serves drive C.", name); return 0;
        }
        for (j = 0; j < i; j++) if (used[j] && same_file(&ids[j], &ids[i])) {
            snprintf(error, size, "%s duplicates another mapping. Choose a different folder or image.", name); return 0;
        }
    }
    return 1;
}

static int append(char *out, size_t size, const char *format, ...)
{
    size_t n = strlen(out);
    int count;
    va_list args;
    if (n >= size) return 0;
    va_start(args, format);
    count = vsnprintf(out + n, size - n, format, args);
    va_end(args);
    return count >= 0 && (size_t)count < size - n;
}

int drives_image_options(const DriveSettings *d, char *out, size_t size)
{
    int i;
    out[0] = 0;
    for (i = EXTRA_FOLDERS; i < EXTRA_DRIVES; i++) {
        /* QEMU keyval escapes a comma by doubling it. Forward slashes
         * also avoid Windows command-line trailing-backslash quoting. */
        const DriveMapping *m = &d->mapping[i];
        char path[MAX_PATH * 2], serial[32];
        size_t j, n = 0;
        if (!*m->path) continue;
        for (j = 0; m->path[j]; j++) {
            char c = m->path[j];
            if (c == '"' || n + 2 >= sizeof(path)) return 0;
            if (c == ',') path[n++] = ',';
            path[n++] = c == '\\' ? '/' : c;
        }
        path[n] = 0;
        if (i < EXTRA_FOLDERS + HARD_IMAGES)
            snprintf(serial, sizeof(serial), "shinogi.disk%d", i - EXTRA_FOLDERS + 1);
        else snprintf(serial, sizeof(serial), "shinogi.floppy%c", 'a' + i - EXTRA_FOLDERS - HARD_IMAGES);
        if (!append(out, size,
                    " -drive \"file=%s,format=raw,if=none,id=img%d,readonly=%s,cache=writethrough\""
                    " -device virtio-blk-device,drive=img%d,serial=%s",
                    path, i, m->readonly ? "on" : "off", i, serial)) return 0;
    }
    return 1;
}

typedef struct {
    DriveSettings draft;
    const char *boot;
    int selected;
} DriveDialog;

static void collect(HWND w, DriveDialog *d)
{
    DriveMapping *m = &d->draft.mapping[d->selected];
    GetDlgItemTextA(w, IDC_DRIVE_PATH, m->path, sizeof(m->path));
    m->readonly = IsDlgButtonChecked(w, IDC_DRIVE_READONLY) == BST_CHECKED;
}

static void show_mapping(HWND w, DriveDialog *d)
{
    DriveMapping *m = &d->draft.mapping[d->selected];
    SetDlgItemTextA(w, IDC_DRIVE_PATH, m->path);
    CheckDlgButton(w, IDC_DRIVE_READONLY, m->readonly ? BST_CHECKED : BST_UNCHECKED);
    EnableWindow(GetDlgItem(w, IDC_DRIVE_PATH), d->selected != 'U' - 'D');
    EnableWindow(GetDlgItem(w, IDC_DRIVE_BROWSE), d->selected != 'U' - 'D');
    EnableWindow(GetDlgItem(w, IDC_DRIVE_READONLY), d->selected != 'U' - 'D');
    SetDlgItemTextA(w, IDC_DRIVE_DESCRIPTION, d->selected == 'U' - 'D' ?
        "U: belongs to FreeMiNT's unified filesystem. Leave this slot empty; Remove mapping clears an old manual entry." :
        d->selected < EXTRA_FOLDERS ?
        "A host folder appears at the chosen Atari letter. Read-only refuses changes through this drive, including attribute repair." :
        d->selected < EXTRA_FOLDERS + HARD_IMAGES ?
        "Raw .img/.hdf disk. Partitions use available letters, skipping folder mappings. The image must already be formatted." :
        "Raw .st/.img floppy: 720 KB or 1.44 MB. Changes take effect after restarting Shinogi.");
}

static int CALLBACK initial(HWND w, UINT msg, LPARAM unused, LPARAM path)
{
    (void)unused;
    if (msg == BFFM_INITIALIZED) SendMessageA(w, BFFM_SETSELECTIONA, TRUE, path);
    return 0;
}

static void choose(HWND w, DriveDialog *d)
{
    char path[MAX_PATH];
    collect(w, d);
    lstrcpynA(path, d->draft.mapping[d->selected].path, sizeof(path));
    if (d->selected < EXTRA_FOLDERS) {
        BROWSEINFOA info;
        PIDLIST_ABSOLUTE item;
        ZeroMemory(&info, sizeof(info));
        info.hwndOwner = w; info.lpszTitle = "Choose an Atari drive folder";
        info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        info.lpfn = initial; info.lParam = (LPARAM)path;
        item = SHBrowseForFolderA(&info);
        if (item) { if (SHGetPathFromIDListA(item, path)) SetDlgItemTextA(w, IDC_DRIVE_PATH, path); CoTaskMemFree(item); }
    } else {
        OPENFILENAMEA info;
        ZeroMemory(&info, sizeof(info));
        info.lStructSize = sizeof(info); info.hwndOwner = w;
        info.lpstrFile = path; info.nMaxFile = sizeof(path);
        info.lpstrFilter = d->selected < EXTRA_FOLDERS + HARD_IMAGES ?
            "Raw hard-disk images (*.img;*.hdf)\0*.img;*.hdf\0\0" :
            "Raw floppy images (*.st;*.img)\0*.st;*.img\0\0";
        info.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameA(&info)) SetDlgItemTextA(w, IDC_DRIVE_PATH, path);
    }
}

static INT_PTR CALLBACK proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    DriveDialog *d = (DriveDialog *)GetWindowLongPtrA(w, DWLP_USER);
    if (msg == WM_INITDIALOG) {
        int i;
        d = (DriveDialog *)lp; SetWindowLongPtrA(w, DWLP_USER, lp);
        for (i = 0; i < EXTRA_DRIVES; i++) {
            char label[64];
            LRESULT row;
            if (i == 'U' - 'D') snprintf(label, sizeof(label), "U: reserved for FreeMiNT");
            else if (i < EXTRA_FOLDERS) snprintf(label, sizeof(label), "Folder drive %c:", 'D' + i);
            else if (i < EXTRA_FOLDERS + HARD_IMAGES) snprintf(label, sizeof(label), "Hard disk image %d", i - EXTRA_FOLDERS + 1);
            else snprintf(label, sizeof(label), "Floppy drive %c:", 'A' + i - EXTRA_FOLDERS - HARD_IMAGES);
            row = SendDlgItemMessageA(w, IDC_DRIVE_SLOT, CB_ADDSTRING, 0, (LPARAM)label);
            SendDlgItemMessageA(w, IDC_DRIVE_SLOT, CB_SETITEMDATA, row, i);
        }
        SendDlgItemMessageA(w, IDC_DRIVE_SLOT, CB_SETCURSEL, 0, 0);
        SendDlgItemMessageA(w, IDC_DRIVE_PATH, EM_SETLIMITTEXT, DRIVE_PATH_MAX, 0);
        show_mapping(w, d); return TRUE;
    }
    if (msg == WM_CLOSE || (msg == WM_COMMAND && LOWORD(wp) == IDCANCEL)) { EndDialog(w, IDCANCEL); return TRUE; }
    if (msg != WM_COMMAND) return FALSE;
    switch (LOWORD(wp)) {
    case IDC_DRIVE_SLOT:
        if (HIWORD(wp) == CBN_SELCHANGE) {
            collect(w, d);
            LRESULT row = SendDlgItemMessageA(w, IDC_DRIVE_SLOT, CB_GETCURSEL, 0, 0);
            d->selected = (int)SendDlgItemMessageA(w, IDC_DRIVE_SLOT, CB_GETITEMDATA, row, 0);
            show_mapping(w, d);
        }
        return TRUE;
    case IDC_DRIVE_BROWSE: choose(w, d); return TRUE;
    case IDC_DRIVE_CLEAR: SetDlgItemTextA(w, IDC_DRIVE_PATH, ""); return TRUE;
    case IDOK: {
        char error[512];
        collect(w, d);
        if (!drives_valid(&d->draft, d->boot, error, sizeof(error))) { MessageBoxA(w, error, "Additional drives", MB_ICONWARNING); return TRUE; }
        EndDialog(w, IDOK); return TRUE;
    }
    }
    return FALSE;
}

int drives_dialog(HINSTANCE instance, HWND owner, DriveSettings *drives, const char *drivec)
{
    DriveDialog d;
    INT_PTR result;
    d.draft = *drives; d.selected = 0; d.boot = drivec;
    result = DialogBoxParamA(instance, MAKEINTRESOURCEA(IDD_DRIVES), owner, proc, (LPARAM)&d);
    if (result == IDOK) *drives = d.draft;
    return result == IDOK;
}
