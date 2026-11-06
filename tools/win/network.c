#include <winsock2.h>
#include <ws2ipdef.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "network.h"

#define LAN_KEY "Software\\shinogi\\LAN"
#define ADAPTER_KEY "SYSTEM\\CurrentControlSet\\Control\\Class\\{4D36E972-E325-11CE-BFC1-08002BE10318}"

static int failure(char *error, size_t size, const char *message)
{
    if (size) snprintf(error, size, "%s", message);
    return 0;
}

static void machine_setting(const char *name, char *out, DWORD size)
{
    DWORD type;
    HKEY key;
    *out = '\0';
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, LAN_KEY, 0,
                      KEY_READ | KEY_WOW64_64KEY, &key) == ERROR_SUCCESS) {
        if (RegQueryValueExA(key, name, NULL, &type, (BYTE *)out, &size) != ERROR_SUCCESS ||
            type != REG_SZ) *out = '\0';
        RegCloseKey(key);
    }
}

static int is_tap(const char *guid)
{
    HKEY root, key;
    DWORD i;
    int match = 0;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, ADAPTER_KEY, 0, KEY_READ, &root) != ERROR_SUCCESS)
        return 0;
    for (i = 0; !match; ++i) {
        char child[256], id[256], instance[128];
        DWORD n = sizeof(child), type;
        if (RegEnumKeyExA(root, i, child, &n, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        if (RegOpenKeyExA(root, child, 0, KEY_READ, &key) != ERROR_SUCCESS) continue;
        n = sizeof(id);
        if (RegQueryValueExA(key, "ComponentId", NULL, &type, (BYTE *)id, &n) == ERROR_SUCCESS &&
            type == REG_SZ && !lstrcmpiA(id, "tap0901")) {
            n = sizeof(instance);
            if (RegQueryValueExA(key, "NetCfgInstanceId", NULL, &type, (BYTE *)instance, &n) == ERROR_SUCCESS &&
                type == REG_SZ && !lstrcmpiA(instance, guid)) match = 1;
        }
        RegCloseKey(key);
    }
    RegCloseKey(root);
    return match;
}

int network_has_setup(void)
{
    char bridge[NETWORK_GUID_SIZE];
    machine_setting("BridgeGuid", bridge, sizeof(bridge));
    return network_guid_valid(bridge);
}

int network_adapters(NetworkAdapter *out, size_t capacity)
{
    ULONG bytes = 16384, status;
    IP_ADAPTER_ADDRESSES *list = NULL, *a;
    size_t count = 0;
    int attempt;
    for (attempt = 0; attempt < 3; ++attempt) {
        list = malloc(bytes);
        if (!list) return -1;
        status = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_ALL_INTERFACES |
                     GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                     NULL, list, &bytes);
        if (status != ERROR_BUFFER_OVERFLOW) break;
        free(list);
        list = NULL;
    }
    if (status == ERROR_NO_DATA) { free(list); return 0; }
    if (status != NO_ERROR) { free(list); return -1; }
    for (a = list; a; a = a->Next) {
        MIB_IF_ROW2 row;
        NetworkAdapter item;
        ZeroMemory(&item, sizeof(item));
        if (!network_guid_valid(a->AdapterName)) continue;
        lstrcpynA(item.guid, a->AdapterName, sizeof(item.guid));
        if (!WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, a->FriendlyName, -1,
                                 item.name, sizeof(item.name), NULL, NULL)) continue;
        ZeroMemory(&row, sizeof(row));
        row.InterfaceLuid = a->Luid;
        item.ethernet = GetIfEntry2(&row) == NO_ERROR &&
                        row.InterfaceAndOperStatusFlags.HardwareInterface &&
                        a->IfType == IF_TYPE_ETHERNET_CSMACD;
        item.dhcp = (a->Flags & IP_ADAPTER_DHCP_ENABLED) != 0;
        item.up = a->OperStatus == IfOperStatusUp;
        item.tap = is_tap(item.guid);
        if (!item.ethernet && !item.tap) continue;
        if (count == capacity) { free(list); return -1; }
        out[count++] = item;
    }
    free(list);
    return (int)count;
}

