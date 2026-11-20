/*
 * Native test client for shinogi-hostfsd.
 *
 * The point of building the host side first is that the protocol can be
 * debugged here, where a cycle is milliseconds, instead of inside a boot.
 * So this is not a smoke test: it round-trips every operation, tries to
 * escape the served folder every way the reviewers and the Windows
 * filename rules could think of, and feeds the helper frames no correct
 * guest would ever send.
 *
 * It spawns the helper itself against a scratch folder, so `make test`
 * needs nothing set up and leaves nothing behind.
 *
 * Two properties matter more than the round trips:
 *
 *   - every escape attempt is REFUSED, and nothing is created outside
 *     the served folder while trying;
 *   - the helper is still answering after every piece of malformed input,
 *     because a helper that dies takes the user's drive with it.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <ftw.h>
#include <signal.h>
#include <time.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>

#include "shinogi-hostfs-proto.h"

static int checks, failures;
static char tmproot[256];       /* scratch dir holding the socket and root */
static char servedir[300];      /* the folder the helper serves */
static char sockpath[300];
static pid_t helper;
static int sock = -1;

/* ------------------------------------------------------------------ */

static void check(int ok, const char *fmt, ...)
{
    va_list ap;

    checks++;
    if (ok)
        return;
    failures++;
    va_start(ap, fmt);
    printf("FAIL ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

static void die(const char *what)
{
    printf("test setup failed: %s: %s\n", what, strerror(errno));
    if (helper > 0)
        kill(helper, SIGKILL);
    exit(2);
}

/* ------------------------------------------------------------------ */
/* Transport                                                           */

static void t_disconnect(void)
{
    if (sock >= 0)
        close(sock);
    sock = -1;
}

/*
 * A helper stuck in a read must fail the run rather than hang it: a test
 * suite that never returns is worse than one that reports a failure,
 * because CI has nothing to print. Every wait is therefore bounded.
 */
static void t_deadline(int fd)
{
    struct timeval tv;

    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

static void t_connect(void)
{
    struct sockaddr_un sa;
    int tries;

    t_disconnect();

    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strcpy(sa.sun_path, sockpath);

    for (tries = 0; tries < 400; tries++)
    {
        sock = socket(AF_UNIX, SOCK_STREAM, 0);
        if (sock < 0)
            die("socket");
        if (connect(sock, (struct sockaddr *)&sa, sizeof(sa)) == 0)
        {
            t_deadline(sock);
            return;
        }
        close(sock);
        sock = -1;
        usleep(5000);
    }
    die("connect");
}

static int send_all(const unsigned char *buf, unsigned n)
{
    unsigned put = 0;

    while (put < n)
    {
        ssize_t w = write(sock, buf + put, n - put);

        if (w <= 0)
            return -1;
        put += (unsigned)w;
    }
    return 0;
}

static int recv_all(unsigned char *buf, unsigned n)
{
    unsigned got = 0;

    while (got < n)
    {
        ssize_t r = read(sock, buf + got, n - got);

        if (r <= 0)
            return -1;
        got += (unsigned)r;
    }
    return 0;
}

/* Reply of the last rpc(), for the tests that look at the payload. */
static unsigned char reply[HOSTFS_MAX_FRAME_LIMIT];
static unsigned reply_len;
static unsigned long reply_a, reply_b;

/*
 * The largest reply this client accepts. It stays at HOSTFS_MAX_FRAME --
 * what every guest older than HOSTFS_CAP_LARGE_FRAME can hold -- except
 * inside test_frames(), so every other test also checks that the helper
 * never sends an old guest more than 4 KB.
 */
static unsigned client_frame = HOSTFS_MAX_FRAME;

/*
 * One request, one reply. Returns the GEMDOS status, or -1000 if the
 * transport itself failed -- a value no operation can return, so a test
 * cannot mistake a dropped connection for an error reply.
 */
#define RPC_DEAD (-1000)

static int rpc(unsigned op, unsigned long a, unsigned long b, unsigned long c,
               const void *data, unsigned dlen)
{
    static unsigned char req[HOSTFS_MAX_FRAME_LIMIT];
    unsigned len = HOSTFS_REQ_HDR + dlen;
    int status;

    if (len > HOSTFS_MAX_FRAME_LIMIT)
        die("test built an oversized request");

    req[0] = (unsigned char)(len & 0xff);
    req[1] = (unsigned char)(len >> 8);
    req[2] = (unsigned char)(op & 0xff);
    req[3] = (unsigned char)(op >> 8);
    req[4] = (unsigned char)(a & 0xff);
    req[5] = (unsigned char)((a >> 8) & 0xff);
    req[6] = (unsigned char)((a >> 16) & 0xff);
    req[7] = (unsigned char)((a >> 24) & 0xff);
    req[8] = (unsigned char)(b & 0xff);
    req[9] = (unsigned char)((b >> 8) & 0xff);
    req[10] = (unsigned char)((b >> 16) & 0xff);
    req[11] = (unsigned char)((b >> 24) & 0xff);
    req[12] = (unsigned char)(c & 0xff);
    req[13] = (unsigned char)((c >> 8) & 0xff);
    req[14] = (unsigned char)((c >> 16) & 0xff);
    req[15] = (unsigned char)((c >> 24) & 0xff);
    if (dlen)
        memcpy(req + HOSTFS_REQ_HDR, data, dlen);

    reply_len = 0;
    reply_a = reply_b = 0;

    if (send_all(req, len) < 0)
        return RPC_DEAD;
    if (recv_all(reply, HOSTFS_REP_HDR) < 0)
        return RPC_DEAD;

    reply_len = (unsigned)reply[0] | ((unsigned)reply[1] << 8);
    if (reply_len < HOSTFS_REP_HDR || reply_len > client_frame)
        return RPC_DEAD;
    if (reply_len > HOSTFS_REP_HDR
     && recv_all(reply + HOSTFS_REP_HDR, reply_len - HOSTFS_REP_HDR) < 0)
        return RPC_DEAD;

    status = (short)((unsigned)reply[2] | ((unsigned)reply[3] << 8));
    reply_a = (unsigned long)reply[4] | ((unsigned long)reply[5] << 8)
            | ((unsigned long)reply[6] << 16) | ((unsigned long)reply[7] << 24);
    reply_b = (unsigned long)reply[8] | ((unsigned long)reply[9] << 8)
            | ((unsigned long)reply[10] << 16) | ((unsigned long)reply[11] << 24);
    return status;
}

static int rpc_path(unsigned op, unsigned long a, const char *path)
{
    return rpc(op, a, 0, 0, path, (unsigned)strlen(path));
}

static const char *reply_data(void)
{
    return (const char *)reply + HOSTFS_REP_HDR;
}

static unsigned reply_datalen(void)
{
    return reply_len - HOSTFS_REP_HDR;
}

/* ------------------------------------------------------------------ */
/* Scratch folder                                                      */

static void host_path(char *out, size_t n, const char *rel)
{
    snprintf(out, n, "%s/%s", servedir, rel);
}

static void host_write(const char *rel, const char *text)
{
    char p[512];
    FILE *f;

    host_path(p, sizeof(p), rel);
    f = fopen(p, "wb");
    if (!f)
        die(rel);
    if (text[0])
        fwrite(text, 1, strlen(text), f);
    fclose(f);
}

static int host_exists(const char *abs)
{
    struct stat st;

    return lstat(abs, &st) == 0;
}

static int nuke(const char *path, const struct stat *st, int flag,
                struct FTW *ftw)
{
    (void)st; (void)flag; (void)ftw;
    return remove(path);
}

static void scratch_make(void)
{
    char p[512], link[512];

    /* $TMPDIR if set, as mktemp(1) does: a machine where /tmp is small
     * or shared can point the run elsewhere. */
    {
        const char *t = getenv("TMPDIR");

        snprintf(tmproot, sizeof(tmproot), "%s/hostfsd-test-XXXXXX",
                 t && *t ? t : "/tmp");
    }
    if (!mkdtemp(tmproot))
        die("mkdtemp");

    snprintf(servedir, sizeof(servedir), "%s/served", tmproot);
    snprintf(sockpath, sizeof(sockpath), "%s/sock", tmproot);
    if (mkdir(servedir, 0777) != 0)
        die("mkdir served");

    /* A file OUTSIDE the served folder. Every escape attempt aims at it,
     * and its content is what a successful escape would return. */
    snprintf(p, sizeof(p), "%s/outside.txt", tmproot);
    {
        FILE *f = fopen(p, "wb");
        if (!f)
            die("outside.txt");
        fputs("SECRET", f);
        fclose(f);
    }

    /* Present before the helper starts: its writable-folder preparation
     * must not follow a symlink and chmod something outside the served
     * root. This specifically covers the startup pass, not request-time
     * containment tested below. */
    snprintf(link, sizeof(link), "%s/startup-escape", servedir);
    if (symlink(p, link) != 0)
        die("symlink startup-escape");
    if (chmod(p, 0444) != 0)
        die("chmod outside.txt");

    host_write("hello.txt", "hello world");
    host_write("empty.txt", "");
    host_write("startup-ro.txt", "read only before helper startup");
    host_path(p, sizeof(p), "startup-ro.txt");
    if (chmod(p, 0444) != 0)
        die("chmod startup-ro.txt");

    host_path(p, sizeof(p), "sub");
    if (mkdir(p, 0777) != 0)
        die("mkdir sub");
    host_write("sub/inner.txt", "inner");
}

static void test_startup_containment(void)
{
    char p[512];
    struct stat st;

    host_path(p, sizeof(p), "startup-ro.txt");
    check(stat(p, &st) == 0 && (st.st_mode & S_IWUSR),
          "startup did not make a served file writable");

    snprintf(p, sizeof(p), "%s/outside.txt", tmproot);
    check(stat(p, &st) == 0, "startup destroyed the file outside the root");
    check(!(st.st_mode & S_IWUSR),
          "startup writable pass followed a symlink outside the root");
    if (chmod(p, 0666) != 0)
        die("restore outside.txt mode");
}

static void scratch_remove(void)
{
    nftw(tmproot, nuke, 16, FTW_DEPTH | FTW_PHYS);
}

static void helper_start(void)
{
    helper = fork();
    if (helper < 0)
        die("fork");
    if (helper == 0)
    {
        execl("../../tools/hostfsd/shinogi-hostfsd", "shinogi-hostfsd",
              "--root", servedir, "--listen", sockpath, (char *)NULL);
        _exit(127);
    }
}

/* The helper must be alive and answering after everything that has been
 * thrown at it -- that is the test, not a teardown detail. */
static int helper_alive(void)
{
    int st;

    if (waitpid(helper, &st, WNOHANG) != 0)
        return 0;
    t_connect();
    return rpc(HOSTFS_OP_HELLO, HOSTFS_PROTO_VERSION, 0, 0, NULL, 0)
           == HOSTFS_E_OK;
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */

static void test_hello(void)
{
    check(rpc(HOSTFS_OP_HELLO, HOSTFS_PROTO_VERSION, 0, 0, NULL, 0)
          == HOSTFS_E_OK, "HELLO refused");
    check(reply_a == HOSTFS_PROTO_VERSION, "HELLO version %lu", reply_a);
    check(reply_b == HOSTFS_MAX_REP_DATA, "HELLO max data %lu", reply_b);
    check(reply_datalen() == 6 && !memcmp(reply_data(), "served", 6),
          "HELLO label is not the folder name");

    /* The label is a name, never a path: a leaked path is a leaked
     * username. */
    check(memchr(reply_data(), '/', reply_datalen()) == NULL,
          "HELLO label contains a path separator");

    check(rpc(HOSTFS_OP_HELLO, 999, 0, 0, NULL, 0) == HOSTFS_EDRIVE,
          "a version mismatch was not refused");

    /* A refused HELLO must not desynchronise anything. */
    check(rpc(HOSTFS_OP_HELLO, HOSTFS_PROTO_VERSION, 0, 0, NULL, 0)
          == HOSTFS_E_OK, "HELLO after a version mismatch");
}

static void test_stat(void)
{
    char p[512];

    check(rpc_path(HOSTFS_OP_STAT, 0, "hello.txt") == HOSTFS_E_OK,
          "STAT hello.txt");
    check(reply_a == 11, "STAT size %lu, want 11", reply_a);
    check(reply_b > 1000000000UL, "STAT mtime %lu is not an epoch", reply_b);
    check(reply_datalen() == 1 && (reply_data()[0] & HOSTFS_FA_SUBDIR) == 0,
          "STAT marked a file as a directory");

    check(rpc_path(HOSTFS_OP_STAT, 0, "sub") == HOSTFS_E_OK, "STAT sub");
    check(reply_datalen() == 1 && (reply_data()[0] & HOSTFS_FA_SUBDIR),
          "STAT did not mark a directory");
    check(reply_a == 0, "STAT gave a directory a length: %lu", reply_a);

    /* The empty path is the drive root itself. */
    check(rpc_path(HOSTFS_OP_STAT, 0, "") == HOSTFS_E_OK, "STAT of the root");
    check(reply_datalen() == 1 && (reply_data()[0] & HOSTFS_FA_SUBDIR),
          "the root is not a directory");

    check(rpc_path(HOSTFS_OP_STAT, 0, "nosuch.txt") == HOSTFS_EFILNF,
          "STAT of a missing file did not give EFILNF");
    check(rpc_path(HOSTFS_OP_STAT, 0, "nosuchdir/x") == HOSTFS_EPTHNF,
          "STAT below a missing directory did not give EPTHNF");

    /* Read-only is the one attribute with a host meaning. */
    host_write("ro.txt", "ro");
    host_path(p, sizeof(p), "ro.txt");
    chmod(p, 0444);
    check(rpc_path(HOSTFS_OP_STAT, 0, "ro.txt") == HOSTFS_E_OK, "STAT ro.txt");
    check(reply_data()[0] & HOSTFS_FA_RO, "a 0444 file is not read-only");

    check(rpc_path(HOSTFS_OP_SETATTR, 0, "ro.txt") == HOSTFS_E_OK,
          "SETATTR clearing read-only");
    check(rpc_path(HOSTFS_OP_STAT, 0, "ro.txt") == HOSTFS_E_OK, "STAT ro.txt");
    check(!(reply_data()[0] & HOSTFS_FA_RO), "read-only was not cleared");

    check(rpc_path(HOSTFS_OP_SETATTR, HOSTFS_FA_RO, "ro.txt") == HOSTFS_E_OK,
          "SETATTR setting read-only");
    check(rpc_path(HOSTFS_OP_STAT, 0, "ro.txt") == HOSTFS_E_OK, "STAT ro.txt");
    check(reply_data()[0] & HOSTFS_FA_RO, "read-only was not set");
    host_path(p, sizeof(p), "ro.txt");
    chmod(p, 0666);
}

static void test_file_roundtrip(void)
{
    int fh;
    char p[512];

    check(rpc_path(HOSTFS_OP_OPEN, HOSTFS_O_RDONLY, "hello.txt")
          == HOSTFS_E_OK, "OPEN hello.txt");
    fh = (int)reply_a;
    check(fh > 0, "OPEN gave handle %d", fh);

    check(rpc(HOSTFS_OP_READ, fh, 0, 5, NULL, 0) == HOSTFS_E_OK, "READ");
    check(reply_a == 5 && reply_datalen() == 5
          && !memcmp(reply_data(), "hello", 5), "READ returned the wrong bytes");

    /* Reads are positioned, not sequential: the guest's Fseek lives on
     * its side of the wire and every READ carries its own offset. */
    check(rpc(HOSTFS_OP_READ, fh, 6, 5, NULL, 0) == HOSTFS_E_OK, "READ at 6");
    check(reply_datalen() == 5 && !memcmp(reply_data(), "world", 5),
          "READ at an offset returned the wrong bytes");

    check(rpc(HOSTFS_OP_READ, fh, 11, 10, NULL, 0) == HOSTFS_E_OK,
          "READ at EOF");
    check(reply_a == 0 && reply_datalen() == 0, "READ at EOF returned data");

    check(rpc(HOSTFS_OP_READ, fh, 1000, 10, NULL, 0) == HOSTFS_E_OK,
          "READ past EOF");
    check(reply_a == 0, "READ past EOF returned %lu bytes", reply_a);

    check(rpc(HOSTFS_OP_CLOSE, fh, 0, 0, NULL, 0) == HOSTFS_E_OK, "CLOSE");
    check(rpc(HOSTFS_OP_CLOSE, fh, 0, 0, NULL, 0) == HOSTFS_EIHNDL,
          "a double CLOSE was accepted");

    /* CREATE, WRITE, and the host folder actually changing. */
    check(rpc_path(HOSTFS_OP_CREATE, 0, "new.txt") == HOSTFS_E_OK, "CREATE");
    fh = (int)reply_a;
    check(rpc(HOSTFS_OP_WRITE, fh, 0, 0, "abcdef", 6) == HOSTFS_E_OK, "WRITE");
    check(reply_a == 6, "WRITE reported %lu bytes", reply_a);
    check(rpc(HOSTFS_OP_WRITE, fh, 3, 0, "XYZ", 3) == HOSTFS_E_OK,
          "WRITE at an offset");
    check(rpc(HOSTFS_OP_CLOSE, fh, 0, 0, NULL, 0) == HOSTFS_E_OK, "CLOSE");

    host_path(p, sizeof(p), "new.txt");
    {
        char buf[16];
        FILE *f = fopen(p, "rb");
        size_t n = f ? fread(buf, 1, sizeof(buf), f) : 0;
        if (f)
            fclose(f);
        /* The write is verified on the HOST, not by asking the helper
         * what it thinks it wrote -- that is the check vvfat failed. */
        check(n == 6 && !memcmp(buf, "abcXYZ", 6),
              "the host file does not hold what was written");
    }

    /* CREATE truncates, as GEMDOS Fcreate does. */
    check(rpc_path(HOSTFS_OP_CREATE, 0, "new.txt") == HOSTFS_E_OK,
          "CREATE over an existing file");
    check(rpc(HOSTFS_OP_CLOSE, reply_a, 0, 0, NULL, 0) == HOSTFS_E_OK, "CLOSE");
    check(rpc_path(HOSTFS_OP_STAT, 0, "new.txt") == HOSTFS_E_OK, "STAT new.txt");
    check(reply_a == 0, "CREATE did not truncate: size %lu", reply_a);

    check(rpc_path(HOSTFS_OP_OPEN, HOSTFS_O_RDONLY, "nosuch") == HOSTFS_EFILNF,
          "OPEN of a missing file did not give EFILNF");
    check(rpc_path(HOSTFS_OP_OPEN, HOSTFS_O_RDONLY, "sub") == HOSTFS_EACCDN,
          "OPEN of a directory was allowed");

    check(rpc(HOSTFS_OP_READ, 99, 0, 4, NULL, 0) == HOSTFS_EIHNDL,
          "READ on an unopened handle");
    check(rpc(HOSTFS_OP_READ, 0, 0, 4, NULL, 0) == HOSTFS_EIHNDL,
          "handle 0 was accepted");
    check(rpc(HOSTFS_OP_WRITE, 7, 0, 0, "x", 1) == HOSTFS_EIHNDL,
          "WRITE on an unopened handle");
}

static void test_big_file(void)
{
    /* Bigger than one frame in both directions, so the chunking on both
     * sides is exercised rather than assumed. */
    enum { BIG = 200000 };
    unsigned char *want = malloc(BIG);
    unsigned char *got = malloc(BIG);
    unsigned long off;
    int fh, ok = 1;
    unsigned i;

    if (!want || !got)
        die("malloc");
    for (i = 0; i < BIG; i++)
        want[i] = (unsigned char)(i * 7 + (i >> 8));

    check(rpc_path(HOSTFS_OP_CREATE, 0, "big.bin") == HOSTFS_E_OK, "CREATE big");
    fh = (int)reply_a;

    for (off = 0; off < BIG; )
    {
        unsigned n = BIG - off > HOSTFS_MAX_REQ_DATA
                   ? HOSTFS_MAX_REQ_DATA : (unsigned)(BIG - off);

        if (rpc(HOSTFS_OP_WRITE, fh, off, 0, want + off, n) != HOSTFS_E_OK
         || reply_a != n)
        {
            ok = 0;
            break;
        }
        off += n;
    }
    check(ok, "writing a large file failed part way through");
    check(rpc(HOSTFS_OP_CLOSE, fh, 0, 0, NULL, 0) == HOSTFS_E_OK, "CLOSE big");

    check(rpc_path(HOSTFS_OP_STAT, 0, "big.bin") == HOSTFS_E_OK, "STAT big");
    check(reply_a == BIG, "big.bin is %lu bytes, want %d", reply_a, BIG);

    check(rpc_path(HOSTFS_OP_OPEN, HOSTFS_O_RDONLY, "big.bin") == HOSTFS_E_OK,
          "OPEN big");
    fh = (int)reply_a;

    /* Ask for more than a frame can hold: the helper must clamp rather
     * than either truncate the frame or refuse. */
    check(rpc(HOSTFS_OP_READ, fh, 0, 65535, NULL, 0) == HOSTFS_E_OK,
          "an oversized READ count was refused");
    check(reply_a == HOSTFS_MAX_REP_DATA,
          "an oversized READ count was not clamped: %lu", reply_a);

    ok = 1;
    for (off = 0; off < BIG; )
    {
        if (rpc(HOSTFS_OP_READ, fh, off, 4000, NULL, 0) != HOSTFS_E_OK
         || reply_a == 0 || reply_datalen() != reply_a)
        {
            ok = 0;
            break;
        }
        memcpy(got + off, reply_data(), reply_datalen());
        off += reply_a;
    }
    check(ok, "reading a large file failed part way through");
    check(ok && !memcmp(want, got, BIG), "a large file did not round-trip");
    check(rpc(HOSTFS_OP_CLOSE, fh, 0, 0, NULL, 0) == HOSTFS_E_OK, "CLOSE big");

    free(want);
    free(got);
}

static void test_dir_ops(void)
{
    char p[512];

    check(rpc_path(HOSTFS_OP_MKDIR, 0, "made") == HOSTFS_E_OK, "MKDIR");
    host_path(p, sizeof(p), "made");
    check(host_exists(p), "MKDIR did not create the folder");
    check(rpc_path(HOSTFS_OP_MKDIR, 0, "made") == HOSTFS_EACCDN,
          "MKDIR over an existing folder was allowed");

    check(rpc_path(HOSTFS_OP_MKDIR, 0, "made/deep") == HOSTFS_E_OK,
          "MKDIR in a subfolder");
    check(rpc_path(HOSTFS_OP_RMDIR, 0, "made") == HOSTFS_EACCDN,
          "RMDIR of a non-empty folder was allowed");
    check(rpc_path(HOSTFS_OP_RMDIR, 0, "made/deep") == HOSTFS_E_OK, "RMDIR");
    check(rpc_path(HOSTFS_OP_RMDIR, 0, "made") == HOSTFS_E_OK, "RMDIR");
    check(!host_exists(p), "RMDIR did not remove the folder");

    host_write("doomed.txt", "x");
    check(rpc_path(HOSTFS_OP_DELETE, 0, "doomed.txt") == HOSTFS_E_OK, "DELETE");
    host_path(p, sizeof(p), "doomed.txt");
    check(!host_exists(p), "DELETE did not remove the file");
    check(rpc_path(HOSTFS_OP_DELETE, 0, "doomed.txt") == HOSTFS_EFILNF,
          "DELETE of a missing file did not give EFILNF");
    check(rpc_path(HOSTFS_OP_DELETE, 0, "sub") == HOSTFS_EACCDN,
          "DELETE of a directory was allowed");

    /* RENAME carries both paths in one payload, NUL-separated. */
    host_write("from.txt", "moved");
    {
        char both[64];
        unsigned n = 0;

        memcpy(both, "from.txt", 8); n = 8;
        both[n++] = '\0';
        memcpy(both + n, "sub/to.txt", 10); n += 10;
        check(rpc(HOSTFS_OP_RENAME, 0, 0, 0, both, n) == HOSTFS_E_OK, "RENAME");
    }
    host_path(p, sizeof(p), "sub/to.txt");
    check(host_exists(p), "RENAME did not move the file");
    host_path(p, sizeof(p), "from.txt");
    check(!host_exists(p), "RENAME left the original behind");

    check(rpc(HOSTFS_OP_RENAME, 0, 0, 0, "onlyone", 7) == HOSTFS_EPTHNF,
          "RENAME without a separator was accepted");

    check(rpc(HOSTFS_OP_DFREE, 0, 0, 0, NULL, 0) == HOSTFS_E_OK, "DFREE");
    check(reply_b > 0, "DFREE reported a zero-sized volume");
    check(reply_a <= reply_b, "DFREE free %lu exceeds total %lu",
          reply_a, reply_b);
    check(reply_b <= HOSTFS_DFREE_MAX_KB, "DFREE was not clamped: %lu",
          reply_b);
}

/*
 * A directory with far more entries than any plausible batch, read one
 * at a time to the end. The 9p transport batched, and the batching is
 * where the cookie bug lived; this is the shape that cannot repeat it.
 */
#define MANY 500

static void test_readdir(void)
{
    char p[512];
    char name[32];
    int seen[MANY];
    int dh, i, n, ok;
    unsigned long cookie;
    char last[HOSTFS_MAX_NAME + 1];

    host_path(p, sizeof(p), "many");
    if (mkdir(p, 0777) != 0)
        die("mkdir many");
    for (i = 0; i < MANY; i++)
    {
        snprintf(name, sizeof(name), "many/f%03d.txt", i);
        host_write(name, "x");
    }

    check(rpc_path(HOSTFS_OP_OPENDIR, 0, "many") == HOSTFS_E_OK, "OPENDIR");
    dh = (int)reply_a;

    memset(seen, 0, sizeof(seen));
    last[0] = '\0';
    cookie = 0;
    n = 0;
    ok = 1;

    for (;;)
    {
        int st = rpc(HOSTFS_OP_READDIR, dh, cookie, 0, NULL, 0);
        char got[HOSTFS_MAX_NAME + 1];
        int idx;

        if (st == HOSTFS_ENMFIL)
            break;
        if (st != HOSTFS_E_OK || reply_datalen() == 0)
        {
            ok = 0;
            break;
        }
        if (++n > MANY + 8)     /* a cookie that never advances */
        {
            ok = 0;
            break;
        }

        memcpy(got, reply_data(), reply_datalen());
        got[reply_datalen()] = '\0';

        /* Sorted, so a golden can be written against a listing. */
        if (last[0] && strcmp(last, got) >= 0)
            check(0, "READDIR is out of order: %s then %s", last, got);
        strcpy(last, got);

        if (sscanf(got, "f%d.txt", &idx) == 1 && idx >= 0 && idx < MANY)
        {
            check(!seen[idx], "READDIR returned %s twice", got);
            seen[idx] = 1;
        }
        else
            check(0, "READDIR returned an unexpected name %s", got);

        cookie = reply_a;
    }

    check(ok, "READDIR did not terminate cleanly");
    check(n == MANY, "READDIR returned %d of %d entries", n, MANY);
    for (i = 0; i < MANY; i++)
        if (!seen[i])
        {
            check(0, "READDIR never returned f%03d.txt", i);
            break;
        }

    /* An exhausted enumeration keeps saying so; it does not restart. */
    check(rpc(HOSTFS_OP_READDIR, dh, cookie, 0, NULL, 0) == HOSTFS_ENMFIL,
          "READDIR past the end did not give ENMFIL");
    check(rpc(HOSTFS_OP_READDIR, dh, 999999, 0, NULL, 0) == HOSTFS_ENMFIL,
          "a cookie past the end was not ENMFIL");

    check(rpc(HOSTFS_OP_CLOSEDIR, dh, 0, 0, NULL, 0) == HOSTFS_E_OK,
          "CLOSEDIR");
    check(rpc(HOSTFS_OP_CLOSEDIR, dh, 0, 0, NULL, 0) == HOSTFS_EIHNDL,
          "a double CLOSEDIR was accepted");
    check(rpc(HOSTFS_OP_READDIR, dh, 0, 0, NULL, 0) == HOSTFS_EIHNDL,
          "READDIR on a closed handle");
    check(rpc_path(HOSTFS_OP_OPENDIR, 0, "hello.txt") == HOSTFS_EPTHNF,
          "OPENDIR of a file was allowed");
    check(rpc_path(HOSTFS_OP_OPENDIR, 0, "nosuch") == HOSTFS_EFILNF,
          "OPENDIR of a missing folder");

    /* An empty folder is an immediate end, not an error. */
    check(rpc_path(HOSTFS_OP_MKDIR, 0, "bare") == HOSTFS_E_OK, "MKDIR bare");
    check(rpc_path(HOSTFS_OP_OPENDIR, 0, "bare") == HOSTFS_E_OK, "OPENDIR bare");
    dh = (int)reply_a;
    check(rpc(HOSTFS_OP_READDIR, dh, 0, 0, NULL, 0) == HOSTFS_ENMFIL,
          "an empty folder did not read as empty");
    check(rpc(HOSTFS_OP_CLOSEDIR, dh, 0, 0, NULL, 0) == HOSTFS_E_OK,
          "CLOSEDIR bare");
}

/*
 * The directory changing mid-enumeration. The snapshot taken at OPENDIR
 * decides what happens, and what happens has to be documented behaviour
 * rather than whatever the host filesystem felt like doing.
 */
static void test_readdir_churn(void)
{
    char p[512];
    int dh, n = 0;
    unsigned long cookie = 0;
    int saw_added = 0, saw_deleted = 0;

    host_path(p, sizeof(p), "churn");
    if (mkdir(p, 0777) != 0)
        die("mkdir churn");
    host_write("churn/a.txt", "a");
    host_write("churn/b.txt", "b");
    host_write("churn/c.txt", "c");
    host_write("churn/d.txt", "d");

    check(rpc_path(HOSTFS_OP_OPENDIR, 0, "churn") == HOSTFS_E_OK,
          "OPENDIR churn");
    dh = (int)reply_a;

    /* First entry, then change the folder underneath the enumeration. */
    check(rpc(HOSTFS_OP_READDIR, dh, cookie, 0, NULL, 0) == HOSTFS_E_OK,
          "READDIR churn");
    cookie = reply_a;
    n++;

    host_write("churn/zz-added.txt", "new");
    host_path(p, sizeof(p), "churn/d.txt");
    remove(p);

    for (;;)
    {
        char got[HOSTFS_MAX_NAME + 1];
        int st = rpc(HOSTFS_OP_READDIR, dh, cookie, 0, NULL, 0);

        if (st == HOSTFS_ENMFIL)
            break;
        if (st != HOSTFS_E_OK || ++n > 16)
        {
            check(0, "READDIR churn did not terminate");
            break;
        }
        memcpy(got, reply_data(), reply_datalen());
        got[reply_datalen()] = '\0';
        if (!strcmp(got, "zz-added.txt"))
            saw_added = 1;
        if (!strcmp(got, "d.txt"))
            saw_deleted = 1;
        cookie = reply_a;
    }

    /* Documented behaviour: the enumeration is of the folder as it was
     * at OPENDIR, so a new file waits for the next one -- but a name the
     * guest could no longer stat is skipped rather than handed over. */
    check(!saw_added, "a file created mid-enumeration appeared in it");
    check(!saw_deleted, "a file deleted mid-enumeration was still returned");
    check(n == 3, "churned enumeration returned %d entries, want 3", n);

    check(rpc(HOSTFS_OP_CLOSEDIR, dh, 0, 0, NULL, 0) == HOSTFS_E_OK,
          "CLOSEDIR churn");

    /* The next OPENDIR sees the new state. */
    check(rpc_path(HOSTFS_OP_OPENDIR, 0, "churn") == HOSTFS_E_OK,
          "OPENDIR churn again");
    dh = (int)reply_a;
    n = 0;
    cookie = 0;
    saw_added = 0;
    while (rpc(HOSTFS_OP_READDIR, dh, cookie, 0, NULL, 0) == HOSTFS_E_OK
           && ++n < 16)
    {
        if (reply_datalen() == 12
         && !memcmp(reply_data(), "zz-added.txt", 12))
            saw_added = 1;
        cookie = reply_a;
    }
    check(saw_added, "a re-opened folder still did not list the new file");
    check(rpc(HOSTFS_OP_CLOSEDIR, dh, 0, 0, NULL, 0) == HOSTFS_E_OK,
          "CLOSEDIR churn");
}

static void test_handle_limits(void)
{
    int handles[64];
    int i, opened = 0, exhausted = 0;

    for (i = 0; i < 64; i++)
    {
        int st = rpc_path(HOSTFS_OP_OPEN, HOSTFS_O_RDONLY, "hello.txt");

        if (st == HOSTFS_ENHNDL)
        {
            exhausted = 1;
            break;
        }
        if (st != HOSTFS_E_OK)
            break;
        handles[opened++] = (int)reply_a;
    }
    check(exhausted, "the file handle table never reported ENHNDL");
    check(opened > 0 && opened <= 64, "opened %d handles", opened);

    for (i = 0; i < opened; i++)
        if (rpc(HOSTFS_OP_CLOSE, handles[i], 0, 0, NULL, 0) != HOSTFS_E_OK)
        {
            check(0, "CLOSE of handle %d failed", handles[i]);
            break;
        }

    /* Handles come back after they are closed. */
    check(rpc_path(HOSTFS_OP_OPEN, HOSTFS_O_RDONLY, "hello.txt")
          == HOSTFS_E_OK, "OPEN after the table was emptied");
    check(rpc(HOSTFS_OP_CLOSE, reply_a, 0, 0, NULL, 0) == HOSTFS_E_OK, "CLOSE");

    /* A HELLO is a new session: it must drop whatever the last one left
     * open, or a rebooted guest inherits handles it never opened. */
    check(rpc_path(HOSTFS_OP_OPEN, HOSTFS_O_RDONLY, "hello.txt")
          == HOSTFS_E_OK, "OPEN before HELLO");
    i = (int)reply_a;
    check(rpc(HOSTFS_OP_HELLO, HOSTFS_PROTO_VERSION, 0, 0, NULL, 0)
          == HOSTFS_E_OK, "HELLO");
    check(rpc(HOSTFS_OP_CLOSE, i, 0, 0, NULL, 0) == HOSTFS_EIHNDL,
          "a handle survived a HELLO");
}

/* ------------------------------------------------------------------ */
/* Case folding                                                        */

/*
 * GEMDOS lookup ignores case, and the guest cannot boot without it:
 * EmuTOS scans \AUTO in upper case while FreeMiNT asks for
 * \mint\1-19-cur\ and xaaes/xaloader.prg in lower case out of compiled-in
 * string literals. Linux is case-sensitive, so the helper has to do the
 * folding the guest used to do for itself.
 */

static int host_size(const char *rel)
{
    char p[512];
    struct stat st;

    host_path(p, sizeof(p), rel);
    return lstat(p, &st) == 0 ? (int)st.st_size : -1;
}

static int host_present(const char *rel)
{
    char p[512];

    host_path(p, sizeof(p), rel);
    return host_exists(p);
}

static void test_case_folding(void)
{
    char p[512];
    int fh, first, i;

    host_path(p, sizeof(p), "casefold");
    if (mkdir(p, 0777) != 0)
        die("mkdir casefold");

    host_write("casefold/lower.txt", "lower");            /* 5 bytes */
    host_write("casefold/UPPER.TXT", "uppercase");        /* 9 bytes */

    /* Lower case on disk, upper case on the wire -- the AUTO folder
     * shape, which is how EmuTOS asks. */
    check(rpc_path(HOSTFS_OP_STAT, 0, "CASEFOLD/LOWER.TXT") == HOSTFS_E_OK,
          "an upper-case request did not find a lower-case file");
    check(reply_a == 5, "the folded lookup found the wrong file: %lu bytes",
          reply_a);

    /* And the reverse -- the FreeMiNT shape. */
    check(rpc_path(HOSTFS_OP_STAT, 0, "casefold/upper.txt") == HOSTFS_E_OK,
          "a lower-case request did not find an upper-case file");
    check(reply_a == 9, "the folded lookup found the wrong file: %lu bytes",
          reply_a);

    /* Mixed in both directions, and the exact spelling still works. */
    check(rpc_path(HOSTFS_OP_STAT, 0, "CaseFold/LoWeR.TxT") == HOSTFS_E_OK,
          "a mixed-case request did not find the file");
    check(reply_a == 5, "the mixed-case lookup found the wrong file");
    check(rpc_path(HOSTFS_OP_STAT, 0, "casefold/lower.txt") == HOSTFS_E_OK,
          "the exact spelling stopped working");
    check(reply_a == 5, "the exact spelling found the wrong file");

    /* Folding is a lookup, not a wildcard: a name that matches nothing
     * in any case is still missing. */
    check(rpc_path(HOSTFS_OP_STAT, 0, "casefold/NOSUCH.TXT") == HOSTFS_EFILNF,
          "a folded lookup invented a file");

    /* Every operation goes through the same resolver, so every operation
     * folds. */
    check(rpc_path(HOSTFS_OP_OPEN, HOSTFS_O_RDONLY, "CASEFOLD/LOWER.TXT")
          == HOSTFS_E_OK, "OPEN did not fold");
    fh = (int)reply_a;
    check(rpc(HOSTFS_OP_READ, fh, 0, 5, NULL, 0) == HOSTFS_E_OK
          && reply_datalen() == 5 && !memcmp(reply_data(), "lower", 5),
          "a folded OPEN returned the wrong file's bytes");
    check(rpc(HOSTFS_OP_CLOSE, fh, 0, 0, NULL, 0) == HOSTFS_E_OK, "CLOSE");
    check(rpc_path(HOSTFS_OP_OPENDIR, 0, "CASEFOLD") == HOSTFS_E_OK,
          "OPENDIR did not fold");
    check(rpc(HOSTFS_OP_CLOSEDIR, reply_a, 0, 0, NULL, 0) == HOSTFS_E_OK,
          "CLOSEDIR");

    /*
     * The real thing: several components, each spelled differently from
     * the disk. This is the FreeMiNT install tree, which is what the fix
     * exists for.
     */
    host_path(p, sizeof(p), "mint");
    if (mkdir(p, 0777) != 0)
        die("mkdir mint");
    host_path(p, sizeof(p), "mint/1-19-cur");
    if (mkdir(p, 0777) != 0)
        die("mkdir 1-19-cur");
    host_path(p, sizeof(p), "mint/1-19-cur/XAAES");
    if (mkdir(p, 0777) != 0)
        die("mkdir XAAES");
    host_write("mint/1-19-cur/XAAES/xaloader.prg", "xaloader");

    check(rpc_path(HOSTFS_OP_STAT, 0, "MINT/1-19-CUR/XAAES/XALOADER.PRG")
          == HOSTFS_E_OK, "an all-upper-case nested path was not found");
    check(rpc_path(HOSTFS_OP_STAT, 0, "mint/1-19-cur/xaaes/xaloader.prg")
          == HOSTFS_E_OK, "an all-lower-case nested path was not found");
    check(rpc_path(HOSTFS_OP_STAT, 0, "MiNt/1-19-CUR/xaAeS/XaLoAdEr.PrG")
          == HOSTFS_E_OK, "a nested path spelled every which way was not found");
    check(reply_a == 8, "the nested folded lookup found the wrong file");

    /* Folding a directory component does not fold the leaf away: a
     * missing leaf under a folded directory is still missing. */
    check(rpc_path(HOSTFS_OP_STAT, 0, "MINT/1-19-CUR/XAAES/NOSUCH.PRG")
          == HOSTFS_EFILNF, "a folded directory invented a leaf");

    /*
     * Two entries differing only in case.
     *
     * An exact spelling must select the exact entry -- that is the rule
     * that makes an unambiguous name unambiguous. The ambiguous spelling
     * takes the first readdir() hit, which is deliberate: it is what
     * Hatari does, and matching Hatari is the decision recorded in
     * docs/phase5-hostfs-design.md. It is not required to be the same
     * entry on another host, only to be one of them and to be the same
     * one every time within a run.
     */
    host_path(p, sizeof(p), "dup");
    if (mkdir(p, 0777) != 0)
        die("mkdir dup");
    host_write("dup/Case.txt", "AAA");          /* 3 bytes */
    host_write("dup/CASE.TXT", "bbbbbb");       /* 6 bytes */

    if (host_size("dup/Case.txt") == 3 && host_size("dup/CASE.TXT") == 6)
    {
        check(rpc_path(HOSTFS_OP_STAT, 0, "dup/Case.txt") == HOSTFS_E_OK
              && reply_a == 3,
              "an exact spelling did not win over a case-insensitive twin");
        check(rpc_path(HOSTFS_OP_STAT, 0, "dup/CASE.TXT") == HOSTFS_E_OK
              && reply_a == 6,
              "the other exact spelling did not win either");

        check(rpc_path(HOSTFS_OP_STAT, 0, "dup/case.TXT") == HOSTFS_E_OK,
              "an ambiguous spelling was not resolved at all");
        first = (int)reply_a;
        check(first == 3 || first == 6,
              "an ambiguous spelling resolved to neither candidate: %d", first);

        /* Stable within a run: readdir order does not wander. */
        for (i = 0; i < 4; i++)
        {
            check(rpc_path(HOSTFS_OP_STAT, 0, "dup/case.TXT") == HOSTFS_E_OK
                  && (int)reply_a == first,
                  "an ambiguous lookup was not stable across repeats");
        }
    }
    else
    {
        /* A case-insensitive host cannot hold both, and there is then
         * nothing to be ambiguous about. */
        check(host_present("dup/Case.txt"),
              "neither spelling of the ambiguous pair exists");
    }

    /*
     * CREATION USES THE GUEST'S SPELLING. Folding is a lookup rule; it
     * must never rename anything and must never decide what a new file
     * is called.
     */
    check(rpc_path(HOSTFS_OP_CREATE, 0, "casefold/NewFile.TxT")
          == HOSTFS_E_OK, "CREATE with a mixed-case name");
    check(rpc(HOSTFS_OP_CLOSE, reply_a, 0, 0, NULL, 0) == HOSTFS_E_OK, "CLOSE");
    check(host_present("casefold/NewFile.TxT"),
          "CREATE did not use the name the guest gave");
    check(!host_present("casefold/newfile.txt")
          && !host_present("casefold/NEWFILE.TXT"),
          "CREATE invented a different case for the new file");

    check(rpc_path(HOSTFS_OP_MKDIR, 0, "casefold/MadeDir") == HOSTFS_E_OK,
          "MKDIR with a mixed-case name");
    check(host_present("casefold/MadeDir"),
          "MKDIR did not use the name the guest gave");
    check(!host_present("casefold/madedir"),
          "MKDIR invented a different case for the new folder");

    /* But a CREATE whose name folds onto an existing file reopens THAT
     * file, as GEMDOS does -- it does not lay a second entry beside it. */
    host_write("casefold/trunc.txt", "abcdef");
    check(rpc_path(HOSTFS_OP_CREATE, 0, "casefold/TRUNC.TXT") == HOSTFS_E_OK,
          "CREATE over a differently-cased file");
    check(rpc(HOSTFS_OP_CLOSE, reply_a, 0, 0, NULL, 0) == HOSTFS_E_OK, "CLOSE");
    check(!host_present("casefold/TRUNC.TXT"),
          "CREATE made a second entry differing only in case");
    check(host_size("casefold/trunc.txt") == 0,
          "CREATE did not truncate the file its name folded onto");

    /* Writing operations resolve the same way. */
    host_write("casefold/gone.txt", "x");
    check(rpc_path(HOSTFS_OP_DELETE, 0, "casefold/GONE.TXT") == HOSTFS_E_OK,
          "DELETE did not fold");
    check(!host_present("casefold/gone.txt"), "DELETE removed nothing");

    {
        char both[64];
        unsigned n = 0;

        host_write("casefold/src.txt", "moved");
        memcpy(both, "CASEFOLD/SRC.TXT", 16); n = 16;
        both[n++] = '\0';
        memcpy(both + n, "CaseFold/Dst.TXT", 16); n += 16;
        check(rpc(HOSTFS_OP_RENAME, 0, 0, 0, both, n) == HOSTFS_E_OK,
              "RENAME did not fold the source");
    }
    check(!host_present("casefold/src.txt"), "the folded RENAME moved nothing");
    check(host_present("casefold/Dst.TXT"),
          "RENAME did not give the destination the guest's spelling");
}

/* ------------------------------------------------------------------ */
/* Containment                                                         */

/*
 * Every one of these is an attempt to touch tmproot/outside.txt, which
 * sits one level above the served folder. The guest is not a trusted
 * peer: it is emulated code running whatever the user launched.
 */
static void refuse(const char *path, const char *why)
{
    int st = rpc(HOSTFS_OP_STAT, 0, 0, 0, path, (unsigned)strlen(path));

    check(st != HOSTFS_E_OK, "escape allowed (%s): STAT was accepted", why);

    st = rpc(HOSTFS_OP_OPEN, HOSTFS_O_RDONLY, 0, 0, path,
             (unsigned)strlen(path));
    check(st != HOSTFS_E_OK, "escape allowed (%s): OPEN was accepted", why);
    if (st == HOSTFS_E_OK)
        rpc(HOSTFS_OP_CLOSE, reply_a, 0, 0, NULL, 0);
}

/* The same path, but through the operations that CREATE things. A read
 * that escapes leaks; a write that escapes destroys. */
static void refuse_write(const char *path, const char *why)
{
    int st = rpc(HOSTFS_OP_CREATE, 0, 0, 0, path, (unsigned)strlen(path));

    check(st != HOSTFS_E_OK, "escape allowed (%s): CREATE was accepted", why);
    if (st == HOSTFS_E_OK)
        rpc(HOSTFS_OP_CLOSE, reply_a, 0, 0, NULL, 0);

    check(rpc(HOSTFS_OP_MKDIR, 0, 0, 0, path, (unsigned)strlen(path))
          != HOSTFS_E_OK, "escape allowed (%s): MKDIR was accepted", why);
    check(rpc(HOSTFS_OP_DELETE, 0, 0, 0, path, (unsigned)strlen(path))
          != HOSTFS_E_OK, "escape allowed (%s): DELETE was accepted", why);
    check(rpc(HOSTFS_OP_OPENDIR, 0, 0, 0, path, (unsigned)strlen(path))
          != HOSTFS_E_OK, "escape allowed (%s): OPENDIR was accepted", why);
    check(rpc(HOSTFS_OP_SETATTR, HOSTFS_FA_RO, 0, 0, path,
              (unsigned)strlen(path))
          != HOSTFS_E_OK, "escape allowed (%s): SETATTR was accepted", why);
}

static void test_containment(void)
{
    char p[512];
    char longpath[HOSTFS_MAX_PATH + 64];
    struct stat before, after;

    snprintf(p, sizeof(p), "%s/outside.txt", tmproot);
    if (stat(p, &before) != 0)
        die("stat outside.txt");

    /* Textual traversal, in every spelling. */
    refuse("..", "bare ..");
    refuse("../outside.txt", "parent traversal");
    refuse("../../etc/passwd", "double parent traversal");
    refuse("sub/../../outside.txt", "traversal through a real subfolder");
    refuse("sub/../..", "traversal ending at the parent");
    refuse("./../outside.txt", "traversal behind a dot component");
    refuse(".", "bare dot");
    refuse("sub/.", "trailing dot component");

    /* Absolute paths: drive C is the root of the guest's world. */
    refuse("/etc/passwd", "absolute path");
    refuse("/", "the host root");
    refuse(p, "the absolute host path of the target");

    /* Windows spellings, which a Linux-only check would wave through and
     * a Windows host would then honour. */
    refuse("..\\outside.txt", "backslash traversal");
    refuse("sub\\inner.txt", "backslash separator");
    refuse("\\outside.txt", "leading backslash");
    refuse("C:\\windows\\win.ini", "drive letter");
    refuse("C:outside.txt", "drive-relative path");
    refuse("//server/share/secret", "UNC prefix");
    refuse("\\\\server\\share\\secret", "backslash UNC prefix");
    refuse("hello.txt:stream", "NTFS alternate data stream");

    /*
     * Names Windows folds onto something else, or onto a device.
     *
     * These exist as REAL FILES in the served folder for the duration of
     * the check: on Linux "NUL" and "trailing. " are ordinary names, so
     * an assertion that they are refused would pass by accident if the
     * files were absent, and the rule that makes the two hosts behave
     * alike would go untested on the one that runs the tests.
     */
    host_write("NUL", "device");
    host_write("CON", "device");
    host_write("aux.txt", "device");
    host_write("COM1", "device");
    host_write("LPT9", "device");
    host_write("sub/NUL", "device");
    host_write("trail.", "folded");
    host_write("trail ", "folded");
    refuse("trail.", "a real file whose name ends in a dot");
    refuse("trail ", "a real file whose name ends in a space");
    refuse("hello.txt.", "trailing dot");
    refuse("hello.txt ", "trailing space");
    refuse("NUL", "device name NUL");
    refuse("nul", "device name nul, lower case");
    refuse("CON", "device name CON");
    refuse("aux.txt", "device name with an extension");
    refuse("sub/NUL", "device name in a subfolder");
    refuse("COM1", "device name COM1");
    refuse("LPT9", "device name LPT9");

    /* Nothing above refused merely because the file was missing. */
    {
        char q[512];

        host_path(q, sizeof(q), "NUL");
        check(host_exists(q), "the NUL fixture was not created");
        host_path(q, sizeof(q), "trail.");
        check(host_exists(q), "the trailing-dot fixture was not created");
    }

    /* Malformed rather than hostile, but nothing sends them by accident. */
    refuse("sub//inner.txt", "empty component");
    refuse("sub/", "trailing separator");
    refuse("hello*.txt", "wildcard");
    refuse("hello?.txt", "wildcard");
    refuse("\001bad", "control character");

    /* A path with an embedded NUL: the length field says one thing and
     * a C string says another, and the difference is the attack. */
    {
        const char embedded[] = "hello.txt\0/../../outside.txt";
        check(rpc(HOSTFS_OP_STAT, 0, 0, 0, embedded, sizeof(embedded) - 1)
              != HOSTFS_E_OK, "escape allowed: a path with an embedded NUL");
        check(rpc(HOSTFS_OP_STAT, 0, 0, 0, "\0", 1) != HOSTFS_E_OK,
              "escape allowed: a path that is one NUL");
    }

    /* Longer than any path may be. */
    memset(longpath, 'a', sizeof(longpath) - 1);
    longpath[sizeof(longpath) - 1] = '\0';
    check(rpc(HOSTFS_OP_STAT, 0, 0, 0, longpath, (unsigned)strlen(longpath))
          == HOSTFS_EPTHNF, "an over-long path was not refused");

    /*
     * Symlinks. The check has to be on the RESOLVED path: nothing about
     * "escape/outside.txt" looks wrong textually, and that is exactly
     * why a textual check is not enough.
     */
    {
        char link[512];

        host_path(link, sizeof(link), "escape");
        if (symlink(tmproot, link) != 0)
            die("symlink escape");
        refuse("escape/outside.txt", "symlink to the parent folder");
        refuse("escape", "the escaping symlink itself");
        refuse_write("escape/created.txt", "writing through an escaping symlink");

        host_path(link, sizeof(link), "escapefile");
        snprintf(p, sizeof(p), "%s/outside.txt", tmproot);
        if (symlink(p, link) != 0)
            die("symlink escapefile");
        refuse("escapefile", "symlink straight to a file outside");

        host_path(link, sizeof(link), "escaperoot");
        if (symlink("/etc", link) != 0)
            die("symlink escaperoot");
        refuse("escaperoot/passwd", "symlink to an absolute path outside");

        /* A symlink that stays inside is ordinary and must still work,
         * or the check is refusing by accident rather than by rule. */
        host_path(link, sizeof(link), "inside");
        if (symlink("hello.txt", link) != 0)
            die("symlink inside");
        check(rpc_path(HOSTFS_OP_STAT, 0, "inside") == HOSTFS_E_OK,
              "a symlink pointing inside the folder was refused");
    }

    /* A folder next to the served one whose name starts the same way:
     * "served-evil" must not pass as a child of "served". */
    {
        char sibling[512];

        snprintf(sibling, sizeof(sibling), "%s/served-evil", tmproot);
        if (mkdir(sibling, 0777) != 0 && errno != EEXIST)
            die("mkdir served-evil");
        host_path(p, sizeof(p), "toSibling");
        if (symlink(sibling, p) != 0)
            die("symlink toSibling");
        refuse("toSibling", "a sibling folder sharing the root's prefix");
    }

    /*
     * The same attacks again, through the CASE-INSENSITIVE lookup.
     *
     * Folding widened what a request can name, so every rule above has
     * to hold for every spelling of it and not just the one the rule was
     * written against. Containment is proved on the RESOLVED path, after
     * any folded match, which is what makes these refusals hold.
     */

    /* Traversal. ".." has no letters, so nothing can fold onto it -- but
     * that is a property to assert rather than to assume, and readdir()
     * hands "." and ".." out to the scan like any other entry. */
    refuse("..", "bare .. with folding in play");
    refuse("../OUTSIDE.TXT", "upper-case parent traversal");
    refuse("SUB/../../OUTSIDE.TXT", "mixed-case traversal through a subfolder");
    refuse("SUB/..", "upper-case traversal to the parent");
    refuse("SUB/.", "upper-case dot component");

    /* Absolute paths in every case. */
    refuse("/ETC/PASSWD", "upper-case absolute path");
    refuse("\\OUTSIDE.TXT", "upper-case leading backslash");
    refuse("C:\\WINDOWS\\WIN.INI", "upper-case drive letter");
    refuse("c:outside.txt", "lower-case drive-relative path");
    {
        char upper[512];
        size_t k;

        snprintf(upper, sizeof(upper), "%s/outside.txt", tmproot);
        for (k = 0; upper[k]; k++)
            if (upper[k] >= 'a' && upper[k] <= 'z')
                upper[k] = (char)(upper[k] - 'a' + 'A');
        refuse(upper, "the absolute host path of the target, upper-cased");
    }

    /*
     * Device names in every case. The fixtures above are real files
     * called "NUL", "CON", "aux.txt", "COM1", "LPT9" and "sub/NUL", so
     * without the refusal a folded lookup would now FIND them: these
     * checks would pass by accident if the files were absent.
     */
    refuse("nul", "device name nul");
    refuse("Con", "device name Con");
    refuse("AuX.txt", "device name AuX with an extension");
    refuse("aux.TXT", "device name aux, other case");
    refuse("cOm1", "device name cOm1");
    refuse("lpt9", "device name lpt9");
    refuse("SUB/nUl", "device name in a folded subfolder");
    refuse("Nul", "device name Nul");

    /* A symlink out of the folder whose name differs from the guest's
     * spelling only by case: the folded match finds the link, and the
     * containment check on the resolved path still refuses it. */
    {
        char link[512];

        host_path(link, sizeof(link), "EscapeMixed");
        if (symlink(tmproot, link) != 0)
            die("symlink EscapeMixed");
        refuse("escapemixed", "an escaping symlink found by folding");
        refuse("ESCAPEMIXED/outside.txt", "through an escaping folded symlink");
        refuse("escapemixed/OUTSIDE.TXT",
               "an upper-case leaf through an escaping folded symlink");
        refuse_write("EscapeMIXED/created.txt",
                     "writing through an escaping folded symlink");

        /* And the ones created earlier, asked for in the wrong case. */
        refuse("ESCAPE/outside.txt", "the escaping symlink, upper-cased");
        refuse("escape/OUTSIDE.TXT", "a folded leaf beyond an escaping symlink");
        refuse("ESCAPEFILE", "the escaping file symlink, upper-cased");
        refuse("EscapeRoot/PASSWD", "the absolute escaping symlink, mixed case");
        refuse("TOSIBLING", "the prefix-sharing sibling, upper-cased");
    }

    /* A symlink that stays inside must still resolve when its case is
     * wrong, or the refusals above are refusing everything rather than
     * refusing escapes. */
    check(rpc_path(HOSTFS_OP_STAT, 0, "INSIDE") == HOSTFS_E_OK,
          "a contained symlink was refused when asked for in upper case");

    /* Writes, through the traversals that matter most. */
    refuse_write("../created.txt", "create above the root");
    refuse_write("../../tmp/created.txt", "create two levels up");
    refuse_write("/tmp/created.txt", "create by absolute path");
    refuse_write("..\\created.txt", "create with a backslash traversal");

    /* RENAME resolves BOTH paths, so an escaping destination is an
     * escape even when the source is innocent. */
    {
        char both[128];
        unsigned n = 0;

        host_write("bait.txt", "bait");
        memcpy(both, "bait.txt", 8); n = 8;
        both[n++] = '\0';
        memcpy(both + n, "../stolen.txt", 13); n += 13;
        check(rpc(HOSTFS_OP_RENAME, 0, 0, 0, both, n) != HOSTFS_E_OK,
              "escape allowed: RENAME to a path above the root");

        n = 0;
        memcpy(both, "../outside.txt", 14); n = 14;
        both[n++] = '\0';
        memcpy(both + n, "stolen.txt", 10); n += 10;
        check(rpc(HOSTFS_OP_RENAME, 0, 0, 0, both, n) != HOSTFS_E_OK,
              "escape allowed: RENAME from a path above the root");
    }

    /* Nothing above the served folder may have been created, removed or
     * even touched by any of that. */
    snprintf(p, sizeof(p), "%s/outside.txt", tmproot);
    check(stat(p, &after) == 0, "the file outside the root was destroyed");
    check(after.st_size == before.st_size,
          "the file outside the root was modified");
    snprintf(p, sizeof(p), "%s/created.txt", tmproot);
    check(!host_exists(p), "a file was created outside the served folder");
    snprintf(p, sizeof(p), "%s/stolen.txt", tmproot);
    check(!host_exists(p), "a file was renamed out of the served folder");
    check(!host_exists("/tmp/created.txt"),
          "a file was created by absolute path outside the served folder");
}

/*
 * The served folder itself must still be reachable after all that: a
 * containment check that refuses everything is not a containment check.
 */
static void test_still_usable(void)
{
    check(rpc_path(HOSTFS_OP_STAT, 0, "hello.txt") == HOSTFS_E_OK,
          "an ordinary path stopped working");
    check(rpc_path(HOSTFS_OP_STAT, 0, "sub/inner.txt") == HOSTFS_E_OK,
          "a nested path stopped working");
    check(rpc_path(HOSTFS_OP_STAT, 0, "hello.txt") == HOSTFS_E_OK,
          "a repeated path stopped working");
}

/* ------------------------------------------------------------------ */
/* Malformed input                                                     */

static void put_hdr(unsigned char *b, unsigned len, unsigned op)
{
    memset(b, 0, HOSTFS_REQ_HDR);
    b[0] = (unsigned char)(len & 0xff);
    b[1] = (unsigned char)(len >> 8);
    b[2] = (unsigned char)(op & 0xff);
    b[3] = (unsigned char)(op >> 8);
}

/* Send a hand-built frame and read whatever comes back. Returns the
 * reply length, 0 for a closed connection. */
static unsigned raw(const unsigned char *buf, unsigned n)
{
    unsigned len;

    if (send_all(buf, n) < 0)
        return 0;
    if (recv_all(reply, HOSTFS_REP_HDR) < 0)
        return 0;
    len = (unsigned)reply[0] | ((unsigned)reply[1] << 8);
    if (len > HOSTFS_REP_HDR && len <= client_frame)
        recv_all(reply + HOSTFS_REP_HDR, len - HOSTFS_REP_HDR);
    return len;
}

static void test_malformed(void)
{
    unsigned char b[HOSTFS_MAX_FRAME + 64];
    unsigned i;

    /* An unknown opcode is answered and the channel stays in step --
     * this one must NOT drop the session, or a newer guest probing for
     * an operation would lose its drive. */
    check(rpc(4242, 0, 0, 0, NULL, 0) == HOSTFS_EDRIVE,
          "an unknown opcode was not refused");
    check(rpc(0, 0, 0, 0, NULL, 0) == HOSTFS_EDRIVE, "opcode 0 was accepted");
    check(rpc(HOSTFS_OP_MAX + 1, 0, 0, 0, NULL, 0) == HOSTFS_EDRIVE,
          "an opcode past the table was accepted");
    check(rpc(HOSTFS_OP_HELLO, HOSTFS_PROTO_VERSION, 0, 0, NULL, 0)
          == HOSTFS_E_OK, "the channel did not survive an unknown opcode");

    /* A length below the header cannot be honoured: there is no way to
     * know where the next frame starts, so the session is dropped. */
    put_hdr(b, 4, HOSTFS_OP_HELLO);
    check(raw(b, HOSTFS_REQ_HDR) == HOSTFS_REP_HDR,
          "an undersized length got no error reply");
    check(helper_alive(), "the helper died on an undersized length");

    /* A length past the buffer, which is the bug that was found twice in
     * the 9p code. */
    put_hdr(b, 0xffff, HOSTFS_OP_HELLO);
    check(raw(b, HOSTFS_REQ_HDR) == HOSTFS_REP_HDR,
          "an absurd length got no error reply");
    check(helper_alive(), "the helper died on an absurd length");

    put_hdr(b, HOSTFS_MAX_FRAME + 1, HOSTFS_OP_READ);
    check(raw(b, HOSTFS_REQ_HDR) == HOSTFS_REP_HDR,
          "a length one past the maximum got no error reply");
    check(helper_alive(), "the helper died on an over-long length");

    /* A frame that claims more than it sends, then vanishes. */
    put_hdr(b, HOSTFS_REQ_HDR + 100, HOSTFS_OP_STAT);
    send_all(b, HOSTFS_REQ_HDR + 10);
    t_disconnect();
    check(helper_alive(), "the helper died on a frame cut short");

    /* A truncated header. */
    send_all(b, 5);
    t_disconnect();
    check(helper_alive(), "the helper died on a truncated header");

    /* A connection that says nothing at all. */
    t_connect();
    t_disconnect();
    check(helper_alive(), "the helper died on an empty session");

    /* The largest legal frame, with every field at its maximum. */
    put_hdr(b, HOSTFS_MAX_FRAME, HOSTFS_OP_WRITE);
    for (i = 4; i < HOSTFS_REQ_HDR; i++)
        b[i] = 0xff;
    memset(b + HOSTFS_REQ_HDR, 'A', HOSTFS_MAX_FRAME - HOSTFS_REQ_HDR);
    check(raw(b, HOSTFS_MAX_FRAME) == HOSTFS_REP_HDR,
          "a maximum-sized frame got no reply");
    check(helper_alive(), "the helper died on a maximum-sized frame");

    /* Every opcode with a maximum-length garbage payload and every
     * field set to 0xffffffff. Nothing here should be servable; nothing
     * here may crash. */
    for (i = 0; i <= HOSTFS_OP_MAX + 2; i++)
    {
        unsigned len = HOSTFS_REQ_HDR + 300;
        unsigned j;

        put_hdr(b, len, i);
        for (j = 4; j < HOSTFS_REQ_HDR; j++)
            b[j] = 0xff;
        for (j = HOSTFS_REQ_HDR; j < len; j++)
            b[j] = (unsigned char)(j * 31 + i);   /* control bytes and all */
        if (raw(b, len) < HOSTFS_REP_HDR)
        {
            check(0, "op %u with a garbage payload got no reply", i);
            t_connect();
        }
    }
    check(helper_alive(), "the helper died fuzzing every opcode");

    /* Random frames of random legal lengths. */
    srand(20260804);
    for (i = 0; i < 400; i++)
    {
        unsigned len = HOSTFS_REQ_HDR + (unsigned)(rand() % 200);
        unsigned j;

        for (j = 0; j < len; j++)
            b[j] = (unsigned char)(rand() & 0xff);
        b[0] = (unsigned char)(len & 0xff);
        b[1] = (unsigned char)(len >> 8);
        if (raw(b, len) < HOSTFS_REP_HDR)
        {
            check(0, "a random frame got no reply");
            t_connect();
        }
    }
    check(helper_alive(), "the helper died on random frames");

    /* And after every one of those, it still serves. */
    check(rpc_path(HOSTFS_OP_STAT, 0, "hello.txt") == HOSTFS_E_OK,
          "the helper stopped serving after the malformed frames");
    {
        char p[512];

        snprintf(p, sizeof(p), "%s/outside.txt", tmproot);
        check(host_exists(p), "fuzzing destroyed the file outside the root");
    }
}

/* ------------------------------------------------------------------ */
/* Frame size                                                          */

/*
 * Larger frames are a per-session agreement made with CAPS. The contract
 * that matters most is the one for guests that never ask: their session
 * stays at 4 KB both ways, whatever the helper could do -- the kernel's
 * and EmuTOS's receive buffers are exactly HOSTFS_MAX_FRAME, and a longer
 * reply kills their drive.
 */
static void test_frames(void)
{
    enum { BIG = 200000 };
    static unsigned char want[BIG], got[BIG], b[HOSTFS_MAX_FRAME_LIMIT];
    unsigned long off;
    unsigned i;
    int fh, ok;

    for (i = 0; i < BIG; i++)
        want[i] = (unsigned char)(i * 13 + (i >> 9));

    check(rpc(HOSTFS_OP_HELLO, HOSTFS_PROTO_VERSION, 0, 0, NULL, 0)
          == HOSTFS_E_OK, "HELLO before the frame tests");

    /* A guest of today: CAPS with nothing in `a`. Header-only reply (the
     * EmuTOS transport refuses anything else), both bits, 4 KB limit. */
    check(rpc(HOSTFS_OP_CAPS, 0, 0, 0, NULL, 0) == HOSTFS_E_OK, "CAPS");
    check(reply_len == HOSTFS_REP_HDR, "CAPS reply carries %u bytes of data",
          reply_datalen());
    check((reply_a & HOSTFS_CAP_READDIR_BATCH)
          && (reply_a & HOSTFS_CAP_LARGE_FRAME), "CAPS bits %lx", reply_a);
    check(reply_b == HOSTFS_MAX_FRAME,
          "CAPS without an offer set the limit to %lu", reply_b);
    check(rpc(HOSTFS_OP_CAPS, HOSTFS_MAX_FRAME, 0, 0, NULL, 0) == HOSTFS_E_OK
          && reply_b == HOSTFS_MAX_FRAME, "an offer of 4 KB gave %lu", reply_b);
    check(rpc(HOSTFS_OP_CAPS, 100, 0, 0, NULL, 0) == HOSTFS_E_OK
          && reply_b == HOSTFS_MAX_FRAME,
          "an offer below 4 KB lowered the limit to %lu", reply_b);

    /* Write the file 4 KB at a time, as any guest can. */
    check(rpc_path(HOSTFS_OP_CREATE, 0, "frames.bin") == HOSTFS_E_OK,
          "CREATE frames.bin");
    fh = (int)reply_a;
    for (ok = 1, off = 0; off < BIG / 2; off += HOSTFS_MAX_REQ_DATA)
    {
        unsigned n = BIG / 2 - off > HOSTFS_MAX_REQ_DATA
                   ? HOSTFS_MAX_REQ_DATA : (unsigned)(BIG / 2 - off);
        if (rpc(HOSTFS_OP_WRITE, fh, off, 0, want + off, n) != HOSTFS_E_OK
         || reply_a != n)
            ok = 0;
    }
    check(ok, "4 KB writes failed");

    /* Still 4 KB: a READ asking for 64 KB is clamped (client_frame would
     * refuse a longer reply), and a request one byte over is a broken
     * stream, as it always was. */
    check(rpc(HOSTFS_OP_READ, fh, 0, 65535, NULL, 0) == HOSTFS_E_OK
          && reply_a == HOSTFS_MAX_REP_DATA,
          "an unraised session READ %lu bytes", reply_a);

    /* A raised session: the guest can take 64 KB. */
    check(rpc(HOSTFS_OP_CAPS, 0x7fffffffUL, 0, 0, NULL, 0) == HOSTFS_E_OK,
          "CAPS with a large offer");
    check(reply_len == HOSTFS_REP_HDR && reply_b == HOSTFS_MAX_FRAME_LIMIT,
          "a large offer gave a limit of %lu", reply_b);
    client_frame = HOSTFS_MAX_FRAME_LIMIT;

    /* The second half in frames of up to 64 KB. */
    for (ok = 1, off = BIG / 2; off < BIG; )
    {
        unsigned max = HOSTFS_MAX_FRAME_LIMIT - HOSTFS_REQ_HDR;
        unsigned n = BIG - off > max ? max : (unsigned)(BIG - off);
        if (rpc(HOSTFS_OP_WRITE, fh, off, 0, want + off, n) != HOSTFS_E_OK
         || reply_a != n)
        {
            ok = 0;
            break;
        }
        off += n;
    }
    check(ok, "64 KB writes failed");

    /* And back in 64 KB frames, exactly as much as asked for. */
    for (ok = 1, off = 0; off < BIG; )
    {
        if (rpc(HOSTFS_OP_READ, fh, off, 65535, NULL, 0) != HOSTFS_E_OK
         || reply_a == 0 || reply_datalen() != reply_a
         || reply_a > HOSTFS_MAX_FRAME_LIMIT - HOSTFS_REP_HDR)
        {
            ok = 0;
            break;
        }
        if (off + reply_a < BIG)
            check(reply_a == HOSTFS_MAX_FRAME_LIMIT - HOSTFS_REP_HDR,
                  "a mid-file READ of 65535 returned %lu", reply_a);
        memcpy(got + off, reply_data(), reply_a);
        off += reply_a;
    }
    check(ok && off == BIG && !memcmp(want, got, BIG),
          "a file did not round-trip through 64 KB frames");
    check(rpc(HOSTFS_OP_READ, fh, 0, 100, NULL, 0) == HOSTFS_E_OK
          && reply_a == 100, "a small READ in a raised session gave %lu",
          reply_a);

    /* READDIR_BATCH does not grow: guests size its buffer from 4 KB. Only
     * long names can reach that bound -- sixteen entries of 6 + 252 bytes
     * are 4128 -- so list a folder of them, as a guest may create. */
    {
        char name[300], pad[245];
        int dh, batches = 0, entries = 0;

        memset(pad, 'x', 244);
        pad[244] = '\0';
        host_path(name, sizeof(name), "longnames");
        if (mkdir(name, 0777) != 0)
            die("mkdir longnames");
        for (i = 0; i < 17; i++)
        {
            snprintf(name, sizeof(name), "longnames/n%02u-%s.txt", i, pad);
            host_write(name, "long");
        }
        check(rpc_path(HOSTFS_OP_OPENDIR, 0, "longnames") == HOSTFS_E_OK,
              "OPENDIR longnames");
        dh = (int)reply_a;
        for (off = 0; ; )
        {
            int st = rpc(HOSTFS_OP_READDIR_BATCH, dh, off, 0, NULL, 0);
            if (st != HOSTFS_E_OK)
                break;
            check(reply_datalen() <= HOSTFS_MAX_REP_DATA,
                  "a batch of %u bytes in a raised session", reply_datalen());
            batches++;
            entries += (int)reply_b;
            off = reply_a;
        }
        check(entries == 17 && batches >= 2,
              "long names listed as %d entries in %d batches", entries, batches);
        check(rpc(HOSTFS_OP_CLOSEDIR, dh, 0, 0, NULL, 0) == HOSTFS_E_OK,
              "CLOSEDIR longnames");
    }

    /* A middle-sized offer is honoured as made: 16 KB both ways. */
    check(rpc(HOSTFS_OP_CAPS, 16384, 0, 0, NULL, 0) == HOSTFS_E_OK
          && reply_b == 16384, "an offer of 16 KB gave %lu", reply_b);
    check(rpc(HOSTFS_OP_READ, fh, 0, 65535, NULL, 0) == HOSTFS_E_OK
          && reply_a == 16384 - HOSTFS_REP_HDR,
          "a 16 KB session READ %lu bytes", reply_a);
    put_hdr(b, 16385, HOSTFS_OP_WRITE);
    check(raw(b, HOSTFS_REQ_HDR) == HOSTFS_REP_HDR
          && (short)(reply[2] | (reply[3] << 8)) == HOSTFS_EDRIVE,
          "a frame over the agreed 16 KB was not refused");
    check(helper_alive(), "the helper died on a frame over the agreed limit");

    /* A new connection starts at 4 KB before any HELLO: raise the limit,
     * hang up, reconnect and send a 4097-byte frame first thing. A helper
     * that reset only on HELLO would take the header and wait for the
     * body. */
    check(rpc(HOSTFS_OP_CAPS, 65535, 0, 0, NULL, 0) == HOSTFS_E_OK
          && reply_b == 65535, "raise before reconnecting");
    t_connect();
    put_hdr(b, HOSTFS_MAX_FRAME + 1, HOSTFS_OP_READ);
    check(raw(b, HOSTFS_REQ_HDR) == HOSTFS_REP_HDR
          && (short)(reply[2] | (reply[3] << 8)) == HOSTFS_EDRIVE,
          "a new connection inherited the last session's frame limit");
    check(helper_alive(), "the helper died after a reconnect");
    client_frame = HOSTFS_MAX_FRAME;

    /* helper_alive() reconnected and said HELLO: a new session, back at
     * 4 KB, with no CAPS. The file must be reopened, and READ is clamped
     * again. */
    check(rpc_path(HOSTFS_OP_OPEN, HOSTFS_O_RDONLY, "frames.bin") == HOSTFS_E_OK,
          "reopen frames.bin");
    fh = (int)reply_a;
    check(rpc(HOSTFS_OP_READ, fh, 0, 65535, NULL, 0) == HOSTFS_E_OK
          && reply_a == HOSTFS_MAX_REP_DATA,
          "a new session READ %lu bytes", reply_a);
    check(rpc(HOSTFS_OP_CLOSE, fh, 0, 0, NULL, 0) == HOSTFS_E_OK, "CLOSE");

    /* HELLO on the same connection resets a raised limit as well: a guest
     * that rebooted may be an older one. */
    check(rpc(HOSTFS_OP_CAPS, 65535, 0, 0, NULL, 0) == HOSTFS_E_OK
          && reply_b == 65535, "raise again");
    check(rpc(HOSTFS_OP_HELLO, HOSTFS_PROTO_VERSION, 0, 0, NULL, 0)
          == HOSTFS_E_OK, "HELLO in a raised session");
    put_hdr(b, HOSTFS_MAX_FRAME + 1, HOSTFS_OP_READ);
    check(raw(b, HOSTFS_REQ_HDR) == HOSTFS_REP_HDR
          && (short)(reply[2] | (reply[3] << 8)) == HOSTFS_EDRIVE,
          "HELLO did not put the limit back to 4 KB");
    check(helper_alive(), "the helper died after the frame tests");

    /* So does a HELLO the helper refuses: the session goes on, at 4 KB. */
    check(rpc(HOSTFS_OP_CAPS, 65535, 0, 0, NULL, 0) == HOSTFS_E_OK
          && reply_b == 65535, "raise before a refused HELLO");
    check(rpc(HOSTFS_OP_HELLO, 999, 0, 0, NULL, 0) == HOSTFS_EDRIVE,
          "HELLO of version 999 was not refused");
    check(rpc(HOSTFS_OP_READ, 99, 0, 1, NULL, 0) == HOSTFS_EIHNDL,
          "the session did not go on after a refused HELLO");
    put_hdr(b, HOSTFS_MAX_FRAME + 1, HOSTFS_OP_READ);
    check(raw(b, HOSTFS_REQ_HDR) == HOSTFS_REP_HDR
          && (short)(reply[2] | (reply[3] << 8)) == HOSTFS_EDRIVE,
          "a refused HELLO did not put the limit back to 4 KB");
    check(helper_alive(), "the helper died after the frame tests");
}

/* ------------------------------------------------------------------ */
/* Durability option                                                   */

/*
 * A second helper of our own on socket NAME, serving the same folder with
 * EXTRA (or nothing), --verbose and --once, its log in LOG and PRELOAD (or
 * nothing) in its LD_PRELOAD; the shim's SLOWSYNC_* settings come from
 * our own environment. Returns its pid with `sock` connected to it, or
 * -1 -- and then the child is gone, not left running.
 */
static pid_t helper_extra(const char *name, const char *extra, const char *log,
                          const char *preload)
{
    char path[400], ready[400];
    pid_t child;
    int tries;

    snprintf(path, sizeof(path), "%s/%s", tmproot, name);
    snprintf(ready, sizeof(ready), "%s/%s.ready", tmproot, name);
    child = fork();
    if (child < 0)
        die("fork");
    if (child == 0)
    {
        int fd = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd >= 0)
            dup2(fd, 2);
        if (preload)
            setenv("LD_PRELOAD", preload, 1);
        execl("../../tools/hostfsd/shinogi-hostfsd", "shinogi-hostfsd",
              "--root", servedir, "--listen", path, "--ready-file", ready,
              "--once", "--verbose", extra, (char *)NULL);
        _exit(127);
    }
    for (tries = 0; tries < 400 && !host_exists(ready); tries++)
        usleep(5000);
    if (host_exists(ready))
    {
        struct sockaddr_un sa;

        memset(&sa, 0, sizeof(sa));
        sa.sun_family = AF_UNIX;
        strcpy(sa.sun_path, path);
        sock = socket(AF_UNIX, SOCK_STREAM, 0);
        if (sock >= 0 && connect(sock, (struct sockaddr *)&sa, sizeof(sa)) == 0)
        {
            t_deadline(sock);
            return child;
        }
        if (sock >= 0)
            close(sock);
        sock = -1;
    }
    kill(child, SIGKILL);
    waitpid(child, NULL, 0);
    return -1;
}

/* Does the file LOG contain WHAT? A --verbose log runs to megabytes. */
static int log_has(const char *log, const char *what)
{
    FILE *f = fopen(log, "rb");
    char *buf = NULL;
    long n = 0;
    int found = 0;

    if (f && fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) >= 0
     && fseek(f, 0, SEEK_SET) == 0 && (buf = (char *)malloc((size_t)n + 1)))
    {
        n = (long)fread(buf, 1, (size_t)n, f);
        buf[n] = '\0';
        found = strstr(buf, what) != NULL;
    }
    if (f)
        fclose(f);
    free(buf);
    return found;
}

/*
 * --fsync-on-close puts the flush back on every close of a written file,
 * and only then. The helper's --verbose log says when it flushes, so a
 * run with the option must log it and a run without must not.
 */
static void test_fsync_option(void)
{
    static const char *const modes[2] = { "--fsync-on-close", NULL };
    int saved = sock, m;

    for (m = 0; m < 2; m++)
    {
        char log[400], path[400], back[16];
        pid_t child;
        int fh;
        FILE *f;

        snprintf(log, sizeof(log), "%s/fsync-%d.log", tmproot, m);
        child = helper_extra(m ? "sock4" : "sock3", modes[m], log, NULL);
        check(child > 0, "a helper %s did not start",
              modes[m] ? modes[m] : "without --fsync-on-close");
        if (child <= 0)
            continue;
        check(rpc(HOSTFS_OP_HELLO, HOSTFS_PROTO_VERSION, 0, 0, NULL, 0)
              == HOSTFS_E_OK, "HELLO (fsync option %d)", m);
        check(rpc_path(HOSTFS_OP_CREATE, 0, "synced.txt") == HOSTFS_E_OK,
              "CREATE (fsync option %d)", m);
        fh = (int)reply_a;
        check(rpc(HOSTFS_OP_WRITE, fh, 0, 0, "flushed", 7) == HOSTFS_E_OK
              && reply_a == 7, "WRITE (fsync option %d)", m);
        check(rpc(HOSTFS_OP_CLOSE, fh, 0, 0, NULL, 0) == HOSTFS_E_OK,
              "CLOSE (fsync option %d)", m);
        /* The CLOSE reply follows the log line, so it is there now. */
        check(log_has(log, "fsync on close") == !m,
              m ? "a close was flushed without --fsync-on-close"
                : "--fsync-on-close did not flush the close");
        host_path(path, sizeof(path), "synced.txt");
        f = fopen(path, "rb");
        check(f && fread(back, 1, sizeof(back), f) == 7
              && !memcmp(back, "flushed", 7),
              "a file closed with fsync option %d is not intact", m);
        if (f)
            fclose(f);
        t_disconnect();
        check(waitpid(child, NULL, 0) == child,
              "the helper with fsync option %d did not exit with --once", m);
    }
    sock = saved;
}

/* ------------------------------------------------------------------ */
/* SYNC                                                                */

static double now_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/* Replies may now wait for a disk: give them longer than t_deadline(). */
static void t_patient(void)
{
    struct timeval tv;

    tv.tv_sec = 60;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

/* CREATE NAME holding TEXT; CLOSE it unless KEEP. Returns the handle. */
static int put_file(const char *name, const char *text, int keep)
{
    int fh;

    if (rpc_path(HOSTFS_OP_CREATE, 0, name) != HOSTFS_E_OK)
    {
        check(0, "CREATE %s", name);
        return -1;
    }
    fh = (int)reply_a;
    check(rpc(HOSTFS_OP_WRITE, fh, 0, 0, text, (unsigned)strlen(text))
          == HOSTFS_E_OK && reply_a == strlen(text), "WRITE %s", name);
    if (!keep)
        check(rpc(HOSTFS_OP_CLOSE, fh, 0, 0, NULL, 0) == HOSTFS_E_OK,
              "CLOSE %s", name);
    return fh;
}

static int rename_path(const char *from, const char *to)
{
    char both[600];
    size_t nf = strlen(from), nt = strlen(to);

    memcpy(both, from, nf);
    both[nf] = '\0';
    memcpy(both + nf + 1, to, nt);
    return rpc(HOSTFS_OP_RENAME, 0, 0, 0, both, (unsigned)(nf + 1 + nt));
}

/* N closed files in folder DIR (made here); FH, if not negative, is a
 * file already open and written that is closed last. */
static void many_files(const char *dir, int n)
{
    char name[64];
    int i, ok = 1;

    check(rpc_path(HOSTFS_OP_MKDIR, 0, dir) == HOSTFS_E_OK, "MKDIR %s", dir);
    for (i = 0; i < n && ok; i++)
    {
        snprintf(name, sizeof(name), "%s/f%05d.txt", dir, i);
        if (rpc_path(HOSTFS_OP_CREATE, 0, name) != HOSTFS_E_OK)
            ok = 0;
        else
        {
            int fh = (int)reply_a;
            ok = rpc(HOSTFS_OP_WRITE, fh, 0, 0, name, (unsigned)strlen(name))
                 == HOSTFS_E_OK
              && rpc(HOSTFS_OP_CLOSE, fh, 0, 0, NULL, 0) == HOSTFS_E_OK;
        }
    }
    check(ok, "writing %d files into %s failed at %d", n, dir, i);
}

/* How many lines in the first UPTO bytes of the file LOG (all of it if
 * UPTO is negative) contain WHAT; 0 if there is no such file. */
static int log_count_upto(const char *log, const char *what, long upto)
{
    FILE *f = fopen(log, "rb");
    char line[8192];
    int n = 0;

    if (!f)
        return 0;
    while ((upto < 0 || ftell(f) < upto) && fgets(line, sizeof(line), f))
        if (strstr(line, what))
            n++;
    fclose(f);
    return n;
}

static int log_count(const char *log, const char *what)
{
    return log_count_upto(log, what, -1);
}

/* Empty the file LOG, making it if need be: 1, or 0 if that failed. */
static int log_clear(const char *log)
{
    FILE *f = fopen(log, "w");

    if (!f)
        return 0;
    fclose(f);
    return 1;
}

/* The size of the file LOG now: where it ends at a given moment. */
static long log_size(const char *log)
{
    struct stat st;

    return stat(log, &st) == 0 ? (long)st.st_size : 0;
}

/* Wait up to MS for LOG to contain WHAT, sending nothing meanwhile. */
static int log_waits_for(const char *log, const char *what, int ms)
{
    double end = now_ms() + ms;

    while (!log_has(log, what))
    {
        if (now_ms() > end)
            return 0;
        usleep(20000);
    }
    return 1;
}

/* The helper's resident memory, in kB. */
static long rss_kb(pid_t pid)
{
    char path[64], line[256];
    long kb = -1;
    FILE *f;

    snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);
    f = fopen(path, "r");
    if (!f)
        return -1;
    while (fgets(line, sizeof(line), f))
        if (!strncmp(line, "VmRSS:", 6))
            kb = atol(line + 6);
    fclose(f);
    return kb;
}

static void shim_env(const char *ms, const char *fail, const char *log)
{
    if (ms) setenv("SLOWSYNC_MS", ms, 1); else unsetenv("SLOWSYNC_MS");
    if (fail) setenv("SLOWSYNC_FAIL", fail, 1); else unsetenv("SLOWSYNC_FAIL");
    if (log) setenv("SLOWSYNC_LOG", log, 1); else unsetenv("SLOWSYNC_LOG");
    unsetenv("SLOWSYNC_FULL");
    unsetenv("SLOWSYNC_QUOTA");
    unsetenv("SLOWSYNC_ONCE");
}

/* Was the helper built with a sanitizer? Its allocator then takes
 * malloc() from under nomem.so. */
static int helper_sanitized(void)
{
    FILE *f = fopen("../../tools/hostfsd/shinogi-hostfsd", "rb");
    static char buf[1 << 16];
    size_t n, keep = 0;
    int found = 0;

    if (!f)
        return 0;
    while (!found && (n = fread(buf + keep, 1, sizeof(buf) - 1 - keep, f)) > 0)
    {
        size_t i, len = keep + n;

        for (i = 0; i + 11 <= len && !found; i++)
            found = !memcmp(buf + i, "__asan_init", 11)
                 || !memcmp(buf + i, "__tsan_init", 11);
        keep = len < 10 ? len : 10;     /* a name split across reads */
        memmove(buf, buf + len - keep, keep);
    }
    fclose(f);
    return found;
}

/* Wait for a helper of ours to exit; kill it if it will not. */
static int helper_done(pid_t child, const char *what)
{
    int i;

    t_disconnect();
    for (i = 0; i < 3000; i++)
    {
        if (waitpid(child, NULL, WNOHANG) == child)
            return 1;
        usleep(10000);
    }
    kill(child, SIGKILL);
    waitpid(child, NULL, 0);
    check(0, "%s did not exit, and was killed", what);
    return 0;
}

/*
 * The helper's --verbose log names each flush, and the slowsync.so shim
 * (Linux) logs each fsync() and syncfs() the C library really made.
 */
static void test_sync(void)
{
    int saved = sock, fh;
    char log[400], shimlog[400], p[512], q[512];
    pid_t child;
    char shim[PATH_MAX];
    int have_shim = realpath("slowsync.so", shim) != NULL;

    /* Offered, and refused when malformed -- with the channel in step. */
    check(rpc(HOSTFS_OP_CAPS, 0, 0, 0, NULL, 0) == HOSTFS_E_OK
          && (reply_a & HOSTFS_CAP_SYNC), "CAPS does not offer SYNC");
    check(rpc(HOSTFS_OP_SYNC, 2, 0, 0, NULL, 0) == HOSTFS_EDRIVE,
          "SYNC with a = 2 was accepted");
    check(rpc(HOSTFS_OP_SYNC, 0xffffffffUL, 0, 0, NULL, 0) == HOSTFS_EDRIVE,
          "SYNC with a = 0xffffffff was accepted");
    check(rpc(HOSTFS_OP_SYNC, 0, 1, 0, NULL, 0) == HOSTFS_EDRIVE,
          "SYNC with b = 1 was accepted");
    check(rpc(HOSTFS_OP_SYNC, 1, 0, 1, NULL, 0) == HOSTFS_EDRIVE,
          "SYNC with c = 1 was accepted");
    check(rpc(HOSTFS_OP_SYNC, 1, 0, 0, "x", 1) == HOSTFS_EDRIVE,
          "SYNC with data was accepted");
    check(rpc(HOSTFS_OP_SYNC, 1, 0, 0, NULL, 0) == HOSTFS_E_OK,
          "SYNC with a = 1 and nothing to flush");
    check(rpc(HOSTFS_OP_SYNC, 0, 0, 0, NULL, 0) == HOSTFS_E_OK,
          "SYNC with a = 0 and nothing to flush");
    check(rpc_path(HOSTFS_OP_STAT, 0, "hello.txt") == HOSTFS_E_OK,
          "the channel is out of step after refused SYNCs");
    shim_env(NULL, NULL, NULL);

    /* ---- With --sync-fs (syncfs() on Linux, file by file elsewhere):
     * a = 1 after writes, the set filling up, and the end of the
     * session. */
    snprintf(log, sizeof(log), "%s/sync-fs.log", tmproot);
    child = helper_extra("sock5", "--sync-fs", log, NULL);
    check(child > 0, "a helper for the SYNC tests did not start");
    if (child > 0)
    {
        t_patient();
        check(rpc(HOSTFS_OP_HELLO, HOSTFS_PROTO_VERSION, 0, 0, NULL, 0)
              == HOSTFS_E_OK, "HELLO (sync)");
        put_file("s1.txt", "one", 0);
        put_file("s2.txt", "two", 0);
        fh = put_file("s3.txt", "three", 1);
        check(rpc(HOSTFS_OP_SYNC, 1, 0, 0, NULL, 0) == HOSTFS_E_OK,
              "SYNC a = 1 after writes");
#ifdef __linux__
        check(log_has(log, "sync: 2 listed and 1 open file(s) by syncfs, 0 failed"),
              "SYNC a = 1 did not flush two closed files and an open one");
#endif
        /* A file written before the set filled and closed after it is
         * flushed at its close, not lost track of (16384 listed). */
        check(rpc(HOSTFS_OP_WRITE, fh, 5, 0, "more", 4) == HOSTFS_E_OK,
              "WRITE s3.txt again");
        many_files("over1", 16384);
        check(rpc(HOSTFS_OP_CLOSE, fh, 0, 0, NULL, 0) == HOSTFS_E_OK,
              "CLOSE s3.txt");
        check(log_has(log, "hostfsd: sync: s3.txt, the list is full, flushed at close"),
              "the close that found the set full did not flush its file");
        check(rpc(HOSTFS_OP_SYNC, 1, 0, 0, NULL, 0) == HOSTFS_E_OK,
              "SYNC a = 1 with a full set");
        put_file("s4.txt", "four", 0);
        fh = put_file("s5.txt", "five", 1);
        if (helper_done(child, "the SYNC helper"))
#ifdef __linux__
            check(log_has(log, "sync: 1 listed and 1 open file(s) by syncfs, 0 failed"),
                  "the end of the session did not flush what was left");
#else
            ;
#endif
    }

    /* ---- The default: file by file. The shim (no delay) logs what is
     * really flushed. */
    snprintf(log, sizeof(log), "%s/sync-per-file.log", tmproot);
    snprintf(shimlog, sizeof(shimlog), "%s/sync-per-file.fsync", tmproot);
    shim_env("0", NULL, shimlog);
    child = helper_extra("sock6", NULL, log, have_shim ? shim : NULL);
    check(child > 0, "a helper for the file-by-file tests did not start");
    if (child > 0)
    {
        t_patient();
        check(rpc(HOSTFS_OP_HELLO, HOSTFS_PROTO_VERSION, 0, 0, NULL, 0)
              == HOSTFS_E_OK, "HELLO (per file)");

        /* A rename, a folder rename and a delete before the SYNC. */
        put_file("ren-a.txt", "renamed", 0);
        check(rename_path("ren-a.txt", "ren-b.txt") == HOSTFS_E_OK, "RENAME file");
        check(rpc_path(HOSTFS_OP_MKDIR, 0, "rdir") == HOSTFS_E_OK, "MKDIR rdir");
        put_file("rdir/in.txt", "inside", 0);
        check(rename_path("rdir", "rdir2") == HOSTFS_E_OK, "RENAME folder");
        put_file("gone.txt", "deleted", 0);
        check(rpc_path(HOSTFS_OP_DELETE, 0, "gone.txt") == HOSTFS_E_OK, "DELETE");
        check(rpc(HOSTFS_OP_SYNC, 1, 0, 0, NULL, 0) == HOSTFS_E_OK,
              "SYNC a = 1 after a rename and a delete");
        check(log_has(log, "hostfsd: sync: ren-b.txt\n")
              && !log_has(log, "hostfsd: sync: ren-a.txt"),
              "the flush did not follow a renamed file");
        check(log_has(log, "hostfsd: sync: rdir2/in.txt\n")
              && !log_has(log, "hostfsd: sync: rdir/in.txt"),
              "the flush did not follow a file in a renamed folder");
        check(!log_has(log, "sync: gone.txt"),
              "a deleted file was still in the set");
        check(log_has(log, "sync: 2 listed and 0 open file(s) file by file, 0 failed"),
              "the rename batch was not flushed cleanly");

        /* Read-only: written then made read-only; created read-only; made
         * read-only while open and written (as cp -p does with fchmod),
         * then written again. Windows could not reopen any of these for
         * writing, so there each is flushed while it still can be -- the
         * Wine smoke run checks that. POSIX reopens a read-only file for
         * a flush without trouble, so here none of them is flushed in
         * place: SETATTR and CLOSE make no fsync, and the next a = 1
         * flushes all three. */
        put_file("ro-after.txt", "then read-only", 0);
        check(rpc_path(HOSTFS_OP_SETATTR, HOSTFS_FA_RO, "ro-after.txt")
              == HOSTFS_E_OK, "SETATTR read-only");
        check(rpc_path(HOSTFS_OP_CREATE, HOSTFS_FA_RO, "ro-created.txt")
              == HOSTFS_E_OK, "CREATE read-only");
        fh = (int)reply_a;
        check(rpc(HOSTFS_OP_WRITE, fh, 0, 0, "born read-only", 14) == HOSTFS_E_OK
              && rpc(HOSTFS_OP_CLOSE, fh, 0, 0, NULL, 0) == HOSTFS_E_OK,
              "WRITE and CLOSE a file created read-only");
        fh = put_file("ro-open.txt", "open, then read-only", 1);
        check(rpc_path(HOSTFS_OP_SETATTR, HOSTFS_FA_RO, "ro-open.txt")
              == HOSTFS_E_OK, "SETATTR read-only on an open file");
        check(rpc(HOSTFS_OP_WRITE, fh, 20, 0, " and more", 9) == HOSTFS_E_OK
              && rpc(HOSTFS_OP_CLOSE, fh, 0, 0, NULL, 0) == HOSTFS_E_OK,
              "WRITE and CLOSE the open read-only file");
        check(!log_has(log, "before it is made read-only")
              && !log_has(log, "read-only, flushed at close"),
              "a read-only file was flushed in place on POSIX");
        if (have_shim)
            check(!log_has(shimlog, "/ro-"),
                  "SETATTR or CLOSE of a read-only file made an fsync on POSIX");
        check(rpc(HOSTFS_OP_SYNC, 1, 0, 0, NULL, 0) == HOSTFS_E_OK,
              "SYNC a = 1 after the read-only files");
        check(log_has(log, "hostfsd: sync: ro-after.txt\n")
              && log_has(log, "hostfsd: sync: ro-created.txt\n")
              && log_has(log, "hostfsd: sync: ro-open.txt\n"),
              "SYNC a = 1 did not flush the read-only files");
        if (have_shim)
            check(log_has(shimlog, "/ro-after.txt\n")
                  && log_has(shimlog, "/ro-created.txt\n")
                  && log_has(shimlog, "/ro-open.txt\n"),
                  "SYNC a = 1 made no fsync of the read-only files");

        /* A failure is reported by every a = 1 until it is resolved, and
         * retried by posted rounds -- but logged once, not every time. */
        if (getuid() != 0)      /* root opens anything */
        {
            int i;

            put_file("locked.txt", "cannot reopen", 0);
            host_path(p, sizeof(p), "locked.txt");
            if (chmod(p, 0) != 0)
                die("chmod locked.txt");
            check(rpc(HOSTFS_OP_SYNC, 1, 0, 0, NULL, 0) == HOSTFS_EIO
                  && reply_len == 12,
                  "a file that could not be flushed did not give EIO, "
                  "header only");
            for (i = 0; i < 3; i++)
            {
                check(rpc(HOSTFS_OP_SYNC, 0, 0, 0, NULL, 0) == HOSTFS_E_OK,
                      "SYNC a = 0 with a failing file listed");
                usleep(200000);         /* idle: the posted round retries */
            }
            check(rpc(HOSTFS_OP_SYNC, 1, 0, 0, NULL, 0) == HOSTFS_EIO,
                  "a failed file was reported once and then forgotten");
            check(log_count(log, "hostfsd: could not flush locked.txt") == 1,
                  "a file that kept failing was logged %d times, not once",
                  log_count(log, "hostfsd: could not flush locked.txt"));
            check(log_count(log, "hostfsd: sync: locked.txt still cannot be "
                                 "flushed") == 4,
                  "%d retries of the failing file were noted, not 3 posted "
                  "and 1 waited", log_count(log, "hostfsd: sync: locked.txt "
                                            "still cannot be flushed"));
            chmod(p, 0644);
            check(rpc(HOSTFS_OP_SYNC, 1, 0, 0, NULL, 0) == HOSTFS_E_OK,
                  "a file that flushes on retry still failed");
            check(log_has(log, "hostfsd: sync: locked.txt\n"),
                  "the retried file was not flushed");
        }

        /* Renamed onto a listed name: the file it replaced is gone, and
         * one entry is left, flushed once. */
        put_file("dup-a.txt", "moves", 0);
        put_file("dup-b.txt", "is replaced", 0);
        check(rename_path("dup-a.txt", "dup-b.txt") == HOSTFS_E_OK,
              "RENAME onto a listed name");
        if (have_shim && !log_clear(shimlog))
            die("empty the shim log");
        {
            static const char one[] = "hostfsd: sync: 1 listed and 0 open "
                                      "file(s) file by file, 0 failed\n";
            static const char two[] = "hostfsd: sync: 2 listed and 0 open "
                                      "file(s) file by file";
            int ones = log_count(log, one), twos = log_count(log, two);

            check(rpc(HOSTFS_OP_SYNC, 1, 0, 0, NULL, 0) == HOSTFS_E_OK,
                  "SYNC a = 1 after a rename onto a listed name");
            check(log_count(log, one) == ones + 1 && log_count(log, two) == twos,
                  "a rename onto a listed name left two entries");
        }
        if (have_shim)
            check(log_count(shimlog, "/dup-b.txt") == 1
                  && !log_has(shimlog, "/dup-a.txt"),
                  "the file renamed onto a listed name was flushed %d times",
                  log_count(shimlog, "/dup-b.txt"));

        /* A file written before a thousand others and closed after them
         * is flushed by the next a = 1. */
        fh = put_file("early.txt", "written first", 1);
        sleep(3);
        many_files("over2", 1100);
        check(rpc(HOSTFS_OP_CLOSE, fh, 0, 0, NULL, 0) == HOSTFS_E_OK,
              "CLOSE early.txt");
        check(rpc(HOSTFS_OP_SYNC, 1, 0, 0, NULL, 0) == HOSTFS_E_OK,
              "SYNC a = 1 after 1100 closes");
        check(log_has(log, "hostfsd: sync: early.txt\n"),
              "a file written before 1100 others and closed after was not flushed");

        /* A HELLO on a live connection closes the open files as CLOSE
         * does: listed, not flushed; the next a = 1 flushes them. */
        fh = put_file("hello-open.txt", "open at HELLO", 1);
        check(rpc(HOSTFS_OP_HELLO, HOSTFS_PROTO_VERSION, 0, 0, NULL, 0)
              == HOSTFS_E_OK, "HELLO with a written file open");
        check(log_has(log, "hostfsd: hello: 1 written file(s) still open, "
                           "closed as CLOSE does")
              && !log_has(log, "hostfsd: sync: hello-open.txt"),
              "HELLO did not close the open file as CLOSE does");
        check(rpc(HOSTFS_OP_SYNC, 1, 0, 0, NULL, 0) == HOSTFS_E_OK,
              "SYNC a = 1 after HELLO");
        check(log_has(log, "hostfsd: sync: hello-open.txt\n"),
              "a file left open at HELLO was not flushed by the next a = 1");

        /* A listed name that is now a folder, a pipe or a link: dropped,
         * without an error and without opening it. */
        put_file("nowdir.txt", "replaced by a folder", 0);
        host_path(p, sizeof(p), "nowdir.txt");
        if (unlink(p) != 0 || mkdir(p, 0777) != 0)
            die("nowdir.txt");
        /* The host replaces listed files with a pipe, a link inside the
         * folder and a link out of it. (A RENAME by the guest onto a
         * listed name drops that entry: the file it named is gone.) */
        put_file("x.txt", "replaced by a pipe", 0);
        host_path(p, sizeof(p), "x.txt");
        if (unlink(p) != 0 || mkfifo(p, 0666) != 0)
            die("mkfifo x.txt");
        put_file("y.txt", "replaced by a link", 0);
        host_path(p, sizeof(p), "y.txt");
        host_path(q, sizeof(q), "hello.txt");
        if (unlink(p) != 0 || symlink(q, p) != 0)
            die("symlink y.txt");
        put_file("z.txt", "replaced by a link out", 0);
        host_path(p, sizeof(p), "z.txt");
        snprintf(q, sizeof(q), "%s/outside.txt", tmproot);
        if (unlink(p) != 0 || symlink(q, p) != 0)
            die("symlink z.txt");
        {
            struct timeval tv = { 10, 0 };      /* a hang must fail, not wait */
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        }
        if (rpc(HOSTFS_OP_SYNC, 1, 0, 0, NULL, 0) != HOSTFS_E_OK)
        {
            /* Blocked opening the pipe, most likely: nothing more can be
             * asked of this helper. */
            check(0, "SYNC a = 1 over names that are no longer files");
            kill(child, SIGKILL);
            waitpid(child, NULL, 0);
            child = -1;
            sock = -1;
            goto per_file_done;
        }
        check(log_has(log, "hostfsd: sync: nowdir.txt is no longer a file")
              && log_has(log, "hostfsd: sync: x.txt is no longer a file")
              && log_has(log, "hostfsd: sync: y.txt is no longer a file")
              && log_has(log, "hostfsd: sync: z.txt is no longer a file"),
              "a name that is no longer a file was not dropped");
        t_patient();

        fh = put_file("open-at-end.txt", "still open", 1);
        if (helper_done(child, "the file-by-file helper"))
            check(log_has(log, "sync: 0 listed and 1 open file(s) file by file, "
                               "0 failed"),
                  "the end of the session left an open file");
    }
per_file_done:

#ifdef __linux__
    if (!have_shim)
        check(0, "slowsync.so is missing (make builds it)");
    else
    {
        /* ---- Posted, with every flush a second long: a = 0 answers at
         * once; nothing is flushed while requests keep coming; once the
         * drive is idle the work drains, and a request that arrives then
         * waits at most the one flush in progress. */
        snprintf(log, sizeof(log), "%s/sync-posted.log", tmproot);
        snprintf(shimlog, sizeof(shimlog), "%s/sync-posted.fsync", tmproot);
        shim_env("1000", NULL, shimlog);
        child = helper_extra("sock7", NULL, log, shim);
        check(child > 0, "a slow-flush helper did not start");
        if (child > 0)
        {
            double t0, t1;

            t_patient();
            check(rpc(HOSTFS_OP_HELLO, HOSTFS_PROTO_VERSION, 0, 0, NULL, 0)
                  == HOSTFS_E_OK, "HELLO (posted)");
            put_file("p1.txt", "one", 0);
            put_file("p2.txt", "two", 0);
            put_file("p3.txt", "three", 0);
            t0 = now_ms();
            check(rpc(HOSTFS_OP_SYNC, 0, 0, 0, NULL, 0) == HOSTFS_E_OK, "SYNC a = 0");
            t1 = now_ms();
            check(t1 - t0 < 1000, "SYNC a = 0 took %.0f ms to answer", t1 - t0);
            check(rpc_path(HOSTFS_OP_CREATE, 0, "behind.txt") == HOSTFS_E_OK,
                  "CREATE behind a posted SYNC");
            check(now_ms() - t1 < 1000, "a CREATE right behind a posted SYNC "
                  "waited %.0f ms: a flush started before the drive was idle",
                  now_ms() - t1);
            fh = (int)reply_a;
            check(rpc(HOSTFS_OP_CLOSE, fh, 0, 0, NULL, 0) == HOSTFS_E_OK, "CLOSE behind.txt");
            check(log_count(shimlog, "fsync ") == 0,
                  "%d files were flushed while requests kept coming",
                  log_count(shimlog, "fsync "));
            /* Idle: the work drains; a request now waits for the one
             * flush in progress at most. */
            usleep(200000);
            t1 = now_ms();
            check(rpc_path(HOSTFS_OP_STAT, 0, "hello.txt") == HOSTFS_E_OK,
                  "STAT while the posted work drains");
            check(now_ms() - t1 < 2000, "a STAT while three posted 1 s flushes "
                  "drained waited %.0f ms -- more than the one in progress",
                  now_ms() - t1);
            check(log_count(shimlog, "fsync ") >= 1 && log_count(shimlog, "fsync ") <= 2,
                  "%d files flushed when a request arrived during the drain",
                  log_count(shimlog, "fsync "));
            check(log_waits_for(shimlog, "/p3.txt", 15000),
                  "the posted work was not finished while the guest was idle");

            /* Renamed while it is being flushed (a save through a temporary
             * file): no flush of a stale name. Nothing else is pending, so
             * the posted work starts on save.tmp at once. */
            check(rpc(HOSTFS_OP_SYNC, 1, 0, 0, NULL, 0) == HOSTFS_E_OK,
                  "SYNC a = 1 before the save");
            put_file("save.tmp", "atomic save", 0);
            check(rpc(HOSTFS_OP_SYNC, 0, 0, 0, NULL, 0) == HOSTFS_E_OK, "SYNC a = 0 (save)");
            usleep(200000);                 /* idle: save.tmp's flush begins */
            check(rename_path("save.tmp", "save.txt") == HOSTFS_E_OK,
                  "RENAME during a posted flush");
            check(rpc(HOSTFS_OP_SYNC, 1, 0, 0, NULL, 0) == HOSTFS_E_OK,
                  "SYNC a = 1 after the rename");
            check(!log_has(log, "could not flush"), "a flush failed around a rename");

            /* Open and written at a = 0, and closed while posted work ahead
             * of it is still being flushed: still flushed, while idle, with
             * no further SYNC. */
            put_file("slow.txt", "ahead of it", 0);
            check(rpc(HOSTFS_OP_SYNC, 0, 0, 0, NULL, 0) == HOSTFS_E_OK, "SYNC a = 0 (slow)");
            fh = put_file("xopen.txt", "open at the post", 1);
            check(rpc(HOSTFS_OP_SYNC, 0, 0, 0, NULL, 0) == HOSTFS_E_OK, "SYNC a = 0 (x)");
            check(rpc(HOSTFS_OP_CLOSE, fh, 0, 0, NULL, 0) == HOSTFS_E_OK, "CLOSE xopen.txt");
            check(log_waits_for(shimlog, "/xopen.txt", 15000),
                  "a file closed after a posted SYNC was not flushed by it");

            /* Written between a = 0 and a = 1: covered by the a = 1. */
            put_file("w1.txt", "before", 0);
            check(rpc(HOSTFS_OP_SYNC, 0, 0, 0, NULL, 0) == HOSTFS_E_OK, "SYNC a = 0 (w)");
            put_file("w2.txt", "between", 0);
            check(rpc(HOSTFS_OP_SYNC, 1, 0, 0, NULL, 0) == HOSTFS_E_OK, "SYNC a = 1 (w)");
            /* w1 by the posted work or by the a = 1, w2 by the a = 1. */
            check(log_has(shimlog, "/w1.txt") && log_has(shimlog, "/w2.txt"),
                  "SYNC a = 1 did not cover writes before and after an a = 0");
            helper_done(child, "the slow-flush helper");
        }

        /* ---- Traffic that never pauses: posted work flushes nothing for
         * 5 s, then one file per 100 ms of traffic, and drains once the
         * drive is idle. The rule rests on the client never pausing for
         * 20 ms; if this one does (a loaded host), the exact counts are
         * not checked, only progress. */
        snprintf(log, sizeof(log), "%s/sync-busy.log", tmproot);
        snprintf(shimlog, sizeof(shimlog), "%s/sync-busy.fsync", tmproot);
        shim_env("0", NULL, shimlog);
        child = helper_extra("sock11", NULL, log, shim);
        check(child > 0, "a helper for the busy test did not start");
        if (child > 0)
        {
            double t0, last, gap = 0, now;
            int early, forced, flushed;

            t_patient();
            check(rpc(HOSTFS_OP_HELLO, HOSTFS_PROTO_VERSION, 0, 0, NULL, 0)
                  == HOSTFS_E_OK, "HELLO (busy)");
            many_files("busy", 200);
            t0 = last = now_ms();
            check(rpc(HOSTFS_OP_SYNC, 0, 0, 0, NULL, 0) == HOSTFS_E_OK,
                  "SYNC a = 0 (busy)");
            /* `gap`: the longest the helper could have seen no request --
             * a whole round trip, plus the time between two of them. */
            while ((now = now_ms()) - t0 < 4000)
            {
                if (rpc_path(HOSTFS_OP_STAT, 0, "hello.txt") != HOSTFS_E_OK)
                    break;
                if (now_ms() - last > gap)
                    gap = now_ms() - last;
                last = now_ms();
            }
            early = log_count(shimlog, "fsync ");
            while ((now = now_ms()) - t0 < 7000)
            {
                if (rpc_path(HOSTFS_OP_STAT, 0, "hello.txt") != HOSTFS_E_OK)
                    break;
                if (now_ms() - last > gap)
                    gap = now_ms() - last;
                last = now_ms();
            }
            /* The flushes first, while the drive cannot yet have been idle
             * for 20 ms: the idle drain starts soon after the traffic stops,
             * and reading the long --verbose log takes a while. A starved
             * step is logged before its flush, so the later count may be
             * one ahead. */
            flushed = log_count(shimlog, "fsync ");
            forced = log_count(log, "hostfsd: sync: busy for 5000 ms, one file");
            printf("busy test: longest round trip and pause %.1f ms; flushes "
                   "%d in the first 4 s, %d by 7 s, %d of them for starvation\n",
                   gap, early, flushed, forced);
            if (gap < 15)
            {
                check(early == 0, "%d files were flushed in 4 s of unbroken "
                      "traffic after a = 0", early);
                check(forced >= 5 && forced <= 23, "%d starved flushes in the "
                      "2 s past the 5 s bound: not one per 100 ms", forced);
                check(flushed <= forced && forced <= flushed + 1,
                      "%d flushes, but %d for starvation", flushed, forced);
            }
            else
                check(forced >= 1 || flushed >= 1,
                      "no progress on posted work in 7 s of traffic");
            check(log_waits_for(shimlog, "/busy/f00199.txt", 15000),
                  "the posted work did not drain once the drive was idle");
            helper_done(child, "the busy helper");
        }

        /* ---- The same with slow flushes, 300 ms each, as on a USB stick
         * or a network share: past the 5 s bound a request still waits at
         * most for the one flush in progress, because 100 ms of traffic
         * is served between two flushes however long each takes. */
        snprintf(log, sizeof(log), "%s/sync-starve.log", tmproot);
        snprintf(shimlog, sizeof(shimlog), "%s/sync-starve.fsync", tmproot);
        shim_env("300", NULL, shimlog);
        child = helper_extra("sock12", NULL, log, shim);
        check(child > 0, "a helper for the slow starvation test did not start");
        if (child > 0)
        {
            double t0, t, last, worst = 0, at = 0, pause = 0;
            int forced, idle, n = 0;
            long end;

            t_patient();
            check(rpc(HOSTFS_OP_HELLO, HOSTFS_PROTO_VERSION, 0, 0, NULL, 0)
                  == HOSTFS_E_OK, "HELLO (slow starvation)");
            many_files("starve", 12);
            t0 = last = now_ms();
            check(rpc(HOSTFS_OP_SYNC, 0, 0, 0, NULL, 0) == HOSTFS_E_OK,
                  "SYNC a = 0 (slow starvation)");
            while ((t = now_ms()) - t0 < 7500)
            {
                if (t - last > pause)
                    pause = t - last;   /* the client's own pause */
                if (rpc_path(HOSTFS_OP_STAT, 0, "hello.txt") != HOSTFS_E_OK)
                    break;
                last = now_ms();
                n++;
                if (last - t > worst)
                {
                    worst = last - t;
                    at = last - t0;
                }
            }
            /* Where the log stood when the traffic stopped: the helper goes
             * idle 20 ms later, and reading the log takes longer. */
            end = log_size(log);
            forced = log_count_upto(log, "hostfsd: sync: busy for 5000 ms, "
                                    "one file", end);
            idle = log_count_upto(log, "hostfsd: sync: idle, flushing", end);
            printf("slow starvation test: %d requests, longest round trip "
                   "%.0f ms at %.1f s, client pause up to %.1f ms, %d starved "
                   "flushes of 300 ms, %d idle windows\n", n, worst, at / 1000,
                   pause, forced, idle);
            check(worst < 300 + 200, "a request waited %.0f ms behind 300 ms "
                  "flushes: more than the one in progress", worst);
            /* Three or more unless the helper saw the client pause for
             * 20 ms (on a loaded host): an idle window restarts the 5 s
             * clock. Some progress either way. */
            if (!idle)
                check(forced >= 3, "only %d starved flushes in the 2.5 s past "
                      "the 5 s bound", forced);
            else
                check(log_count(shimlog, "fsync ") >= 1,
                      "no progress on posted work in 7.5 s of traffic");
            helper_done(child, "the slow starvation helper");
        }

        /* ---- A HELLO is not held up by a slow flush: with every flush a
         * second long, it answers at once and flushes nothing; the file
         * is flushed by the next a = 1, or else at the end of the
         * session. */
        snprintf(log, sizeof(log), "%s/sync-hello.log", tmproot);
        snprintf(shimlog, sizeof(shimlog), "%s/sync-hello.fsync", tmproot);
        shim_env("1000", NULL, shimlog);
        child = helper_extra("sock10", NULL, log, shim);
        check(child > 0, "a slow-flush helper for HELLO did not start");
        if (child > 0)
        {
            double t0;

            t_patient();
            check(rpc(HOSTFS_OP_HELLO, HOSTFS_PROTO_VERSION, 0, 0, NULL, 0)
                  == HOSTFS_E_OK, "HELLO (slow)");
            put_file("hello-a.txt", "open at the first HELLO", 1);
            t0 = now_ms();
            check(rpc(HOSTFS_OP_HELLO, HOSTFS_PROTO_VERSION, 0, 0, NULL, 0)
                  == HOSTFS_E_OK, "HELLO with a written file open (slow)");
            check(now_ms() - t0 < 1000, "a HELLO with a written file open took "
                  "%.0f ms: it flushed", now_ms() - t0);
            check(!log_has(shimlog, "/hello-a.txt"), "HELLO flushed hello-a.txt");
            check(rpc(HOSTFS_OP_SYNC, 1, 0, 0, NULL, 0) == HOSTFS_E_OK,
                  "SYNC a = 1 after the slow HELLO");
            check(log_has(shimlog, "/hello-a.txt"),
                  "the next a = 1 did not flush the file left open at HELLO");
            put_file("hello-b.txt", "open at the second HELLO", 1);
            t0 = now_ms();
            check(rpc(HOSTFS_OP_HELLO, HOSTFS_PROTO_VERSION, 0, 0, NULL, 0)
                  == HOSTFS_E_OK, "a second HELLO with a written file open");
            check(now_ms() - t0 < 1000, "the second HELLO took %.0f ms",
                  now_ms() - t0);
            check(!log_has(shimlog, "/hello-b.txt"), "HELLO flushed hello-b.txt");
            if (helper_done(child, "the slow HELLO helper"))
                check(log_has(shimlog, "/hello-b.txt"),
                      "the end of the session did not flush the file left at HELLO");
        }

        /* ---- Failures: an open file left at a HELLO that will not flush
         * is not forgotten; every a = 1 reports it until it is deleted.
         * A full disk is reported as ENOSPC, anything else as EIO. */
        snprintf(log, sizeof(log), "%s/sync-fail.log", tmproot);
        shim_env("0", "/fail-", NULL);
        setenv("SLOWSYNC_FULL", "/full-", 1);
        setenv("SLOWSYNC_QUOTA", "/quota-", 1);
        child = helper_extra("sock9", NULL, log, shim);
        check(child > 0, "a failing-flush helper did not start");
        if (child > 0)
        {
            t_patient();
            check(rpc(HOSTFS_OP_HELLO, HOSTFS_PROTO_VERSION, 0, 0, NULL, 0)
                  == HOSTFS_E_OK, "HELLO (fail)");
            put_file("fail-open.txt", "will not flush", 1);
            check(rpc(HOSTFS_OP_HELLO, HOSTFS_PROTO_VERSION, 0, 0, NULL, 0)
                  == HOSTFS_E_OK, "HELLO with an unflushable file open");
            check(log_has(log, "hostfsd: hello: 1 written file(s) still open, "
                               "closed as CLOSE does"),
                  "HELLO did not list the open file");
            check(rpc(HOSTFS_OP_SYNC, 1, 0, 0, NULL, 0) == HOSTFS_EIO,
                  "the file HELLO could not flush was forgotten");
            check(rpc(HOSTFS_OP_SYNC, 1, 0, 0, NULL, 0) == HOSTFS_EIO,
                  "a failure was reported only once");
            check(rpc_path(HOSTFS_OP_DELETE, 0, "fail-open.txt") == HOSTFS_E_OK,
                  "DELETE the unflushable file");
            check(rpc(HOSTFS_OP_SYNC, 1, 0, 0, NULL, 0) == HOSTFS_E_OK,
                  "a deleted file still failed SYNC");

            /* ENOSPC and EDQUOT are a full disk; with an I/O error
             * pending as well, the full disk is what the guest hears. */
            put_file("full-a.txt", "no room", 0);
            check(rpc(HOSTFS_OP_SYNC, 1, 0, 0, NULL, 0) == HOSTFS_ENOSPC
                  && reply_len == 12,
                  "a flush that found the disk full did not give ENOSPC, "
                  "header only");
            check(rpc_path(HOSTFS_OP_DELETE, 0, "full-a.txt") == HOSTFS_E_OK,
                  "DELETE full-a.txt");
            fh = put_file("quota-a.txt", "over quota", 1);
            check(rpc(HOSTFS_OP_SYNC, 1, 0, 0, NULL, 0) == HOSTFS_ENOSPC,
                  "a flush over quota did not give ENOSPC");
            check(rpc(HOSTFS_OP_CLOSE, fh, 0, 0, NULL, 0) == HOSTFS_E_OK
                  && rpc_path(HOSTFS_OP_DELETE, 0, "quota-a.txt") == HOSTFS_E_OK,
                  "CLOSE and DELETE quota-a.txt");
            put_file("fail-b.txt", "I/O error", 0);
            put_file("full-b.txt", "no room", 0);
            check(rpc(HOSTFS_OP_SYNC, 1, 0, 0, NULL, 0) == HOSTFS_ENOSPC,
                  "with a full disk and an I/O error, SYNC did not give ENOSPC");
            check(rpc_path(HOSTFS_OP_DELETE, 0, "full-b.txt") == HOSTFS_E_OK,
                  "DELETE full-b.txt");
            check(rpc(HOSTFS_OP_SYNC, 1, 0, 0, NULL, 0) == HOSTFS_EIO,
                  "the I/O error left alone did not give EIO");
            check(rpc_path(HOSTFS_OP_DELETE, 0, "fail-b.txt") == HOSTFS_E_OK
                  && rpc(HOSTFS_OP_SYNC, 1, 0, 0, NULL, 0) == HOSTFS_E_OK,
                  "SYNC after the failing files were deleted");
            helper_done(child, "the failing-flush helper");
        }

        /* ---- A failure met before the a = 1 is reported by it. The shim
         * fails the first fsync of each once-* file and lets every later
         * one pass, as Linux does with a failed writeback: the retry by
         * a = 1 succeeds with nothing written, so only a count of the
         * earlier failure can tell the guest. */
        snprintf(log, sizeof(log), "%s/sync-once.log", tmproot);
        snprintf(shimlog, sizeof(shimlog), "%s/sync-once.fsync", tmproot);
        shim_env("0", NULL, shimlog);
        setenv("SLOWSYNC_ONCE", "/once-", 1);
        child = helper_extra("sock13", NULL, log, shim);
        check(child > 0, "a helper for failures before a = 1 did not start");
        if (child > 0)
        {
            t_patient();
            check(rpc(HOSTFS_OP_HELLO, HOSTFS_PROTO_VERSION, 0, 0, NULL, 0)
                  == HOSTFS_E_OK, "HELLO (once)");
            /* Listed, and failed by posted work. */
            put_file("once-closed.txt", "fails in posted work", 0);
            check(rpc(HOSTFS_OP_SYNC, 0, 0, 0, NULL, 0) == HOSTFS_E_OK,
                  "SYNC a = 0 (once, closed)");
            check(log_waits_for(log, "could not flush once-closed.txt", 5000),
                  "posted work did not try once-closed.txt while idle");
            check(rpc(HOSTFS_OP_SYNC, 1, 0, 0, NULL, 0) == HOSTFS_EIO
                  && reply_len == 12,
                  "a listed file that failed in posted work was not reported "
                  "by the next a = 1");
            check(log_has(log, ", failed 0, and 1 failure(s) since the last "
                               "a=1\n"),
                  "the a = 1 did not log the earlier failure");
            check(rpc(HOSTFS_OP_SYNC, 1, 0, 0, NULL, 0) == HOSTFS_E_OK,
                  "an earlier failure was reported twice");
            /* Open, and failed by posted work. */
            fh = put_file("once-open.txt", "fails while open", 1);
            check(rpc(HOSTFS_OP_SYNC, 0, 0, 0, NULL, 0) == HOSTFS_E_OK,
                  "SYNC a = 0 (once, open)");
            check(log_waits_for(log, "could not flush once-open.txt", 5000),
                  "posted work did not try once-open.txt while idle");
            check(rpc(HOSTFS_OP_SYNC, 1, 0, 0, NULL, 0) == HOSTFS_EIO,
                  "an open file that failed in posted work was not reported "
                  "by the next a = 1");
            check(rpc(HOSTFS_OP_SYNC, 1, 0, 0, NULL, 0) == HOSTFS_E_OK,
                  "an earlier failure of an open file was reported twice");
            check(rpc(HOSTFS_OP_CLOSE, fh, 0, 0, NULL, 0) == HOSTFS_E_OK,
                  "CLOSE once-open.txt");
            /* The control: a failure that a = 1 meets itself. */
            put_file("once-wait.txt", "fails in a = 1", 0);
            check(rpc(HOSTFS_OP_SYNC, 1, 0, 0, NULL, 0) == HOSTFS_EIO,
                  "a failure a = 1 met itself was not reported");
            check(rpc(HOSTFS_OP_SYNC, 1, 0, 0, NULL, 0) == HOSTFS_E_OK,
                  "a = 1 after its own failure was retried");
            /* A HELLO is a new boot: a failure the last boot's a = 1 never
             * reported is dropped, with a log line, and the new boot's
             * first a = 1 answers for its own session only. */
            put_file("once-hello.txt", "fails, then the guest reboots", 0);
            check(rpc(HOSTFS_OP_SYNC, 0, 0, 0, NULL, 0) == HOSTFS_E_OK,
                  "SYNC a = 0 (once, hello)");
            check(log_waits_for(log, "could not flush once-hello.txt", 5000),
                  "posted work did not try once-hello.txt while idle");
            check(rpc(HOSTFS_OP_HELLO, HOSTFS_PROTO_VERSION, 0, 0, NULL, 0)
                  == HOSTFS_E_OK, "HELLO after an unreported failure");
            check(log_has(log, "hostfsd: dropping 1 flush failure(s) no SYNC "
                               "a=1 reported: the guest started over\n"),
                  "HELLO did not drop the unreported failure, with a line");
            check(rpc(HOSTFS_OP_SYNC, 1, 0, 0, NULL, 0) == HOSTFS_E_OK,
                  "the first a = 1 after HELLO answered for the last boot's "
                  "failure");
            /* The same at the end of the connection. */
            put_file("once-end.txt", "fails, then the guest goes away", 0);
            check(rpc(HOSTFS_OP_SYNC, 0, 0, 0, NULL, 0) == HOSTFS_E_OK,
                  "SYNC a = 0 (once, end)");
            check(log_waits_for(log, "could not flush once-end.txt", 5000),
                  "posted work did not try once-end.txt while idle");
            if (helper_done(child, "the helper for failures before a = 1"))
                check(log_has(log, "hostfsd: dropping 1 flush failure(s) no "
                                   "SYNC a=1 reported: the guest went away\n"),
                      "the end of the connection did not drop the unreported "
                      "failure, with a line");
        }

        /* ---- The same for --fsync-on-close: its failed flush is reported
         * by the next a = 1, and the file is listed for it. */
        snprintf(log, sizeof(log), "%s/sync-once-close.log", tmproot);
        shim_env("0", NULL, shimlog);
        setenv("SLOWSYNC_ONCE", "/once-", 1);
        child = helper_extra("sock14", "--fsync-on-close", log, shim);
        check(child > 0, "a --fsync-on-close helper for failures did not start");
        if (child > 0)
        {
            t_patient();
            check(rpc(HOSTFS_OP_HELLO, HOSTFS_PROTO_VERSION, 0, 0, NULL, 0)
                  == HOSTFS_E_OK, "HELLO (once, fsync on close)");
            put_file("once-close.txt", "fails at its close", 0);
            check(log_has(log, "hostfsd: fsync on close\n")
                  && log_has(log, "could not flush once-close.txt"),
                  "the close's flush did not fail as the shim says");
            check(rpc(HOSTFS_OP_SYNC, 1, 0, 0, NULL, 0) == HOSTFS_EIO,
                  "a failed flush at CLOSE was not reported by the next a = 1");
            check(log_has(log, "hostfsd: sync: once-close.txt\n"),
                  "a file whose flush at CLOSE failed was not listed");
            check(rpc(HOSTFS_OP_SYNC, 1, 0, 0, NULL, 0) == HOSTFS_E_OK,
                  "a failed flush at CLOSE was reported twice");
            helper_done(child, "the --fsync-on-close helper for failures");
        }
        shim_env(NULL, NULL, NULL);

        /* ---- An open file renamed when there is no memory for its new
         * name: its CLOSE lists the old name, which is gone, so nothing
         * flushes it -- and the next a = 1 says so. nomem.so fails only
         * the allocation of that one name, picked by its length. */
        if (helper_sanitized())
            printf("the out-of-memory rename check is skipped: the helper "
                   "was built with a sanitizer, whose allocator nomem.so "
                   "cannot reach\n");
        else
        {
            char nomem[PATH_MAX], served[PATH_MAX], size[32];
            char longname[256];

            memset(longname, 'n', 230);
            strcpy(longname + 230, ".txt");
            if (!realpath("nomem.so", nomem) || !realpath(servedir, served))
                die("realpath nomem.so");
            snprintf(size, sizeof(size), "%lu",
                     (unsigned long)(strlen(served) + 1 + strlen(longname) + 1));
            setenv("NOMEM_SIZE", size, 1);
            snprintf(log, sizeof(log), "%s/sync-nomem.log", tmproot);
            child = helper_extra("sock15", NULL, log, nomem);
            unsetenv("NOMEM_SIZE");
            check(child > 0, "a helper with nomem.so did not start");
            if (child > 0)
            {
                t_patient();
                check(rpc(HOSTFS_OP_HELLO, HOSTFS_PROTO_VERSION, 0, 0, NULL, 0)
                      == HOSTFS_E_OK, "HELLO (nomem)");
                fh = put_file("m4.txt", "renamed while open", 1);
                check(rename_path("m4.txt", longname) == HOSTFS_E_OK,
                      "RENAME an open file (nomem)");
                check(log_has(log, "hostfsd: out of memory following m4.txt"),
                      "a rename that could not follow an open file was not "
                      "logged");
                check(rpc(HOSTFS_OP_CLOSE, fh, 0, 0, NULL, 0) == HOSTFS_E_OK,
                      "CLOSE the renamed file (nomem)");
                check(rpc(HOSTFS_OP_SYNC, 1, 0, 0, NULL, 0) == HOSTFS_EIO,
                      "an open file the rename could not follow was not "
                      "reported by the next a = 1");
                check(log_has(log, "hostfsd: sync: m4.txt is gone"),
                      "the file was listed under a name it no longer has");
                check(rpc(HOSTFS_OP_SYNC, 1, 0, 0, NULL, 0) == HOSTFS_E_OK,
                      "the lost rename was reported twice");
                helper_done(child, "the nomem helper");
            }
        }

        /* ---- Names. a = 1 also flushes each folder whose entries
         * changed -- a CREATE, a DELETE, both sides of a RENAME, a MKDIR,
         * an RMDIR -- so that a rename, an empty file or a deletion is on
         * the disk too, not just the data. A renamed folder's entry
         * follows it. Past the list's 1024 folders, a = 1 makes one
         * syncfs() as well. */
        snprintf(log, sizeof(log), "%s/sync-names.log", tmproot);
        snprintf(shimlog, sizeof(shimlog), "%s/sync-names.fsync", tmproot);
        shim_env("0", NULL, shimlog);
        child = helper_extra("sock16", NULL, log, shim);
        check(child > 0, "a helper for the folder flushes did not start");
        if (child > 0)
        {
            char served[PATH_MAX], top[PATH_MAX + 16], d1[PATH_MAX + 16];
            char d2[PATH_MAX + 16], d4[PATH_MAX + 16], f[PATH_MAX + 32];
            int i, ok = 1;

#define NAMES_SYNC(what) \
            (log_clear(shimlog) \
             && rpc(HOSTFS_OP_SYNC, 1, 0, 0, NULL, 0) == HOSTFS_E_OK) \
            || (check(0, "SYNC a = 1 after %s", what), 0)

            if (!realpath(servedir, served))
                die("realpath served");
            snprintf(top, sizeof(top), "fsync %s\n", served);
            snprintf(d1, sizeof(d1), "fsync %s/nm1\n", served);
            snprintf(d2, sizeof(d2), "fsync %s/nm2\n", served);
            snprintf(d4, sizeof(d4), "fsync %s/nm4\n", served);
            t_patient();
            check(rpc(HOSTFS_OP_HELLO, HOSTFS_PROTO_VERSION, 0, 0, NULL, 0)
                  == HOSTFS_E_OK, "HELLO (names)");
            check(rpc_path(HOSTFS_OP_MKDIR, 0, "nm1") == HOSTFS_E_OK
                  && rpc_path(HOSTFS_OP_MKDIR, 0, "nm2") == HOSTFS_E_OK,
                  "MKDIR nm1, nm2");
            put_file("nm1/f.txt", "moves later", 0);
            if (NAMES_SYNC("MKDIR and CREATE"))
            {
                snprintf(f, sizeof(f), "fsync %s/nm1/f.txt\n", served);
                check(log_has(shimlog, f) && log_has(shimlog, d1)
                      && log_has(shimlog, top),
                      "a = 1 did not flush the new file, its folder and the "
                      "folder holding the new folders");
            }
            /* A rename of a file already flushed: only names changed. */
            check(rename_path("nm1/f.txt", "nm2/g.txt") == HOSTFS_E_OK,
                  "RENAME nm1/f.txt to nm2/g.txt");
            if (NAMES_SYNC("a rename across folders"))
                check(log_has(shimlog, d1) && log_has(shimlog, d2)
                      && log_count(shimlog, "fsync ") == 2,
                      "a = 1 after a rename did not flush both folders, and "
                      "only them");
            check(rpc_path(HOSTFS_OP_CREATE, 0, "nm2/empty.txt") == HOSTFS_E_OK
                  && rpc(HOSTFS_OP_CLOSE, reply_a, 0, 0, NULL, 0) == HOSTFS_E_OK,
                  "CREATE an empty file");
            if (NAMES_SYNC("an empty CREATE"))
                check(log_has(shimlog, d2) && log_count(shimlog, "fsync ") == 1,
                      "a = 1 after an empty CREATE did not flush its folder");
            check(rpc_path(HOSTFS_OP_DELETE, 0, "nm2/g.txt") == HOSTFS_E_OK,
                  "DELETE nm2/g.txt");
            if (NAMES_SYNC("a DELETE"))
                check(log_has(shimlog, d2) && log_count(shimlog, "fsync ") == 1,
                      "a = 1 after a DELETE did not flush its folder");
            check(rpc_path(HOSTFS_OP_RMDIR, 0, "nm1") == HOSTFS_E_OK,
                  "RMDIR nm1");
            if (NAMES_SYNC("an RMDIR"))
                check(log_has(shimlog, top) && log_count(shimlog, "fsync ") == 1,
                      "a = 1 after an RMDIR did not flush the folder above");
            /* A listed folder renamed before the a = 1: flushed under its
             * new name, never the old one. */
            put_file("nm2/h.txt", "in a folder about to move", 0);
            check(rename_path("nm2", "nm4") == HOSTFS_E_OK, "RENAME nm2 to nm4");
            if (NAMES_SYNC("a folder rename"))
                check(log_has(shimlog, d4) && log_has(shimlog, top)
                      && !log_has(shimlog, d2)
                      && log_has(log, "hostfsd: sync: folder nm4\n")
                      && !log_has(log, "hostfsd: sync: folder nm2 is gone"),
                      "a listed folder did not follow its rename");
            check(rpc(HOSTFS_OP_SYNC, 1, 0, 0, NULL, 0) == HOSTFS_E_OK
                  && log_has(log, "hostfsd: SYNC a=1: nothing to do\n"),
                  "a = 1 with every folder flushed still had work");
            /* More folders than the list holds. */
            check(rpc_path(HOSTFS_OP_MKDIR, 0, "ov") == HOSTFS_E_OK, "MKDIR ov");
            for (i = 0; i < 1100 && ok; i++)
            {
                snprintf(f, sizeof(f), "ov/d%04d", i);
                ok = rpc_path(HOSTFS_OP_MKDIR, 0, f) == HOSTFS_E_OK;
                snprintf(f, sizeof(f), "ov/d%04d/e", i);
                ok = ok && rpc_path(HOSTFS_OP_CREATE, 0, f) == HOSTFS_E_OK
                        && rpc(HOSTFS_OP_CLOSE, reply_a, 0, 0, NULL, 0)
                           == HOSTFS_E_OK;
            }
            check(ok, "1100 folders with a file each failed at %d", i);
            if (NAMES_SYNC("1100 folders changed"))
                check(log_count(shimlog, "syncfs ") == 1
                      && log_count(shimlog, "fsync ") == 1024
                      && log_has(log, "hostfsd: sync: more than 1024 folders "
                                      "changed: syncfs\n"),
                      "1100 changed folders did not give 1024 folder flushes "
                      "and one syncfs (%d and %d)",
                      log_count(shimlog, "fsync "), log_count(shimlog, "syncfs "));
            check(rpc(HOSTFS_OP_SYNC, 1, 0, 0, NULL, 0) == HOSTFS_E_OK
                  && log_count(log, "hostfsd: SYNC a=1: nothing to do\n") == 2,
                  "the fallback was not done with");
#undef NAMES_SYNC
            helper_done(child, "the helper for the folder flushes");
        }

        /* ---- a = 1 runs out of time: it starts no flush past its
         * deadline (240 s; 1 s here), answers EIO, and leaves the rest
         * posted, for idle time to finish. Every flush takes 300 ms. */
        snprintf(log, sizeof(log), "%s/sync-deadline.log", tmproot);
        snprintf(shimlog, sizeof(shimlog), "%s/sync-deadline.fsync", tmproot);
        shim_env("300", NULL, shimlog);
        setenv("HOSTFSD_TEST_SYNC_DEADLINE_MS", "1000", 1);
        child = helper_extra("sock17", NULL, log, shim);
        unsetenv("HOSTFSD_TEST_SYNC_DEADLINE_MS");
        check(child > 0, "a helper for the a = 1 deadline did not start");
        if (child > 0)
        {
            double t0, took;
            int st;

            t_patient();
            check(rpc(HOSTFS_OP_HELLO, HOSTFS_PROTO_VERSION, 0, 0, NULL, 0)
                  == HOSTFS_E_OK, "HELLO (deadline)");
            many_files("dl", 10);       /* 10 files and 2 folders to flush */
            t0 = now_ms();
            st = rpc(HOSTFS_OP_SYNC, 1, 0, 0, NULL, 0);
            took = now_ms() - t0;
            printf("deadline test: a = 1 answered %d after %.0f ms, with a "
                   "1000 ms deadline and 300 ms flushes\n", st, took);
            check(st == HOSTFS_EIO && reply_len == 12,
                  "a = 1 out of time answered %d, not EIO", st);
            check(took >= 1000 && took < 1000 + 300 + 300,
                  "a = 1 with a 1000 ms deadline answered after %.0f ms", took);
            check(log_has(log, "hostfsd: SYNC a=1: out of time after ")
                  && log_has(log, " of 12 flush(es) in "),
                  "a = 1 running out of time was not logged");
            check(log_count(shimlog, "fsync ") >= 3
                  && log_count(shimlog, "fsync ") <= 5,
                  "%d flushes before a 1000 ms deadline, at 300 ms each",
                  log_count(shimlog, "fsync "));
            t0 = now_ms();
            check(rpc_path(HOSTFS_OP_STAT, 0, "hello.txt") == HOSTFS_E_OK
                  && now_ms() - t0 < 300 + 200,
                  "a request right after a = 1 ran out of time waited %.0f ms",
                  now_ms() - t0);
            check(log_waits_for(log, "hostfsd: sync: posted work done", 15000),
                  "what a = 1 left was not finished while the guest was idle");
            check(log_count(shimlog, "fsync ") == 12
                  && log_has(shimlog, "/dl/f00009.txt\n"),
                  "%d of 12 files and folders flushed in all",
                  log_count(shimlog, "fsync "));
            check(rpc(HOSTFS_OP_SYNC, 1, 0, 0, NULL, 0) == HOSTFS_E_OK
                  && log_has(log, "hostfsd: SYNC a=1: nothing to do\n"),
                  "a = 1 after the posted rest was done");
            check(!log_has(log, "HOSTFSD_TEST_SYNC_DEADLINE_MS"),
                  "a deadline of 1000 ms was not taken as given");
            helper_done(child, "the deadline helper");
        }
        shim_env(NULL, NULL, NULL);

        /* The variable can only shorten the deadline: 1..240000 ms, and
         * anything else is ignored with a line under --verbose. */
        {
            static const char *const values[] = {
                "240000", "240001", "0", "-5", "5x", "", "99999999999999999999"
            };
            unsigned v;

            for (v = 0; v < sizeof(values) / sizeof(values[0]); v++)
            {
                char name[32];
                int ignored;

                snprintf(name, sizeof(name), "sock%u", 18 + v);
                snprintf(log, sizeof(log), "%s/sync-deadline-%u.log", tmproot, v);
                setenv("HOSTFSD_TEST_SYNC_DEADLINE_MS", values[v], 1);
                child = helper_extra(name, NULL, log, NULL);
                unsetenv("HOSTFSD_TEST_SYNC_DEADLINE_MS");
                check(child > 0, "a helper with the deadline \"%s\" did not "
                      "start", values[v]);
                if (child <= 0)
                    continue;
                ignored = log_has(log, "ignored: not 1 to 240000\n");
                check(ignored == (v != 0), "the deadline \"%s\" was %s",
                      values[v], ignored ? "ignored" : "taken");
                /* One request, or --once would not let the helper go. */
                check(rpc(HOSTFS_OP_HELLO, HOSTFS_PROTO_VERSION, 0, 0, NULL, 0)
                      == HOSTFS_E_OK, "HELLO (deadline \"%s\")", values[v]);
                helper_done(child, "a helper with a test deadline");
            }
        }
        shim_env(NULL, NULL, NULL);
    }

    /* ---- Posting in a loop costs nothing beyond the flushes. */
    snprintf(log, sizeof(log), "%s/sync-flood.log", tmproot);
    child = helper_extra("sock8", NULL, log, NULL);
    check(child > 0, "a flood helper did not start");
    if (child > 0)
    {
        long before, after;
        double t0;
        int i, ok = 1;

        t_patient();
        check(rpc(HOSTFS_OP_HELLO, HOSTFS_PROTO_VERSION, 0, 0, NULL, 0)
              == HOSTFS_E_OK, "HELLO (flood)");
        /* Every SYNC gets one line in the --verbose log, whatever it
         * does: these three, the 20000 below, the a = 1 after them and
         * a refused one -- 20005 in all. */
        check(rpc(HOSTFS_OP_SYNC, 1, 0, 0, NULL, 0) == HOSTFS_E_OK
              && rpc(HOSTFS_OP_SYNC, 0, 0, 0, NULL, 0) == HOSTFS_E_OK,
              "SYNC with nothing to do (flood)");
        fh = put_file("flood.txt", "one write, then only SYNCs", 1);
        check(rpc(HOSTFS_OP_SYNC, 0, 0, 0, NULL, 0) == HOSTFS_E_OK,
              "SYNC a = 0 (flood)");
        before = rss_kb(child);
        for (i = 0; i < 20000 && ok; i++)
            ok = rpc(HOSTFS_OP_SYNC, 0, 0, 0, NULL, 0) == HOSTFS_E_OK;
        after = rss_kb(child);
        check(ok, "SYNC a = 0 number %d failed", i);
        check(after - before < 4096, "20000 posted SYNCs grew the helper by %ld kB",
              after - before);
        t0 = now_ms();
        check(rpc(HOSTFS_OP_SYNC, 1, 0, 0, NULL, 0) == HOSTFS_E_OK,
              "SYNC a = 1 after 20000 posted");
        check(now_ms() - t0 < 2000, "SYNC a = 1 after 20000 posted took %.0f ms",
              now_ms() - t0);
        check(rpc(HOSTFS_OP_SYNC, 2, 0, 0, NULL, 0) == HOSTFS_EDRIVE,
              "SYNC a = 2 (flood)");
        check(log_count(log, "hostfsd: SYNC a=") == 20005,
              "20005 SYNC requests, %d SYNC lines in the log",
              log_count(log, "hostfsd: SYNC a="));
        check(log_has(log, "hostfsd: SYNC a=1: nothing to do\n"
                           "hostfsd: op 19 -> status 0 len 12\n"
                           "hostfsd: SYNC a=0: nothing to do\n")
              && log_has(log, "hostfsd: SYNC a=0: posted\n")
              && log_has(log, "hostfsd: SYNC a=2 b=0 c=0 data=0: refused\n"),
              "a SYNC's outcome is missing from the log");
        check(rpc(HOSTFS_OP_CLOSE, fh, 0, 0, NULL, 0) == HOSTFS_E_OK, "CLOSE flood.txt");
        helper_done(child, "the flood helper");
    }
#endif
    (void)have_shim;
    sock = saved;
}

/* ------------------------------------------------------------------ */

/*
 * The other end of the transport. QEMU's chardev with server=on is the
 * listener, so the shipped launcher runs the helper with --connect --
 * the mode that never gets exercised is the mode that breaks.
 */
static void test_connect_mode(void)
{
    char path[400];
    struct sockaddr_un sa;
    pid_t child;
    int lfd, saved = sock;

    snprintf(path, sizeof(path), "%s/sock2", tmproot);
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strcpy(sa.sun_path, path);

    lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (lfd < 0 || bind(lfd, (struct sockaddr *)&sa, sizeof(sa)) != 0
     || listen(lfd, 1) != 0)
        die("listen for --connect");

    child = fork();
    if (child < 0)
        die("fork");
    if (child == 0)
    {
        execl("../../tools/hostfsd/shinogi-hostfsd", "shinogi-hostfsd",
              "--root", servedir, "--connect", path, (char *)NULL);
        _exit(127);
    }

    sock = accept(lfd, NULL, NULL);
    if (sock < 0)
        die("accept for --connect");
    t_deadline(sock);

    check(rpc(HOSTFS_OP_HELLO, HOSTFS_PROTO_VERSION, 0, 0, NULL, 0)
          == HOSTFS_E_OK, "HELLO in --connect mode");
    check(rpc_path(HOSTFS_OP_STAT, 0, "hello.txt") == HOSTFS_E_OK,
          "STAT in --connect mode");

    /* Dropping the connection must end that helper, and only that one:
     * the launcher owns its lifetime and expects it to exit. */
    close(sock);
    sock = saved;
    close(lfd);
    check(waitpid(child, NULL, 0) == child,
          "the --connect helper did not exit when the pipe closed");

    check(rpc_path(HOSTFS_OP_STAT, 0, "hello.txt") == HOSTFS_E_OK,
          "the listening helper was disturbed by a second one");
}

static void on_alarm(int sig)
{
    (void)sig;
    if (helper > 0)
        kill(helper, SIGKILL);
    /* write() rather than printf(): this is a signal handler. */
    ssize_t ignored = write(1, "FAIL the test run did not finish in time\n", 41);
    (void)ignored;
    _exit(2);
}

int main(void)
{
    /* Line by line, so the FAIL lines before a timeout are not lost. */
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGALRM, on_alarm);
    /* A helper that drops a session while a request is still being sent
     * must show up as a failed check, not kill the run with SIGPIPE. */
    signal(SIGPIPE, SIG_IGN);
    alarm(240);

    scratch_make();
    helper_start();
    t_connect();

    test_startup_containment();
    test_hello();
    test_stat();
    test_file_roundtrip();
    test_big_file();
    test_dir_ops();
    test_readdir();
    test_readdir_churn();
    test_handle_limits();
    test_case_folding();
    test_containment();
    test_still_usable();
    test_malformed();
    test_frames();
    test_fsync_option();
    test_sync();
    test_connect_mode();

    check(helper_alive(), "the helper is not answering at the end of the run");

    t_disconnect();
    kill(helper, SIGTERM);
    waitpid(helper, NULL, 0);
    if (getenv("HOSTFSD_TEST_KEEP"))    /* the helpers' logs, for a failure */
        printf("scratch folder kept: %s\n", tmproot);
    else
        scratch_remove();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
