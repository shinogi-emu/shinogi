/*
 * Host checks for the memory allocator of EmuTOS's outline font engine
 * (vdi/vdi_ftmem.c), built from the EmuTOS tree with -m32 and ASan/UBSan.
 *
 * The allocator takes 64 KB arenas and large blocks from the system
 * through ftmem_os_alloc()/ftmem_os_free(); here those come from malloc(),
 * cut to the exact size (so ASan sees a write past a system block), only
 * 2-byte aligned like a GEMDOS block can be, counted, and refusable.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "vdi_ftmem.h"

static int checks, failures;

#define CHECK(cond, ...) \
    do { \
        checks++; \
        if (!(cond)) { \
            failures++; \
            printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
            printf(__VA_ARGS__); \
            printf("\n"); \
        } \
    } while (0)

#define ARENA       65536UL     /* what the allocator asks for small blocks */
#define LARGE       16384UL     /* the biggest request served from an arena */


/*
 * the system's blocks
 */

typedef struct
{
    unsigned char *base;        /* from malloc */
    unsigned char *block;       /* handed out */
    ULONG size;
} OsBlock;

static OsBlock os_blocks[4096];
static long os_count;           /* live blocks */
static long os_total;           /* blocks handed out so far */
static long os_bytes;           /* live bytes */
static long os_fail_after = -1; /* as host_alloc_fail_after() */
static ULONG os_largest;        /* refuse blocks bigger than this, 0: none */
static int os_offset = 2;       /* the misalignment of the blocks */

void *ftmem_os_alloc(ULONG size)
{
    unsigned char *base;

    if (os_fail_after == 0 || (os_largest && size > os_largest) || size > 0x10000000UL ||
        os_count >= (long)(sizeof(os_blocks) / sizeof(os_blocks[0])))
        return NULL;
    base = malloc(size + os_offset);
    if (!base)
        return NULL;
    if (os_fail_after > 0)
        os_fail_after--;
    os_blocks[os_count].base = base;
    os_blocks[os_count].block = base + os_offset;
    os_blocks[os_count].size = size;
    os_count++;
    os_total++;
    os_bytes += size;
    return base + os_offset;
}

void ftmem_os_free(void *block)
{
    long i;

    for (i = 0; i < os_count; i++)
    {
        if (os_blocks[i].block == block)
        {
            free(os_blocks[i].base);
            os_bytes -= os_blocks[i].size;
            os_blocks[i] = os_blocks[--os_count];
            return;
        }
    }
    printf("FAIL: ftmem_os_free(%p) of a block the system never gave\n", block);
    failures++;
}


/*
 * blocks the tests hold, each filled with its own pattern
 */

typedef struct
{
    unsigned char *p;
    ULONG size;
    unsigned char seed;
} Held;

static unsigned long rng = 12345;

static unsigned long next_random(void)
{
    rng = rng * 1103515245UL + 12345UL;
    return (rng >> 8) & 0xffffff;
}

static void fill(Held *h)
{
    ULONG i;

    for (i = 0; i < h->size; i++)
        h->p[i] = (unsigned char)(h->seed + i * 7);
}

static int intact(const Held *h, ULONG n)
{
    ULONG i;

    for (i = 0; i < n && i < h->size; i++)
        if (h->p[i] != (unsigned char)(h->seed + i * 7))
            return 0;
    return 1;
}

static int aligned(const void *p)
{
    return ((uintptr_t)p & 7) == 0;
}

static int by_address(const void *a, const void *b)
{
    const Held *x = a, *y = b;

    return (x->p > y->p) - (x->p < y->p);
}

/* no two blocks overlap, and each still holds its pattern */
static int sound(Held *held, long n)
{
    Held *sorted;
    long i;
    int ok = 1;

    if (n <= 0)
        return 1;
    sorted = malloc(n * sizeof(Held));
    memcpy(sorted, held, n * sizeof(Held));
    qsort(sorted, n, sizeof(Held), by_address);
    for (i = 0; i < n; i++)
    {
        if (!intact(&sorted[i], sorted[i].size))
            ok = 0;
        if (i > 0 && sorted[i - 1].p + sorted[i - 1].size > sorted[i].p)
            ok = 0;
    }
    free(sorted);
    return ok;
}


/*
 * the checks, in an order that matters: the first one needs an allocator
 * that holds no system block yet
 */

static void check_no_memory_at_all(void)
{
    printf("no memory at all\n");
    os_fail_after = 0;
    CHECK(ftmem_alloc(16) == NULL, "small block from nothing");
    CHECK(ftmem_alloc(100000) == NULL, "large block from nothing");
    CHECK(ftmem_realloc(NULL, 100) == NULL, "realloc from nothing");
    ftmem_free(NULL);
    CHECK(ftmem_realloc(NULL, 0) == NULL, "realloc of nothing to nothing");
    os_fail_after = -1;
    CHECK(os_count == 0, "%ld system blocks", os_count);
}

