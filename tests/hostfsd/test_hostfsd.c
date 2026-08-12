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
static unsigned char reply[HOSTFS_MAX_FRAME];
static unsigned reply_len;
static unsigned long reply_a, reply_b;

/*
 * One request, one reply. Returns the GEMDOS status, or -1000 if the
 * transport itself failed -- a value no operation can return, so a test
 * cannot mistake a dropped connection for an error reply.
 */
#define RPC_DEAD (-1000)

static int rpc(unsigned op, unsigned long a, unsigned long b, unsigned long c,
               const void *data, unsigned dlen)
{
    unsigned char req[HOSTFS_MAX_FRAME];
    unsigned len = HOSTFS_REQ_HDR + dlen;
    int status;

    if (len > HOSTFS_MAX_FRAME)
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
    if (reply_len < HOSTFS_REP_HDR || reply_len > HOSTFS_MAX_FRAME)
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

    strcpy(tmproot, "/tmp/hostfsd-test-XXXXXX");
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
    if (len > HOSTFS_REP_HDR && len <= HOSTFS_MAX_FRAME)
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
    signal(SIGALRM, on_alarm);
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
    test_connect_mode();

    check(helper_alive(), "the helper is not answering at the end of the run");

    t_disconnect();
    kill(helper, SIGTERM);
    waitpid(helper, NULL, 0);
    scratch_remove();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
