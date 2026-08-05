/*
 * shinogi-hostfsd -- the host end of the hostfs link.
 *
 * The guest's GEMDOS layer for drive C (bdos/hostfs.c) is finished and
 * unchanged; what it needs is a channel to a real filesystem. QEMU
 * cannot build virtio-9p on Windows and vvfat's write mode corrupts
 * silently, so the channel is virtio-serial -- a plain byte pipe present
 * in every QEMU build -- and this program sits on the other end doing
 * the real open/read/readdir. It is the same job Hatari's gemdos.c does
 * inside the emulator and ARAnyM's hostfs.cpp does behind NatFeats, with
 * the difference that it is an ordinary process and so behaves
 * identically on all three platforms.
 *
 * Protocol: tools/hostfsd/shinogi-hostfs-proto.h.
 * Rationale: docs/hostfs-link-design.md.
 *
 * Two rules govern everything below.
 *
 * 1. THE GUEST IS NOT A TRUSTED PEER. It is emulated code that may be
 *    running anything the user double-clicked. Every path is resolved
 *    and confirmed to be inside the served folder BEFORE any file
 *    operation, and the check is made on the RESOLVED path, so a symlink
 *    inside the folder that points out of it is refused like any other
 *    escape. No host path string is ever sent back.
 *
 * 2. A LENGTH FROM THE PEER IS NEVER TRUSTED. `len` is checked against
 *    both the buffer and what actually arrived before a single field is
 *    read. This class of bug was found twice in the 9p code by review,
 *    which is cheaper to start with than to retrofit.
 *
 * Malformed input produces an error reply, never an exit: a helper that
 * dies takes the user's drive with it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "shinogi-hostfs-proto.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Room for the served root plus any path the guest may send. */
#define FULLPATH_MAX  (PATH_MAX + HOSTFS_MAX_PATH + 2)

/*
 * ===========================================================================
 * PLATFORM TRANSPORT -- the only part a Windows build replaces.
 * ===========================================================================
 *
 * Everything below this block speaks in terms of a listener and a
 * connection and never touches a descriptor or a SOCKET, so porting is
 * this section and nothing else.
 *
 * The transport is a Unix-domain socket on EVERY platform, Windows
 * included. Windows has had AF_UNIX since Windows 10 1803 and QEMU's
 * `socket` chardev uses it there like anywhere else; the QEMU builds
 * this project ships require a newer Windows than that, so there is no
 * host it can run on where the socket is unavailable. The alternative,
 * a named pipe through QEMU's `pipe` chardev, was rejected because that
 * chardev can only ever be the SERVER, which forces the startup race
 * described below rather than removing it.
 *
 * The helper can be either end of the connection because the launcher
 * may want either: QEMU with `server=on` listens and the helper connects
 * (--connect), while the tests and a `server=off` chardev want the
 * helper to listen (--listen).
 *
 * --listen is what the shipped launchers use. QEMU DISCARDS a guest
 * write to a port whose far end is not connected, so if QEMU listens
 * the guest can probe the port before the helper has arrived and lose
 * the drive for the whole session, silently. With the helper listening
 * first, QEMU connects while it is parsing its own command line, long
 * before the guest runs, and there is no window at all.
 */

#ifdef _WIN32

#include <winsock2.h>
#include <afunix.h>
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <direct.h>
#include <dirent.h>

typedef SOCKET conn_t;
#define CONN_NONE INVALID_SOCKET

typedef struct { SOCKET fd; } listener_t;

#define plat_mkdir(p)   _mkdir(p)
#define plat_access_w(p) _access((p), 2)
/* No lstat: Windows has no symlink that stat() would not follow in the
 * only case this probe cares about -- "does this name exist". */
#define plat_lstat(p, st) stat((p), (st))

static int sockaddr_for(struct sockaddr_un *sa, const char *path)
{
    if (strlen(path) >= sizeof(sa->sun_path))
        return -1;
    memset(sa, 0, sizeof(*sa));
    sa->sun_family = AF_UNIX;
    strcpy(sa->sun_path, path);
    return 0;
}

static int listener_open(listener_t *l, const char *path)
{
    struct sockaddr_un sa;

    if (sockaddr_for(&sa, path) < 0)
        return -1;

    l->fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (l->fd == INVALID_SOCKET)
        return -1;

    /* Windows will not bind over an existing file, and the socket file
     * is not removed when the process that made it goes away, so a
     * previous run's leftover has to go first. */
    DeleteFileA(path);

    if (bind(l->fd, (struct sockaddr *)&sa, sizeof(sa)) == SOCKET_ERROR
     || listen(l->fd, 1) == SOCKET_ERROR)
    {
        closesocket(l->fd);
        l->fd = INVALID_SOCKET;
        return -1;
    }
    return 0;
}

static conn_t listener_accept(listener_t *l)
{
    return accept(l->fd, NULL, NULL);
}

static void listener_close(listener_t *l)
{
    if (l->fd != INVALID_SOCKET)
        closesocket(l->fd);
    l->fd = INVALID_SOCKET;
}

static conn_t conn_connect(const char *path)
{
    struct sockaddr_un sa;
    SOCKET s;

    if (sockaddr_for(&sa, path) < 0)
        return CONN_NONE;

    s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET)
        return CONN_NONE;

    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) == SOCKET_ERROR)
    {
        closesocket(s);
        return CONN_NONE;
    }
    return s;
}

static long conn_recv(conn_t c, void *buf, unsigned long n)
{
    return (long)recv(c, (char *)buf, (int)n, 0);
}

static long conn_send(conn_t c, const void *buf, unsigned long n)
{
    return (long)send(c, (const char *)buf, (int)n, 0);
}

static void conn_close(conn_t c)
{
    if (c != CONN_NONE)
        closesocket(c);
}