static void check_sizes(int offset)
{
    static const ULONG sizes[] =
    {
        0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 24, 31, 32, 33, 100, 1000, 4095,
        4096, 13404, LARGE - 1, LARGE, LARGE + 1, 20000, ARENA - 1, ARENA, ARENA + 1,
        200000, 1000000
    };
    Held held[sizeof(sizes) / sizeof(sizes[0])];
    long i, n = sizeof(sizes) / sizeof(sizes[0]), bad = 0;

    printf("sizes, system blocks at offset %d\n", offset);
    os_offset = offset;
    for (i = 0; i < n; i++)
    {
        held[i].p = ftmem_alloc(sizes[i]);
        held[i].size = sizes[i];
        held[i].seed = (unsigned char)i;
        if (!held[i].p || !aligned(held[i].p))
            bad++;
        else
            fill(&held[i]);
    }
    CHECK(bad == 0, "%ld blocks missing or not 8-byte aligned", bad);
    CHECK(sound(held, n), "blocks overlap or were overwritten");
    for (i = 0; i < n; i += 2)
        ftmem_free(held[i].p);
    for (i = 1; i < n; i += 2)
        CHECK(intact(&held[i], held[i].size), "block %ld changed when others were freed", i);
    for (i = 1; i < n; i += 2)
        ftmem_free(held[i].p);
    /* one empty arena may be kept for the next request, nothing else */
    CHECK(os_count <= 1, "%ld system blocks left", os_count);

    /* every small size, one at a time */
    bad = 0;
    for (i = 0; i <= 2100; i++)
    {
        unsigned char *p = ftmem_alloc(i);

        if (!p || !aligned(p))
            bad++;
        else
            memset(p, 0xa5, i);
        ftmem_free(p);
    }
    CHECK(bad == 0, "%ld sizes failed", bad);
    CHECK(os_count <= 1, "%ld system blocks left", os_count);
}

static void check_large_and_small(void)
{
    long before;
    void *p, *q;

    printf("large blocks have their own system block\n");
    p = ftmem_alloc(100);       /* an arena exists */
    before = os_total;
    q = ftmem_alloc(LARGE);
    CHECK(q && os_total == before, "%ld system blocks for %lu bytes", os_total - before, LARGE);
    ftmem_free(q);
    q = ftmem_alloc(LARGE + 1);
    CHECK(q && os_total == before + 1, "%ld system blocks for %lu bytes", os_total - before,
          LARGE + 1);
    before = os_count;
    ftmem_free(q);
    CHECK(os_count == before - 1, "large block not given back");
    ftmem_free(p);
}

static void check_arenas(void)
{
    static Held held[400];
    long i, total;

    printf("arenas are taken and given back\n");
    for (i = 0; i < 400; i++)
    {
        held[i].p = ftmem_alloc(1000);
        held[i].size = 1000;
        held[i].seed = (unsigned char)(i * 3);
        if (held[i].p)
            fill(&held[i]);
    }
    CHECK(os_count >= 6 && os_count <= 8, "%ld arenas for 400 KB", os_count);
    CHECK(os_bytes <= (long)(8 * ARENA), "%ld bytes from the system", os_bytes);
    CHECK(sound(held, 400), "blocks overlap or were overwritten");

    /* free every other block: no arena empties */
    for (i = 0; i < 400; i += 2)
        ftmem_free(held[i].p);
    CHECK(os_count >= 6, "an arena with blocks in use was given back");
    for (i = 1; i < 400; i += 2)
        CHECK(intact(&held[i], 1000), "block %ld", i);
    for (i = 1; i < 400; i += 2)
        ftmem_free(held[i].p);
    CHECK(os_count == 1, "%ld system blocks left, one empty arena expected", os_count);

    /* the kept arena serves the next requests */
    total = os_total;
    held[0].p = ftmem_alloc(1000);
    CHECK(held[0].p && os_total == total, "a new arena instead of the kept one");
    ftmem_free(held[0].p);
}

/*
 * Every order of freeing seven blocks must merge their arena whole again.
 * That is seen from outside: an arena that empties while another empty
 * one is kept is given back to the system.  So each round puts the seven
 * blocks in the kept arena, takes 16 KB blocks until a second arena is
 * needed, frees those (the second arena is now the kept one), and frees
 * the seven in one of the 5040 orders: the first arena must go back.
 */
