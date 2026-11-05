/*
 * shinogi -- Linux launcher.
 *
 * A real binary rather than a shell script, so it can be launched from a
 * desktop entry, a file manager or a dock without a terminal.
 *
 * It resolves its own location, so it works from an installed prefix or
 * from an unpacked tarball, and it takes the version from the build.
 *
 * Display backend
 * ---------------
 * SDL is the project default on every platform. But QEMU's SDL frontend
 * grabs the pointer as soon as it enters a *focused* window when an
 * absolute pointing device is present, and only releases it when the
 * pointer reaches a window edge. Under a remote-desktop session the grab
 * also stops motion being delivered, so the edge is unreachable, the grab
 * never lifts, and the guest pointer is dead until the window loses
 * focus. GTK never grabs while the device is absolute.
 *
 * A user double-clicking an icon should not have to know any of that, so
 * the launcher asks logind whether this session is remote and picks GTK
 * when it is. SHINOGI_DISPLAY overrides, and an argument overrides that.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#ifndef SHINOGI_VERSION
#error "SHINOGI_VERSION not defined - build through tools/make-linux-package.sh"
#endif

/* The .app carries a CPU; a bare developer build does not need one. */
#ifndef SHINOGI_CPU
#define SHINOGI_CPU "m68040"
#endif
#ifndef SHINOGI_EDITION
#define SHINOGI_EDITION "Shinogi"
#endif

/*
 * Ask logind whether the session this process belongs to arrived over
 * the network. Returns 1 for remote, 0 for local, and 0 if we cannot
 * tell -- an unknown session is treated as local, because SDL is the
 * intended default and GTK is the exception.
 */
#ifndef __APPLE__
static int session_is_remote(void)
{
#ifdef __APPLE__
    /* No logind, and cocoa does not have SDL's grab problem anyway. */
    return 0;
#else
    const char *id = getenv("XDG_SESSION_ID");
    char cmd[256];
    char line[128];
    FILE *p;
    int remote = 0;

    if (!id || !*id)
        return 0;

    snprintf(cmd, sizeof(cmd),
             "loginctl show-session %s -p Remote 2>/dev/null", id);

    p = popen(cmd, "r");
    if (!p)
        return 0;

    if (fgets(line, sizeof(line), p))
        remote = (strstr(line, "Remote=yes") != NULL);

    pclose(p);
    return remote;
#endif
}

#endif

/*
 * Everything this program has to say, to the terminal AND to a log file.
 *
 * A .app started from the Finder has no terminal at all, so every
 * diagnostic here would vanish: a launcher that failed and one that was
 * still working looked identical from the outside -- "it starts, then
 * nothing". The log is a plain file in the user's home directory so it
 * can be found without knowing where a sandboxed TMPDIR went.
 */
static FILE *logfp;

static void note(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    if (logfp) {
        va_start(ap, fmt);
        vfprintf(logfp, fmt, ap);
        va_end(ap);
        fflush(logfp);
    }
}

/*
 * Is this directory empty, or absent?
 *
 * An empty drive C is not a user's drive C. It is what an older build
 * left behind: before the tree shipped inside the bundle, the launcher
 * simply mkdir'd the folder, so anyone who ran one of those has an empty
 * one sitting there. Treating it as "theirs" means the tree is never laid
 * down, the AUTO folder is never found, and what comes up is the bare
 * EmuTOS desktop in EmuTOS's own palette -- which reads as a broken build
 * rather than as an empty folder. That is exactly how it was reported.
 */
static int dir_is_empty_or_absent(const char *path)
{
    DIR *d = opendir(path);
    struct dirent *e;
    int n = 0;

    if (!d)
        return 1;

    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        n++;
        break;
    }
    closedir(d);

    return n == 0;
}

/* The directory holding this executable. */
static int self_dir(char *out, size_t n)
{
    char *slash;

#ifdef __APPLE__
    /* macOS has no /proc; the executable path comes from dyld. */
    {
        uint32_t sz = (uint32_t)n;
        if (_NSGetExecutablePath(out, &sz) != 0)
            return -1;
    }
#else
    ssize_t len = readlink("/proc/self/exe", out, n - 1);
    if (len <= 0)
        return -1;
    out[len] = '\0';
#endif

    slash = strrchr(out, '/');
    if (!slash)
        return -1;
    *slash = '\0';

    return 0;
}

