/*
 * Host checks of the index path of vdi/vdi_xfercore.c: inverse colour maps
 * (v_create_itab), the remapping of 8-bit sources and the error diffusion of
 * vr_transfer_bits mode 128.  See README.txt.
 *
 * The references are independent of the code under test, as fVDI's
 * tests/indexed.c has them: the nearest palette colour by a full search at
 * the centre of each cell, the filter as a formula on 64-bit integers, and
 * the diffusion over two wide error rows (the code keeps one row of words).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "portab.h"
#include "vdi_xfercore.h"

static int failures;
static long checks;

static void check(int ok, const char *what, long a, long b)
{
    checks++;
    if (!ok)
    {
        failures++;
        if (failures < 40)
            printf("FAIL: %s (%ld, %ld)\n", what, a, b);
    }
}

static uint32_t rng_state = 4242;

static uint32_t rnd(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state >> 8;
}

/* ------------------------------------------------------------------ */
/* palettes */

static void random_palette(COLOR_ENTRY *p, int count, int dups)
{
    int i;

    for (i = 0; i < count; i++)
    {
        p[i].rgb.reserved = 0;
        p[i].rgb.red = (uint16_t)rnd();
        p[i].rgb.green = (uint16_t)rnd();
        p[i].rgb.blue = (uint16_t)rnd();
        if (dups && i > 0 && (rnd() % 5) == 0)
            p[i] = p[rnd() % i];        /* equal colours: the first must win */
    }
}

/* the nearest colour at the centre of the cell of rgb, by a full search */
static int nearest(const COLOR_ENTRY *p, int count, int bits, unsigned long rgb)
{
    long bin = 256 >> bits, best = 0x7fffffffL;
    long r = (((rgb >> 16) & 255) / bin) * bin + bin / 2;
    long g = (((rgb >> 8) & 255) / bin) * bin + bin / 2;
    long b = ((rgb & 255) / bin) * bin + bin / 2;
    int i, result = 0;

    for (i = 0; i < count; i++)
    {
        long dr = r - p[i].rgb.red / 256;
        long dg = g - p[i].rgb.green / 256;
        long db = b - p[i].rgb.blue / 256;
        long d = dr * dr + dg * dg + db * db;

        if (d < best)
        {
            best = d;
            result = i;
        }
    }
    return result;
}

static unsigned char *make_map(const COLOR_ENTRY *p, int count, int bits)
{
    unsigned char *map = malloc(XFER_ITAB_CELLS(bits));
    void *work = malloc(XFER_ITAB_WORK(count));     /* exactly the size asked for */

    xfer_itab_fill(p, (WORD)count, (WORD)bits, map, work);
    free(work);
    return map;
}

/* ------------------------------------------------------------------ */
/* the inverse maps, every cell */

static void test_itab(void)
{
    static const int counts[] = { 1, 2, 3, 4, 16, 17, 100, 255, 256 };
    COLOR_ENTRY pal[256];
    int bits, c, dups, i;
    long cell;

    for (bits = 3; bits <= 5; bits++)
    {
        for (c = 0; c < (int)(sizeof(counts) / sizeof(counts[0])); c++)
        {
            for (dups = 0; dups < 2; dups++)
            {
                unsigned char *map;
                int n = counts[c];

                random_palette(pal, n, dups);
                map = make_map(pal, n, bits);
                for (cell = 0; cell < XFER_ITAB_CELLS(bits); cell++)
                {
                    long mask = (1L << bits) - 1;
                    int shift = 8 - bits;
                    unsigned long rgb = ((((cell >> (2 * bits)) & mask) << shift) << 16) |
                                        ((((cell >> bits) & mask) << shift) << 8) |
                                        ((cell & mask) << shift);

                    check(map[cell] == nearest(pal, n, bits, rgb), "inverse map cell", bits, cell);
                }
                /* the lookup takes the top bits of each component */
                for (i = 0; i < 2000; i++)
                {
                    unsigned long rgb = rnd() & 0xffffff;

                    check(xfer_itab_index(map, (WORD)bits, rgb) == nearest(pal, n, bits, rgb),
                          "inverse map lookup", bits, (long)rgb);
                }
                free(map);
            }
        }
    }

    /* a palette of the cell centres: each cell finds its own colour */
    for (bits = 3; bits <= 4; bits++)
    {
        int n = 1 << bits, step = 256 >> bits;
        unsigned char *map;

        for (i = 0; i < n; i++)
        {
            pal[i].rgb.reserved = 0;
            pal[i].rgb.red = pal[i].rgb.green = pal[i].rgb.blue = (uint16_t)((i * step + step / 2) * 256);
        }
        map = make_map(pal, n, bits);
        for (i = 0; i < n; i++)
        {
            long v = (long)i * step + step / 2;

            check(xfer_itab_index(map, (WORD)bits, (unsigned long)((v << 16) | (v << 8) | v)) == i,
                  "grey ramp", bits, i);
        }
        free(map);
    }
}

