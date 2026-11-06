/* API boundary for native tests of the production new-device transaction. */
#ifndef INSTALL_TAP_TEST_API_H
#define INSTALL_TAP_TEST_API_H
#include <stdint.h>
#include <stddef.h>
#include <wchar.h>
typedef uint32_t DWORD;
typedef int32_t LONG;
typedef int BOOL;
typedef wchar_t WCHAR;
typedef unsigned char BYTE;
typedef void *HDEVINFO;
typedef void *HKEY;
typedef struct { uint32_t Data1; uint16_t Data2, Data3; uint8_t Data4[8]; } GUID;
typedef struct { DWORD cbSize; GUID ClassGuid; DWORD DevInst; uintptr_t Reserved; } SP_DEVINFO_DATA;
typedef struct { DWORD cbSize, Flags, FlagsEx; WCHAR DriverPath[260]; } SP_DEVINSTALL_PARAMS_W;
typedef struct { DWORD cbSize; uintptr_t Reserved; } SP_DRVINFO_DATA_W;
typedef struct { DWORD cbSize, InstallFunction; } SP_CLASSINSTALL_HEADER;
typedef struct { SP_CLASSINSTALL_HEADER ClassInstallHeader; DWORD Scope, HwProfile; } SP_REMOVEDEVICE_PARAMS;
#define TRUE 1
#define FALSE 0
#define MAX_PATH 260
#define INVALID_HANDLE_VALUE ((void *)(intptr_t)-1)
#define ERROR_SUCCESS 0
#define ERROR_ACCESS_DENIED 5
#define ERROR_WRITE_FAULT 29
#define ERROR_NOT_FOUND 1168
#define REG_SZ 1
#define REG_BINARY 3
#define KEY_QUERY_VALUE 1
#define SPOST_PATH 1
#define SPDRP_HARDWAREID 1
#define DI_ENUMSINGLEINF 0x10000
#define DI_QUIETINSTALL 0x800000
#define DI_FLAGSEX_ALLOWEXCLUDEDDRVS 0x800
#define SPDIT_COMPATDRIVER 2
#define DIF_REGISTERDEVICE 0x19
#define DIF_REMOVE 5
#define DI_REMOVEDEVICE_GLOBAL 1
#define DICS_FLAG_GLOBAL 1
#define DIREG_DRV 2
#define DI_NEEDRESTART 0x80
#define DI_NEEDREBOOT 0x100
DWORD GetLastError(void);
BOOL SetupCopyOEMInfW(const WCHAR *, const WCHAR *, DWORD, DWORD, WCHAR *, DWORD, DWORD *, WCHAR **);
HDEVINFO SetupDiCreateDeviceInfoList(const GUID *, void *);
BOOL SetupDiCreateDeviceInfoW(HDEVINFO, const WCHAR *, const GUID *, const WCHAR *, void *, DWORD, SP_DEVINFO_DATA *);
BOOL SetupDiSetDeviceRegistryPropertyW(HDEVINFO, SP_DEVINFO_DATA *, DWORD, const BYTE *, DWORD);
BOOL SetupDiGetDeviceInstallParamsW(HDEVINFO, SP_DEVINFO_DATA *, SP_DEVINSTALL_PARAMS_W *);
BOOL SetupDiSetDeviceInstallParamsW(HDEVINFO, SP_DEVINFO_DATA *, SP_DEVINSTALL_PARAMS_W *);
BOOL SetupDiBuildDriverInfoList(HDEVINFO, SP_DEVINFO_DATA *, DWORD);
BOOL SetupDiEnumDriverInfoW(HDEVINFO, SP_DEVINFO_DATA *, DWORD, DWORD, SP_DRVINFO_DATA_W *);
BOOL SetupDiSetSelectedDriverW(HDEVINFO, SP_DEVINFO_DATA *, SP_DRVINFO_DATA_W *);
BOOL SetupDiCallClassInstaller(DWORD, HDEVINFO, SP_DEVINFO_DATA *);
BOOL SetupDiSetClassInstallParamsW(HDEVINFO, SP_DEVINFO_DATA *, SP_CLASSINSTALL_HEADER *, DWORD);
BOOL SetupDiDestroyDriverInfoList(HDEVINFO, SP_DEVINFO_DATA *, DWORD);
BOOL SetupDiDestroyDeviceInfoList(HDEVINFO);
HKEY SetupDiOpenDevRegKey(HDEVINFO, SP_DEVINFO_DATA *, DWORD, DWORD, DWORD, DWORD);
LONG RegQueryValueExW(HKEY, const WCHAR *, void *, DWORD *, BYTE *, DWORD *);
LONG RegCloseKey(HKEY);
void Sleep(DWORD);
#endif
