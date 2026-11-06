/* Exercise the actual transaction against a stateful SetupAPI boundary. */
#define INSTALL_TAP_TEST
#include "../../tools/win/install-tap.c"
#include <assert.h>

static const WCHAR expected_instance[] = L"ROOT\\AtariTAP\\{01234567-89AB-CDEF-0123-456789ABCDEF}";
static const WCHAR expected_guid[] = L"{11234567-89AB-CDEF-0123-456789ABCDEF}";
static const WCHAR published[] = L"C:\\Windows\\INF\\oem42.inf";
static SP_DEVINFO_DATA *owned;
static int fail_at, step, sets, lists, keys, creates, registers, installs, removes;
static BOOL fail_rollback, need_reboot, malformed_guid, no_guid, flags_reboot, registered_node;
static DWORD last_error;
static BOOL progress_probe, fail_remove_parameters, removal_reboot;
static SP_DEVINSTALL_PARAMS_W installed_parameters;
static BOOL next(void) { ++step; if (step == fail_at) { last_error = 1234; return FALSE; } return TRUE; }
static void own(HDEVINFO set, SP_DEVINFO_DATA *device) {
    assert(set == (HDEVINFO)(uintptr_t)42); assert(device == owned); assert(device->DevInst == 99);
}
DWORD GetLastError(void) { return last_error; }
static BOOL fresh_instance(WCHAR instance[TAP_ID_CAPACITY]) {
    if (!next()) return FALSE;
    wcscpy(instance, expected_instance); return TRUE;
}
static BOOL verify_tap_inf(const WCHAR *path) { assert(!wcscmp(path, published)); return next(); }
BOOL SetupCopyOEMInfW(const WCHAR *source, const WCHAR *media, DWORD type, DWORD flags,
    WCHAR *destination, DWORD capacity, DWORD *required, WCHAR **component) {
    assert(!wcscmp(source, L"C:\\package\\OemVista.inf")); assert(!media && type == SPOST_PATH && !flags);
    assert(capacity == MAX_PATH && !required && !component);
    if (progress_probe) assert(getchar() == 'x'); /* parent must first receive flushed identity */
    if (!next()) return FALSE;
    wcscpy(destination, published); return TRUE;
}
HDEVINFO SetupDiCreateDeviceInfoList(const GUID *guid, void *window) {
    assert(!memcmp(guid, &tap_net_class, sizeof(*guid)) && !window);
    if (!next()) return INVALID_HANDLE_VALUE;
    ++sets; return (HDEVINFO)(uintptr_t)42;
}
BOOL SetupDiCreateDeviceInfoW(HDEVINFO set, const WCHAR *name, const GUID *guid, const WCHAR *description,
    void *window, DWORD flags, SP_DEVINFO_DATA *device) {
    assert(set == (HDEVINFO)(uintptr_t)42 && !wcscmp(name, expected_instance));
    assert(!memcmp(guid, &tap_net_class, sizeof(*guid)) && description && !window && !flags);
    assert(device->cbSize == sizeof(*device));
    if (!next()) return FALSE;
    owned = device; device->DevInst = 99; ++creates; return TRUE;
}
BOOL SetupDiSetDeviceRegistryPropertyW(HDEVINFO set, SP_DEVINFO_DATA *device, DWORD property, const BYTE *data, DWORD size) {
    static const WCHAR expected[] = L"tap0901\0";
    own(set, device); assert(property == SPDRP_HARDWAREID && size == sizeof(expected));
    assert(!memcmp(data, expected, sizeof(expected))); return next();
}
BOOL SetupDiGetDeviceInstallParamsW(HDEVINFO set, SP_DEVINFO_DATA *device, SP_DEVINSTALL_PARAMS_W *parameters) {
    own(set, device); assert(parameters->cbSize == sizeof(*parameters));
    if (!next()) return FALSE;
    if ((installs && flags_reboot) || (removes && removal_reboot)) parameters->Flags |= DI_NEEDRESTART;
    return TRUE;
}
BOOL SetupDiSetDeviceInstallParamsW(HDEVINFO set, SP_DEVINFO_DATA *device, SP_DEVINSTALL_PARAMS_W *parameters) {
    own(set, device);
    assert((parameters->Flags & (DI_ENUMSINGLEINF | DI_QUIETINSTALL)) == (DI_ENUMSINGLEINF | DI_QUIETINSTALL));
    assert(parameters->FlagsEx & DI_FLAGSEX_ALLOWEXCLUDEDDRVS);
    assert(!wcscmp(parameters->DriverPath, published)); installed_parameters = *parameters; return next();
}
BOOL SetupDiBuildDriverInfoList(HDEVINFO set, SP_DEVINFO_DATA *device, DWORD type) {
    own(set, device); assert(type == SPDIT_COMPATDRIVER);
    assert(!wcscmp(installed_parameters.DriverPath, published));
    if (!next()) return FALSE;
    ++lists; return TRUE;
}
BOOL SetupDiEnumDriverInfoW(HDEVINFO set, SP_DEVINFO_DATA *device, DWORD type, DWORD index, SP_DRVINFO_DATA_W *driver) {
    own(set, device); assert(type == SPDIT_COMPATDRIVER && index == 0 && driver->cbSize == sizeof(*driver));
    if (!next()) return FALSE;
    driver->Reserved = 777; return TRUE;
}
BOOL SetupDiSetSelectedDriverW(HDEVINFO set, SP_DEVINFO_DATA *device, SP_DRVINFO_DATA_W *driver) {
    own(set, device); assert(driver->Reserved == 777); return next();
}
BOOL SetupDiCallClassInstaller(DWORD function, HDEVINFO set, SP_DEVINFO_DATA *device) {
    own(set, device);
    if (function == DIF_REGISTERDEVICE) {
        ++registers; registered_node = TRUE; // register can partially succeed before reporting error
        return next();
    }
    assert(function == DIF_REMOVE && registered_node);
    ++removes;
    if (fail_rollback) { last_error = 4321; return FALSE; }
    registered_node = FALSE; return TRUE;
}
BOOL SetupDiSetClassInstallParamsW(HDEVINFO set, SP_DEVINFO_DATA *device, SP_CLASSINSTALL_HEADER *header, DWORD size) {
    SP_REMOVEDEVICE_PARAMS *remove = (SP_REMOVEDEVICE_PARAMS *)header;
    own(set, device);
    assert(size == sizeof(*remove) && header->cbSize == sizeof(*header) && header->InstallFunction == DIF_REMOVE);
    assert(remove->Scope == DI_REMOVEDEVICE_GLOBAL && remove->HwProfile == 0);
    if (fail_remove_parameters) { last_error = 4322; return FALSE; }
    return TRUE;
}
BOOL SetupDiDestroyDriverInfoList(HDEVINFO set, SP_DEVINFO_DATA *device, DWORD type) {
    own(set, device); assert(type == SPDIT_COMPATDRIVER); --lists; return TRUE;
}
BOOL SetupDiDestroyDeviceInfoList(HDEVINFO set) { assert(set == (HDEVINFO)(uintptr_t)42); --sets; return TRUE; }
static BOOL install_one_device(HDEVINFO set, SP_DEVINFO_DATA *device, SP_DRVINFO_DATA_W *driver, BOOL *reboot) {
    own(set, device); assert(driver && driver->Reserved == 777 && registered_node);
    ++installs; *reboot = need_reboot; return next();
}
HKEY SetupDiOpenDevRegKey(HDEVINFO set, SP_DEVINFO_DATA *device, DWORD scope, DWORD profile, DWORD type, DWORD access) {
    own(set, device); assert(scope == DICS_FLAG_GLOBAL && !profile && type == DIREG_DRV && access == KEY_QUERY_VALUE);
    if (no_guid) return INVALID_HANDLE_VALUE;
    ++keys; return (HKEY)(uintptr_t)55;
}
LONG RegQueryValueExW(HKEY key, const WCHAR *name, void *reserved, DWORD *type, BYTE *buffer, DWORD *size) {
    assert(key == (HKEY)(uintptr_t)55 && !wcscmp(name, L"NetCfgInstanceId") && !reserved);
    assert(*size >= sizeof(expected_guid)); memcpy(buffer, expected_guid, sizeof(expected_guid));
    *size = sizeof(expected_guid); *type = malformed_guid ? REG_BINARY : REG_SZ; return ERROR_SUCCESS;
}
LONG RegCloseKey(HKEY key) { assert(key == (HKEY)(uintptr_t)55); --keys; return ERROR_SUCCESS; }
void Sleep(DWORD milliseconds) { assert(milliseconds == 100); }
static void reset(void) {
    assert(!sets && !lists && !keys);
    owned = NULL; fail_at = step = creates = registers = installs = removes = 0;
    fail_rollback = need_reboot = malformed_guid = no_guid = flags_reboot = registered_node = FALSE;
    fail_remove_parameters = removal_reboot = FALSE;
    last_error = 0; memset(&installed_parameters, 0, sizeof(installed_parameters));
}
static int run(struct tap_install_result *result) {
    int status = install_tap(L"C:\\package\\OemVista.inf", result);
    print_result(result, status);
    assert(!sets && !lists && !keys); assert(creates <= 1 && registers <= 1 && installs <= 1 && removes <= 1);
    return status;
}
int main(int argc, char **argv) {
    struct tap_install_result result;
    int steps, i;
    if (argc == 2 && !strcmp(argv[1], "--broken-progress")) {
        reset(); assert(freopen("/dev/full", "w", stdout));
        assert(install_tap(L"C:\\package\\OemVista.inf", &result) == 2);
        assert(step == 1 && result.error == ERROR_WRITE_FAULT && !sets && !creates && !result.instance[0]);
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--progress-probe")) {
        reset(); progress_probe = TRUE; assert(run(&result) == 0); return 0;
    }
    reset(); assert(run(&result) == 0); steps = step;
    assert(!wcscmp(result.instance, expected_instance) && !wcscmp(result.guid, expected_guid));
    assert(creates == 1 && registers == 1 && installs == 1 && removes == 0 && registered_node);
    for (i = 1; i <= steps; ++i) {
        reset(); fail_at = i; assert(run(&result) == 2);
        assert(result.error == 1234 && !result.recovery_required && !registered_node && !result.instance[0]);
        assert(removes == (registers ? 1 : 0));
    }
    reset(); fail_at = steps - 1; fail_rollback = TRUE; assert(run(&result) == 2);
    assert(result.recovery_required && result.rollback_error == 4321 && registered_node);
    assert(!wcscmp(result.instance, expected_instance));
    reset(); fail_at = steps - 1; fail_remove_parameters = TRUE; assert(run(&result) == 2);
    assert(result.recovery_required && result.rollback_error == 4322 && registered_node && !removes);
    assert(!wcscmp(result.instance, expected_instance));
    reset(); fail_at = steps - 1; removal_reboot = TRUE; assert(run(&result) == 2);
    assert(result.recovery_required && result.reboot_required && !wcscmp(result.instance, expected_instance));
    reset(); need_reboot = TRUE; no_guid = TRUE; assert(run(&result) == 1);
    assert(result.reboot_required && !result.guid[0] && removes == 0);
    reset(); flags_reboot = TRUE; assert(run(&result) == 1); assert(result.reboot_required);
    reset(); no_guid = TRUE; assert(run(&result) == 2); assert(removes == 1 && !registered_node);
    reset(); malformed_guid = TRUE; assert(run(&result) == 2); assert(removes == 1 && !registered_node);
    puts("PASS targeted TAP install, per-stage failures, own-node rollback, recovery identity and reboot/GUID states");
    return 0;
}