/* ------------------------------------------------------------------ */
/* the remapping of 8-bit sources */

static int ref_exact(const COLOR_ENTRY *src, int index, const COLOR_ENTRY *dst, int dcount)
{
    const COLOR_RGB *c = &src[index].rgb;
    int i;

    if (index < dcount && c->red == dst[index].rgb.red && c->green == dst[index].rgb.green &&
        c->blue == dst[index].rgb.blue)
        return index;
    for (i = 0; i < dcount; i++)
        if (c->red == dst[i].rgb.red && c->green == dst[i].rgb.green && c->blue == dst[i].rgb.blue)
            return i;
    return -1;
}

static void test_remap(void)
{
    COLOR_ENTRY dst[256], src[256];
    unsigned char remap[256];
    ULONG spal[256];
    int round, i, scount, dcount, flags, exact, same, want;

    for (round = 0; round < 300; round++)
    {
        unsigned char *map;

        dcount = 1 + rnd() % 256;
        scount = 1 + rnd() % 256;
        random_palette(dst, dcount, 1);
        for (i = 0; i < scount; i++)
        {
            switch (round % 4)
            {
            case 0:                     /* the same table, or its start */
                src[i] = dst[i % dcount];
                break;
            case 1:                     /* colours of dst in another order */
                src[i] = dst[rnd() % dcount];
                break;
            case 2:                     /* other colours */
                random_palette(&src[i], 1, 0);
                break;
            default:                    /* mixed, with the reserved word set */
                src[i] = (rnd() & 1) ? dst[rnd() % dcount] : dst[0];
                if (rnd() & 1)
                    random_palette(&src[i], 1, 0);
                src[i].rgb.reserved = (uint16_t)rnd();
                break;
            }
        }
        map = make_map(dst, dcount, 4);
        flags = xfer_remap(src, (WORD)scount, dst, (WORD)dcount, map, 4, remap, spal);
        exact = 1;
        same = 1;
        for (i = 0; i < scount; i++)
        {
            unsigned long rgb = ((unsigned long)(src[i].rgb.red >> 8) << 16) |
                                ((unsigned long)(src[i].rgb.green >> 8) << 8) | (src[i].rgb.blue >> 8);

            want = ref_exact(src, i, dst, dcount);
            if (want < 0)
            {
                exact = 0;
                want = nearest(dst, dcount, 4, rgb);
            }
            if (want != i)
                same = 0;
            check(remap[i] == want, "remap index", round, i);
            check(spal[i] == rgb, "remap source colour", round, i);
        }
        check(((flags & XFER_REMAP_EXACT) != 0) == exact, "remap exact flag", round, flags);
        check(((flags & XFER_REMAP_SAME) != 0) == same, "remap same flag", round, flags);
        free(map);
    }
}

/* ------------------------------------------------------------------ */
/* the index path, against fVDI's reference */

static unsigned long ref_rgb(const unsigned char *p, int kind)
{
    unsigned long v, r, g, b;
    int rev = (kind & XK_REV) != 0;

    switch (kind & XK_MASK)
    {
    case XK_32:
        return ((unsigned long)p[1] << 16) | (p[2] << 8) | p[3];
    case XK_24:
        return rev ? (((unsigned long)p[2] << 16) | (p[1] << 8) | p[0])
                   : (((unsigned long)p[0] << 16) | (p[1] << 8) | p[2]);
    default:
        v = rev ? p[1] * 256UL + p[0] : p[0] * 256UL + p[1];
        b = v & 31;
        if ((kind & XK_MASK) == XK_16)
        {
            r = v >> 11;
            g = (v >> 5) & 63;
            g = g * 4 + g / 16;
        }
        else
        {
            r = (v >> 10) & 31;
            g = (v >> 5) & 31;
            g = g * 8 + g / 4;
        }
        return ((r * 8 + r / 4) << 16) | (g << 8) | (b * 8 + b / 4);
    }
}

typedef struct
{
    int kind;
    const unsigned char *base;  /* the first pixel of the source rectangle */
    long pitch;
    const COLOR_ENTRY *spal;    /* XK_8 */
    int scount;
} Src;

