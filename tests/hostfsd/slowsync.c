/*
 * slowsync.so -- LD_PRELOAD shim for test_hostfsd (Linux). Every fsync()
 * and syncfs() first appends the path behind its descriptor to
 * $SLOWSYNC_LOG, then takes $SLOWSYNC_MS milliseconds longer, and fails
 * when that path contains $SLOWSYNC_FAIL (with EIO), $SLOWSYNC_FULL
 * (ENOSPC) or $SLOWSYNC_QUOTA (EDQUOT). A test can then see which files
 * really were flushed, in what order, when a flush is slow, and what a
 * failing one does.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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
