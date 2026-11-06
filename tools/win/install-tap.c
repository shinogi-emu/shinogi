/* Install the pinned TAP package on one new device; never update by hardware ID.
 * Build: x86_64-w64-mingw32-gcc -std=c11 -Wall -Wextra -Werror -municode
 *   install-tap.c -o install-tap.exe -lsetupapi -lnewdev -ladvapi32 -lole32
 * The caller must elevate explicitly. No physical adapters are configured.
 */
#ifdef INSTALL_TAP_TEST
#include "install-tap-test-api.h"
#else
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <setupapi.h>
#include <newdev.h>
#include <wincrypt.h>
#include <objbase.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define TAP_ID_CAPACITY 256
static const GUID tap_net_class = {0x4d36e972, 0xe325, 0x11ce, {0xbf,0xc1,0x08,0x00,0x2b,0xe1,0x03,0x18}};

struct tap_install_result {
    WCHAR instance[TAP_ID_CAPACITY];
    WCHAR guid[39];
    BOOL reboot_required;
    BOOL recovery_required;
    DWORD error;
    DWORD rollback_error;
    const char *step;
};

static void print_result(const struct tap_install_result *result, int status);
static BOOL verify_tap_inf(const WCHAR *path);
static BOOL fresh_instance(WCHAR instance[TAP_ID_CAPACITY]);
static BOOL install_one_device(HDEVINFO set, SP_DEVINFO_DATA *device, SP_DRVINFO_DATA_W *driver, BOOL *reboot);

static BOOL tap_guid(const WCHAR *text)
{
    size_t i;
    if (wcslen(text) != 38 || text[0] != L'{' || text[37] != L'}') return FALSE;
    for (i = 1; i < 37; ++i) {
        WCHAR c = text[i];
        if (i == 9 || i == 14 || i == 19 || i == 24) {
            if (c != L'-') return FALSE;
        } else if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F'))) {
            return FALSE;
        }
    }
    return TRUE;
}

static BOOL query_tap_guid(HDEVINFO set, SP_DEVINFO_DATA *device, WCHAR guid[39])
{
    HKEY key = SetupDiOpenDevRegKey(set, device, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_QUERY_VALUE);
    WCHAR text[40] = {0};
    DWORD type = 0, bytes = 39 * sizeof(WCHAR);
    LONG status;
    if (key == INVALID_HANDLE_VALUE) return FALSE;
    status = RegQueryValueExW(key, L"NetCfgInstanceId", NULL, &type, (BYTE *)text, &bytes);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS || type != REG_SZ || bytes < 38 * sizeof(WCHAR)
        || bytes > 39 * sizeof(WCHAR) || bytes % sizeof(WCHAR) || !tap_guid(text)) return FALSE;
    memcpy(guid, text, 39 * sizeof(WCHAR));
    return TRUE;
}