/* GetFinalPathNameByHandle is the only call that resolves junctions and
 * symlinks, which is exactly what the containment check needs. */
static char *plat_realpath(const char *in, char *out)
{
    HANDLE h;
    DWORD n;
    char tmp[FULLPATH_MAX];

    h = CreateFileA(in, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE)
    {
        errno = ENOENT;
        return NULL;
    }
    n = GetFinalPathNameByHandleA(h, tmp, sizeof(tmp), FILE_NAME_NORMALIZED);
    CloseHandle(h);
    if (n == 0 || n >= sizeof(tmp))
    {
        errno = ENOENT;
        return NULL;
    }
    /* Strip the "\\?\" prefix so the containment comparison sees the
     * same spelling for the root and for everything under it. */
    strcpy(out, (strncmp(tmp, "\\\\?\\", 4) == 0) ? tmp + 4 : tmp);
    return out;
}

static int plat_dfree(const char *root, unsigned long *freekb,
                      unsigned long *totalkb)
{
    ULARGE_INTEGER avail, total, dummy;

    if (!GetDiskFreeSpaceExA(root, &avail, &total, &dummy))
        return -1;
    *freekb = (unsigned long)((avail.QuadPart / 1024ULL > HOSTFS_DFREE_MAX_KB)
                              ? HOSTFS_DFREE_MAX_KB : avail.QuadPart / 1024ULL);
    *totalkb = (unsigned long)((total.QuadPart / 1024ULL > HOSTFS_DFREE_MAX_KB)
                               ? HOSTFS_DFREE_MAX_KB : total.QuadPart / 1024ULL);
    return 0;
}

static void plat_unlink_socket(const char *path)
{
    DeleteFileA(path);
}

/* Winsock has to be started before any of the calls above will work,
 * and it is the only global setup Windows needs. */
static int plat_init(void)
{
    WSADATA wsa;

    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0 ? 0 : -1;
}

#else /* POSIX */

#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <dirent.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/statvfs.h>

typedef int conn_t;
#define CONN_NONE (-1)

typedef struct { int fd; } listener_t;

#define plat_mkdir(p)    mkdir((p), 0777)
#define plat_access_w(p) access((p), W_OK)
#define plat_lstat(p, st) lstat((p), (st))

static int sockaddr_for(struct sockaddr_un *sa, const char *path)
{
    if (strlen(path) >= sizeof(sa->sun_path))
        return -1;
    memset(sa, 0, sizeof(*sa));
    sa->sun_family = AF_UNIX;
    strcpy(sa->sun_path, path);
    return 0;
}

static int listener_open(listener_t *l, const char *path)
{
    struct sockaddr_un sa;

    if (sockaddr_for(&sa, path) < 0)
        return -1;

    l->fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (l->fd < 0)
        return -1;

    unlink(path);               /* a stale socket file is not a conflict */

    if (bind(l->fd, (struct sockaddr *)&sa, sizeof(sa)) < 0
     || listen(l->fd, 1) < 0)
    {
        close(l->fd);
        l->fd = -1;
        return -1;
    }
    return 0;
}

static conn_t listener_accept(listener_t *l)
{
    int fd;

    do {
        fd = accept(l->fd, NULL, NULL);
    } while (fd < 0 && errno == EINTR);

    return fd;
}

static void listener_close(listener_t *l)
{
    if (l->fd >= 0)
        close(l->fd);
    l->fd = -1;
}

static conn_t conn_connect(const char *path)
{
    struct sockaddr_un sa;
    int fd;

    if (sockaddr_for(&sa, path) < 0)
        return CONN_NONE;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return CONN_NONE;

    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0)
    {
        close(fd);
        return CONN_NONE;
    }
    return fd;
}

static long conn_recv(conn_t c, void *buf, unsigned long n)
{
    ssize_t got;

    do {
        got = read(c, buf, (size_t)n);
    } while (got < 0 && errno == EINTR);

    return (long)got;
}

static long conn_send(conn_t c, const void *buf, unsigned long n)
{
    ssize_t put;

    do {
        put = write(c, buf, (size_t)n);
    } while (put < 0 && errno == EINTR);

    return (long)put;
}

static void conn_close(conn_t c)
{
    if (c >= 0)
        close(c);
}

static char *plat_realpath(const char *in, char *out)
{
    return realpath(in, out);
}

static int plat_dfree(const char *root, unsigned long *freekb,
                      unsigned long *totalkb)
{
    struct statvfs vfs;
    unsigned long long unit, f, t;

    if (statvfs(root, &vfs) != 0)
        return -1;

    unit = (unsigned long long)(vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize);
    f = (unsigned long long)vfs.f_bavail * unit / 1024ULL;
    t = (unsigned long long)vfs.f_blocks * unit / 1024ULL;

    *freekb = (unsigned long)(f > HOSTFS_DFREE_MAX_KB ? HOSTFS_DFREE_MAX_KB : f);
    *totalkb = (unsigned long)(t > HOSTFS_DFREE_MAX_KB ? HOSTFS_DFREE_MAX_KB : t);
    return 0;
}

static void plat_unlink_socket(const char *path)
{
    unlink(path);
}

/*
 * A guest that vanishes mid-reply must not kill the helper: the write
 * fails, the connection is dropped, and the next client is accepted.
 */
static int plat_init(void)
{
    signal(SIGPIPE, SIG_IGN);
    return 0;
}

#endif /* _WIN32 */

/*
 * ===========================================================================
 * State
 * ===========================================================================
 */

#define MAX_FILES  32
#define MAX_DIRS    8

typedef struct
{
    int used;
    int fd;
} filehandle;

typedef struct
{
    int used;
    char **names;               /* the OPENDIR snapshot, strcmp-sorted */
    unsigned count;
    char path[FULLPATH_MAX];    /* host path, never sent to the guest */
} dirhandle;

