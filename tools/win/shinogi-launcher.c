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
 * Drive C is a folder on the host, exposed through QEMU's vvfat driver
 * as a FAT16 block device on virtio-blk.
 *
 * It is not 9p, which is what the Linux build has used until now: QEMU
 * cannot build virtfs on Windows at all (meson.build requires host_os to
 * be linux, darwin or freebsd), so the Windows binary carries the
 * virtio-9p device name with none of the implementation behind it.
 * vvfat is in every build, and EmuTOS reads the DOS MBR and FAT16 it
 * synthesises using its own stock filesystem code.
 *
 * readonly=on is mandatory, not caution: virtio-blk asks for write
 * permission when it opens the drive, and QEMU refuses to start at all
 * without it. Writes are refused by the guest driver as well -- vvfat's
 * read-write mode is documented experimental and is being evaluated
 * separately.
 *
 * Display backend: sdl by default, the same as the other platforms so
 * all three bundles behave alike. Verified working on Windows.
 *
 * Pass an argument to override, e.g. "shinogi.exe gtk". That matters
 * on hosts where SDL is unusable: with an absolute pointing device SDL
 * grabs the pointer as soon as it enters a *focused* window and only
 * releases it at a window edge, so on a host where the grab also stops
 * motion being delivered the pointer is frozen until the window loses
 * focus. GTK never grabs while the device is absolute, and adds a
 * menubar and window scaling that SDL does not have.
 */

#include <windows.h>
#include <stdio.h>

/* Supplied by the build; see tools/make-windows-package.sh. Kept out of
 * the source so the version lives in exactly one file, VERSION. */
#ifndef SHINOGI_VERSION
#error "SHINOGI_VERSION not defined - build through tools/make-windows-package.sh"
#endif

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
    char drivec[MAX_PATH];
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

    /*
     * gtk needs zoom-to-fit turned off, because it defaults to on for
     * virtio-gpu and then scales our fixed 1280x720 into whatever window
     * size GTK chose; scale enlarges it afterwards. SDL has neither
     * option, so it is passed through untouched.
     */
    if (!cmdline || !*cmdline) {
        display = "sdl";
    } else if (lstrcmpiA(cmdline, "gtk") == 0) {
        display = "gtk,zoom-to-fit=off,scale=1.5";
    } else {
        display = cmdline;
    }
    log_dir(logs, sizeof(logs));

    /* The host folder the guest sees as drive C:. */
    {
        const char *profile = getenv("USERPROFILE");
        _snprintf(drivec, sizeof(drivec), "%s\\shinogi-drive-c",
                  (profile && *profile) ? profile : dir);
        drivec[sizeof(drivec) - 1] = '\0';
        CreateDirectoryA(drivec, NULL);
    }

    _snprintf(cmd, sizeof(cmd),
              "\"%s\\qemu\\qemu-system-m68kw.exe\""
              " -name \"shinogi " SHINOGI_VERSION "\""
              " -M virt"
              " -m 128"
              " -kernel \"%s\\emutos-virt.elf\""
              " -device virtio-gpu-device"
              " -device virtio-keyboard-device"
              " -device virtio-tablet-device"
              " -drive \"file=fat:%s,format=raw,if=none,id=hostblk,readonly=on\""
              " -device virtio-blk-device,drive=hostblk"
              " -display %s"
              " -serial \"file:%s\\shinogi-serial.log\""
              " -d guest_errors -D \"%s\\shinogi-guest-errors.log\"",
              dir, dir, drivec, display, logs, logs);
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