static int install_tap(const WCHAR *source_inf, struct tap_install_result *result)
{
    HDEVINFO set = INVALID_HANDLE_VALUE;
    SP_DEVINFO_DATA device;
    SP_DEVINSTALL_PARAMS_W parameters;
    SP_DRVINFO_DATA_W driver;
    WCHAR published_inf[MAX_PATH] = {0};
    static const WCHAR hardware_id[] = L"tap0901\0"; /* MULTI_SZ, double NUL */
    BOOL registered = FALSE, driver_list = FALSE;
    int status = 2;
    unsigned attempt;
    memset(result, 0, sizeof(*result));
    memset(&device, 0, sizeof(device)); device.cbSize = sizeof(device);
    memset(&parameters, 0, sizeof(parameters)); parameters.cbSize = sizeof(parameters);
    memset(&driver, 0, sizeof(driver)); driver.cbSize = sizeof(driver);

#define REQUIRE(call, name) do { if (!(call)) { result->step = (name); result->error = GetLastError(); goto cleanup; } } while (0)
    REQUIRE(fresh_instance(result->instance), "generate fresh TAP device identity");
    print_result(result, -1);
    /* Abort before mutation if the caller cannot receive recovery identity. */
    if (fflush(stdout) == EOF || ferror(stdout)) {
        result->step = "publish new device identity"; result->error = ERROR_WRITE_FAULT; goto cleanup;
    }
    REQUIRE(SetupCopyOEMInfW(source_inf, NULL, SPOST_PATH, 0, published_inf, MAX_PATH, NULL, NULL), "stage signed INF");
    /* Recheck the protected published INF, not only a caller-writable source. */
    REQUIRE(verify_tap_inf(published_inf), "validate staged TAP INF");
    set = SetupDiCreateDeviceInfoList(&tap_net_class, NULL);
    REQUIRE(set != INVALID_HANDLE_VALUE, "create device information set");
    REQUIRE(SetupDiCreateDeviceInfoW(set, result->instance, &tap_net_class, L"Atari TAP adapter", NULL,
        0, &device), "create fresh TAP device element");
    REQUIRE(SetupDiSetDeviceRegistryPropertyW(set, &device, SPDRP_HARDWAREID,
        (const BYTE *)hardware_id, sizeof(hardware_id)), "set new TAP hardware ID");
    REQUIRE(SetupDiGetDeviceInstallParamsW(set, &device, &parameters), "read new-device install parameters");
    parameters.Flags |= DI_ENUMSINGLEINF | DI_QUIETINSTALL;
    parameters.FlagsEx |= DI_FLAGSEX_ALLOWEXCLUDEDDRVS;
    wcscpy(parameters.DriverPath, published_inf);
    REQUIRE(SetupDiSetDeviceInstallParamsW(set, &device, &parameters), "restrict driver search to staged INF");
    REQUIRE(SetupDiBuildDriverInfoList(set, &device, SPDIT_COMPATDRIVER), "build new-device driver list");
    driver_list = TRUE;
    REQUIRE(SetupDiEnumDriverInfoW(set, &device, SPDIT_COMPATDRIVER, 0, &driver), "select staged TAP driver");
    REQUIRE(SetupDiSetSelectedDriverW(set, &device, &driver), "set new-device driver");
    /* Only this newly created element is ever registered, installed or removed.
       A failed register may have partially registered it, so attempt own-node
       rollback even when DIF_REGISTERDEVICE reports failure. */
    registered = TRUE;
    REQUIRE(SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set, &device), "register fresh TAP device");
    REQUIRE(install_one_device(set, &device, &driver, &result->reboot_required), "install driver on new TAP device");
    REQUIRE(SetupDiGetDeviceInstallParamsW(set, &device, &parameters), "read restart flags");
    if (parameters.Flags & (DI_NEEDREBOOT | DI_NEEDRESTART)) result->reboot_required = TRUE;
    for (attempt = 0; attempt < 50; ++attempt) {
        if (query_tap_guid(set, &device, result->guid)) break;
        if (result->reboot_required) break;
        Sleep(100);
    }
    if (!result->guid[0] && !result->reboot_required) {
        result->step = "read new adapter GUID"; result->error = ERROR_NOT_FOUND; goto cleanup;
    }
    status = result->reboot_required ? 1 : 0;
cleanup:
    if (status > 1 && registered) {
        SP_REMOVEDEVICE_PARAMS remove;
        memset(&remove, 0, sizeof(remove));
        remove.ClassInstallHeader.cbSize = sizeof(remove.ClassInstallHeader);
        remove.ClassInstallHeader.InstallFunction = DIF_REMOVE;
        remove.Scope = DI_REMOVEDEVICE_GLOBAL;
        if (!SetupDiSetClassInstallParamsW(set, &device, &remove.ClassInstallHeader, sizeof(remove))
            || !SetupDiCallClassInstaller(DIF_REMOVE, set, &device)) {
            result->recovery_required = TRUE; result->rollback_error = GetLastError();
        } else {
            /* A successful remove can still require a restart. Keep the known
               identity until removal is complete rather than declaring it gone. */
            if (!SetupDiGetDeviceInstallParamsW(set, &device, &parameters)) {
                result->recovery_required = TRUE; result->rollback_error = GetLastError();
            } else if (parameters.Flags & (DI_NEEDREBOOT | DI_NEEDRESTART)) {
                result->reboot_required = TRUE; result->recovery_required = TRUE;
            }
        }
        result->guid[0] = 0;
    }
    if (status > 1 && !result->recovery_required) result->instance[0] = 0;
    if (driver_list) SetupDiDestroyDriverInfoList(set, &device, SPDIT_COMPATDRIVER);
    if (set != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(set);
    return status;
#undef REQUIRE
}

static void json_string(const WCHAR *text)
{
    putchar('"');
    while (*text) {
        unsigned c = (unsigned)*text++;
        if (c == '"' || c == '\\') { putchar('\\'); putchar((int)c); }
        else if (c >= 32 && c < 127) putchar((int)c);
        else printf("\\u%04x", c & 0xffff);
    }
    putchar('"');
}

static void print_result(const struct tap_install_result *result, int status)
{
    fputs("{\"deviceInstanceId\":", stdout);
    if (result->instance[0]) json_string(result->instance); else fputs("null", stdout);
    fputs(",\"guid\":", stdout);
    if (result->guid[0]) json_string(result->guid); else fputs("null", stdout);
    printf(",\"progress\":%s,\"rebootRequired\":%s,\"recoveryRequired\":%s,\"success\":%s,\"error\":%lu,\"rollbackError\":%lu}\n",
        status == -1 ? "true" : "false", result->reboot_required ? "true" : "false", result->recovery_required ? "true" : "false",
        status >= 0 && status <= 1 ? "true" : "false", (unsigned long)result->error, (unsigned long)result->rollback_error);
}

#ifndef INSTALL_TAP_TEST
/* Pinned official tap-windows6 9.27.0 win10/amd64 OemVista.inf. The CAT/SYS are
   subsequently verified by Windows package staging/device installation. */