int network_load(NetworkSettings *s, const char *file, char *error, size_t size)
{
    char mode[32], address_mode[32];
    unsigned char random[3];
    ZeroMemory(s, sizeof(*s));
    GetPrivateProfileStringA("Network", "Mode", "nat", mode, sizeof(mode), file);
    s->lan = !lstrcmpiA(mode, "lan") ? 1 : (!lstrcmpiA(mode, "nat") ? 0 : -1);
    GetPrivateProfileStringA("Network", "AddressMode", "dhcp", address_mode, sizeof(address_mode), file);
    s->dhcp = !lstrcmpiA(address_mode, "dhcp") ? 1 : (!lstrcmpiA(address_mode, "static") ? 0 : -1);
#define READ(key, member, fallback) GetPrivateProfileStringA("Network", key, fallback, s->member, sizeof(s->member), file)
    READ("EthernetGuid", ethernet_guid, "");
    READ("TapGuid", tap_guid, "");
    READ("Address", address, "");
    READ("Netmask", netmask, "255.255.255.0");
    READ("Gateway", gateway, "");
    READ("DNS", dns, "");
    READ("MAC", mac, "");
#undef READ
    if (!*s->mac) {
        if (BCryptGenRandom(NULL, random, sizeof(random), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
            return failure(error, size, "Could not generate a network hardware address.");
        snprintf(s->mac, sizeof(s->mac), "52:54:00:%02x:%02x:%02x", random[0], random[1], random[2]);
        if (!WritePrivateProfileStringA("Network", "MAC", s->mac, file))
            return failure(error, size, "Could not save the network hardware address in your settings.");
    }
    return 1;
}

int network_save(const NetworkSettings *s, const char *file)
{
#define WRITE(key, value) if (!WritePrivateProfileStringA("Network", key, value, file)) return 0
    WRITE("EthernetGuid", s->ethernet_guid);
    WRITE("TapGuid", s->tap_guid);
    WRITE("Address", s->address);
    WRITE("Netmask", s->netmask);
    WRITE("Gateway", s->gateway);
    WRITE("DNS", s->dns);
    WRITE("MAC", s->mac);
    WRITE("AddressMode", s->dhcp ? "dhcp" : "static");
    WRITE("Mode", s->lan ? "lan" : "nat");
#undef WRITE
    return 1;
}

static int setup_command(char *exe, size_t exe_size, char *parameters, size_t size,
                          const char *ethernet, const char *tap, int check, int remove)
{
    char program[MAX_PATH], system[MAX_PATH], *slash;
    int n;
    if (!network_guid_valid(ethernet) || (check && !network_guid_valid(tap))) return 0;
    if (!GetModuleFileNameA(NULL, program, sizeof(program)) ||
        !GetSystemDirectoryA(system, sizeof(system))) return 0;
    slash = strrchr(program, '\\');
    if (!slash) return 0;
    *slash = '\0';
    n = snprintf(exe, exe_size, "%s\\WindowsPowerShell\\v1.0\\powershell.exe", system);
    if (n < 0 || (size_t)n >= exe_size) return 0;
    n = snprintf(parameters, size,
            "-NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"%s\\network\\lan-setup.ps1\""
            " -EthernetGuid %s%s%s%s", program, ethernet,
            check ? " -CheckOnly -TapGuid " : "", check ? tap : "", remove ? " -Remove" : "");
    return n >= 0 && (size_t)n < size;
}

int network_setup_dialog(HWND owner, const char *ethernet, int remove,
                          char *tap, char *error, size_t size)
{
    char exe[MAX_PATH + 64], parameters[1024], detail[512];
    SHELLEXECUTEINFOA info;
    DWORD code;
    if (!setup_command(exe, sizeof(exe), parameters, sizeof(parameters), ethernet, "", 0, remove))
        return failure(error, size, "Choose an Ethernet adapter before setting up LAN networking.");
    ZeroMemory(&info, sizeof(info));
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.hwnd = owner;
    info.lpVerb = "runas";
    info.lpFile = exe;
    info.lpParameters = parameters;
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExA(&info))
        return failure(error, size, "LAN setup did not start. Administrator approval is needed to install the network driver.");
    EnableWindow(owner, FALSE);
    /* The setup process owns rollback. Do not terminate it during bridge changes. */
    while (WaitForSingleObject(info.hProcess, 50) == WAIT_TIMEOUT) {
        MSG message;
        while (PeekMessageA(&message, NULL, 0, 0, PM_REMOVE)) {
            if (message.hwnd == owner && message.message == WM_CLOSE) continue;
            TranslateMessage(&message);
            DispatchMessageA(&message);
        }
    }
    GetExitCodeProcess(info.hProcess, &code);
    CloseHandle(info.hProcess);
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    if (code) {
        machine_setting("LastError", detail, sizeof(detail));
        if (*detail) return failure(error, size, detail);
        return failure(error, size, "LAN setup failed. See %PROGRAMDATA%\\AtariLAN\\Shinogi-lan-setup.log.");
    }
    machine_setting("TapGuid", tap, NETWORK_GUID_SIZE);
    return 1;
}

static int check_lan(const NetworkSettings *s, char *error, size_t size)
{
    char exe[MAX_PATH + 64], parameters[1024], command[1600];
    char managed[NETWORK_GUID_SIZE], ethernet[NETWORK_GUID_SIZE];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    DWORD code, waited;
    machine_setting("TapGuid", managed, sizeof(managed));
    machine_setting("EthernetGuid", ethernet, sizeof(ethernet));
    if (lstrcmpiA(managed, s->tap_guid) || lstrcmpiA(ethernet, s->ethernet_guid) ||
        !setup_command(exe, sizeof(exe), parameters, sizeof(parameters), s->ethernet_guid,
                        s->tap_guid, 1, 0))
        return failure(error, size, "LAN networking is not set up for this adapter. Open Settings and choose Set up LAN.");
    snprintf(command, sizeof(command), "\"%s\" %s", exe, parameters);
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));
    if (!CreateProcessA(exe, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
        return failure(error, size, "Could not check the LAN driver and bridge. Open Settings and run Set up LAN.");
    waited = WaitForSingleObject(pi.hProcess, 30000);
    if (waited != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, 1); /* CheckOnly never changes networking. */
        code = 1;
    } else {
        GetExitCodeProcess(pi.hProcess, &code);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (code)
        return failure(error, size, "The selected LAN adapter, TAP driver or managed bridge is unavailable.\n"
                       "Open Settings and run Set up LAN, or choose Internet networking.");
    return 1;
}

static char *read_file(const char *path)
{
    FILE *file = fopen(path, "rb");
    long length;
    char *text;
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) || (length = ftell(file)) < 0 || length > 262144 ||
        fseek(file, 0, SEEK_SET)) { fclose(file); return NULL; }
    text = malloc((size_t)length + 1);
    if (!text) { fclose(file); return NULL; }
    if (fread(text, 1, (size_t)length, file) != (size_t)length || ferror(file)) {
        free(text); fclose(file); return NULL;
    }
    text[length] = '\0';
    fclose(file);
    if (memchr(text, '\0', (size_t)length)) { free(text); return NULL; }
    return text;
}