static void check_free_orders(void)
{
    static const ULONG sizes[7] = { 24, 100, 1, 500, 64, 3000, 16 };
    int order[7], i, j, k, n;
    long permutations = 0, broken = 0, kept = 0;
    unsigned char *p[7];
    void *big[8];

    printf("every order of freeing seven blocks\n");
    for (i = 0; i < 7; i++)
        order[i] = i;
    for (;;)
    {
        long before;

        for (i = 0; i < 7; i++)
        {
            p[i] = ftmem_alloc(sizes[i]);
            if (p[i])
                memset(p[i], i + 1, sizes[i]);
        }
        before = os_total;
        for (n = 0; n < 8 && os_total == before; n++)
            big[n] = ftmem_alloc(LARGE);
        while (n > 0)
            ftmem_free(big[--n]);

        before = os_count;
        for (i = 0; i < 7; i++)
        {
            for (j = i; j < 7; j++)
            {
                unsigned char *q = p[order[j]];

                for (k = 0; q && k < (int)sizes[order[j]]; k++)
                    if (q[k] != order[j] + 1)
                    {
                        broken++;
                        break;
                    }
            }
            ftmem_free(p[order[i]]);
        }
        if (os_count != before - 1)
            kept++;
        permutations++;

        /* the next permutation */
        for (i = 5; i >= 0 && order[i] > order[i + 1]; i--)
            ;
        if (i < 0)
            break;
        for (j = 6; order[j] < order[i]; j--)
            ;
        k = order[i]; order[i] = order[j]; order[j] = k;
        for (j = i + 1, k = 6; j < k; j++, k--)
        {
            int t = order[j];

            order[j] = order[k];
            order[k] = t;
        }
    }
    CHECK(permutations == 5040, "%ld permutations", permutations);
    CHECK(broken == 0, "%ld blocks overwritten", broken);
    CHECK(kept == 0, "%ld orders left their arena in pieces", kept);
    CHECK(os_count == 1, "%ld system blocks left", os_count);
}

static void check_realloc(void)
{
    Held h;
    unsigned char *p;
    long before;

    printf("realloc\n");
    h.p = ftmem_realloc(NULL, 100);
    h.size = 100;
    h.seed = 77;
    CHECK(h.p != NULL, "realloc(NULL)");
    if (!h.p)
        return;
    fill(&h);

    /* growing keeps what was there, small to large and back */
    {
        static const ULONG steps[] = { 5000, LARGE, LARGE + 1, 30000, 200000, 9000, 100, 60, 8 };
        ULONG i, old = 100;

        for (i = 0; i < sizeof(steps) / sizeof(steps[0]); i++)
        {
            p = ftmem_realloc(h.p, steps[i]);
            CHECK(p && aligned(p), "realloc to %lu", steps[i]);
            if (!p)
                break;
            h.p = p;
            CHECK(intact(&h, old < steps[i] ? old : steps[i]), "content lost going %lu -> %lu",
                  old, steps[i]);
            h.size = steps[i];
            fill(&h);
            old = steps[i];
        }
    }

    /* a little smaller stays in place */
    p = ftmem_realloc(h.p, 1000);
    h.p = p;
    h.size = 1000;
    fill(&h);
    CHECK(ftmem_realloc(h.p, 900) == h.p, "shrinking a little moved the block");
    h.size = 900;

    /* failure leaves the block alone, and still in use: blocks taken
       next do not land on it */
    os_fail_after = 0;
    p = ftmem_realloc(h.p, 300000);
    os_fail_after = -1;
    CHECK(p == NULL && intact(&h, 900), "failed realloc");
    {
        static Held more[65];
        int i;

        more[0] = h;
        for (i = 1; i < 65; i++)
        {
            more[i].p = ftmem_alloc(900);
            more[i].size = more[i].p ? 900 : 0;
            more[i].seed = (unsigned char)(i * 11);
            if (more[i].p)
                fill(&more[i]);
        }
        CHECK(sound(more, 65), "the block was freed by the failed realloc");
        for (i = 1; i < 65; i++)
            ftmem_free(more[i].p);
    }

    /* to nothing is a free */
    before = os_count;
    CHECK(ftmem_realloc(h.p, 0) == NULL, "realloc to 0");
    CHECK(os_count <= 1 && os_count <= before, "%ld system blocks left", os_count);
}