static BOOL verify_tap_inf(const WCHAR *path)
{
    static const BYTE expected[32] = {
        0x13,0x27,0xab,0x3a,0x8c,0x50,0x69,0x1f,0x04,0xbe,0xa8,0xe2,0xca,0x35,0x6c,0x5b,
        0x60,0x40,0x92,0xa7,0x19,0xe2,0x19,0x46,0x4f,0x8c,0xc4,0xb4,0x2e,0x19,0x2d,0xe9
    };
    HANDLE file = INVALID_HANDLE_VALUE;
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    BYTE buffer[4096], digest[32];
    DWORD bytes, length = sizeof(digest), error = ERROR_SUCCESS;
    BOOL ok = FALSE;
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    if (!CryptAcquireContextW(&provider, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)
        || !CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash)) goto cleanup;
    for (;;) {
        if (!ReadFile(file, buffer, sizeof(buffer), &bytes, NULL)) goto cleanup;
        if (!bytes) break;
        if (!CryptHashData(hash, buffer, bytes, 0)) goto cleanup;
    }
    if (!CryptGetHashParam(hash, HP_HASHVAL, digest, &length, 0)) goto cleanup;
    ok = length == sizeof(expected) && !memcmp(expected, digest, sizeof(expected));
    error = ok ? ERROR_SUCCESS : ERROR_INVALID_DATA;
cleanup:
    if (!ok && error == ERROR_SUCCESS) error = GetLastError();
    if (hash) CryptDestroyHash(hash);
    if (provider) CryptReleaseContext(provider, 0);
    CloseHandle(file);
    SetLastError(error);
    return ok;
}

static BOOL install_one_device(HDEVINFO set, SP_DEVINFO_DATA *device, SP_DRVINFO_DATA_W *driver, BOOL *reboot)
{
    typedef BOOL (WINAPI *InstallFn)(HWND, HDEVINFO, PSP_DEVINFO_DATA, PSP_DRVINFO_DATA_W, DWORD, PBOOL);
    WCHAR path[MAX_PATH];
    UINT length = GetSystemDirectoryW(path, MAX_PATH);
    HMODULE library;
    FARPROC address;
    InstallFn install;
    BOOL success;
    DWORD error;
    if (!length || length + wcslen(L"\\newdev.dll") >= MAX_PATH) {
        SetLastError(ERROR_BAD_PATHNAME); return FALSE;
    }
    wcscat(path, L"\\newdev.dll");
    library = LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!library) return FALSE;
    address = GetProcAddress(library, "DiInstallDevice");
    _Static_assert(sizeof(install) == sizeof(address), "Windows procedure pointer size");
    memcpy(&install, &address, sizeof(install));
    if (!install) { FreeLibrary(library); SetLastError(ERROR_PROC_NOT_FOUND); return FALSE; }
    success = install(NULL, set, device, driver, 0, reboot);
    error = GetLastError();
    FreeLibrary(library);
    SetLastError(error);
    return success;
}

static BOOL fresh_instance(WCHAR instance[TAP_ID_CAPACITY])
{
    GUID uuid;
    WCHAR text[39];
    if (FAILED(CoCreateGuid(&uuid)) || !StringFromGUID2(&uuid, text, 39)) {
        SetLastError(ERROR_GEN_FAILURE); return FALSE;
    }
    wcscpy(instance, L"ROOT\\AtariTAP\\");
    wcscat(instance, text);
    return TRUE;
}

static BOOL is_administrator(void)
{
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    PSID administrators = NULL;
    BOOL member = FALSE;
    if (AllocateAndInitializeSid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
        0, 0, 0, 0, 0, 0, &administrators)) {
        CheckTokenMembership(NULL, administrators, &member);
        FreeSid(administrators);
    }
    return member;
}

int wmain(int argc, WCHAR **argv)
{
    struct tap_install_result result;
    WCHAR full_path[MAX_PATH];
    DWORD length;
    int status;
    memset(&result, 0, sizeof(result));
    if (!is_administrator()) {
        fputs("Run Atari TAP Setup and approve its administrator prompt.\n", stderr);
        result.error = ERROR_ACCESS_DENIED; print_result(&result, 2); return 2;
    }
    if (argc != 2 || !(length = GetFullPathNameW(argv[1], MAX_PATH, full_path, NULL)) || length >= MAX_PATH
        || !verify_tap_inf(full_path)) {
        fputs("Expected the pinned official TAP-Windows 9.27.0 amd64 INF.\n", stderr);
        result.error = ERROR_INVALID_DATA; print_result(&result, 2); return 2;
    }
    status = install_tap(full_path, &result);
    if (status > 1) fprintf(stderr, "TAP setup failed at %s (Windows error %lu); own-node rollback error %lu.\n",
        result.step ? result.step : "validation", (unsigned long)result.error, (unsigned long)result.rollback_error);
    print_result(&result, status);
    return status;
}
#endif