static char root[FULLPATH_MAX];         /* resolved, no trailing separator */
static size_t rootlen;
static char label[16];                  /* folder basename, for HELLO */
static filehandle files[MAX_FILES];
static dirhandle dirs[MAX_DIRS];
static int verbose;

static void dbg(const char *fmt, ...)
{
    va_list ap;

    if (!verbose)
        return;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

/*
 * ===========================================================================
 * Errors
 * ===========================================================================
 *
 * The guest must never see an errno. Anything not specifically mapped
 * becomes EACCDN: "you may not do that" is the honest answer for a host
 * failure GEMDOS has no word for, and it is what TOS returns for the
 * conditions that have no closer match (a full disc, a busy file).
 */
static int errno_to_gemdos(int e)
{
    switch (e)
    {
    case ENOENT:        return HOSTFS_EFILNF;
    case ENOTDIR:       return HOSTFS_EPTHNF;
    case ENAMETOOLONG:  return HOSTFS_EPTHNF;
    case ELOOP:         return HOSTFS_EPTHNF;
    case EMFILE:
    case ENFILE:        return HOSTFS_ENHNDL;
    case ENOMEM:        return HOSTFS_ENSMEM;
    default:            return HOSTFS_EACCDN;
    }
}

/*
 * ===========================================================================
 * Path containment -- a security boundary, not a convenience
 * ===========================================================================
 */

/*
 * True if RESOLVED is the served root or something beneath it. The
 * comparison is textual, but only ever on a path that has already been
 * through plat_realpath(), so ".." and symlinks are gone by this point.
 */
static int under_root(const char *resolved)
{
    if (strcmp(resolved, root) == 0)
        return 1;
    if (strncmp(resolved, root, rootlen) != 0)
        return 0;
    /* The next character must be a separator, or "/srv/data-evil" would
     * pass as a child of "/srv/data". */
#ifdef _WIN32
    return resolved[rootlen] == '\\' || resolved[rootlen] == '/';
#else
    return resolved[rootlen] == '/';
#endif
}

/*
 * A DOS device name is a name whose stem, ignoring case, is one of the
 * reserved devices. Windows resolves "NUL", "NUL.txt" and even
 * "sub/NUL" to the device rather than a file, so a request naming one
 * would open a device on a Windows host and an ordinary file on a Linux
 * one -- the sort of divergence that only shows up in the field.
 */
static int is_dos_device(const char *comp, size_t len)
{
    static const char *const names[] = {
        "CON", "PRN", "AUX", "NUL", "CLOCK$",
        "COM1", "COM2", "COM3", "COM4", "COM5",
        "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5",
        "LPT6", "LPT7", "LPT8", "LPT9", NULL
    };
    size_t stem = 0;
    int i;

    while (stem < len && comp[stem] != '.')
        stem++;

    for (i = 0; names[i]; i++)
    {
        size_t n = strlen(names[i]);
        size_t j;

        if (n != stem)
            continue;
        for (j = 0; j < n; j++)
        {
            char c = comp[j];
            if (c >= 'a' && c <= 'z')
                c = (char)(c - 'a' + 'A');
            if (c != names[i][j])
                break;
        }
        if (j == n)
            return 1;
    }
    return 0;
}

/*
 * Vet one path component. Everything refused here is something a correct
 * guest never sends, so refusing outright costs nothing and removes a
 * whole class of host-specific surprise.
 */
static int component_ok(const char *comp, size_t len)
{
    size_t i;

    if (len == 0 || len > HOSTFS_MAX_NAME)
        return 0;

    /* "." and ".." never reach the wire: hostfs.c reduces them
     * textually before a request is built, so their presence is an
     * escape attempt rather than a path. */
    if (len == 1 && comp[0] == '.')
        return 0;
    if (len == 2 && comp[0] == '.' && comp[1] == '.')
        return 0;

    /* Windows silently strips a trailing dot or space, so "secret." and
     * "secret" name the same file there and different ones here. */
    if (comp[len - 1] == '.' || comp[len - 1] == ' ')
        return 0;

    for (i = 0; i < len; i++)
    {
        unsigned char c = (unsigned char)comp[i];

        if (c < 0x20 || c == 0x7f)      /* control characters, incl. NUL */
            return 0;
        /* ':' would make a drive letter or an NTFS alternate data
         * stream; '\\' is a separator on Windows and would slip a
         * component past the split below; the rest are illegal in a
         * Windows filename anyway and are wildcards or redirections. */
        if (strchr(":\\*?\"<>|", (char)c))
            return 0;
    }

    return !is_dos_device(comp, len);
}

/*
 * ---------------------------------------------------------------------
 * Case folding
 * ---------------------------------------------------------------------
 *
 * GEMDOS lookup is case-insensitive and the guest depends on it: EmuTOS
 * scans \AUTO in upper case while FreeMiNT asks for \mint\1-19-cur\ and
 * xaaes/xaloader.prg in lower case from compiled-in string literals, so
 * no single spelling on disk satisfies both. Resolution used to happen
 * in the guest, which folded; doing it here means folding here.
 *
 * The fold is ASCII-only and deliberately so. tolower() follows the
 * host's locale, which would make the same folder behave differently on
 * two machines -- the class of bug that has already cost this project a
 * tmpfs-versus-ext4 readdir difference and a timezone-dependent fixture.
 * The guest is 8-bit and its high half is Atari ST, not Latin-1, so
 * there is nothing above 0x7f a host fold could get right anyway.
 */
static int ascii_lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

static int name_eq_fold(const char *a, const char *b, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++)
        if (ascii_lower((unsigned char)a[i]) != ascii_lower((unsigned char)b[i]))
            return 0;
    return 1;
}