static void check_partial_memory(void)
{
    void *a, *b, *c;
    long before;

    printf("the system refuses more blocks\n");
    a = ftmem_alloc(1000);      /* in an arena */
    os_fail_after = 0;
    b = ftmem_alloc(1000);
    CHECK(b != NULL, "room left in the arena, but no block");
    CHECK(ftmem_alloc(LARGE + 1) == NULL, "large block without the system");
    c = ftmem_alloc(LARGE);
    CHECK(c != NULL, "the arena has room for %lu bytes", LARGE);
    {
        /* fill the arena, then the next block must fail */
        void *more[8];
        int i, got = 0;

        for (i = 0; i < 8; i++)
            if ((more[i] = ftmem_alloc(LARGE)) != NULL)
                got++;
        CHECK(got < 8, "%d blocks of 16 KB from one arena", got + 1);
        for (i = 0; i < 8; i++)
            ftmem_free(more[i]);
    }
    os_fail_after = -1;
    ftmem_free(a);
    ftmem_free(b);
    ftmem_free(c);

    /* no arena from the system, but small blocks: each gets its own */
    a = ftmem_alloc(100);       /* the kept arena */
    os_largest = 32768;
    before = os_total;
    {
        static Held held[200];
        int i, ok = 1;

        for (i = 0; i < 200; i++)
        {
            held[i].p = ftmem_alloc(1000);
            held[i].size = 1000;
            held[i].seed = (unsigned char)i;
            if (!held[i].p || !aligned(held[i].p))
                ok = 0;
            else
                fill(&held[i]);
        }
        CHECK(ok, "small blocks without arenas");
        CHECK(sound(held, 200), "blocks overlap or were overwritten");
        CHECK(os_total > before, "no block of their own");
        for (i = 0; i < 200; i++)
            ftmem_free(held[i].p);
    }
    os_largest = 0;
    ftmem_free(a);
    CHECK(os_count <= 1, "%ld system blocks left", os_count);
}

static void check_random(void)
{
    enum { MAX_HELD = 3000, OPS = 400000 };
    static Held held[MAX_HELD];
    long n = 0, op, i, bad = 0, failed = 0, peak_bytes = 0, peak_os = 0;
    long live_bytes = 0;

    printf("random allocations, frees and reallocs\n");
    for (op = 0; op < OPS; op++)
    {
        unsigned long r = next_random() % 100;
        unsigned long s = next_random();
        ULONG size;

        if (s % 100 < 70)
            size = s % 256 + 1;
        else if (s % 100 < 90)
            size = s % 4096 + 1;
        else if (s % 100 < 98)
            size = s % LARGE + 1;
        else
            size = s % 100000 + 1;

        if ((r < 45 && n < MAX_HELD) || n == 0)
        {
            Held *h = &held[n];

            h->p = ftmem_alloc(size);
            if (!h->p)
            {
                failed++;
                continue;
            }
            if (!aligned(h->p))
                bad++;
            h->size = size;
            h->seed = (unsigned char)op;
            fill(h);
            live_bytes += size;
            n++;
        }
        else if (r < 85)
        {
            i = next_random() % n;
            if (!intact(&held[i], held[i].size))
                bad++;
            ftmem_free(held[i].p);
            live_bytes -= held[i].size;
            held[i] = held[--n];
        }
        else
        {
            unsigned char *p;

            i = next_random() % n;
            p = ftmem_realloc(held[i].p, size);
            if (!p)
            {
                failed++;
                continue;
            }
            held[i].p = p;
            if (!intact(&held[i], held[i].size < size ? held[i].size : size) || !aligned(p))
                bad++;
            live_bytes += (long)size - (long)held[i].size;
            held[i].size = size;
            fill(&held[i]);
        }
        if (live_bytes > peak_bytes)
            peak_bytes = live_bytes;
        if (os_bytes > peak_os)
            peak_os = os_bytes;
        if (op % 20000 == 0 && !sound(held, n))
            bad++;
    }
    CHECK(bad == 0, "%ld blocks misaligned, overlapping or overwritten", bad);
    CHECK(failed == 0, "%ld requests failed", failed);
    CHECK(sound(held, n), "blocks overlap or were overwritten at the end");
    /* the system memory follows what is in use */
    CHECK(peak_os < 2 * peak_bytes + 8 * (long)ARENA, "%ld bytes from the system at most for %ld in use",
          peak_os, peak_bytes);
    printf("  peak: %ld bytes in use, %ld from the system\n", peak_bytes, peak_os);
    for (i = 0; i < n; i++)
        ftmem_free(held[i].p);
    CHECK(os_count <= 1, "%ld system blocks left", os_count);
}

int main(void)
{
    check_no_memory_at_all();
    check_sizes(2);
    check_sizes(0);
    check_sizes(6);
    os_offset = 2;
    check_large_and_small();
    check_arenas();
    check_free_orders();
    check_realloc();
    check_partial_memory();
    check_random();

    printf("vdi-ftmem: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
