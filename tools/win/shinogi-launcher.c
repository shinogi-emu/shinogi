/*
 * shinogi.exe -- Windows launcher for the bundled QEMU.
 *
 * Everything ships in one directory tree:
 *
 *     shinogi.exe
 *     shinogi-hostfsd.exe
 *     emutos-virt.elf
 *     qemu\qemu-system-m68kw.exe  (+ DLLs, share\, lib\)
 *
 * The launcher resolves its own location rather than relying on the
 * working directory, so shortcuts and "run as" both behave.
 *
 * Drive C is a folder on the host:
 *
 *     %USERPROFILE%\shinogi-drive-c (default; chosen in Settings)
 *
 * It reaches the guest over a virtio-serial port. shinogi-hostfsd.exe
 * sits on the host end of that port and does the real open/read/readdir
 * against the folder; the guest's GEMDOS layer talks to it and never
 * sees a host path. This is the same mechanism, the same helper and the
 * same wire protocol as the Linux and macOS builds, which is the point
 * of it: one implementation, one set of goldens, three platforms.
 *
 * It replaces QEMU's vvfat driver, which is what this launcher used up
 * to b4. vvfat could only ever be attached READ-ONLY, because its
 * read-write mode loses host data -- measured, not assumed, by
 * tools/check-vvfat-write.py: delete a file on the drive, write a
 * different file in a subdirectory, and the host file is truncated to
 * the deleted one's length while the guest is told the write succeeded.
 * Nothing in the new path can do that: every operation is an explicit
 * request that either completed or did not.
 *
 * 9p is not an option on Windows and never was: QEMU cannot build
 * virtfs there at all -- meson.build requires host_os to be linux,
 * darwin or freebsd -- so the Windows binary carries the virtio-9p
 * device name with none of the implementation behind it.
 *
 * =========================================================================
 * THE STARTUP RACE, AND WHY THERE ISN'T ONE
 * =========================================================================
 *
 * QEMU DISCARDS a guest write to a virtio-serial port whose far end is
 * not connected -- it does not queue it. The guest probes the port in
 * the first moments of boot, so if it probes before the helper has
 * arrived, drive C is lost for the whole session with no error anywhere.
 * Measured on Linux: helper up within 50 ms, drive registered; helper
 * one second late, drive gone.
 *
 * So the roles are inverted from the obvious arrangement. THE HELPER
 * LISTENS and QEMU connects (server=off), which means QEMU makes the
 * connection while it is still parsing its own command line, long before
 * the guest runs. There is no window for the guest to probe into.
 *
 * The launcher's remaining job is to not start QEMU until the helper is
 * actually accepting. It does that by WAITING FOR A FILE THE HELPER
 * CREATES (--ready-file), polling at 10 ms and giving up if the helper
 * exits. Not a sleep: a fixed delay is either wasted time or a lost
 * drive depending on the machine, which is the whole failure this
 * arrangement exists to remove.
 *
 * If the helper never becomes ready, show its diagnostic and leave the
 * selected folder intact. QEMU startup failures are reported separately.
 *
 * =========================================================================
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
#include <string.h>
#include <shellapi.h>
#include "settings.h"

/* Supplied by the build; see tools/make-windows-package.sh. Kept out of
 * the source so the version lives in exactly one file, VERSION. */
#ifndef SHINOGI_VERSION
#error "SHINOGI_VERSION not defined - build through tools/make-windows-package.sh"
#endif
#ifndef SHINOGI_EDITION
#error "SHINOGI_EDITION not defined - build through tools/make-windows-package.sh"
#endif
#ifndef SHINOGI_CPU
#error "SHINOGI_CPU not defined - build through tools/make-windows-package.sh"
#endif

/* How long to wait for the helper before giving up on drive C, and how
 * often to look. The wait is bounded because a helper that never
 * answers must not stop the emulator starting. */
#define READY_TIMEOUT_MS  5000
#define READY_POLL_MS       10

/* Dynamic/private Windows ports live in the IANA private range. The launcher
 * derives one from its PID; this avoids a filesystem socket path entirely. */
#define HOSTFS_PORT_BASE 49152
#define HOSTFS_PORT_COUNT 16384

static void note(const char *text)
{
    MessageBoxA(NULL, text, "shinogi", MB_ICONWARNING);
}