/*
 * The on-disk spelling of COMP (LEN bytes) inside DIR, when the literal
 * spelling does not exist. Returns 1 and writes LEN bytes -- an ASCII
 * fold never changes a name's length -- into OUT, or 0 for no match.
 *
 * AMBIGUITY IS DELIBERATE. Where two entries differ only in case, the
 * FIRST one readdir() hands over wins: no sort, no preferred case, no
 * error. That is precisely what the reference implementation does
 * (Hatari gemdos.c match_host_dir_entry() breaks on the first strcasecmp
 * hit in raw readdir order), and matching it byte for byte is a decision
 * the project has already taken -- see "Collisions: match Hatari
 * exactly" in docs/phase5-hostfs-design.md. Which file wins is therefore
 * not stable across hosts, and the mitigation is the shipping rule that
 * everything the guest must find is unique when folded.
 *
 * An exact hit never reaches here, so a directory is scanned only for
 * the lookups that would otherwise have failed outright.
 */
static int match_fold(const char *dir, const char *comp, size_t len, char *out)
{
    DIR *dp = opendir(dir);
    struct dirent *de;
    int found = 0;

    if (!dp)
        return 0;

    while ((de = readdir(dp)) != NULL)
    {
        /* "." and ".." are not candidates for anything. component_ok()
         * has already refused them as a spelling the guest may send, and
         * they must not come back in by the side door either. */
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        if (strlen(de->d_name) != len)
            continue;
        if (!name_eq_fold(de->d_name, comp, len))
            continue;
        /* Nothing more to vet: a fold changes only letters, so an entry
         * matching a component that passed component_ok() passes it too
         * -- same length, same punctuation, and is_dos_device() already
         * ignores case. */
        memcpy(out, de->d_name, len);
        found = 1;
        break;
    }

    closedir(dp);
    return found;
}

/* True if DIR is the served root or resolves inside it. The fold scan
 * asks this before opening a directory, so a case-insensitive match can
 * never be drawn from outside the folder even though the containment
 * check below would refuse the result anyway. */
static int dir_under_root(const char *dir)
{
    char resolved[FULLPATH_MAX];

    return plat_realpath(dir, resolved) != NULL && under_root(resolved);
}

/*
 * Resolve a guest path against the served root.
 *
 * REL/RELLEN is the raw wire path: drive-relative, '/'-separated, not
 * NUL-terminated. It is vetted component by component, joined to the
 * root, and then RESOLVED -- textual vetting alone would still let a
 * symlink inside the folder point out of it.
 *
 * Each component is joined in the spelling the guest sent if that
 * spelling exists on disk, and otherwise in the on-disk spelling that
 * matches it ignoring case. A component that matches nothing at all is
 * left exactly as the guest sent it, which is what makes CREATE and
 * MKDIR name the new object the way the guest asked.
 *
 * The leaf may legitimately not exist yet (CREATE, MKDIR, RENAME's
 * destination), so containment is proved on the resolved PARENT and, if
 * the leaf does exist, on the resolved leaf as well. OUT is built from
 * the resolved parent plus the literal leaf, so an operation that should
 * act on a symlink itself (DELETE) still does.
 *
 * Returns 0 and fills OUT, or a GEMDOS error.
 */
static int resolve_path(const char *rel, size_t rellen, char *out)
{
    char joined[FULLPATH_MAX];
    char parent[FULLPATH_MAX];
    char resolved[FULLPATH_MAX];
    const char *leaf = NULL;
    size_t leaflen = 0;
    size_t joinedlen;
    size_t i, start;

    if (rellen > HOSTFS_MAX_PATH)
        return HOSTFS_EPTHNF;

    /* An absolute path is not the guest's to send: drive C is the root
     * of its own world and the host's root is not reachable from it. */
    if (rellen > 0 && (rel[0] == '/' || rel[0] == '\\'))
        return HOSTFS_EPTHNF;

    if (rellen == 0)
    {
        /* The empty path is the root itself: STAT and OPENDIR both need
         * to name the drive. */
        strcpy(out, root);
        return 0;
    }

    if (rootlen + 1 + rellen >= sizeof(joined))
        return HOSTFS_EPTHNF;

    memcpy(joined, root, rootlen);
    joined[rootlen] = '\0';
    joinedlen = rootlen;

    for (start = 0, i = 0; i <= rellen; i++)
    {
        size_t len;
        struct stat probe;

        if (i < rellen && rel[i] != '/')
            continue;
        /* An empty component is "//", a leading '/' or a trailing one.
         * All three are sloppy rather than hostile, but a UNC prefix
         * looks exactly like the first, so none of them is accepted. */
        if (i == start)
            return HOSTFS_EPTHNF;
        len = i - start;
        if (!component_ok(rel + start, len))
            return HOSTFS_EPTHNF;

        joined[joinedlen] = '/';
        memcpy(joined + joinedlen + 1, rel + start, len);
        joined[joinedlen + 1 + len] = '\0';

        /* An exact hit always wins and costs no directory scan. lstat()
         * rather than stat() so that a dangling symlink still counts as
         * present: DELETE has to be able to name one. */
        if (plat_lstat(joined, &probe) != 0)
        {
            char folded[HOSTFS_MAX_NAME + 1];

            joined[joinedlen] = '\0';           /* cut back to the parent */
            if (dir_under_root(joined)
             && match_fold(joined, rel + start, len, folded))
                memcpy(joined + joinedlen + 1, folded, len);
            joined[joinedlen] = '/';            /* and put it back */
        }

        leaf = joined + joinedlen + 1;
        leaflen = len;
        joinedlen += 1 + len;
        start = i + 1;
    }

    /* Split off the leaf and resolve the directory holding it. */
    {
        size_t cut = joinedlen - leaflen - 1;

        memcpy(parent, joined, cut);
        parent[cut] = '\0';
    }

    /* A parent that will not resolve is a bad path, whatever the host
     * called the failure -- EPTHNF is the only answer GEMDOS has. */
    if (!plat_realpath(parent, resolved))
        return HOSTFS_EPTHNF;
    if (!under_root(resolved))
        return HOSTFS_EPTHNF;
    if (strlen(resolved) + 1 + leaflen >= (size_t)FULLPATH_MAX)
        return HOSTFS_EPTHNF;

    strcpy(out, resolved);
    strcat(out, "/");
    strncat(out, leaf, leaflen);

    /* If the leaf exists it must resolve inside the root too -- this is
     * the check that refuses a symlink pointing out of the folder. */
    if (plat_realpath(out, resolved) && !under_root(resolved))
        return HOSTFS_EPTHNF;

    return 0;
}

