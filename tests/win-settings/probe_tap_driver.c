#define UNICODE
#define _UNICODE
#include <windows.h>
#include <winioctl.h>
#include <stdio.h>

static int control(HANDLE device, DWORD code, ULONG *data, DWORD size, DWORD *length)
{
    OVERLAPPED io = {0};
    BOOL ok;
    io.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!io.hEvent) return 0;
    ok = DeviceIoControl(device, code, data, size, data, size, length, &io);
    if (!ok && GetLastError() == ERROR_IO_PENDING)
        ok = GetOverlappedResult(device, &io, length, TRUE);
    CloseHandle(io.hEvent);
    return ok != FALSE;
}

int wmain(int argc, WCHAR **argv)
{
    WCHAR path[128];
    HANDLE device;
    ULONG version[3] = {0}, media = 1;
    DWORD bytes = 0;
    int ok;
    if (argc != 2 || wcslen(argv[1]) != 38) return 2;
    if (swprintf(path, sizeof(path) / sizeof(path[0]), L"\\\\.\\Global\\%ls.tap", argv[1]) < 0) return 2;
    device = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                         FILE_ATTRIBUTE_SYSTEM | FILE_FLAG_OVERLAPPED, NULL);
    if (device == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "Open TAP failed: %lu\n", (unsigned long)GetLastError()); return 1;
    }
    ok = control(device, CTL_CODE(FILE_DEVICE_UNKNOWN, 2, METHOD_BUFFERED, FILE_ANY_ACCESS),
                 version, sizeof(version), &bytes) && bytes == sizeof(version) && version[0] == 9;
    if (ok) ok = control(device, CTL_CODE(FILE_DEVICE_UNKNOWN, 6, METHOD_BUFFERED, FILE_ANY_ACCESS), &media, sizeof(media), &bytes);
    media = 0;
    if (!control(device, CTL_CODE(FILE_DEVICE_UNKNOWN, 6, METHOD_BUFFERED, FILE_ANY_ACCESS), &media, sizeof(media), &bytes)) ok = 0;
    CloseHandle(device);
    printf("TAP %lu.%lu, open/version/media-up/media-down/close: %s\n",
           (unsigned long)version[0], (unsigned long)version[1], ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
