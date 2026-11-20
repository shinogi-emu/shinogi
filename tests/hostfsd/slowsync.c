/*
 * slowsync.so -- LD_PRELOAD shim for test_hostfsd (Linux). Every fsync()
 * and syncfs() first appends the path behind its descriptor to
 * $SLOWSYNC_LOG, then takes $SLOWSYNC_MS milliseconds longer, and fails
 * when that path contains $SLOWSYNC_FAIL (with EIO), $SLOWSYNC_FULL
 * (ENOSPC) or $SLOWSYNC_QUOTA (EDQUOT). A test can then see which files
 * really were flushed, in what order, when a flush is slow, and what a
 * failing one does.
 *
 * $SLOWSYNC_ONCE models how Linux reports a failed writeback: the FIRST
 * fsync() of a file whose path contains it fails with EIO, and every
 * later one succeeds, through the same descriptor or a new one -- the
 * pages were marked clean, so there is nothing left to write and
 * nothing left to report.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* The files $SLOWSYNC_ONCE has already failed once. */
static struct { dev_t dev; ino_t ino; } failed_once[256];
static int nfailed_once;

/* Is this the first flush of FD's file? Remembers it either way. */
static int first_flush(int fd)
{
    struct stat st;
    int i;

    if (fstat(fd, &st) != 0)
        return 0;
    for (i = 0; i < nfailed_once; i++)
        if (failed_once[i].dev == st.st_dev && failed_once[i].ino == st.st_ino)
            return 0;
    if (nfailed_once == 256)
        return 0;
    failed_once[nfailed_once].dev = st.st_dev;
    failed_once[nfailed_once].ino = st.st_ino;
    nfailed_once++;
    return 1;
}

/* Does PATH contain the text in the variable NAME? */
static int names(const char *path, const char *name)
{
    const char *v = getenv(name);

    return v && *v && strstr(path, v) != NULL;
}

/* Log, sleep, and say how this one is to fail: an errno, or 0. */
static int shim(const char *what, int fd)
{
    const char *log = getenv("SLOWSYNC_LOG"), *ms = getenv("SLOWSYNC_MS");
    char link[64], target[4096] = "?", line[4200];
    struct timespec t;
    long n = ms ? atol(ms) : 0;
    ssize_t k;

    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    if ((k = readlink(link, target, sizeof(target) - 1)) > 0)
        target[k] = '\0';
    if (log)
    {
        int f = open(log, O_WRONLY | O_CREAT | O_APPEND, 0666);

        if (f >= 0)
        {
            int len = snprintf(line, sizeof(line), "%s %s\n", what, target);

            ssize_t ignored = write(f, line, (size_t)len);

            (void)ignored;
            close(f);
        }
    }
    t.tv_sec = n / 1000;
    t.tv_nsec = (n % 1000) * 1000000L;
    nanosleep(&t, NULL);
    if (names(target, "SLOWSYNC_FULL"))
        return ENOSPC;
    if (names(target, "SLOWSYNC_QUOTA"))
        return EDQUOT;
    if (names(target, "SLOWSYNC_ONCE") && first_flush(fd))
        return EIO;
    return names(target, "SLOWSYNC_FAIL") ? EIO : 0;
}

int fsync(int fd)
{
    int (*real)(int) = (int (*)(int))dlsym(RTLD_NEXT, "fsync");
    int e = shim("fsync", fd);

    if (e)
    {
        errno = e;
        return -1;
    }
    return real(fd);
}

int syncfs(int fd)
{
    int (*real)(int) = (int (*)(int))dlsym(RTLD_NEXT, "syncfs");
    int e = shim("syncfs", fd);

    if (e)
    {
        errno = e;
        return -1;
    }
    return real(fd);
}