static long channel_at(const Src *s, long x, long y, int k)
{
    unsigned long c;

    if ((s->kind & XK_MASK) == XK_8)
    {
        const COLOR_RGB *e = &s->spal[s->base[y * s->pitch + x]].rgb;

        return (k == 0 ? e->red : k == 1 ? e->green : e->blue) / 256;
    }
    c = ref_rgb(s->base + y * s->pitch + x * (xfer_kind_bits((WORD)s->kind) / 8), s->kind);
    return (c >> (16 - 8 * k)) & 255;
}

/* the colour of destination pixel (dx, dy): nearest, or filtered when 'filtered' and reduced */
static unsigned long sample(const Src *s, long sw, long sh, long dw, long dh, long dx, long dy, int filtered)
{
    long x, y, fx = 0, fy = 0, x1, y1;
    unsigned long rgb = 0;
    int k;

    if (filtered && (sw > dw || sh > dh))
    {
        x = (long)(((2 * (int64_t)dx + 1) * sw * 128) / dw - 128);
        y = (long)(((2 * (int64_t)dy + 1) * sh * 128) / dh - 128);
        if (x < 0)
            x = 0;
        if (y < 0)
            y = 0;
        if (x > (sw - 1) * 256)
            x = (sw - 1) * 256;
        if (y > (sh - 1) * 256)
            y = (sh - 1) * 256;
        fx = x % 256;
        fy = y % 256;
        x /= 256;
        y /= 256;
    }
    else
    {
        x = (long)((int64_t)dx * sw / dw);
        y = (long)((int64_t)dy * sh / dh);
    }
    x1 = x + (fx != 0);
    y1 = y + (fy != 0);
    for (k = 0; k < 3; k++)
    {
        uint64_t sum = (uint64_t)channel_at(s, x, y, k) * (256 - fx) * (256 - fy) +
                       (uint64_t)channel_at(s, x1, y, k) * fx * (256 - fy) +
                       (uint64_t)channel_at(s, x, y1, k) * (256 - fx) * fy +
                       (uint64_t)channel_at(s, x1, y1, k) * fx * fy;

        rgb = (rgb << 8) | (unsigned long)((sum + 32768) / 65536);
    }
    return rgb;
}

/*
 * the whole destination rectangle, dw x dh indices into out: fVDI's
 * reference (tests/indexed.c), two wide error rows
 */
static void reference(const Src *s, long sw, long sh, long dw, long dh,
                      const COLOR_ENTRY *dpal, int dcount, int bits, int dither, unsigned char *out)
{
    long *a = calloc((dw + 2) * 3, sizeof(long)), *b = calloc((dw + 2) * 3, sizeof(long)), *t;
    int indexed = (s->kind & XK_MASK) == XK_8, k;
    long x, y;

    if (indexed)
    {
        int exact = 1;

        for (k = 0; k < s->scount; k++)
            if (ref_exact(s->spal, k, dpal, dcount) < 0)
                exact = 0;
        if (exact)
            dither = 0;
    }
    for (y = 0; y < dh; y++)
    {
        memset(b, 0, (dw + 2) * 3 * sizeof(long));
        for (x = 0; x < dw; x++)
        {
            unsigned long rgb = sample(s, sw, sh, dw, dh, x, y, !indexed || dither), adjusted = 0;
            long channel[3];
            int index;

            for (k = 0; k < 3; k++)
            {
                long e = dither ? a[(x + 1) * 3 + k] + 8 : 0;
                long correction = e < 0 ? -((-e + 15) / 16) : e / 16;
                long c = (long)((rgb >> (16 - 8 * k)) & 255) + correction;

                channel[k] = c < 0 ? 0 : c > 255 ? 255 : c;
                adjusted = (adjusted << 8) | (unsigned long)channel[k];
            }
            index = nearest(dpal, dcount, bits, adjusted);
            if (indexed && !dither)
            {
                long sx = (long)((int64_t)x * sw / dw), sy = (long)((int64_t)y * sh / dh);
                int e = ref_exact(s->spal, s->base[sy * s->pitch + sx], dpal, dcount);

                if (e >= 0)
                    index = e;
            }
            out[y * dw + x] = (unsigned char)index;
            if (!dither)
                continue;
            channel[0] -= dpal[index].rgb.red / 256;
            channel[1] -= dpal[index].rgb.green / 256;
            channel[2] -= dpal[index].rgb.blue / 256;
            for (k = 0; k < 3; k++)
            {
                a[(x + 2) * 3 + k] += channel[k] * 7;
                b[x * 3 + k] += channel[k] * 3;
                b[(x + 1) * 3 + k] += channel[k] * 5;
                b[(x + 2) * 3 + k] += channel[k];
            }
        }
        t = a;
        a = b;
        b = t;
    }
    free(a);
    free(b);
}