/*
 * ===========================================================================
 * Handles
 * ===========================================================================
 */

static void dir_release(dirhandle *d)
{
    unsigned i;

    for (i = 0; i < d->count; i++)
        free(d->names[i]);
    free(d->names);
    d->names = NULL;
    d->count = 0;
    d->used = 0;
}

/* A new client is a new session: nothing may survive from the last one,
 * or a handle number the previous guest opened would still work. */
static void handles_reset(void)
{
    int i;

    for (i = 0; i < MAX_FILES; i++)
        if (files[i].used)
        {
            close(files[i].fd);
            files[i].used = 0;
        }

    for (i = 0; i < MAX_DIRS; i++)
        if (dirs[i].used)
            dir_release(&dirs[i]);
}

static filehandle *file_of(unsigned long h)
{
    if (h < 1 || h > MAX_FILES || !files[h - 1].used)
        return NULL;
    return &files[h - 1];
}

static dirhandle *dir_of(unsigned long h)
{
    if (h < 1 || h > MAX_DIRS || !dirs[h - 1].used)
        return NULL;
    return &dirs[h - 1];
}

/*
 * ===========================================================================
 * Frame encoding
 * ===========================================================================
 *
 * Byte at a time rather than through a struct: the guest is big-endian
 * m68k and the wire is little-endian, so nothing may depend on the
 * host's layout or alignment.
 */

static unsigned long get32(const unsigned char *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8)
         | ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

static unsigned get16(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static void put32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
    p[2] = (unsigned char)((v >> 16) & 0xff);
    p[3] = (unsigned char)((v >> 24) & 0xff);
}

static void put16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
}

/*
 * ===========================================================================
 * Operations
 * ===========================================================================
 */

/* The GEMDOS attribute byte for a host object. Only the two bits the
 * guest's hostfs_attr() produces are set, so a STAT here and a DTA there
 * cannot disagree. */
static int attr_of(const struct stat *st, const char *path)
{
    int attr = 0;

    if (S_ISDIR(st->st_mode))
        attr |= HOSTFS_FA_SUBDIR;
    if (plat_access_w(path) != 0)
        attr |= HOSTFS_FA_RO;

    return attr;
}

static int cmp_names(const void *a, const void *b)
{
    return strcmp(*(const char * const *)a, *(const char * const *)b);
}

/*
 * Snapshot a directory at OPENDIR time.
 *
 * The whole listing is taken at once and the cookie is an index into it,
 * so a cookie cannot be invalidated by the directory changing and cannot
 * be a host-specific opaque value the guest might truncate. The cost is
 * that an entry created after OPENDIR is not listed until the next one;
 * the benefit is that enumeration always terminates.
 */
static int dir_snapshot(dirhandle *d, const char *path)
{
    DIR *dp = opendir(path);
    struct dirent *de;
    unsigned cap = 0;

    if (!dp)
        return errno_to_gemdos(errno);

    d->names = NULL;
    d->count = 0;

    while ((de = readdir(dp)) != NULL)
    {
        char *copy;

        /* "." and ".." are never listed: the guest reduces them
         * textually and skips them in its own scan, and a name it can
         * never resolve is worse than one that is simply absent. */
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        /* A host name the guest could never send back is not listable. */
        if (!component_ok(de->d_name, strlen(de->d_name)))
            continue;

        if (d->count == cap)
        {
            unsigned newcap = cap ? cap * 2 : 64;
            char **grown = (char **)realloc(d->names,
                                            newcap * sizeof(char *));
            if (!grown)
            {
                closedir(dp);
                dir_release(d);
                return HOSTFS_ENSMEM;
            }
            d->names = grown;
            cap = newcap;
        }

        copy = (char *)malloc(strlen(de->d_name) + 1);
        if (!copy)
        {
            closedir(dp);
            dir_release(d);
            return HOSTFS_ENSMEM;
        }
        strcpy(copy, de->d_name);
        d->names[d->count++] = copy;
    }

    closedir(dp);

    /* readdir order is whatever the filesystem feels like; sorting makes
     * a listing reproducible between hosts, which is what a golden can
     * be written against. */
    if (d->count > 1)
        qsort(d->names, d->count, sizeof(char *), cmp_names);

    return 0;
}

/*
 * Serve one request.
 *
 * REQ/REQLEN is a frame whose length has already been validated, so the
 * header is known to be present and `data` is known to be reqlen - 16
 * bytes. REP is a HOSTFS_MAX_FRAME buffer; the return value is its
 * length. This function never fails fatally: everything wrong with the
 * request becomes a status.
 */
