#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "settings.h"
#include "resolution.h"
#include "network.h"

static NetworkAdapter network_devices[64];
static int network_device_count;

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
    char network_error[512];
    const char *profile = getenv("USERPROFILE");
    ZeroMemory(s, sizeof(*s));
    if (FAILED(SHGetFolderPathA(NULL, CSIDL_APPDATA | CSIDL_FLAG_CREATE,
                              NULL, 0, base)))
        return 0;
    if (!child_path(s->file, sizeof(s->file), base, SHINOGI_EDITION)) return 0;
    if (!CreateDirectoryA(s->file, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        return 0;
    if (!child_path(s->file, sizeof(s->file), base, SHINOGI_EDITION "\\settings.ini") ||
        !child_path(fallback, sizeof(fallback),
                    profile && *profile ? profile : program_dir, SHINOGI_EDITION "\\drive-c"))
        return 0;
    GetPrivateProfileStringA("Drives", "C", fallback,
                             s->drivec, sizeof(s->drivec), s->file);
    s->fullscreen = GetPrivateProfileIntA("Display", "Fullscreen", 0, s->file) != 0;
    settings_resolution(s);
    return drives_load(&s->drives, s->file) &&
        network_load(&s->network, s->file, network_error, sizeof(network_error));
}

int settings_drive_valid(const char *path)
{
    return drives_folder_valid(path);
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

static void network_controls(HWND dialog)
{
    int lan = SendDlgItemMessageA(dialog, IDC_NETWORK_MODE, CB_GETCURSEL, 0, 0) == 1;
    int ids[] = {IDC_ETHERNET, IDC_LAN_ADVANCED, IDC_LAN_SETUP};
    size_t i;
    for (i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i)
        EnableWindow(GetDlgItem(dialog, ids[i]), lan);
    EnableWindow(GetDlgItem(dialog, IDC_LAN_REMOVE), network_has_setup());
}

static int selected_ethernet(HWND dialog)
{
    LRESULT selected = SendDlgItemMessageA(dialog, IDC_ETHERNET, CB_GETCURSEL, 0, 0);
    LRESULT index;
    if (selected == CB_ERR) return -1;
    index = SendDlgItemMessageA(dialog, IDC_ETHERNET, CB_GETITEMDATA, selected, 0);
    return index >= 0 && index < network_device_count ? (int)index : -1;
}

static void address_mode_label(HWND dialog, const NetworkSettings *s)
{
    SetDlgItemTextA(dialog, IDC_ADDRESS_MODE_LABEL, s->dhcp ?
                    "Atari obtains its LAN address automatically (DHCP)." :
                    "Atari uses the static address chosen in Advanced settings.");
}

static void last_lan_address(HWND dialog, const NetworkSettings *s)
{
    char drivec[MAX_PATH], path[MAX_PATH], ip[32] = "", label[128];
    unsigned long value;
    FILE *file;
    int lan = SendDlgItemMessageA(dialog, IDC_NETWORK_MODE, CB_GETCURSEL, 0, 0) == 1;
    if (lan && !s->dhcp && network_ipv4(s->address, &value)) {
        snprintf(label, sizeof(label), "Atari LAN address (static): %s", s->address);
    } else {
        GetDlgItemTextA(dialog, IDC_DRIVEC, drivec, sizeof(drivec));
        if (child_path(path, sizeof(path), drivec, "SHNNET.IP") && (file = fopen(path, "r")) != NULL) {
            if (fgets(ip, sizeof(ip), file)) ip[strcspn(ip, "\r\n")] = '\0';
            fclose(file);
        }
        snprintf(label, sizeof(label), "Last Atari LAN address: %s",
                 network_ipv4(ip, &value) ? ip : "unavailable");
    }
    SetDlgItemTextA(dialog, IDC_LAST_LAN_ADDRESS, label);
}

static void network_dialog_init(HWND dialog, const Settings *s)
{
    int i;
    SendDlgItemMessageA(dialog, IDC_NETWORK_MODE, CB_ADDSTRING, 0, (LPARAM)"Internet (NAT)");
    SendDlgItemMessageA(dialog, IDC_NETWORK_MODE, CB_ADDSTRING, 0, (LPARAM)"LAN (Ethernet)");
    SendDlgItemMessageA(dialog, IDC_NETWORK_MODE, CB_SETCURSEL, s->network.lan == 1, 0);
    network_device_count = network_adapters(network_devices, 64);
    for (i = 0; i < network_device_count; ++i) {
        LRESULT item;
        char label[NETWORK_NAME_SIZE + 32];
        if (!network_devices[i].ethernet) continue;
        snprintf(label, sizeof(label), "%s%s", network_devices[i].name,
                 network_devices[i].up ? "" : " (disconnected)");
        item = SendDlgItemMessageA(dialog, IDC_ETHERNET, CB_ADDSTRING, 0, (LPARAM)label);
        SendDlgItemMessageA(dialog, IDC_ETHERNET, CB_SETITEMDATA, item, i);
        if (!lstrcmpiA(network_devices[i].guid, s->network.ethernet_guid))
            SendDlgItemMessageA(dialog, IDC_ETHERNET, CB_SETCURSEL, item, 0);
    }
    if (SendDlgItemMessageA(dialog, IDC_ETHERNET, CB_GETCURSEL, 0, 0) == CB_ERR)
        SendDlgItemMessageA(dialog, IDC_ETHERNET, CB_SETCURSEL, 0, 0);
    network_controls(dialog);
    address_mode_label(dialog, &s->network);
    last_lan_address(dialog, &s->network);
}

static void advanced_controls(HWND dialog)
{
    int automatic = IsDlgButtonChecked(dialog, IDC_DHCP) == BST_CHECKED;
    int ids[] = {IDC_IP_ADDRESS, IDC_NETMASK, IDC_GATEWAY, IDC_DNS};
    size_t i;
    for (i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i)
        EnableWindow(GetDlgItem(dialog, ids[i]), !automatic);
}

static INT_PTR CALLBACK advanced_proc(HWND dialog, UINT message, WPARAM wp, LPARAM lp)
{
    NetworkSettings *s = (NetworkSettings *)GetWindowLongPtrA(dialog, DWLP_USER);
    if (message == WM_INITDIALOG) {
        int ids[] = {IDC_IP_ADDRESS, IDC_NETMASK, IDC_GATEWAY, IDC_DNS};
        size_t i;
        s = (NetworkSettings *)lp;
        SetWindowLongPtrA(dialog, DWLP_USER, lp);
        for (i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i)
            SendDlgItemMessageA(dialog, ids[i], EM_SETLIMITTEXT, 15, 0);
        CheckDlgButton(dialog, IDC_DHCP, s->dhcp ? BST_CHECKED : BST_UNCHECKED);
        SetDlgItemTextA(dialog, IDC_IP_ADDRESS, s->address);
        SetDlgItemTextA(dialog, IDC_NETMASK, s->netmask);
        SetDlgItemTextA(dialog, IDC_GATEWAY, s->gateway);
        SetDlgItemTextA(dialog, IDC_DNS, s->dns);
        advanced_controls(dialog);
        return TRUE;
    }
    if (message == WM_CLOSE || (message == WM_COMMAND && LOWORD(wp) == IDCANCEL)) {
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
    if (message != WM_COMMAND) return FALSE;
    if (LOWORD(wp) == IDC_DHCP) { advanced_controls(dialog); return TRUE; }
    if (LOWORD(wp) == IDOK) {
        NetworkSettings next = *s;
        char error[512];
        next.dhcp = IsDlgButtonChecked(dialog, IDC_DHCP) == BST_CHECKED;
        GetDlgItemTextA(dialog, IDC_IP_ADDRESS, next.address, sizeof(next.address));
        GetDlgItemTextA(dialog, IDC_NETMASK, next.netmask, sizeof(next.netmask));
        GetDlgItemTextA(dialog, IDC_GATEWAY, next.gateway, sizeof(next.gateway));
        GetDlgItemTextA(dialog, IDC_DNS, next.dns, sizeof(next.dns));
        if (!network_config_valid(&next, error, sizeof(error))) {
            MessageBoxA(dialog, error, "Advanced LAN Settings", MB_ICONWARNING);
            return TRUE;
        }
        *s = next;
        EndDialog(dialog, IDOK);
        return TRUE;
    }
    return FALSE;
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
        SetWindowTextA(dialog, SHINOGI_EDITION " Settings");
        SetWindowLongPtrA(dialog, DWLP_USER, lp);
        SendDlgItemMessageA(dialog, IDC_DRIVEC, EM_SETLIMITTEXT, DRIVE_PATH_MAX, 0);
        SendDlgItemMessageA(dialog, IDC_RESOLUTION, CB_LIMITTEXT, 32, 0);
        SetDlgItemTextA(dialog, IDC_DRIVEC, s->drivec);
        for (i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i)
            SendDlgItemMessageA(dialog, IDC_RESOLUTION, CB_ADDSTRING, 0, (LPARAM)modes[i]);
        snprintf(text, sizeof(text), "%dx%d", s->width, s->height);
        SetDlgItemTextA(dialog, IDC_RESOLUTION, text);
        CheckDlgButton(dialog, IDC_FULLSCREEN, s->fullscreen ? BST_CHECKED : BST_UNCHECKED);
        network_dialog_init(dialog, s);
        return TRUE;
    }
    if (message == WM_CLOSE || (message == WM_COMMAND && LOWORD(wp) == IDCANCEL)) {
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
    if (message != WM_COMMAND) return FALSE;
    if (LOWORD(wp) == IDC_NETWORK_MODE && HIWORD(wp) == CBN_SELCHANGE) {
        network_controls(dialog);
        last_lan_address(dialog, &s->network);
        return TRUE;
    }
    if (LOWORD(wp) == IDC_DRIVEC && HIWORD(wp) == EN_CHANGE) {
        last_lan_address(dialog, &s->network);
        return TRUE;
    }
    if (LOWORD(wp) == IDC_LAN_ADVANCED) {
        NetworkSettings next = s->network;
        int index = selected_ethernet(dialog);
        next.lan = 1;
        if (index >= 0) lstrcpynA(next.ethernet_guid, network_devices[index].guid, sizeof(next.ethernet_guid));
        if (DialogBoxParamA((HINSTANCE)GetWindowLongPtrA(dialog, GWLP_HINSTANCE),
                            MAKEINTRESOURCEA(IDD_LAN_ADVANCED), dialog, advanced_proc, (LPARAM)&next) == IDOK) {
            s->network = next;
            address_mode_label(dialog, &s->network);
            last_lan_address(dialog, &s->network);
        }
        return TRUE;
    }
    if (LOWORD(wp) == IDC_LAN_SETUP || LOWORD(wp) == IDC_LAN_REMOVE) {
        char error[768];
        int index = selected_ethernet(dialog), remove = LOWORD(wp) == IDC_LAN_REMOVE;
        if (index < 0) {
            MessageBoxA(dialog, "Choose a physical Ethernet adapter. Wi-Fi LAN mode is not supported.",
                        "Shinogi Settings", MB_ICONWARNING);
            return TRUE;
        }
        if (MessageBoxA(dialog, remove ?
                "Remove Shinogi's LAN bridge and restore Ethernet?\nThe shared driver is kept for reuse." :
                "Set up LAN on the selected Ethernet adapter?\nThis installs the signed TAP driver directly and creates a Windows bridge.\nOpenVPN is not required.\n"
                "Ethernet may disconnect briefly. Windows must use automatic (DHCP) addressing.",
                "Shinogi LAN", MB_OKCANCEL | MB_ICONINFORMATION) != IDOK) return TRUE;
        if (!network_setup_dialog(dialog, network_devices[index].guid, remove,
                                   s->network.tap_guid, error, sizeof(error)))
            MessageBoxA(dialog, error, "Shinogi LAN", MB_ICONERROR);
        else
            MessageBoxA(dialog, remove ? "LAN bridge removed. Choose Internet networking and Save." :
                        "LAN setup is ready. Save Settings and restart Shinogi.\nThe Atari address, router and DNS are obtained automatically by default.",
                        "Shinogi LAN", MB_ICONINFORMATION);
        network_controls(dialog);
        return TRUE;
    }
    if (LOWORD(wp) == IDC_DRIVES) {
        char boot[MAX_PATH];
        GetDlgItemTextA(dialog, IDC_DRIVEC, boot, sizeof(boot));
        drives_dialog((HINSTANCE)GetWindowLongPtrA(dialog, GWLP_HINSTANCE), dialog, &s->drives, boot);
        return TRUE;
    }
    if (LOWORD(wp) == IDC_BROWSE) { browse(dialog); return TRUE; }
    if (LOWORD(wp) == IDOK) {
        Settings next = *s;
        char text[64], absolute[MAX_PATH];
        char network_error[512];
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
        next.network.lan = SendDlgItemMessageA(dialog, IDC_NETWORK_MODE, CB_GETCURSEL, 0, 0) == 1;
        {
            int index = selected_ethernet(dialog);
            if (index >= 0)
                lstrcpynA(next.network.ethernet_guid, network_devices[index].guid, sizeof(next.network.ethernet_guid));
        }
        if (!network_config_valid(&next.network, network_error, sizeof(network_error))) {
            MessageBoxA(dialog, network_error, "Shinogi Settings", MB_ICONWARNING);
            return TRUE;
        }
        if (!drives_valid(&next.drives, next.drivec, network_error, sizeof(network_error))) {
            MessageBoxA(dialog, network_error, "Additional drives", MB_ICONWARNING);
            return TRUE;
        }
        if (!save_resolution(&next) ||
            !drives_save(&next.drives, next.file) ||
            !WritePrivateProfileStringA("Display", "Fullscreen", next.fullscreen ? "1" : "0", next.file) ||
            !WritePrivateProfileStringA("Drives", "C", next.drivec, next.file) ||
            !network_save(&next.network, next.file)) {
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
    Settings draft = *s;
    HRESULT initialized = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    result = DialogBoxParamA(instance, MAKEINTRESOURCEA(IDD_SETTINGS), NULL,
                             dialog_proc, (LPARAM)&draft);
    if (result == IDOK) *s = draft;
    if (SUCCEEDED(initialized)) CoUninitialize();
    if (result == -1)
        MessageBoxA(NULL, "Could not open Shinogi Settings.", "shinogi", MB_ICONERROR);
    return result == IDOK;
}
