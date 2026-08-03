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
#include <sys/stat.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#ifndef SHINOGI_VERSION
#error "SHINOGI_VERSION not defined - build through tools/make-linux-package.sh"
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
    char hostfs[PATH_MAX], fsdev[PATH_MAX + 64], log[PATH_MAX];
    char logerr[PATH_MAX + 8];
    char serial[PATH_MAX + 8];
    char display[128];
    const char *want;
    const char *home = getenv("HOME");
    struct stat st;

    if (self_dir(dir, sizeof(dir)) != 0) {
        fprintf(stderr, "shinogi: cannot determine my own location\n");
        return 1;
    }

    /* A bundled QEMU sits beside us; otherwise use the system one. */
    snprintf(qemu, sizeof(qemu), "%s/qemu/bin/qemu-system-m68k", dir);
    if (stat(qemu, &st) != 0)
        snprintf(qemu, sizeof(qemu), "qemu-system-m68k");

    snprintf(elf, sizeof(elf), "%s/emutos-virt.elf", dir);
    if (stat(elf, &st) != 0) {
        fprintf(stderr, "shinogi: no guest image at %s\n", elf);
        return 1;
    }

    /* The host folder the guest sees as drive C:. */
    if (getenv("SHINOGI_HOSTFS"))
        snprintf(hostfs, sizeof(hostfs), "%s", getenv("SHINOGI_HOSTFS"));
    else
        snprintf(hostfs, sizeof(hostfs), "%s/shinogi-drive-c",
                 home ? home : ".");
    mkdir(hostfs, 0755);

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
    snprintf(fsdev, sizeof(fsdev),
             "local,id=hostfs,path=%s,security_model=mapped-xattr", hostfs);

    printf("shinogi " SHINOGI_VERSION "\n");
    printf("drive C: %s\n", hostfs);
    printf("display: %s\n", display);
    fflush(stdout);

    execlp(qemu, qemu,
           "-name", "shinogi " SHINOGI_VERSION,
           "-M", "virt",
           "-m", "128",
           "-kernel", elf,
           "-device", "virtio-gpu-device",
           "-device", "virtio-keyboard-device",
           "-device", "virtio-tablet-device",
           "-fsdev", fsdev,
           "-device", "virtio-9p-device,fsdev=hostfs,mount_tag=shinogi",
           "-display", display,
           "-serial", serial,
           "-d", "guest_errors", "-D", logerr,
           (char *)NULL);

    fprintf(stderr, "shinogi: could not start %s\n", qemu);
    return 1;
}