static int write_atomic(const char *folder, const char *path, const char *text)
{
    char temp[MAX_PATH];
    FILE *out;
    int ok;
    if (!GetTempFileNameA(folder, "shn", 0, temp)) return 0;
    out = fopen(temp, "wb");
    if (!out) { DeleteFileA(temp); return 0; }
    ok = fwrite(text, 1, strlen(text), out) == strlen(text);
    if (fclose(out)) ok = 0;
    if (ok) ok = MoveFileExA(temp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok) DeleteFileA(temp);
    return ok;
}

static int prepare_guest(const NetworkSettings *s, const char *drivec, char *error, size_t size)
{
    char folder[MAX_PATH], config[MAX_PATH], backup[MAX_PATH], script_path[MAX_PATH], script[1024];
    char *old, *hook = NULL, *old_script;
    int changed, ok;
    if (snprintf(folder, sizeof(folder), "%s\\MINT\\1-19-CUR", drivec) >= (int)sizeof(folder) ||
        snprintf(config, sizeof(config), "%s\\MINT.CNF", folder) >= (int)sizeof(config) ||
        snprintf(backup, sizeof(backup), "%s\\SHNNET.BAK", folder) >= (int)sizeof(backup) ||
        snprintf(script_path, sizeof(script_path), "%s\\SHNNET.SH", folder) >= (int)sizeof(script_path))
        return failure(error, size, "The drive C folder path is too long for the guest network files.");
    if (s->lan && s->dhcp) {
        char client[MAX_PATH], callback[MAX_PATH];
        DWORD client_attrs, callback_attrs;
        if (snprintf(client, sizeof(client), "%s\\SYS-ROOT\\BIN\\DHCLIENT", folder) >= (int)sizeof(client) ||
            snprintf(callback, sizeof(callback), "%s\\SHNDHCP.SH", folder) >= (int)sizeof(callback))
            return failure(error, size, "The DHCP file paths are too long.");
        client_attrs = GetFileAttributesA(client);
        callback_attrs = GetFileAttributesA(callback);
        if (client_attrs == INVALID_FILE_ATTRIBUTES || callback_attrs == INVALID_FILE_ATTRIBUTES ||
            (client_attrs & FILE_ATTRIBUTE_DIRECTORY) || (callback_attrs & FILE_ATTRIBUTE_DIRECTORY))
            return failure(error, size, "The Atari DHCP files are missing. Reinstall the current Shinogi build with this drive C folder selected.");
    }
    old = read_file(config);
    if (!old) {
        if (!s->lan && GetFileAttributesA(config) == INVALID_FILE_ATTRIBUTES) return 1;
        return failure(error, size, "Could not read MINT\\1-19-CUR\\MINT.CNF in drive C.");
    }
    /* Legacy Internet-only installs already have the exact NAT setup.
     * Install the hook when LAN is first selected; subsequent switches
     * rewrite only our script, leaving MINT.CNF intact. */
    if (!s->lan && !strstr(old, SHINOGI_NETWORK_HOOK)) { free(old); return 1; }
    if (!network_boot_hook(old, &hook, error, size)) { free(old); return 0; }
    changed = strcmp(old, hook) != 0;
    old_script = read_file(script_path);
    if (!old_script && GetFileAttributesA(script_path) != INVALID_FILE_ATTRIBUTES) {
        free(old); free(hook);
        return failure(error, size, "Could not read the existing SHNNET.SH. It has not been changed.");
    }
    if (old_script && strncmp(old_script, "# Shinogi network settings v1;", 30)) {
        free(old_script); free(old); free(hook);
        return failure(error, size, "SHNNET.SH already exists and is not managed by Shinogi. Keep it under another name before enabling LAN.");
    }
    if (changed && !CopyFileA(config, backup, TRUE)) {
        char *saved = GetLastError() == ERROR_FILE_EXISTS ? read_file(backup) : NULL;
        int matching = saved && !strcmp(saved, old);
        free(saved);
        if (!matching) {
            free(old_script); free(old); free(hook);
            return failure(error, size, "Could not save a matching MINT.CNF backup. Keep any existing SHNNET.BAK under another name before enabling LAN.");
        }
    }
    ok = network_guest_script(s, script, sizeof(script));
    if (ok && (!old_script || strcmp(script, old_script))) ok = write_atomic(folder, script_path, script);
    if (ok && changed) {
        ok = write_atomic(folder, config, hook);
        if (!ok) {
            if (old_script) write_atomic(folder, script_path, old_script);
            else DeleteFileA(script_path);
        }
    }
    free(old_script); free(old); free(hook);
    if (!ok) return failure(error, size, "Could not save guest networking. Check drive C permissions and free space.");
    return 1;
}

int network_prepare(const NetworkSettings *s, const char *drivec,
                    char *options, size_t options_size, char *error, size_t size)
{
    NetworkAdapter adapters[64];
    int count, i;
    const char *tap_name = "";
    if (!network_config_valid(s, error, size)) return 0;
    if (s->lan) {
        if (!check_lan(s, error, size)) return 0;
        count = network_adapters(adapters, 64);
        for (i = 0; i < count; ++i)
            if (adapters[i].tap && !lstrcmpiA(adapters[i].guid, s->tap_guid)) tap_name = adapters[i].name;
        if (!*tap_name) return failure(error, size, "The managed TAP adapter is missing. Open Settings and run Set up LAN.");
    }
    if (!network_qemu_options(s, tap_name, options, options_size))
        return failure(error, size, "Could not construct the selected network configuration.");
    return prepare_guest(s, drivec, error, size);
}