static unsigned serve(const unsigned char *req, unsigned reqlen,
                      unsigned char *rep)
{
    unsigned op = get16(req + 2);
    unsigned long a = get32(req + 4);
    unsigned long b = get32(req + 8);
    unsigned long c = get32(req + 12);
    const char *data = (const char *)req + HOSTFS_REQ_HDR;
    unsigned datalen = reqlen - HOSTFS_REQ_HDR;
    unsigned char *out = rep + HOSTFS_REP_HDR;
    int status = HOSTFS_E_OK;
    unsigned long ra = 0, rb = 0;
    unsigned outlen = 0;
    char path[FULLPATH_MAX];

    switch (op)
    {
    case HOSTFS_OP_HELLO:
        if (a != HOSTFS_PROTO_VERSION)
        {
            /* Refuse rather than guess: a version we do not understand
             * would have us read fields that may have moved. */
            status = HOSTFS_EDRIVE;
            break;
        }
        /* A HELLO is the start of a session -- the guest may have
         * rebooted without the connection ever dropping. */
        handles_reset();
        ra = HOSTFS_PROTO_VERSION;
        rb = HOSTFS_MAX_REP_DATA;
        outlen = (unsigned)strlen(label);
        memcpy(out, label, outlen);
        break;

    case HOSTFS_OP_OPEN:
    case HOSTFS_OP_CREATE:
    {
        int i, fd, flags;

        status = resolve_path(data, datalen, path);
        if (status != HOSTFS_E_OK)
            break;

        for (i = 0; i < MAX_FILES && files[i].used; i++)
            ;
        if (i == MAX_FILES)
        {
            status = HOSTFS_ENHNDL;
            break;
        }

        if (op == HOSTFS_OP_CREATE)
            flags = O_RDWR | O_CREAT | O_TRUNC;
        else if (a == HOSTFS_O_WRONLY)
            flags = O_WRONLY;
        else if (a == HOSTFS_O_RDWR)
            flags = O_RDWR;
        else
            flags = O_RDONLY;
#ifdef O_BINARY
        flags |= O_BINARY;
#endif
        fd = open(path, flags,
                  (op == HOSTFS_OP_CREATE && (a & HOSTFS_FA_RO)) ? 0444 : 0666);
        if (fd < 0)
        {
            status = errno_to_gemdos(errno);
            break;
        }

        /* A directory opens happily on POSIX and then fails every read,
         * which GEMDOS has no word for. Refuse it at the door. */
        {
            struct stat st;
            if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode))
            {
                close(fd);
                status = HOSTFS_EACCDN;
                break;
            }
        }

        files[i].used = 1;
        files[i].fd = fd;
        ra = (unsigned long)(i + 1);
        break;
    }

    case HOSTFS_OP_CLOSE:
    {
        filehandle *f = file_of(a);

        if (!f)
        {
            status = HOSTFS_EIHNDL;
            break;
        }
        close(f->fd);
        f->used = 0;
        break;
    }

    case HOSTFS_OP_READ:
    {
        filehandle *f = file_of(a);
        long n;

        if (!f)
        {
            status = HOSTFS_EIHNDL;
            break;
        }
        if (c > HOSTFS_MAX_REP_DATA)
            c = HOSTFS_MAX_REP_DATA;    /* the guest reads in chunks */

        if (lseek(f->fd, (off_t)b, SEEK_SET) == (off_t)-1)
        {
            status = errno_to_gemdos(errno);
            break;
        }
        n = (long)read(f->fd, out, (size_t)c);
        if (n < 0)
        {
            status = errno_to_gemdos(errno);
            break;
        }
        outlen = (unsigned)n;
        ra = (unsigned long)n;
        break;
    }

    case HOSTFS_OP_WRITE:
    {
        filehandle *f = file_of(a);
        long n;

        if (!f)
        {
            status = HOSTFS_EIHNDL;
            break;
        }
        if (lseek(f->fd, (off_t)b, SEEK_SET) == (off_t)-1)
        {
            status = errno_to_gemdos(errno);
            break;
        }
        n = (long)write(f->fd, data, (size_t)datalen);
        if (n < 0)
        {
            status = errno_to_gemdos(errno);
            break;
        }
        ra = (unsigned long)n;
        break;
    }

    case HOSTFS_OP_STAT:
    {
        struct stat st;

        status = resolve_path(data, datalen, path);
        if (status != HOSTFS_E_OK)
            break;
        if (stat(path, &st) != 0)
        {
            status = errno_to_gemdos(errno);
            break;
        }
        /* A directory reports length 0, as a FAT directory entry does:
         * the host's own size for one is a filesystem-internal number
         * that changes when files are added to the folder. */
        ra = S_ISDIR(st.st_mode) ? 0UL : (unsigned long)st.st_size;
        rb = (unsigned long)st.st_mtime;
        out[0] = (unsigned char)attr_of(&st, path);
        outlen = 1;
        break;
    }

    case HOSTFS_OP_OPENDIR:
    {
        int i;

        status = resolve_path(data, datalen, path);
        if (status != HOSTFS_E_OK)
            break;

        for (i = 0; i < MAX_DIRS && dirs[i].used; i++)
            ;
        if (i == MAX_DIRS)
        {
            status = HOSTFS_ENHNDL;
            break;
        }

        status = dir_snapshot(&dirs[i], path);
        if (status != HOSTFS_E_OK)
            break;

        strcpy(dirs[i].path, path);
        dirs[i].used = 1;
        ra = (unsigned long)(i + 1);
        break;
    }

    case HOSTFS_OP_READDIR:
    {
        dirhandle *d = dir_of(a);
        unsigned long idx = b;

        if (!d)
        {
            status = HOSTFS_EIHNDL;
            break;
        }

        status = HOSTFS_ENMFIL;
        while (idx < d->count)
        {
            const char *name = d->names[idx++];
            char full[FULLPATH_MAX];
            struct stat st;

            if (strlen(d->path) + 1 + strlen(name) >= sizeof(full))
                continue;
            strcpy(full, d->path);
            strcat(full, "/");
            strcat(full, name);

            /* Deleted since the snapshot: skip it rather than hand back
             * a name the guest would then fail to stat. */
            if (stat(full, &st) != 0)
                continue;

            outlen = (unsigned)strlen(name);
            memcpy(out, name, outlen);
            ra = idx;           /* the cookie for the NEXT call */
            status = HOSTFS_E_OK;
            break;
        }
        break;
    }

    case HOSTFS_OP_CLOSEDIR:
    {
        dirhandle *d = dir_of(a);

        if (!d)
        {
            status = HOSTFS_EIHNDL;
            break;
        }
        dir_release(d);
        break;
    }

    case HOSTFS_OP_DELETE:
        status = resolve_path(data, datalen, path);
        if (status == HOSTFS_E_OK && unlink(path) != 0)
            status = errno_to_gemdos(errno);
        break;

    case HOSTFS_OP_MKDIR:
        status = resolve_path(data, datalen, path);
        if (status == HOSTFS_E_OK && plat_mkdir(path) != 0)
            status = errno_to_gemdos(errno);
        break;

    case HOSTFS_OP_RMDIR:
        status = resolve_path(data, datalen, path);
        if (status == HOSTFS_E_OK && rmdir(path) != 0)
            status = errno_to_gemdos(errno);
        break;

    case HOSTFS_OP_RENAME:
    {
        char dst[FULLPATH_MAX];
        unsigned split = 0;

        while (split < datalen && data[split] != '\0')
            split++;
        if (split == datalen)   /* no separator: not two paths */
        {
            status = HOSTFS_EPTHNF;
            break;
        }

        status = resolve_path(data, split, path);
        if (status != HOSTFS_E_OK)
            break;
        status = resolve_path(data + split + 1, datalen - split - 1, dst);
        if (status != HOSTFS_E_OK)
            break;
        if (rename(path, dst) != 0)
            status = errno_to_gemdos(errno);
        break;
    }

    case HOSTFS_OP_SETATTR:
    {
        struct stat st;
        int mode;

        status = resolve_path(data, datalen, path);
        if (status != HOSTFS_E_OK)
            break;
        if (stat(path, &st) != 0)
        {
            status = errno_to_gemdos(errno);
            break;
        }
        /*
         * Only the read-only bit has a host meaning. Hidden, system and
         * archive are dropped: there is nowhere to put them on a POSIX
         * filesystem, and inventing a place (a dotfile, an xattr) would
         * make the folder stop being an ordinary folder, which is the
         * entire point of the feature.
         */
        mode = (int)(st.st_mode & 0777);
        if (a & HOSTFS_FA_RO)
            mode &= ~0222;
        else
            mode |= (mode & 0444) >> 1;
        if (chmod(path, (mode_t)mode) != 0)
            status = errno_to_gemdos(errno);
        break;
    }

    case HOSTFS_OP_DFREE:
    {
        unsigned long freekb, totalkb;

        if (plat_dfree(root, &freekb, &totalkb) != 0)
        {
            status = errno_to_gemdos(errno);
            break;
        }
        ra = freekb;
        rb = totalkb;
        break;
    }

    default:
        /* An opcode from a newer guest, or noise. Either way there is
         * nothing to do but say so and keep the channel in step. */
        status = HOSTFS_EDRIVE;
        break;
    }

    if (status != HOSTFS_E_OK)
    {
        outlen = 0;
        ra = rb = 0;
    }

    put16(rep + 0, HOSTFS_REP_HDR + outlen);
    put16(rep + 2, (unsigned)(status & 0xffff));
    put32(rep + 4, ra);
    put32(rep + 8, rb);

    dbg("hostfsd: op %u -> status %d len %u\n", op, status,
         HOSTFS_REP_HDR + outlen);

    return HOSTFS_REP_HDR + outlen;
}

