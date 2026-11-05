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
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>
#include <shellapi.h>
#include "settings.h"
#include "network.h"
#include "window-controls.h"

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

/* Each helper asks Winsock for an available loopback port and reports it
 * in its readiness file, so concurrent CPU editions cannot collide. */

static void note(const char *text)
{
    MessageBoxA(NULL, text, SHINOGI_EDITION, MB_ICONWARNING);
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

    _snprintf(out, n, "%s\\" SHINOGI_EDITION, base);
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
                           char *port, const char *ready,
                           const char *logs, int drive, int readonly, char *failure,
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

    _snprintf(logpath, sizeof(logpath), "%s\\shinogi-hostfsd-%c.log", logs, 'A' + drive);
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
              " --once%s",
              dir, drivec, "0", ready, readonly ? " --read-only" : "");
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

        FILE *ready_file = fopen(ready, "r");
        unsigned chosen = 0;
        if (ready_file) {
            if (fscanf(ready_file, "%u", &chosen) != 1) chosen = 0;
            fclose(ready_file);
        }
        if (chosen > 0 && chosen <= 65535) {
            snprintf(port, 16, "%u", chosen);
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
static DWORD run_qemu(const char *dir, const char *launcher, const char *logs, const char *display,
                      const char *hostfs, const char *kernel, const char *network,
                      int res_w, int res_h, int fullscreen, DWORD *start_error,
                      char *cmdout, size_t cmdoutlen)
{
    char cmd[32768], logpath[MAX_PATH];
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
              " -name \"" SHINOGI_EDITION " " SHINOGI_VERSION " - Ctrl+Alt+S: Settings\""
              /* Sound is deferred. The maintained 060 QEMU does not
               * provide the older virt machine's audiodev property. */
              " -M virt"
              " -cpu " SHINOGI_CPU
              " -m 128"
              " -kernel \"%s\""
              "%s"
              " -device virtio-gpu-device,xres=%d,yres=%d"
              " -device virtio-keyboard-device"
              " -device virtio-tablet-device"
              "%s"
              " -display %s%s"
              " -serial \"file:%s\\shinogi-serial.log\""
              " -d guest_errors -D \"%s\\shinogi-guest-errors.log\"",
              dir, kernel, network, res_w, res_h, hostfs, display,
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

    window_controls_wait(pi.hProcess, pi.dwProcessId, launcher);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    return code;
}

/* An installer upgrade may stop only processes from its own edition tree.
 * Image-name taskkill would also terminate other CPU editions. */
static int stop_edition(const char *dir)
{
    PROCESSENTRY32 entry;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    int ok = 1;
    size_t n = strlen(dir);
    if (snapshot == INVALID_HANDLE_VALUE) return 1;
    entry.dwSize = sizeof(entry);
    if (Process32First(snapshot, &entry)) do {
        HANDLE process;
        char path[MAX_PATH];
        DWORD size = sizeof(path);
        if (entry.th32ProcessID == GetCurrentProcessId()) continue;
        if (lstrcmpiA(entry.szExeFile, "shinogi.exe") &&
            lstrcmpiA(entry.szExeFile, "shinogi-hostfsd.exe") &&
            lstrcmpiA(entry.szExeFile, "qemu-system-m68kw.exe") &&
            lstrcmpiA(entry.szExeFile, "qemu-system-m68k.exe")) continue;
        process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE,
                              FALSE, entry.th32ProcessID);
        if (!process) continue;
        if (QueryFullProcessImageNameA(process, 0, path, &size) &&
            size > n && !_strnicmp(path, dir, n) && path[n] == '\\') {
            if (!TerminateProcess(process, 0) || WaitForSingleObject(process, 5000) != WAIT_OBJECT_0) ok = 0;
        }
        CloseHandle(process);
    } while (Process32Next(snapshot, &entry));
    CloseHandle(snapshot);
    return ok ? 0 : 1;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
    char exe[MAX_PATH], dir[MAX_PATH], logs[MAX_PATH];
    char drivec[MAX_PATH], ready[24][MAX_PATH], port[16];
    HANDLE helpers[24] = {0};
    int d;
    char kernel[MAX_PATH];
    char helper_failure[768];
    int res_w, res_h;
    char hostfs[DRIVE_OPTIONS_MAX], lastcmd[32768], images[8192], drive_error[512];
    const char *display;
    DWORD len, code, start_error;
    Settings settings;
    char network_options[1024], network_error[768];
    char *slash;

    (void)prev; (void)show;

    len = GetModuleFileNameA(NULL, exe, sizeof(exe));
    if (len == 0 || len >= sizeof(exe)) {
        MessageBoxA(NULL, "Could not determine the program location.",
                    SHINOGI_EDITION, MB_ICONERROR);
        return 1;
    }

    lstrcpynA(dir, exe, sizeof(dir));
    slash = strrchr(dir, '\\');
    if (!slash) {
        MessageBoxA(NULL, "Unexpected program path.", SHINOGI_EDITION, MB_ICONERROR);
        return 1;
    }
    *slash = '\0';

    if (!lstrcmpiA(cmdline, "--stop-edition")) return stop_edition(dir);

    if (!settings_load(&settings, dir)) {
        MessageBoxA(NULL, "Could not open your settings folder.", SHINOGI_EDITION, MB_ICONERROR);
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
    if (!drives_valid(&settings.drives, settings.drivec, drive_error, sizeof(drive_error))) {
        note(drive_error);
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

    if (!network_prepare(&settings.network, drivec, network_options, sizeof(network_options),
                          network_error, sizeof(network_error))) {
        note(network_error);
        return 1;
    }

    /* Boot the firmware paired with this application build. CUSTOM.IMG
     * is an explicit user override and has never been installed in drive C.
     * Older bundles shipped EMUTOS.IMG there, so treating that name as an
     * override silently kept every upgraded install on its original firmware.
     * Preserve those files; only a deliberate CUSTOM.IMG overrides the bundle. */
    {
        char user_img[MAX_PATH];
        DWORD attrs;

        _snprintf(user_img, sizeof(user_img), "%s\\CUSTOM.IMG", drivec);
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

    hostfs[0] = '\0';
    for (d = 2; d < 26; d++) {
        const char *folder = d == 2 ? drivec : settings.drives.mapping[d - 3].path;
        int readonly = d == 2 ? 0 : settings.drives.mapping[d - 3].readonly;
        size_t used = strlen(hostfs);
        int n;
        char name[32];
        if (!*folder) continue;
        n = snprintf(ready[d - 2], sizeof(ready[0]), "%s\\hostfs-%c.ready", logs, 'A' + d);
        if (n < 0 || (size_t)n >= sizeof(ready[0])) { note("The log folder path is too long."); goto drive_failure; }
        helpers[d - 2] = start_helper(dir, folder, port, ready[d - 2], logs, d, readonly,
                                      helper_failure, sizeof(helper_failure));
        if (!helpers[d - 2]) {
            char msg[1400];
            snprintf(msg, sizeof(msg), "Could not open drive %c:\n%s\n\n%s\n"
                     "Choose its folder in Shinogi Settings. Logs: %s", 'A' + d, folder, helper_failure, logs);
            note(msg);
            goto drive_failure;
        }
        snprintf(name, sizeof(name), d == 2 ? "shinogi.hostfs" : "shinogi.hostfs.%c", 'a' + d);
        n = snprintf(hostfs + used, sizeof(hostfs) - used,
                     "%s -chardev socket,id=fs%d,host=127.0.0.1,port=%s,server=off,nodelay=on"
                     " -device virtserialport,bus=folders.0,chardev=fs%d,name=%s",
                     d == 2 ? " -device virtio-serial-device,id=folders,max_ports=26" : "",
                     d, port, d, name);
        if (n < 0 || (size_t)n >= sizeof(hostfs) - used) { note("Too many drive options."); goto drive_failure; }
    }
    if (!drives_image_options(&settings.drives, images, sizeof(images)) ||
        strlen(images) + strlen(hostfs) >= sizeof(hostfs)) {
        note("The drive configuration is too long."); goto drive_failure;
    }
    strcat(hostfs, images);

    code = run_qemu(dir, exe, logs, display, hostfs, kernel, network_options, res_w, res_h,
                    settings.fullscreen, &start_error,
                    lastcmd, sizeof(lastcmd));

    for (d = 0; d < 24; d++) stop_helper(helpers[d], ready[d]);

    if (code == (DWORD)-1) {
        char msg[4608];
        _snprintf(msg, sizeof(msg),
                  "Could not start the bundled QEMU (error %lu).\n\n%s",
                  (unsigned long)start_error, lastcmd);
        msg[sizeof(msg) - 1] = '\0';
        MessageBoxA(NULL, msg, SHINOGI_EDITION, MB_ICONERROR);
        return 1;
    }

    if (code != 0) {
        char msg[1400], logpath[MAX_PATH], detail[768] = "", line[768];
        FILE *log;

        _snprintf(logpath, sizeof(logpath), "%s\\shinogi-qemu.log", logs);
        logpath[sizeof(logpath) - 1] = '\0';
        log = fopen(logpath, "r");
        if (log) {
            while (fgets(line, sizeof(line), log)) {
                size_t n = strlen(line);
                while (n && (line[n - 1] == '\r' || line[n - 1] == '\n'))
                    line[--n] = '\0';
                if (n) lstrcpynA(detail, line, sizeof(detail));
            }
            fclose(log);
        }
        _snprintf(msg, sizeof(msg),
                  "QEMU exited with code %lu.\n\n%s\n\n"
                  "Full log:\n%s",
                  (unsigned long)code,
                  detail[0] ? detail : "No diagnostic was written by QEMU.",
                  logpath);
        msg[sizeof(msg) - 1] = '\0';
        MessageBoxA(NULL, msg, SHINOGI_EDITION, MB_ICONWARNING);
    }

    return (int)code;

drive_failure:
    for (d = 0; d < 24; d++) stop_helper(helpers[d], ready[d]);
    return 1;
}