static void helper_log_line(HANDLE log, const char *text)
{
    DWORD written;

    if (log != INVALID_HANDLE_VALUE)
        WriteFile(log, text, (DWORD)strlen(text), &written, NULL);
}

static void helper_win32_failure(char *out, size_t n, const char *phase,
                                 DWORD error)
{
    char detail[512];
    DWORD got;

    detail[0] = '\0';
    got = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM |
                         FORMAT_MESSAGE_IGNORE_INSERTS,
                         NULL, error, 0, detail, sizeof(detail), NULL);
    while (got && (detail[got - 1] == '\r' || detail[got - 1] == '\n'))
        detail[--got] = '\0';

    if (got)
        _snprintf(out, n, "%s (Windows error %lu: %s)\r\n",
                  phase, (unsigned long)error, detail);
    else
        _snprintf(out, n, "%s (Windows error %lu)\r\n",
                  phase, (unsigned long)error);
    out[n - 1] = '\0';
}

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

/*
 * Start the host-folder helper, listening, and wait for it to say so.
 *
 * Returns its process handle, or NULL if it could not be started or
 * never became ready -- in which case the caller runs without a drive.
 * The helper's own diagnostics go to a log beside QEMU's, which needs
 * the write handle to be inheritable and named in STARTUPINFO.
 */
static HANDLE start_helper(const char *dir, const char *drivec,
                           const char *port, const char *ready,
                           const char *logs, char *failure,
                           size_t failure_len)
{
    char cmd[2048], logpath[MAX_PATH], line[768];
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    HANDLE hlog;
    DWORD waited;

    failure[0] = '\0';

    /* A leftover ready file from a previous run would be believed. */
    DeleteFileA(ready);

    _snprintf(logpath, sizeof(logpath), "%s\\shinogi-hostfsd.log", logs);
    logpath[sizeof(logpath) - 1] = '\0';

    ZeroMemory(&sa, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    hlog = CreateFileA(logpath, GENERIC_WRITE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    /*
     * --once: serve this QEMU and then exit.
     *
     * Without it the helper goes back to accepting after the guest
     * disconnects, and there is nothing left to connect to it -- one
     * run of QEMU is exactly one connection. It then outlives the
     * session, and if this launcher was killed rather than closed it is
     * never told to stop, so it keeps running and keeps
     * shinogi-hostfsd.exe open. The next installer cannot overwrite the
     * file and fails with "Error opening file for writing".
     */
    _snprintf(cmd, sizeof(cmd),
              "\"%s\\shinogi-hostfsd.exe\""
              " --root \"%s\\.\" --listen-tcp %s --ready-file \"%s\""
              " --once",
              dir, drivec, port, ready);
    cmd[sizeof(cmd) - 1] = '\0';

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    if (hlog != INVALID_HANDLE_VALUE) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = NULL;
        si.hStdOutput = hlog;
        si.hStdError = hlog;
    }
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessA(NULL, cmd, NULL, NULL,
                        hlog != INVALID_HANDLE_VALUE, CREATE_NO_WINDOW,
                        NULL, dir, &si, &pi)) {
        DWORD error = GetLastError();

        helper_win32_failure(failure, failure_len,
                             "Could not start shinogi-hostfsd.exe", error);
        helper_log_line(hlog, failure);
        if (hlog != INVALID_HANDLE_VALUE) {
            CloseHandle(hlog);
        }
        return NULL;
    }
    CloseHandle(pi.hThread);

    /*
     * Poll rather than sleep. The helper is ready in a millisecond or
     * two in practice; the timeout is only here so a broken one cannot
     * hold the emulator up for ever.
     */
    for (waited = 0; waited < READY_TIMEOUT_MS; waited += READY_POLL_MS) {
        DWORD code = 0;

        if (GetFileAttributesA(ready) != INVALID_FILE_ATTRIBUTES) {
            if (hlog != INVALID_HANDLE_VALUE) {
                CloseHandle(hlog);  /* the child holds its own copy */
            }
            return pi.hProcess;
        }
        if (GetExitCodeProcess(pi.hProcess, &code) && code != STILL_ACTIVE) {
            char detail[512] = "", current[512];
            FILE *log;

            if (hlog != INVALID_HANDLE_VALUE) {
                CloseHandle(hlog);
                hlog = INVALID_HANDLE_VALUE;
            }
            log = fopen(logpath, "r");
            if (log) {
                while (fgets(current, sizeof(current), log)) {
                    current[strcspn(current, "\r\n")] = '\0';
                    if (*current) lstrcpynA(detail, current, sizeof(detail));
                }
                fclose(log);
            }
            _snprintf(failure, failure_len,
                      "shinogi-hostfsd.exe exited before its socket was ready "
                      "(exit code %lu).\r\n%s\r\n", (unsigned long)code, detail);
            failure[failure_len - 1] = '\0';
            helper_log_line(hlog, failure);
            if (hlog != INVALID_HANDLE_VALUE) {
                CloseHandle(hlog);
            }
            CloseHandle(pi.hProcess);
            return NULL;        /* waiting longer is pointless */
        }
        Sleep(READY_POLL_MS);
    }

    _snprintf(line, sizeof(line),
              "shinogi-hostfsd.exe did not make its socket ready within "
              "%lu ms and was stopped.\r\n", (unsigned long)READY_TIMEOUT_MS);
    line[sizeof(line) - 1] = '\0';
    lstrcpynA(failure, line, (int)failure_len);
    helper_log_line(hlog, line);
    TerminateProcess(pi.hProcess, 1);
    WaitForSingleObject(pi.hProcess, 1000);
    if (hlog != INVALID_HANDLE_VALUE) {
        CloseHandle(hlog);
    }
    CloseHandle(pi.hProcess);
    return NULL;
}