int main(int argc, char *argv[])
{
    char dir[PATH_MAX], qemu[PATH_MAX + 32], elf[PATH_MAX + 32];
    char hostfs[PATH_MAX], log[PATH_MAX];
    char logerr[PATH_MAX + 8];
    char serial[PATH_MAX + 8];
    char sock[PATH_MAX + 32], ready[PATH_MAX + 40];
    char chardev[PATH_MAX + 96], hostfsd[PATH_MAX + 32];
    char display[128];
    char datadir[PATH_MAX + 32];
    char pristine[PATH_MAX + 32];
    char gpudev[64];
    const char *want;
    const char *home = getenv("HOME");
    struct stat st;
    pid_t hostfsd_pid;

    {
        char logpath[PATH_MAX + 32];

        snprintf(logpath, sizeof(logpath), "%s/shinogi-launcher.log",
                 home ? home : "/tmp");
        logfp = fopen(logpath, "w");
        note("shinogi " SHINOGI_VERSION " " SHINOGI_EDITION
             " (" SHINOGI_CPU ") starting\n");
    }

    if (self_dir(dir, sizeof(dir)) != 0) {
        note("shinogi: cannot determine my own location\n");
        return 1;
    }

    /* A bundled QEMU sits beside us; otherwise use the system one. */
    snprintf(qemu, sizeof(qemu), "%s/qemu/bin/qemu-system-m68k", dir);
    if (stat(qemu, &st) != 0)
        snprintf(qemu, sizeof(qemu), "qemu-system-m68k");

    /*
     * QEMU finds its data files at ../share/qemu relative to its own
     * binary.  In the .app they cannot be there - that path is inside
     * Contents/MacOS, which codesign requires to hold only signed code -
     * so they ship in Resources and QEMU is told where to look.  -L is a
     * search path and QEMU keeps its built-in entries as well, so on the
     * Linux bundle, where this directory does not exist, it costs
     * nothing and changes nothing.
     */
    snprintf(datadir, sizeof(datadir), "%s/../Resources/qemu/share", dir);

    /*
     * Drive C ships pristine inside the bundle and is copied out on first
     * run.  The copy the user edits is therefore never inside an .app that
     * the next release replaces, and a guest that wrecks its own C: is one
     * deleted folder away from working again.
     *
     * Only when there is nothing there: a drive C with files in it
     * belongs to the user, and re-laying the tree over it would overwrite
     * their edited configs. An EMPTY folder is not that -- see
     * dir_is_empty_or_absent above.
     */
    /* The host folder the guest sees as drive C:. */
    if (getenv("SHINOGI_HOSTFS"))
        snprintf(hostfs, sizeof(hostfs), "%s", getenv("SHINOGI_HOSTFS"));
    else {
#ifdef __APPLE__
        char parent[PATH_MAX];
        snprintf(parent, sizeof(parent), "%s/" SHINOGI_EDITION, home ? home : ".");
        if (mkdir(parent, 0755) && errno != EEXIST) {
            note("shinogi: cannot create %s: %s\n", parent, strerror(errno));
            return 1;
        }
        snprintf(hostfs, sizeof(hostfs), "%s/" SHINOGI_EDITION "/drive-c", home ? home : ".");
#else
        snprintf(hostfs, sizeof(hostfs), "%s/shinogi-drive-c", home ? home : ".");
#endif
    }
    if (dir_is_empty_or_absent(hostfs)) {
        snprintf(pristine, sizeof(pristine), "%s/../Resources/drive-c", dir);
        if (stat(pristine, &st) == 0) {
            char source[PATH_MAX + 40];
            pid_t copy;
            int status;
            note("shinogi: creating drive C at %s\n", hostfs);
            fflush(stdout);
            /* Copy the CONTENTS, so an existing empty folder is filled
             * rather than nested inside itself. */
            mkdir(hostfs, 0755);
            snprintf(source, sizeof(source), "%s/.", pristine);
            copy = fork();
            if (copy == 0) { execlp("cp", "cp", "-a", source, hostfs, (char *)NULL); _exit(127); }
            if (copy < 0 || waitpid(copy, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status)) {
                note("shinogi: could not lay down drive C at %s\n", hostfs);
                return 1;
            }
        } else {
            mkdir(hostfs, 0755);
            note("shinogi: drive C at %s is empty and this bundle has no "
                 "tree to put there\n", hostfs);
        }
    }

    /*
     * Screen size. GEM draws with fixed-size bitmap fonts and icons, so
     * a big screen makes everything small rather than roomy; 1024x768 is
     * the compromise.
     *
     * Read from SHINOGI.INI in the drive C folder, so the guest can change
     * it: nothing inside the emulator can resize a virtio-gpu scanout that
     * is already up, but the guest can write a file and ask to be shut
     * down, and we read it on the way back in.  SHINOGI_RES overrides it
     * for testing.
     *
     * Always passed: QEMU's own virtio-gpu default is 1280x800 and would
     * apply otherwise. The guest asks the host for this size at boot and
     * rounds the width down to a multiple of 8.
     */
    {
        const char *res = getenv("SHINOGI_RES");
        char line[256], ini_path[PATH_MAX + 16];
        int rw = 1024, rh = 768, w, h;
        FILE *ini;

        if (!res) {
            snprintf(ini_path, sizeof(ini_path), "%s/SHINOGI.INI", hostfs);
            if ((ini = fopen(ini_path, "r")) != NULL) {
                while (fgets(line, sizeof(line), ini)) {
                    /* "res = 1280x720", spaces optional, anything else ignored */
                    if (sscanf(line, " res = %dx%d", &w, &h) == 2 ||
                        sscanf(line, " res=%dx%d", &w, &h) == 2) {
                        if (w >= 320 && h >= 200 && w <= 1920 && h <= 1080) {
                            rw = w;
                            rh = h;
                        }
                        break;
                    }
                }
                fclose(ini);
            }
        } else if (sscanf(res, "%dx%d", &w, &h) == 2 &&
                   w >= 320 && h >= 200 && w <= 1920 && h <= 1080) {
            rw = w;
            rh = h;
        }
        snprintf(gpudev, sizeof(gpudev),
                 "virtio-gpu-device,xres=%d,yres=%d", rw, rh);
    }


    /* CUSTOM.IMG is an explicit user firmware override. The system tree's
     * EMUTOS.IMG is a packaged copy and must not hide newer app firmware. */
    snprintf(elf, sizeof(elf), "%s/CUSTOM.IMG", hostfs);
    if (stat(elf, &st) != 0 || !S_ISREG(st.st_mode)) {
        snprintf(elf, sizeof(elf), "%s/emutos-virt.elf", dir);
        if (stat(elf, &st) != 0) {
            /*
             * Inside a .app the guest image cannot live beside this
             * binary. Contents/MacOS is for Mach-O, and codesign walks
             * it expecting to find code: an m68k ELF there is a nested
             * "code object is not signed at all", which fails the whole
             * bundle after every dylib has already signed cleanly. So
             * the bundle puts it in Contents/Resources, which is where a
             * payload that is data to the host belongs anyway.
             */
            snprintf(elf, sizeof(elf), "%s/../Resources/emutos-virt.elf",
                     dir);
            if (stat(elf, &st) != 0) {
                note("shinogi: no guest image at "
                        "%s/emutos-virt.elf\n", dir);
                return 1;
            }
        }
    }

    /*
     * Display: an argument beats the environment, which beats the
     * session-based guess. gtk needs zoom-to-fit off, because it
     * defaults to on for virtio-gpu and then scales our fixed 1280x720
     * into whatever window size GTK chose.
     */
    want = (argc > 1) ? argv[1] : getenv("SHINOGI_DISPLAY");
    if (!want || !*want)
#ifdef __APPLE__
        want = "cocoa";
#else
        want = session_is_remote() ? "gtk" : "sdl";
#endif

    if (strcmp(want, "gtk") == 0)
        snprintf(display, sizeof(display),
                 "gtk,zoom-to-fit=off,scale=%s",
                 getenv("SHINOGI_SCALE") ? getenv("SHINOGI_SCALE") : "1.5");
    else
        snprintf(display, sizeof(display), "%s", want);

    snprintf(log, sizeof(log), "%s/shinogi-serial.log",
             getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    snprintf(logerr, sizeof(logerr), "%s.err", log);
    snprintf(serial, sizeof(serial), "file:%s", log);

    /*
     * Drive C is served by shinogi-hostfsd over virtio-serial, NOT by 9p.
     *
     * This used to pass -fsdev/-virtio-9p-device, and stayed that way
     * after the guest moved to the hostfs link.  The result was a machine
     * with no drive C at all: bios.c refuses to register one unless
     * hostfs_link_present(), so there was no AUTO folder, MINT.PRG never
     * ran, and what came up was the bare EmuTOS desktop -- which reads as
     * a broken or ancient build rather than as a missing helper.
     *
     * The helper LISTENS and QEMU connects (server=off).  That way round
     * on purpose: QEMU discards a guest write to a port whose far end is
     * absent, so if QEMU listened, the guest could probe the port before
     * the helper arrived and lose the drive silently for the whole
     * session.  With the helper up first there is no window at all.
     */
    snprintf(sock, sizeof(sock), "%s/shinogi-hostfs-%d.sock",
             getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
    snprintf(ready, sizeof(ready), "%s.ready", sock);
    snprintf(chardev, sizeof(chardev),
             "socket,id=hostfs,path=%s,server=off", sock);

    snprintf(hostfsd, sizeof(hostfsd), "%s/shinogi-hostfsd", dir);
    if (stat(hostfsd, &st) != 0)
        snprintf(hostfsd, sizeof(hostfsd), "shinogi-hostfsd");

    unlink(sock);
    unlink(ready);

    hostfsd_pid = fork();
    if (hostfsd_pid == 0) {
        execlp(hostfsd, hostfsd,
               "--root", hostfs,
               "--listen", sock,
               "--ready-file", ready,
               /* It ends itself when the emulator disconnects, so
                * nothing has to survive QEMU in order to reap it. */
               "--once",
               (char *)NULL);
        note("shinogi: could not start %s\n", hostfsd);
        _exit(127);
    }
    if (hostfsd_pid < 0) {
        note("shinogi: cannot fork for the drive C helper\n");
        return 1;
    }

    /* Wait for it to be listening. Without drive C there is no point
     * starting the emulator: it would boot to a bare desktop. */
    {
        int waited = 0;

        while (stat(ready, &st) != 0) {
            int status;

            if (waitpid(hostfsd_pid, &status, WNOHANG) == hostfsd_pid) {
                note("shinogi: the drive C helper exited "
                                "before it was ready\n");
                return 1;
            }
            if (++waited > 200) {        /* 10 seconds */
                note("shinogi: the drive C helper never became "
                                "ready\n");
                kill(hostfsd_pid, SIGTERM);
                return 1;
            }
            usleep(50000);
        }
    }

    note("drive C: %s\n", hostfs);
    note("display: %s\n", display);
    note("guest:   %s\n", elf);
    note("qemu:    %s\n", qemu);
    fflush(stdout);

    /*
     * BECOME QEMU, rather than starting it as a child.
     *
     * On macOS the running application is the process the bundle
     * launched -- Contents/MacOS/shinogi -- and that is what owns the
     * Dock icon, the activation state and the right to put a window on
     * screen. A QEMU forked underneath it is a separate, unbundled
     * process that the window server does not consider part of the app,
     * so the guest boots perfectly and nothing is ever displayed.
     *
     * This used to fork because the helper had to be reaped when QEMU
     * exited. It does not: --once above makes it end itself when the
     * emulator disconnects, which is the same event, observed from the
     * other side.
     *
     * QEMU's own complaints go to the log first. Without this a refused
     * option or a missing dylib kills the emulator silently under a
     * Finder launch.
     */
    if (logfp)
        dup2(fileno(logfp), 2);
    {
        execlp(qemu, qemu,
               "-name", SHINOGI_EDITION " (" SHINOGI_VERSION ")",
               "-M", "virt",
               "-cpu", SHINOGI_CPU,
               "-m", "128",
               "-L", datadir,
               "-kernel", elf,
               /* slirp: NAT with no setup, see the Windows launcher. */
               "-netdev", "user,id=net0",
               "-device", "virtio-net-device,netdev=net0",
               "-device", gpudev,
               "-device", "virtio-keyboard-device",
               "-device", "virtio-tablet-device",
               "-chardev", chardev,
               "-device", "virtio-serial-device",
               "-device", "virtserialport,chardev=hostfs,name=shinogi.hostfs",
               "-display", display,
               "-serial", serial,
               "-d", "guest_errors", "-D", logerr,
               (char *)NULL);
    }

    /* Only reached if the emulator could not be replaced into. */
    note("shinogi: could not start %s\n", qemu);
    kill(hostfsd_pid, SIGTERM);
    unlink(sock);
    unlink(ready);
    return 127;
}
