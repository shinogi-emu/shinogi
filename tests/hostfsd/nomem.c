/*
 * nomem.so -- LD_PRELOAD shim for test_hostfsd (Linux): malloc() of
 * exactly $NOMEM_SIZE bytes fails with ENOMEM, and every other size is
 * left alone. A test picks a size that only the allocation it means to
 * fail can have (a path of a length nothing else uses), and so reaches
 * an out-of-memory branch without starving the rest of the program.
 *
 * The real malloc() is found with dlsym() on first use; a malloc() made
 * while that is under way fails, which glibc's dlsym() does not need.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>

static void *(*real_malloc)(size_t);
static size_t fail_size;
static int resolving;

void *malloc(size_t n)
{
    if (!real_malloc)
    {
        const char *v;

        if (resolving)
            return NULL;
        resolving = 1;
        real_malloc = (void *(*)(size_t))dlsym(RTLD_NEXT, "malloc");
        v = getenv("NOMEM_SIZE");
        fail_size = v ? strtoul(v, NULL, 10) : 0;
        resolving = 0;
        if (!real_malloc)
            return NULL;
    }
    if (fail_size && n == fail_size)
    {
        errno = ENOMEM;
        return NULL;
    }
    return real_malloc(n);
}
