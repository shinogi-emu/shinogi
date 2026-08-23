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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>

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

/* The directory holding this executable. */
static int self_dir(char *out, size_t n)
{
    ssize_t len;
    char *slash;

#ifdef __APPLE__
    /* macOS has no /proc; the executable path comes from dyld. */
    {
        uint32_t sz = (uint32_t)n;
        if (_NSGetExecutablePath(out, &sz) != 0)
            return -1;
        len = (ssize_t)strlen(out);
    }
#else
    len = readlink("/proc/self/exe", out, n - 1);
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
    char chardev[PATH_MAX + 64], hostfsd[PATH_MAX + 32];
    char display[128];
    char datadir[PATH_MAX + 32];
    char pristine[PATH_MAX + 32];
    char gpudev[64];
    const char *want;
    const char *home = getenv("HOME");
    struct stat st;
    pid_t hostfsd_pid, qemu_pid;

    if (self_dir(dir, sizeof(dir)) != 0) {
        fprintf(stderr, "shinogi: cannot determine my own location\n");
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
     * Only when the folder does not exist: an existing drive C belongs to
     * the user, and re-laying the tree over it would overwrite their
     * edited configs.
     */
    /* The host folder the guest sees as drive C:. */
    if (getenv("SHINOGI_HOSTFS"))
        snprintf(hostfs, sizeof(hostfs), "%s", getenv("SHINOGI_HOSTFS"));
    else
        snprintf(hostfs, sizeof(hostfs), "%s/shinogi-drive-c",
                 home ? home : ".");
    if (stat(hostfs, &st) != 0) {
        snprintf(pristine, sizeof(pristine), "%s/../Resources/drive-c", dir);
        if (stat(pristine, &st) == 0) {
            char cmd[2 * PATH_MAX + 64];
            printf("shinogi: creating drive C at %s\n", hostfs);
            fflush(stdout);
            /* -a for the whole tree; the destination must not exist, which
             * the stat above has already established. */
            snprintf(cmd, sizeof(cmd), "cp -a '%s' '%s'", pristine, hostfs);
            if (system(cmd) != 0)
                fprintf(stderr, "shinogi: could not lay down drive C at "
                        "%s\n", hostfs);
        } else {
            mkdir(hostfs, 0755);
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
        char line[256];
        int rw = 1024, rh = 768, w, h;
        FILE *ini;

        if (!res) {
            snprintf(line, sizeof(line), "%s/SHINOGI.INI", hostfs);
            if ((ini = fopen(line, "r")) != NULL) {
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


    /*
     * The guest image, which the user may replace without reinstalling.
     * Drop a newer EmuTOS into the drive C folder as EMUTOS.IMG and it is
     * used instead of the bundled one; remove it and the bundled one comes
     * back. The guest cannot load this itself -- drive C only exists once
     * EmuTOS is running -- but nothing stops US from reading it, and the
     * drive C folder is the one directory the user already knows.
     *
     * EMUTOS.IMG, not EMUTOS.ELF, because .IMG is the name every EmuTOS
     * release already uses. The extension is cosmetic: this target's
     * emutos.img keeps its ELF wrapper and emutos-virt.elf is a copy of
     * it, so the two names have always held identical bytes.
     *
     * EMUTOS.ELF is deliberately no longer honoured. The install tree used
     * to ship one, so any drive C snapshot carried an override that
     * silently outranked a newer installer -- fatal on 68060, where an
     * EmuTOS built before the 64-bit DIVU workaround panics with exception
     * 61 during boot.
     *
     * Only a REGULAR file counts: a directory of that name would be handed
     * to -kernel, and QEMU would fail to start for a reason the user has
     * no way to guess.
     */
    snprintf(elf, sizeof(elf), "%s/EMUTOS.IMG", hostfs);
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
                fprintf(stderr, "shinogi: no guest image at "
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
               (char *)NULL);
        fprintf(stderr, "shinogi: could not start %s\n", hostfsd);
        _exit(127);
    }
    if (hostfsd_pid < 0) {
        fprintf(stderr, "shinogi: cannot fork for the drive C helper\n");
        return 1;
    }

    /* Wait for it to be listening. Without drive C there is no point
     * starting the emulator: it would boot to a bare desktop. */
    {
        int waited = 0;

        while (stat(ready, &st) != 0) {
            int status;

            if (waitpid(hostfsd_pid, &status, WNOHANG) == hostfsd_pid) {
                fprintf(stderr, "shinogi: the drive C helper exited "
                                "before it was ready\n");
                return 1;
            }
            if (++waited > 200) {        /* 10 seconds */
                fprintf(stderr, "shinogi: the drive C helper never became "
                                "ready\n");
                kill(hostfsd_pid, SIGTERM);
                return 1;
            }
            usleep(50000);
        }
    }

    printf("shinogi " SHINOGI_VERSION "\n");
    printf("drive C: %s\n", hostfs);
    printf("display: %s\n", display);
    fflush(stdout);

    /* Not execlp: the helper has to be cleaned up when QEMU exits, and a
     * replaced process image cannot do that. */
    qemu_pid = fork();
    if (qemu_pid == 0) {
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
        fprintf(stderr, "shinogi: could not start %s\n", qemu);
        _exit(127);
    }
    if (qemu_pid > 0) {
        int status;

        while (waitpid(qemu_pid, &status, 0) < 0 && errno == EINTR)
            ;
        kill(hostfsd_pid, SIGTERM);
        waitpid(hostfsd_pid, NULL, 0);
        unlink(sock);
        unlink(ready);
        return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    }

    fprintf(stderr, "shinogi: could not start %s\n", qemu);
    return 1;
}