/*
 * One job of the index path: sw x sh onto dw x dh, the window (skipx,
 * skipy, count, rows), compared with the reference of the whole rectangle;
 * guard bytes around each row stay.
 */
static long cases;

static void run_index(int kind, long sw, long sh, long dw, long dh, long skipx, long skipy,
                      long count, long rows, int dither, int dcount, int scount, int exact_src)
{
    int k = kind & XK_MASK, bytes = xfer_kind_bits((WORD)kind) / 8;
    long pitch = sw * bytes + 3, x, y, i;
    unsigned char *src = malloc(pitch * sh + 1), *want = malloc(dw * dh);
    unsigned char *got = malloc((count + 4) * rows), *map, remap[256];
    COLOR_ENTRY dpal[256], spal[256];
    ULONG spalrgb[256];
    WORD *err = NULL;
    int use_dither = dither, flags;
    XferJob job;
    XferState st;
    Src s;

    random_palette(dpal, dcount, 1);
    if (k == XK_8)
    {
        for (i = 0; i < scount; i++)
            spal[i] = exact_src ? dpal[rnd() % dcount] : dpal[i % dcount];
        if (!exact_src)
            random_palette(spal + scount / 2, scount - scount / 2, 0);
    }
    for (i = 0; i < pitch * sh + 1; i++)
        src[i] = (unsigned char)(k == XK_8 ? rnd() % scount : rnd());
    map = make_map(dpal, dcount, 4);

    s.kind = kind;
    s.base = src + 1;               /* an odd start: bytes have no alignment */
    s.pitch = pitch;
    s.spal = spal;
    s.scount = scount;
    reference(&s, sw, sh, dw, dh, dpal, dcount, 4, dither, want);

    memset(&job, 0, sizeof(job));
    job.kind = (WORD)kind;
    job.planes = XD_INDEX8;
    job.src = s.base;
    job.pitch = pitch;
    job.sw = sw;
    job.sh = sh;
    job.dw = dw;
    job.dh = dh;
    job.skipx = skipx;
    job.skipy = skipy;
    job.count = count;
    job.rows = rows;
    job.index = TRUE;
    job.imap = map;
    job.ibits = 4;
    job.dcol = dpal;
    if (k == XK_8)
    {
        flags = xfer_remap(spal, (WORD)scount, dpal, (WORD)dcount, map, 4, remap, spalrgb);
        if (flags & XFER_REMAP_EXACT)
            use_dither = 0;
        job.spal = spalrgb;
        job.remap = remap;
    }
    if (use_dither)
    {
        job.dither = TRUE;
        err = calloc(3 * dw, sizeof(WORD));
        job.err = err;
    }
    memset(got, 0xa7, (count + 4) * rows);
    xfer_begin(&st, &job);
    for (y = 0; y < rows; y++)
        xfer_row_index(&st, got + y * (count + 4) + 2);
    for (y = 0; y < rows; y++)
    {
        unsigned char *r = got + y * (count + 4);

        check(r[0] == 0xa7 && r[1] == 0xa7 && r[count + 2] == 0xa7 && r[count + 3] == 0xa7,
              "index row guards", kind, y);
        for (x = 0; x < count; x++)
        {
            if (r[x + 2] != want[(y + skipy) * dw + x + skipx])
            {
                check(0, "index pixel", kind * 1000 + dither * 100 + (k == XK_8 ? exact_src : 0),
                      ((sw * 100 + sh) * 1000 + dw) * 100 + dh);
                y = rows;
                break;
            }
        }
    }
    check(1, "index case", 0, 0);
    cases++;
    free(src);
    free(want);
    free(got);
    free(map);
    free(err);
}

