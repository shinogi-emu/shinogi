/*
 * shinogi.exe -- Windows launcher for the bundled QEMU.
 *
 * Everything ships in one directory tree:
 *
 *     shinogi.exe
 *     emutos-virt.elf
 *     qemu\qemu-system-m68kw.exe  (+ DLLs, share\, lib\)
 *
 * The launcher resolves its own location rather than relying on the
 * working directory, so shortcuts and "run as" both behave.
 *
 * Display backend: gtk by default. With an absolute pointing device
 * QEMU's SDL frontend grabs the pointer as soon as it enters a focused
 * window and only releases it at a window edge; where that grab also
 * stops motion being delivered the pointer is stuck until the window
 * loses focus. GTK never grabs while the device is absolute.
 *
 * Pass an argument to override, e.g. "shinogi.exe sdl", which is how
 * the SDL behaviour gets tested on a given host.
 */

#include <windows.h>
#include <stdio.h>

/* Logs go beside the guest image only if that is writable; a bundle
 * installed under Program Files is not, so use LOCALAPPDATA instead. */
static void log_dir(char *out, size_t n)
{
    const char *base = getenv("LOCALAPPDATA");

    if (!base || !*base) {
        base = getenv("TEMP");
    }
    if (!base || !*base) {
        base = ".";
    }

    _snprintf(out, n, "%s\\shinogi", base);
    out[n - 1] = '\0';
    CreateDirectoryA(out, NULL);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
    char exe[MAX_PATH], dir[MAX_PATH], logs[MAX_PATH];
    char cmd[4096];
    const char *display;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    DWORD len, code = 0;
    char *slash;

    (void)inst; (void)prev; (void)show;

    len = GetModuleFileNameA(NULL, exe, sizeof(exe));
    if (len == 0 || len >= sizeof(exe)) {
        MessageBoxA(NULL, "Could not determine the program location.",
                    "shinogi", MB_ICONERROR);
        return 1;
    }

    lstrcpynA(dir, exe, sizeof(dir));
    slash = strrchr(dir, '\\');
    if (!slash) {
        MessageBoxA(NULL, "Unexpected program path.", "shinogi", MB_ICONERROR);
        return 1;
    }
    *slash = '\0';

    display = (cmdline && *cmdline) ? cmdline : "gtk";
    log_dir(logs, sizeof(logs));

    _snprintf(cmd, sizeof(cmd),
              "\"%s\\qemu\\qemu-system-m68kw.exe\""
              " -M virt"
              " -m 128"
              " -kernel \"%s\\emutos-virt.elf\""
              " -device virtio-gpu-device"
              " -device virtio-keyboard-device"
              " -device virtio-tablet-device"
              " -display %s"
              " -serial \"file:%s\\shinogi-serial.log\""
              " -d guest_errors -D \"%s\\shinogi-guest-errors.log\"",
              dir, dir, display, logs, logs);
    cmd[sizeof(cmd) - 1] = '\0';

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE,
                        0, NULL, dir, &si, &pi)) {
        char msg[4608];
        _snprintf(msg, sizeof(msg),
                  "Could not start the bundled QEMU (error %lu).\n\n%s",
                  (unsigned long)GetLastError(), cmd);
        msg[sizeof(msg) - 1] = '\0';
        MessageBoxA(NULL, msg, "shinogi", MB_ICONERROR);
        return 1;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (code != 0) {
        char msg[512];
        _snprintf(msg, sizeof(msg),
                  "QEMU exited with code %lu.\n\nLogs are in:\n%s",
                  (unsigned long)code, logs);
        msg[sizeof(msg) - 1] = '\0';
        MessageBoxA(NULL, msg, "shinogi", MB_ICONWARNING);
    }

    return (int)code;
}