static void stop_helper(HANDLE helper, const char *ready)
{
    DWORD code = 0;

    if (!helper) {
        return;
    }
    if (GetExitCodeProcess(helper, &code) && code == STILL_ACTIVE) {
        TerminateProcess(helper, 0);
    }
    CloseHandle(helper);
    DeleteFileA(ready);
}

/*
 * Run QEMU to completion.
 *
 * HOSTFS is the chardev and device triple for drive C, or "" for a run
 * without one. START_ERROR preserves a CreateProcess failure before
 * closing handles can overwrite the Windows error code.
 */
static DWORD run_qemu(const char *dir, const char *logs, const char *display,
                      const char *hostfs, const char *kernel,
                      int res_w, int res_h, int fullscreen, DWORD *start_error,
                      char *cmdout, size_t cmdoutlen)
{
    char cmd[4096], logpath[MAX_PATH];
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    HANDLE hlog;
    DWORD code = 0;

    *start_error = 0;

    /*
     * ORDER ON THE COMMAND LINE IS LOAD-BEARING. QEMU fills the
     * virtio-mmio transport slots from the top down, so the first device
     * listed lands in the highest slot, and tests/golden/phase5-attach
     * pins a slot by number. The hostfs devices therefore go LAST, after
     * everything that was already here.
     */
    _snprintf(cmd, sizeof(cmd),
              "\"%s\\qemu\\qemu-system-m68kw.exe\""
              " -name \"" SHINOGI_EDITION " " SHINOGI_VERSION "\""
              /*
               * Sound. The DMA sound device is only created when the
               * machine is given an audiodev, and it answers the guest's
               * probe only when it exists -- so with no audiodev the
               * guest correctly reports no sound rather than a device
               * that stays silent.
               *
               * dsound is the native Windows backend and QEMU's default
               * there.
               */
              " -audiodev dsound,id=snd0"
              " -M virt,audiodev=snd0"
              " -cpu " SHINOGI_CPU
              " -m 128"
              " -kernel \"%s\""
              /*
               * Networking. slirp gives the guest a NAT'd connection
               * with no setup and no privileges -- it is not a host on
               * the LAN and nothing can reach in, but name resolution
               * and outbound connections work anywhere, which a TAP
               * device would not without the user installing a driver
               * first. The guest configuration is IPv4-only, so keeping
               * slirp's unused IPv6 stack enabled can only add work and
               * misleading IPv6 DNS/address results.
               */
              " -netdev user,id=net0,ipv6=off"
              " -device virtio-net-device,netdev=net0"
              " -device virtio-gpu-device,xres=%d,yres=%d"
              " -device virtio-keyboard-device"
              " -device virtio-tablet-device"
              "%s"
              " -display %s%s"
              " -serial \"file:%s\\shinogi-serial.log\""
              " -d guest_errors -D \"%s\\shinogi-guest-errors.log\"",
              dir, kernel, res_w, res_h, hostfs, display,
              fullscreen ? " -full-screen" : "", logs, logs);
    cmd[sizeof(cmd) - 1] = '\0';

    lstrcpynA(cmdout, cmd, (int)cmdoutlen);

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    _snprintf(logpath, sizeof(logpath), "%s\\shinogi-qemu.log", logs);
    logpath[sizeof(logpath) - 1] = '\0';
    ZeroMemory(&sa, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    hlog = CreateFileA(logpath, GENERIC_WRITE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hlog != INVALID_HANDLE_VALUE) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = NULL;
        si.hStdOutput = hlog;
        si.hStdError = hlog;
    }
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessA(NULL, cmd, NULL, NULL,
                        hlog != INVALID_HANDLE_VALUE,
                        0, NULL, dir, &si, &pi)) {
        *start_error = GetLastError();
        if (hlog != INVALID_HANDLE_VALUE) {
            CloseHandle(hlog);
        }
        return (DWORD)-1;
    }
    if (hlog != INVALID_HANDLE_VALUE) {
        CloseHandle(hlog);
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    return code;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
    char exe[MAX_PATH], dir[MAX_PATH], logs[MAX_PATH];
    char drivec[MAX_PATH], ready[MAX_PATH], port[16];
    char kernel[MAX_PATH];
    char helper_failure[768];
    int res_w, res_h;
    char hostfs[1024], lastcmd[4096];
    const char *display;
    HANDLE helper = NULL;
    DWORD len, code, start_error;
    Settings settings;
    char *slash;

    (void)prev; (void)show;

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

    if (!settings_load(&settings, dir)) {
        MessageBoxA(NULL, "Could not open your settings folder.", "shinogi", MB_ICONERROR);
        return 1;
    }
    if (lstrcmpiA(cmdline, "--settings") == 0)
        return settings_dialog(inst, &settings) ? 0 : 1;
    if (lstrcmpiA(cmdline, "--open-drive-c") == 0) {
        if ((INT_PTR)ShellExecuteA(NULL, "open", settings.drivec, NULL, NULL, SW_SHOWNORMAL) <= 32)
            note("Could not open drive C. Choose its folder in Shinogi Settings.");
        return 0;
    }
    if (!settings_drive_valid(settings.drivec)) {
        char message[768];
        snprintf(message, sizeof(message),
                 "Drive C folder is not available:\n%s\n\n"
                 "Choose the folder containing your drive C in Shinogi Settings.",
                 settings.drivec);
        note(message);
        if (!settings_dialog(inst, &settings)) return 1;
    }
    lstrcpynA(drivec, settings.drivec, sizeof(drivec));

    /*
     * gtk needs zoom-to-fit turned off, because it defaults to on for
     * virtio-gpu and then scales our fixed 1280x720 into whatever window
     * size GTK chose; scale enlarges it afterwards. SDL has neither
     * option, so it is passed through untouched.
     */
    /*
     * Screen size. GEM draws with fixed-size bitmap fonts and icons, so a
     * big screen makes everything small rather than roomy -- 1024x768 is
     * the compromise. Anything else is given as WIDTHxHEIGHT on the
     * command line, e.g. "shinogi.exe 1920x1080".
     *
     * The width is rounded down to a multiple of 8 by the guest, which
     * computes its stride from it. QEMU's own default is 1280x800 and
     * would apply if we passed nothing, so it is always passed.
     */
    res_w = settings.width;
    res_h = settings.height;
    if (cmdline && *cmdline) {
        int w = 0, h = 0;

        if (sscanf(cmdline, "%dx%d", &w, &h) == 2 &&
            w >= 320 && h >= 200 && w <= 1920 && h <= 1080) {
            res_w = w;
            res_h = h;
            cmdline = "";       /* consumed; not a display backend */
        }
    }

    if (!cmdline || !*cmdline) {
        display = "sdl";
    } else if (lstrcmpiA(cmdline, "gtk") == 0) {
        display = "gtk,zoom-to-fit=off,scale=1.5";
    } else {
        display = cmdline;
    }
    log_dir(logs, sizeof(logs));

    /*
     * The guest image, which the user may replace without reinstalling.
     * Drop a newer EmuTOS into the drive C folder as EMUTOS.IMG and it is
     * used instead of the bundled one; delete it and the bundled one comes
     * back. The guest cannot load this itself -- drive C only exists once
     * EmuTOS is running -- but nothing stops US from reading it, and the
     * drive C folder is the one directory the user already knows.
     *
     * EMUTOS.IMG, not EMUTOS.ELF, because .IMG is the name every EmuTOS
     * release already uses and the one a user arriving with a downloaded
     * build will recognise. The extension is cosmetic here: this target's
     * emutos.img keeps its ELF wrapper (see emutos.ld under
     * TARGET_QEMU_VIRT) and emutos-virt.elf is a copy of it, so the two
     * names have always held identical bytes.
     *
     * EMUTOS.ELF is deliberately NOT honoured any more. The install tree
     * used to ship one, so a drive C snapshot taken at any point carried an
     * override that silently outranked a newer installer -- and on 68060
     * that is fatal rather than merely surprising, because an EmuTOS built
     * before the 64-bit DIVU workaround panics with exception 61 during
     * boot. Anyone who deliberately wants an override renames it.
     *
     * Only a REGULAR FILE counts. A directory of that name would otherwise
     * be handed to -kernel and QEMU would fail to start, with the cause
     * sitting in a log the user has no reason to open.
     */
    {
        char user_img[MAX_PATH];
        DWORD attrs;

        _snprintf(user_img, sizeof(user_img), "%s\\EMUTOS.IMG", drivec);
        user_img[sizeof(user_img) - 1] = '\0';

        attrs = GetFileAttributesA(user_img);
        if (attrs != INVALID_FILE_ATTRIBUTES &&
            !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            lstrcpynA(kernel, user_img, sizeof(kernel));
        } else {
            _snprintf(kernel, sizeof(kernel), "%s\\emutos-virt.elf", dir);
            kernel[sizeof(kernel) - 1] = '\0';
        }
    }

    _snprintf(ready, sizeof(ready), "%s\\hostfs.ready", logs);
    ready[sizeof(ready) - 1] = '\0';
    _snprintf(port, sizeof(port), "%u",
              HOSTFS_PORT_BASE +
              (unsigned)(GetCurrentProcessId() % HOSTFS_PORT_COUNT));
    port[sizeof(port) - 1] = '\0';

    hostfs[0] = '\0';
    helper = start_helper(dir, drivec, port, ready, logs,
                          helper_failure, sizeof(helper_failure));
    if (!helper) {
        char msg[1400];

        _snprintf(msg, sizeof(msg),
                  "Could not open drive C:\n%s\n\n%s\n"
                  "See shinogi-hostfsd.log in:\n"
                  "%%LOCALAPPDATA%%\\shinogi\n\n"
                  "You can change the folder using Shinogi Settings.",
                  drivec,
                  helper_failure[0] ? helper_failure :
                  "No diagnostic was returned by the helper.");
        msg[sizeof(msg) - 1] = '\0';
        note(msg);
        return 1;
    } else {
        /*
         * server=off: QEMU is the CLIENT and the helper above is already
         * listening on loopback, so the connection is made while QEMU parses
         * its command line, before the guest can probe the port.
         */
        _snprintf(hostfs, sizeof(hostfs),
                  " -chardev \"socket,id=hostfs,host=127.0.0.1,port=%s,"
                  "server=off\""
                  " -device virtio-serial-device"
                  " -device virtserialport,chardev=hostfs,"
                  "name=shinogi.hostfs",
                  port);
        hostfs[sizeof(hostfs) - 1] = '\0';
    }

    code = run_qemu(dir, logs, display, hostfs, kernel, res_w, res_h,
                    settings.fullscreen, &start_error,
                    lastcmd, sizeof(lastcmd));

    stop_helper(helper, ready);

    if (code == (DWORD)-1) {
        char msg[4608];
        _snprintf(msg, sizeof(msg),
                  "Could not start the bundled QEMU (error %lu).\n\n%s",
                  (unsigned long)start_error, lastcmd);
        msg[sizeof(msg) - 1] = '\0';
        MessageBoxA(NULL, msg, "shinogi", MB_ICONERROR);
        return 1;
    }

    if (code != 0) {
        char msg[512];
        _snprintf(msg, sizeof(msg),
                  "QEMU exited with code %lu.\n\n"
                  "See shinogi-qemu.log for the startup error.\n"
                  "Logs are in:\n%s",
                  (unsigned long)code, logs);
        msg[sizeof(msg) - 1] = '\0';
        MessageBoxA(NULL, msg, "shinogi", MB_ICONWARNING);
    }

    return (int)code;
}
