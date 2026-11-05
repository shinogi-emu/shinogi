#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../tools/win/drives.h"

static char folder[MAX_PATH], image[MAX_PATH], config[MAX_PATH];
static int cancel_dialog;

static DWORD WINAPI edit_dialog(void *unused)
{
    HWND w = NULL;
    int i;
    (void)unused;
    for (i = 0; i < 400; i++) {
        w = FindWindowA("#32770", "Additional drives");
        if (w && GetDlgItem(w, IDC_DRIVE_SLOT)) break;
        Sleep(25);
    }
    assert(w);
    SetDlgItemTextA(w, IDC_DRIVE_PATH, folder);
    CheckDlgButton(w, IDC_DRIVE_READONLY, BST_CHECKED);
    /* Switching away and back must retain edits, including the last slot. */
    SendDlgItemMessageA(w, IDC_DRIVE_SLOT, CB_SETCURSEL, 22, 0);
    SendMessageA(w, WM_COMMAND, MAKEWPARAM(IDC_DRIVE_SLOT, CBN_SELCHANGE), 0);
    SetDlgItemTextA(w, IDC_DRIVE_PATH, "");
    SendDlgItemMessageA(w, IDC_DRIVE_SLOT, CB_SETCURSEL, 23, 0);
    SendMessageA(w, WM_COMMAND, MAKEWPARAM(IDC_DRIVE_SLOT, CBN_SELCHANGE), 0);
    SetDlgItemTextA(w, IDC_DRIVE_PATH, image);
    SendDlgItemMessageA(w, IDC_DRIVE_SLOT, CB_SETCURSEL, 0, 0);
    SendMessageA(w, WM_COMMAND, MAKEWPARAM(IDC_DRIVE_SLOT, CBN_SELCHANGE), 0);
    {
        char path[MAX_PATH];
        GetDlgItemTextA(w, IDC_DRIVE_PATH, path, sizeof(path));
        assert(!strcmp(path, folder));
        assert(IsDlgButtonChecked(w, IDC_DRIVE_READONLY) == BST_CHECKED);
    }
    SendMessageA(w, WM_COMMAND, cancel_dialog ? IDCANCEL : IDOK, 0);
    return 0;
}

int main(void)
{
    DriveSettings d, loaded, before;
    char base[MAX_PATH], error[512], options[8192], tiny[10], other[MAX_PATH];
    FILE *f;
    HANDLE thread;
    unsigned char sector[512] = {0};
    assert(GetTempPathA(sizeof(base), base));
    assert(GetTempFileNameA(base, "drv", 0, folder));
    assert(DeleteFileA(folder) && CreateDirectoryA(folder, NULL));
    assert(snprintf(image, sizeof(image), "%s\\disk,with spaces.img", folder) > 0);
    assert(snprintf(config, sizeof(config), "%s\\settings.ini", folder) > 0);
    assert(snprintf(other, sizeof(other), "%s\\other", folder) > 0);
    assert(CreateDirectoryA(other, NULL));
    f = fopen(image, "wb"); assert(f); assert(fwrite(sector, 1, sizeof(sector), f) == sizeof(sector)); fclose(f);
    assert(drives_load(&d, config));
    assert(d.mapping[0].readonly == 0 && d.mapping[23].readonly == 1);
    assert(drives_valid(&d, other, error, sizeof(error)));
    before = d;
    cancel_dialog = 1;
    thread = CreateThread(NULL, 0, edit_dialog, NULL, 0, NULL); assert(thread);
    assert(!drives_dialog(GetModuleHandleA(NULL), NULL, &d, other));
    assert(WaitForSingleObject(thread, 15000) == WAIT_OBJECT_0); CloseHandle(thread);
    assert(!memcmp(&d, &before, sizeof(d)));
    cancel_dialog = 0;
    thread = CreateThread(NULL, 0, edit_dialog, NULL, 0, NULL); assert(thread);
    assert(drives_dialog(GetModuleHandleA(NULL), NULL, &d, other));
    assert(WaitForSingleObject(thread, 15000) == WAIT_OBJECT_0); CloseHandle(thread);
    assert(!strcmp(d.mapping[0].path, folder) && d.mapping[0].readonly);
    assert(!strcmp(d.mapping[23].path, image) && d.mapping[23].readonly);
    assert(drives_save(&d, config) && drives_load(&loaded, config));
    assert(!memcmp(&d, &loaded, sizeof(d)));
    assert(drives_image_options(&d, options, sizeof(options)));
    assert(strstr(options, "disk,,with spaces.img,format=raw,if=none"));
    assert(strstr(options, "readonly=on,cache=writethrough"));
    assert(strstr(options, "serial=shinogi.disk1"));
    assert(!drives_image_options(&d, tiny, sizeof(tiny)));
    loaded.mapping[24] = loaded.mapping[23];
    assert(!drives_valid(&loaded, other, error, sizeof(error)) && strstr(error, "duplicates"));
    loaded = d; strcpy(loaded.mapping[0].path, "relative");
    assert(!drives_valid(&loaded, other, error, sizeof(error)));
    loaded = d; strcpy(loaded.mapping[23].path, folder);
    assert(!drives_valid(&loaded, other, error, sizeof(error)));
    loaded = d; strcpy(loaded.mapping[27].path, image);
    assert(!drives_valid(&loaded, other, error, sizeof(error)) && strstr(error, "720"));
    /* Removing a mapping persists an empty slot and keeps its backing file. */
    d.mapping[23].path[0] = 0;
    assert(drives_save(&d, config) && drives_load(&loaded, config));
    assert(!loaded.mapping[23].path[0] && GetFileAttributesA(image) != INVALID_FILE_ATTRIBUTES);
    puts("PASS: drive dialog edits/cancel, INI roundtrip/removal, defaults, validation, duplicate files and quoted comma paths");
    return 0;
}