static void test_index(void)
{
    static const int kinds[] = { XK_8, XK_15, XK_15 | XK_REV, XK_16, XK_16 | XK_REV,
                                 XK_24, XK_24 | XK_REV, XK_32 };
    static const long sizes[] = { 1, 2, 3, 5, 8, 13, 17, 31 };
    int ki, dither, a, b, c, d;

    for (ki = 0; ki < (int)(sizeof(kinds) / sizeof(kinds[0])); ki++)
        for (dither = 0; dither < 2; dither++)
            for (a = 0; a < 8; a++)
                for (b = 0; b < 8; b++)
                {
                    long sw = sizes[a], dw = sizes[b];
                    long sh = sizes[(a + 3) % 8], dh = sizes[(b + 5) % 8];

                    /* the whole rectangle, then a window inside it */
                    run_index(kinds[ki], sw, sh, dw, dh, 0, 0, dw, dh, dither, 16, 16, a & 1);
                    if (dw > 2 && dh > 2)
                        run_index(kinds[ki], sw, sh, dw, dh, 1, dh / 2, dw - 2, dh - dh / 2,
                                  dither, 2 + b * 30, 1 + a * 30, b & 1);
                }

    /* wide and tall, palettes of 2 and 256 colours */
    for (ki = 0; ki < (int)(sizeof(kinds) / sizeof(kinds[0])); ki++)
        for (dither = 0; dither < 2; dither++)
            for (c = 0; c < 2; c++)
                for (d = 0; d < 2; d++)
                {
                    long sw = c ? 197 : 61, sh = c ? 7 : 113, dw = d ? 160 : 320, dh = d ? 50 : 9;

                    run_index(kinds[ki], sw, sh, dw, dh, 0, 0, dw, dh, dither, c ? 256 : 2, 256, 0);
                    run_index(kinds[ki], sw, sh, dw, dh, dw / 3, dh / 3, dw / 3, dh / 2,
                              dither, d ? 2 : 256, 17, 1);
                }
}

/* a mid grey dithered onto black and white: both appear, no index beyond the palette */
static void test_grey(void)
{
    COLOR_ENTRY bw[2] = { { { 0, 0, 0, 0 } }, { { 0, 0xffff, 0xffff, 0xffff } } };
    unsigned char src[4 * 4], out[64 * 64], *map = make_map(bw, 2, 4);
    WORD err[3 * 64];
    long ones = 0, i, y;
    XferJob job;
    XferState st;

    for (i = 0; i < 16; i += 4)
    {
        src[i] = 0;
        src[i + 1] = src[i + 2] = src[i + 3] = 0x80;
    }
    memset(err, 0, sizeof(err));
    memset(&job, 0, sizeof(job));
    job.kind = XK_32;
    job.planes = XD_INDEX8;
    job.src = src;
    job.pitch = 8;
    job.sw = job.sh = 2;
    job.dw = job.dh = 64;
    job.count = job.rows = 64;
    job.index = job.dither = TRUE;
    job.imap = map;
    job.ibits = 4;
    job.dcol = bw;
    job.err = err;
    xfer_begin(&st, &job);
    for (y = 0; y < 64; y++)
        xfer_row_index(&st, out + y * 64);
    for (i = 0; i < 64 * 64; i++)
    {
        check(out[i] <= 1, "dither index bounds", i, out[i]);
        ones += out[i];
    }
    check(ones > 64 * 64 * 40 / 100 && ones < 64 * 64 * 60 / 100, "mid grey half white", ones, 0);
    free(map);
}

/* ------------------------------------------------------------------ */
/* the memory of an 8-bit destination */

static void test_dst_size(void)
{
    unsigned char mem[64];
    GCBITMAP bm;

    memset(&bm, 0, sizeof(bm));
    bm.addr = mem + 1;                  /* bytes: an odd address is fine */
    bm.width = 7;
    bm.bits = 8;
    bm.px_format = XF_PX_8;
    bm.xmax = 7;
    bm.ymax = 3;
    check(xfer_dst_size(&bm, XD_INDEX8) == 21, "index8 size", (long)xfer_dst_size(&bm, XD_INDEX8), 21);
    bm.width = 6;
    check(xfer_dst_size(&bm, XD_INDEX8) == 0, "index8 line too short", 0, 0);
    bm.width = 7;
    bm.px_format = 0x01000808UL;        /* planes are not bytes */
    check(xfer_dst_size(&bm, XD_INDEX8) == 0, "index8 needs its format", 0, 0);
    bm.px_format = XF_PX_8;
    bm.bits = 16;
    check(xfer_dst_size(&bm, XD_INDEX8) == 0, "index8 needs 8 bits", 0, 0);
    bm.bits = 8;
    bm.addr = NULL;
    check(xfer_dst_size(&bm, XD_INDEX8) == 0, "index8 needs an address", 0, 0);
}

int main(void)
{
    test_itab();
    test_remap();
    test_index();
    test_grey();
    test_dst_size();
    printf("%ld checks (%ld index cases), %d failures\n", checks, cases, failures);
    printf("test_dither %s\n", failures ? "FAIL" : "PASS");
    return failures != 0;
}