/*
 * ===========================================================================
 * Framing
 * ===========================================================================
 */

/* Read exactly N bytes. 0 means the peer closed cleanly, -1 an error or
 * a close mid-frame -- both end the session. */
static int recv_exact(conn_t c, unsigned char *buf, unsigned n)
{
    unsigned got = 0;

    while (got < n)
    {
        long r = conn_recv(c, buf + got, n - got);

        if (r == 0)
            return got == 0 ? 0 : -1;
        if (r < 0)
            return -1;
        got += (unsigned)r;
    }
    return 1;
}

static int send_all(conn_t c, const unsigned char *buf, unsigned n)
{
    unsigned put = 0;

    while (put < n)
    {
        long w = conn_send(c, buf + put, n - put);

        if (w <= 0)
            return -1;
        put += (unsigned)w;
    }
    return 0;
}

/*
 * Serve one client until it goes away.
 *
 * A `len` outside [HOSTFS_REQ_HDR, HOSTFS_MAX_FRAME] means the stream is
 * desynchronised: there is no way to know where the next frame starts,
 * and guessing risks answering the wrong request with the wrong file.
 * The session gets one error reply and is then dropped -- the guest sees
 * EDRIVE, which is diagnosable, and the helper stays up for the next
 * connection.
 */
static long serve_client(conn_t c)
{
    long served = 0;

    unsigned char req[HOSTFS_MAX_FRAME];
    unsigned char rep[HOSTFS_MAX_FRAME];

    handles_reset();

    for (;;)
    {
        unsigned len, replen;
        int r = recv_exact(c, req, HOSTFS_REQ_HDR);

        if (r <= 0)
            break;              /* clean close, or a truncated header */

        served++;

        len = get16(req);
        if (len < HOSTFS_REQ_HDR || len > HOSTFS_MAX_FRAME)
        {
            dbg("hostfsd: bad frame length %u, dropping the session\n", len);
            put16(rep + 0, HOSTFS_REP_HDR);
            put16(rep + 2, (unsigned)(HOSTFS_EDRIVE & 0xffff));
            put32(rep + 4, 0);
            put32(rep + 8, 0);
            send_all(c, rep, HOSTFS_REP_HDR);
            break;
        }

        if (len > HOSTFS_REQ_HDR
         && recv_exact(c, req + HOSTFS_REQ_HDR, len - HOSTFS_REQ_HDR) <= 0)
            break;

        replen = serve(req, len, rep);
        if (send_all(c, rep, replen) < 0)
            break;
    }

    handles_reset();

    return served;
}
/*
 * ===========================================================================
 * Startup
 * ===========================================================================
 */

static void usage(void)
{
    fprintf(stderr,
        "usage: shinogi-hostfsd --root DIR (--listen PATH | --connect PATH)\n"
        "                       [--once] [--verbose]\n"
        "\n"
        "  --root DIR      the folder served as the guest's drive C\n"
        "  --listen PATH   listen on PATH (a unix socket, on Windows\n"
        "                  too) and serve each client in turn\n"
        "  --connect PATH  connect to PATH instead -- QEMU's chardev with\n"
        "                  server=on is the listener in that arrangement\n"
        "  --once          exit after the first client disconnects\n"
        "  --ready-file P  create P once the channel is up, and only\n"
        "                  then -- the launcher waits for it rather than\n"
        "                  sleeping, so QEMU never starts before the\n"
        "                  socket is accepting\n");
}

/*
 * Announce readiness.
 *
 * The launcher must not start QEMU until the listening socket exists,
 * and must not guess how long that takes: a fixed sleep is either a
 * wasted second or a lost drive, depending on the machine. The socket
 * file itself appears at bind() rather than at listen(), so it is not
 * the right thing to watch; this file is created after the listener is
 * complete, or after --connect has connected, and nothing else creates
 * it.
 */
static void signal_ready(const char *path)
{
    FILE *f;

    if (!path)
        return;
    f = fopen(path, "wb");
    if (f)
        fclose(f);
}

/* The folder's own name, for HELLO. The guest gets a label and never a
 * path: nothing above the transport has any use for the host's layout,
 * and a leaked path is a leaked username. */
static void make_label(const char *dir)
{
    const char *base = dir;
    const char *p;
    size_t i;

    for (p = dir; *p; p++)
        if (*p == '/' || *p == '\\')
            base = p + 1;

    for (i = 0; i + 1 < sizeof(label) && base[i]; i++)
    {
        unsigned char ch = (unsigned char)base[i];
        label[i] = (ch >= 0x20 && ch < 0x7f) ? (char)ch : '_';
    }
    label[i] = '\0';

    if (!label[0])
        strcpy(label, "HOSTFS");
}

int main(int argc, char **argv)
{
    const char *dir = NULL;
    const char *sockpath = NULL;
    const char *readyfile = NULL;
    int do_listen = 0, once = 0;
    struct stat st;
    listener_t l;
    int i;

    for (i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--root") && i + 1 < argc)
            dir = argv[++i];
        else if (!strcmp(argv[i], "--listen") && i + 1 < argc)
        {
            sockpath = argv[++i];
            do_listen = 1;
        }
        else if (!strcmp(argv[i], "--connect") && i + 1 < argc)
        {
            sockpath = argv[++i];
            do_listen = 0;
        }
        else if (!strcmp(argv[i], "--ready-file") && i + 1 < argc)
            readyfile = argv[++i];
        else if (!strcmp(argv[i], "--once"))
            once = 1;
        else if (!strcmp(argv[i], "--verbose"))
            verbose = 1;
        else
        {
            usage();
            return 2;
        }
    }

    if (!dir || !sockpath)
    {
        usage();
        return 2;
    }

    if (!plat_realpath(dir, root))
    {
        fprintf(stderr, "shinogi-hostfsd: cannot resolve %s: %s\n",
                dir, strerror(errno));
        return 1;
    }
    if (stat(root, &st) != 0 || !S_ISDIR(st.st_mode))
    {
        fprintf(stderr, "shinogi-hostfsd: %s is not a folder\n", dir);
        return 1;
    }

    rootlen = strlen(root);
    /* A trailing separator would make every containment comparison off
     * by one; "/" itself is the only path where it is not trailing. */
    while (rootlen > 1 && (root[rootlen - 1] == '/'
                        || root[rootlen - 1] == '\\'))
        root[--rootlen] = '\0';

    make_label(root);
    if (plat_init() != 0)
    {
        fprintf(stderr, "shinogi-hostfsd: cannot initialise sockets\n");
        return 1;
    }

    if (!do_listen)
    {
        conn_t c = conn_connect(sockpath);

        if (c == CONN_NONE)
        {
            fprintf(stderr, "shinogi-hostfsd: cannot connect to %s: %s\n",
                    sockpath, strerror(errno));
            return 1;
        }
        signal_ready(readyfile);
        dbg("hostfsd: serving %s to %s\n", label, sockpath);
        serve_client(c);
        conn_close(c);
        return 0;
    }

    if (listener_open(&l, sockpath) < 0)
    {
        fprintf(stderr, "shinogi-hostfsd: cannot listen on %s: %s\n",
                sockpath, strerror(errno));
        return 1;
    }

    signal_ready(readyfile);
    dbg("hostfsd: serving %s on %s\n", label, sockpath);

    for (;;)
    {
        conn_t c = listener_accept(&l);
        long served;

        if (c == CONN_NONE)
        {
#ifndef _WIN32
            /* accept() already retries on EINTR; this is belt and
             * braces, and errno means nothing to Winsock. */
            if (errno == EINTR)
                continue;
#endif
            break;
        }
        /*
         * --once must not be spent on a connection that carried nothing.
         * A probe, a reset, or a client that connects and goes away again
         * would otherwise consume the single session and leave the guest
         * with no helper -- intermittently, depending on what else touched
         * the socket first.
         */
        served = serve_client(c);
        conn_close(c);
        if (once && served > 0)
            break;
    }

    listener_close(&l);
    plat_unlink_socket(sockpath);
    return 0;
}
