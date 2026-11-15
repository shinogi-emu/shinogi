/*
 * Host checks of vdi/vdi_xfercore.c, the pixel conversion of vr_transfer_bits
 * in EmuTOS's VDI.  See README.txt.
 *
 * The expected pixels come from direct formulas on 64-bit integers (the
 * sampling position of a pixel, the four-sample filter of a reduction),
 * independent of the rational stepping the core runs.
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

static uint32_t rng_state = 12345;

static uint32_t rnd(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state >> 8;
}

/* ------------------------------------------------------------------ */
/* the unpackers, against the format as written down */

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
            /* 6 bits to 8: g * 255 / 63, which is 4 g + g / 16 */
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

static void test_unpack(void)
{
    static const int kinds[] = { XK_15, XK_16, XK_15 | XK_REV, XK_16 | XK_REV,
                                 XK_24, XK_24 | XK_REV, XK_32 };
    unsigned i;
    unsigned long v;
    unsigned char p[4];

    for (i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++)
    {
        int kind = kinds[i];

        if (xfer_kind_bits(kind) == 16)
        {
            /* every 16-bit value, both orders */
            for (v = 0; v < 0x10000UL; v++)
            {
                p[0] = (kind & XK_REV) ? v & 255 : v >> 8;
                p[1] = (kind & XK_REV) ? v >> 8 : v & 255;
                check(xfer_unpack(p, kind) == ref_rgb(p, kind), "unpack 16 bits", kind, v);
            }
        }
        else
        {
            for (v = 0; v < 100000UL; v++)
            {
                p[0] = rnd();
                p[1] = rnd();
                p[2] = rnd();
                p[3] = rnd();
                check(xfer_unpack(p, kind) == ref_rgb(p, kind), "unpack", kind, v);
            }
        }
    }
    /* the 5-6-5 pixel is its own round trip, the 5-5-5 one lands in the same bits */
    for (v = 0; v < 0x10000UL; v++)
    {
        p[0] = v >> 8;
        p[1] = v & 255;
        check(xfer_pack565(xfer_unpack(p, XK_16)) == v, "565 round trip", v, 0);
        check(xfer_pack565(xfer_unpack(p, XK_16 | XK_REV))
              == (unsigned long)(((v & 255) << 8) | (v >> 8)), "565 reversed round trip", v, 0);
    }
    /* 0x00rrggbb cut to 5-6-5 */
    check(xfer_pack565(0xffffffUL) == 0xffff, "pack white", 0, 0);
    check(xfer_pack565(0xff0000UL) == 0xf800, "pack red", 0, 0);
    check(xfer_pack565(0x00ff00UL) == 0x07e0, "pack green", 0, 0);
    check(xfer_pack565(0x0000ffUL) == 0x001f, "pack blue", 0, 0);
    check(xfer_pack565(0x070307UL) == 0, "pack cuts", 0, 0);
}

static void test_kinds(void)
{
    check(xfer_kind(0x01020101UL) == XK_1, "kind 1", 0, 0);
    check(xfer_kind(0x01020808UL) == XK_8, "kind 8", 0, 0);
    check(xfer_kind(0x03420f10UL) == XK_15, "kind 15", 0, 0);
    check(xfer_kind(0x03021010UL) == XK_16, "kind 16", 0, 0);
    check(xfer_kind(0x03421820UL) == XK_32, "kind 32", 0, 0);
    check(xfer_kind(0x03021818UL) == XK_24, "kind 24", 0, 0);
    check(xfer_kind(0x03420f10UL | 0x00800000UL) == (XK_15 | XK_REV), "kind 15 reversed", 0, 0);
    check(xfer_kind(0x03021010UL | 0x00800000UL) == (XK_16 | XK_REV), "kind 16 reversed", 0, 0);
    check(xfer_kind(0x03021818UL | 0x00800000UL) == (XK_24 | XK_REV), "kind 24 reversed", 0, 0);
    check(xfer_kind(0x01020101UL | 0x00800000UL) == XK_NONE, "1 bit is not reversed", 0, 0);
    check(xfer_kind(0x01020808UL | 0x00800000UL) == XK_NONE, "8 bits is not reversed", 0, 0);
    check(xfer_kind(0x03421820UL | 0x00800000UL) == XK_NONE, "32 bits is not reversed", 0, 0);
    check(xfer_kind(0) == XK_NONE, "kind 0", 0, 0);
    check(xfer_kind(0x01000404UL) == XK_NONE, "planar is not a source", 0, 0);
    check(xfer_kind(0x03420f11UL) == XK_NONE, "kind near 15", 0, 0);
}

/* ------------------------------------------------------------------ */
/* the sampling, against the formulas */

typedef struct
{
    int kind;
    unsigned char *base;        /* the source memory */
    long pitch;
    int bit;                    /* XK_1: the bit of the first byte of a row of the rectangle */
    uint32_t pal[256];          /* XK_8: the rgb of an index */
    uint16_t fg, bg;
    int planes;                 /* XD_16 or planes */
} Ref;

static int rgb_of(const Ref *r, long x, long y, int channel)
{
    const unsigned char *p;
    unsigned long c;
    int k = r->kind & XK_MASK;

    if (k == XK_8)
        p = r->base + y * r->pitch + x;
    else
        p = r->base + y * r->pitch + x * (xfer_kind_bits(r->kind) / 8);
    if (k == XK_8)
        c = r->pal[*p];
    else
        c = ref_rgb(p, r->kind);
    return channel == 0 ? (c >> 16) & 255 : channel == 1 ? (c >> 8) & 255 : c & 255;
}

static unsigned long rgb_pixel(const Ref *r, long x, long y)
{
    return ((unsigned long)rgb_of(r, x, y, 0) << 16) | (rgb_of(r, x, y, 1) << 8) | rgb_of(r, x, y, 2);
}

/* the direct source pixel of the filter at (dx, dy) of the destination rectangle, as 0x00rrggbb */
static unsigned long ref_filtered(const Ref *r, long sw, long sh, long dw, long dh, long dx, long dy)
{
    int64_t x = ((2 * (int64_t)dx + 1) * sw * 128) / dw - 128;
    int64_t y = ((2 * (int64_t)dy + 1) * sh * 128) / dh - 128;
    long fx, fy, x2, y2;
    unsigned long rgb = 0;
    int k;

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
    x2 = x + (fx != 0);
    y2 = y + (fy != 0);
    for (k = 0; k < 3; k++)
    {
        uint64_t sum = (uint64_t)rgb_of(r, x, y, k) * (256 - fx) * (256 - fy) +
                       (uint64_t)rgb_of(r, x2, y, k) * fx * (256 - fy) +
                       (uint64_t)rgb_of(r, x, y2, k) * (256 - fx) * fy +
                       (uint64_t)rgb_of(r, x2, y2, k) * fx * fy;
        rgb = (rgb << 8) | ((sum + 32768) / 65536);
    }
    return rgb;
}

/* the test's own colour to pixel value functions for planes */
static unsigned long test_pen_rgb(void *ctx, unsigned long rgb)
{
    int planes = *(int *)ctx;
    unsigned long v = ((rgb >> 16) * 3 + ((rgb >> 8) & 255) * 5 + (rgb & 255) * 7) >> 4;

    return v & ((1UL << planes) - 1);
}

static unsigned long pen_index_calls;

static unsigned long test_pen_index(void *ctx, unsigned long index)
{
    int planes = *(int *)ctx;

    pen_index_calls++;
    return (index * 37 + 11) & ((1UL << planes) - 1);
}

static long pen_rgb_calls;

static unsigned long counting_pen_rgb(void *ctx, unsigned long rgb)
{
    pen_rgb_calls++;
    return test_pen_rgb(ctx, rgb);
}

/*
 * One job: sw x sh onto dw x dh, the part (skipx, skipy, count, rows) drawn,
 * onto 16-bit words or planes.  Returns after comparing every byte of the
 * destination (guard bytes around and between rows included).
 */
static void run_case(int kind, int planes, int transparent, long sw, long sh, long dw, long dh,
                     long skipx, long skipy, long count, long rows, int shift)
{
    long bits = xfer_kind_bits(kind), pbytes = bits / 8;
    long pitch, srows = sh + 2;
    unsigned char *s, *saved, *d, *expected;
    long dpitch, dsize, x, y, i;
    Ref ref;
    XferJob job;
    XferState st;
    uint16_t map[256], map_ref[256];
    unsigned char *pix = NULL, *mask = NULL;
    int bit = kind == XK_1 ? shift & 7 : 0;
    int filtered;
    int pl = planes;

    memset(&ref, 0, sizeof(ref));
    if (kind == XK_1)
        pitch = (sw + bit + 7) / 8 + 1 + (shift & 3);
    else
        pitch = sw * pbytes + (shift & 3) * 2;
    s = malloc(pitch * srows + 8);
    saved = malloc(pitch * srows + 8);
    for (i = 0; i < pitch * srows + 8; i++)
        s[i] = rnd();
    memcpy(saved, s, pitch * srows + 8);

    ref.kind = kind;
    ref.base = s + 4;
    ref.pitch = pitch;
    ref.bit = bit;
    ref.planes = planes;
    for (i = 0; i < 256; i++)
        ref.pal[i] = rnd() & 0xffffff;
    ref.fg = (uint16_t)(rnd() | 1);
    ref.bg = (uint16_t)(rnd() & ~1);
    if (planes != XD_16)
    {
        ref.fg &= (1 << planes) - 1;
        ref.bg &= (1 << planes) - 1;
    }

    memset(&job, 0, sizeof(job));
    job.kind = kind;
    job.planes = planes;
    job.transparent = transparent;
    job.src = ref.base;
    job.bit = bit;
    job.pitch = pitch;
    job.sw = sw;
    job.sh = sh;
    job.dw = dw;
    job.dh = dh;
    job.skipx = skipx;
    job.skipy = skipy;
    job.count = count;
    job.rows = rows;
    job.map = map;
    job.fg = ref.fg;
    job.bg = ref.bg;
    job.pen_rgb = (XferPenFn)counting_pen_rgb;
    job.pen_index = (XferPenFn)test_pen_index;
    job.ctx = &pl;
    for (i = 0; i < 256; i++)
    {
        if (kind == XK_8)
        {
            if (planes == XD_16)
                map[i] = xfer_pack565(ref.pal[i]);
            else
                map[i] = 0xffff;
        }
        map_ref[i] = planes == XD_16 ? xfer_pack565(ref.pal[i]) : 0;
    }

    filtered = (kind != XK_1 && kind != XK_8 && planes == XD_16 && (sw > dw || sh > dh));

    /* what pixel a destination position is */
    #define PIXEL_AT(dx, dy, outv) do { \
        long sx_ = (long)((int64_t)(dx) * sw / dw), sy_ = (long)((int64_t)(dy) * sh / dh); \
        if (kind == XK_1) { \
            long b_ = bit + sx_; \
            int set_ = (ref.base[sy_ * pitch + (b_ >> 3)] >> (7 - (b_ & 7))) & 1; \
            (outv) = set_ ? ref.fg : ref.bg; \
        } else if (kind == XK_8) { \
            unsigned idx_ = ref.base[sy_ * pitch + sx_]; \
            (outv) = planes == XD_16 ? map_ref[idx_] : (long)((idx_ * 37 + 11) & ((1 << planes) - 1)); \
        } else if (filtered) { \
            (outv) = xfer_pack565(ref_filtered(&ref, sw, sh, dw, dh, (dx), (dy))); \
        } else if (planes == XD_16) { \
            (outv) = xfer_pack565(rgb_pixel(&ref, sx_, sy_)); \
        } else { \
            (outv) = test_pen_rgb(&pl, rgb_pixel(&ref, sx_, sy_)); \
        } \
    } while (0)

    xfer_begin(&st, &job);
    if (planes == XD_16)
    {
        dpitch = count * 2 + 4;
        dsize = dpitch * rows + 8;
        d = malloc(dsize);
        expected = malloc(dsize);
        memset(d, 0xcd, dsize);
        memset(expected, 0xcd, dsize);
        for (y = 0; y < rows; y++)
            for (x = 0; x < count; x++)
            {
                long v;
                int set = 1;

                if (kind == XK_1 && transparent)
                {
                    long b_ = bit + (long)((int64_t)(skipx + x) * sw / dw);
                    long sy_ = (long)((int64_t)(skipy + y) * sh / dh);

                    set = (ref.base[sy_ * pitch + (b_ >> 3)] >> (7 - (b_ & 7))) & 1;
                }
                PIXEL_AT(skipx + x, skipy + y, v);
                if (set)
                {
                    expected[y * dpitch + x * 2] = v >> 8;
                    expected[y * dpitch + x * 2 + 1] = v & 255;
                }
            }
        for (y = 0; y < rows; y++)
            xfer_row16(&st, d + y * dpitch, y ? d + (y - 1) * dpitch : NULL);
        check(memcmp(d, expected, dsize) == 0, "16-bit rows", kind, planes);
        if (memcmp(d, expected, dsize) != 0)
            printf("  kind %d %ldx%ld -> %ldx%ld skip %ld,%ld part %ldx%ld tr %d bit %d\n", kind,
                   sw, sh, dw, dh, skipx, skipy, count, rows, transparent, bit);
    }
    else
    {
        long x0 = (shift * 5) & 31;
        long words = ((x0 + count + 15) / 16);

        dpitch = words * 2 * planes + 6;
        dsize = dpitch * rows + 8;
        d = malloc(dsize);
        expected = malloc(dsize);
        pix = malloc(count + 1);
        mask = malloc(count + 1);
        for (i = 0; i < dsize; i++)
            d[i] = expected[i] = rnd();
        for (y = 0; y < rows; y++)
            for (x = 0; x < count; x++)
            {
                long v, col = x0 + x;
                int set = 1, p;

                if (kind == XK_1 && transparent)
                {
                    long b_ = bit + (long)((int64_t)(skipx + x) * sw / dw);
                    long sy_ = (long)((int64_t)(skipy + y) * sh / dh);

                    set = (ref.base[sy_ * pitch + (b_ >> 3)] >> (7 - (b_ & 7))) & 1;
                }
                if (!set)
                    continue;
                PIXEL_AT(skipx + x, skipy + y, v);
                for (p = 0; p < planes; p++)
                {
                    unsigned char *w = expected + y * dpitch + (col >> 4) * 2 * planes + 2 * p + ((col & 15) >> 3);
                    int m = 0x80 >> (col & 7);

                    if ((v >> p) & 1)
                        *w |= m;
                    else
                        *w &= ~m;
                }
            }
        pen_rgb_calls = 0;
        for (y = 0; y < rows; y++)
        {
            xfer_row_pix(&st, pix, transparent ? mask : NULL);
            xfer_store_planar(d + y * dpitch, planes, x0, count, pix, transparent ? mask : NULL);
        }
        check(memcmp(d, expected, dsize) == 0, "planar rows", kind, planes);
        if (memcmp(d, expected, dsize) != 0)
            printf("  kind %d planes %d %ldx%ld -> %ldx%ld skip %ld,%ld part %ldx%ld tr %d x0 %ld\n", kind,
                   planes, sw, sh, dw, dh, skipx, skipy, count, rows, transparent, x0);
        if (kind != XK_1 && kind != XK_8)
            check(pen_rgb_calls <= count * rows, "the colour cache never asks more than once a pixel",
                  pen_rgb_calls, count * rows);
        free(pix);
        free(mask);
    }
    check(memcmp(s, saved, pitch * srows + 8) == 0, "source unchanged", kind, 0);
    free(s);
    free(saved);
    free(d);
    free(expected);
}

static const int all_kinds[] = {
    XK_1, XK_8, XK_15, XK_16, XK_15 | XK_REV, XK_16 | XK_REV, XK_24, XK_24 | XK_REV, XK_32
};
#define NKINDS ((int)(sizeof(all_kinds) / sizeof(all_kinds[0])))

/* every size 1-40 by 1-40, enlargements and reductions in both directions */
static void test_sweep(void)
{
    long sw, dw, n = 0;
    int k;

    for (sw = 1; sw <= 40; sw++)
        for (dw = 1; dw <= 40; dw++)
        {
            /* two heights for each pair of widths, one of them the same ratio the other way */
            long h1s = (sw * 7 + dw * 3) % 40 + 1, h1d = (sw + dw * 5) % 40 + 1;
            long h2s = dw, h2d = sw;

            for (k = 0; k < NKINDS; k++)
            {
                int kind = all_kinds[k];
                int planes = (n & 1) ? XD_16 : (n % 4 == 0 ? 8 : 4);
                int tr = kind == XK_1 && (n & 2);

                run_case(kind, XD_16, tr, sw, h1s, dw, h1d, 0, 0, dw, h1d, (int)n);
                run_case(kind, planes, tr, sw, h2s, dw, h2d, 0, 0, dw, h2d, (int)n);
                n++;
            }
        }
}

/* the part drawn is a window of the destination: the phase of the sampling stays */
static void test_clipped(void)
{
    static const long sizes[] = { 1, 2, 3, 7, 16, 19, 33, 40 };
    int a, b, c, k, n = 0;

    for (a = 0; a < 8; a++)
        for (b = 0; b < 8; b++)
            for (c = 0; c < 8; c++)
                for (k = 0; k < NKINDS; k++)
                {
                    long sw = sizes[a], sh = sizes[b], dw = sizes[c], dh = sizes[(a + b + c) % 8];
                    long sx = dw > 2 ? 1 : 0, sy = dh > 2 ? 1 : 0;
                    long cx = dw - sx - (dw > 3 ? 1 : 0), cy = dh - sy - (dh > 3 ? 2 : 0);
                    int kind = all_kinds[k];
                    int tr = kind == XK_1 && (n & 1);
                    static const int pl[4] = { XD_16, 1, 2, 8 };
                    int planes = pl[(a + c + k) & 3];

                    if (cx < 1 || cy < 1)
                        continue;
                    run_case(kind, planes, tr, sw, sh, dw, dh, sx, sy, cx, cy, n);
                    n++;
                }
}

/* planes 1, 2, 4, 8 and 16-bit, with the source narrower and wider than a word of the destination */
static void test_wide(void)
{
    static const int pl[5] = { XD_16, 1, 2, 4, 8 };
    int p, k;

    for (p = 0; p < 5; p++)
        for (k = 0; k < NKINDS; k++)
        {
            int kind = all_kinds[k];

            run_case(kind, pl[p], 0, 37, 29, 83, 53, 0, 0, 83, 53, p + k);
            run_case(kind, pl[p], 0, 83, 53, 37, 29, 0, 0, 37, 29, p + k);
            run_case(kind, pl[p], kind == XK_1, 37, 53, 83, 29, 19, 3, 40, 20, p * 3 + k);
            run_case(kind, pl[p], 0, 1, 1, 70, 5, 0, 0, 70, 5, k);
            run_case(kind, pl[p], 0, 70, 5, 1, 1, 0, 0, 1, 1, k);
        }
}

/* sources of 65536 pixels, destinations of 65536, and the skips at the ends */
static void test_extreme(void)
{
    int k;

    for (k = 0; k < NKINDS; k++)
    {
        int kind = all_kinds[k];

        run_case(kind, XD_16, 0, 65536L, 1, 1, 1, 0, 0, 1, 1, k);
        run_case(kind, XD_16, 0, 1, 1, 65536L, 1, 0, 0, 65536L, 1, k);
        run_case(kind, XD_16, 0, 1, 1, 65536L, 1, 65535L, 0, 1, 1, k);
        run_case(kind, XD_16, 0, 65536L, 1, 65536L, 1, 65535L, 0, 1, 1, k);
        run_case(kind, XD_16, 0, 65536L, 1, 65535L, 1, 65534L, 0, 1, 1, k);
        run_case(kind, XD_16, 0, 65535L, 1, 65536L, 1, 65535L, 0, 1, 1, k);
        run_case(kind, XD_16, 0, 3, 65536L, 5, 7, 2, 0, 3, 7, k);
        run_case(kind, XD_16, 0, 3, 7, 5, 65536L, 2, 65530L, 3, 6, k);
        run_case(kind, 4, 0, 65536L, 1, 40, 1, 0, 0, 40, 1, k);
    }
}

/* the colour cache and the lazy table of an 8-bit source onto planes ask for what they need */
static void test_lazy(void)
{
    unsigned char row[64], pix[64];
    uint16_t map[256];
    XferJob job;
    XferState st;
    int planes = 4, i;

    for (i = 0; i < 64; i++)
        row[i] = (unsigned char)((i * 3) % 7 + 100);
    for (i = 0; i < 256; i++)
        map[i] = 0xffff;
    memset(&job, 0, sizeof(job));
    job.kind = XK_8;
    job.planes = planes;
    job.src = row;
    job.pitch = 64;
    job.sw = 64;
    job.sh = 1;
    job.dw = 64;
    job.dh = 1;
    job.count = 64;
    job.rows = 1;
    job.map = map;
    job.pen_index = (XferPenFn)test_pen_index;
    job.ctx = &planes;
    pen_index_calls = 0;
    xfer_begin(&st, &job);
    xfer_row_pix(&st, pix, NULL);
    check(pen_index_calls == 7, "an 8-bit source asks for each index once", (long)pen_index_calls, 7);
    for (i = 0; i < 64; i++)
        check(pix[i] == ((row[i] * 37 + 11) & 15), "lazy index pixel", i, pix[i]);
    for (i = 0; i < 256; i++)
        check(map[i] == 0xffff || (i >= 100 && i < 107), "no other index was asked", i, map[i]);
}

/* a repeated row of an enlargement is the one before it, also with 16-bit words; a transparent one is not */
static void test_repeat(void)
{
    unsigned char row[2 * 8 * 4];
    unsigned char d[2 * 16 * 6];
    XferJob job;
    XferState st;
    int i, y;

    for (i = 0; i < (int)sizeof(row); i++)
        row[i] = rnd();
    memset(&job, 0, sizeof(job));
    job.kind = XK_16;
    job.planes = XD_16;
    job.src = row;
    job.pitch = 16;
    job.sw = 8;
    job.sh = 2;
    job.dw = 16;
    job.dh = 6;
    job.count = 16;
    job.rows = 6;
    xfer_begin(&st, &job);
    for (y = 0; y < 6; y++)
        xfer_row16(&st, d + y * 32, y ? d + (y - 1) * 32 : NULL);
    for (y = 0; y < 6; y++)
        for (i = 0; i < 16; i++)
        {
            const unsigned char *p = row + (y * 2 / 6) * 16 + (i * 8 / 16) * 2;

            check(d[y * 32 + i * 2] == p[0] && d[y * 32 + i * 2 + 1] == p[1], "enlarged row", y, i);
        }
}

/* ------------------------------------------------------------------ */
/* the packer on its own: only the pixels named are written */

static void test_packer(void)
{
    static const int planes_tab[4] = { 1, 2, 4, 8 };
    int pi;
    long n;

    for (pi = 0; pi < 4; pi++)
    {
        int planes = planes_tab[pi];

        for (n = 0; n < 3000; n++)
        {
            long x0 = rnd() % 40, count = rnd() % 70 + 1;
            long words = (x0 + count + 15) / 16 + 1;
            long size = words * 2 * planes;
            unsigned char line[8 * 2 * 8], expected[8 * 2 * 8], pix[80], mask[80];
            int use_mask = rnd() & 1;
            long i;
            int p;

            for (i = 0; i < size; i++)
                line[i] = expected[i] = rnd();
            for (i = 0; i < count; i++)
            {
                pix[i] = rnd() & ((1 << planes) - 1);
                mask[i] = rnd() & 1;
            }
            for (i = 0; i < count; i++)
            {
                long col = x0 + i;

                if (use_mask && !mask[i])
                    continue;
                for (p = 0; p < planes; p++)
                {
                    unsigned char *w = expected + (col >> 4) * 2 * planes + 2 * p + ((col & 15) >> 3);
                    int m = 0x80 >> (col & 7);

                    if ((pix[i] >> p) & 1)
                        *w |= m;
                    else
                        *w &= ~m;
                }
            }
            xfer_store_planar(line, planes, x0, count, pix, use_mask ? mask : NULL);
            check(memcmp(line, expected, size) == 0, "planar packer", planes, n);
        }
    }
}

/* ------------------------------------------------------------------ */
/* refusals */

static GCBITMAP mkbm(void *addr, long width, long bits, unsigned long fmt, long w, long h)
{
    GCBITMAP b;

    memset(&b, 0, sizeof(b));
    b.addr = addr;
    b.width = width;
    b.bits = bits;
    b.px_format = fmt;
    b.xmax = w;
    b.ymax = h;
    return b;
}

static void test_rejected(void)
{
    static unsigned long storage[1024];
    unsigned char *p = (unsigned char *)storage;
    GCBITMAP b;
    RECT16 r;

    /* the 32-bit source of 4 x 4 */
    b = mkbm(p, 16, 32, 0x03421820UL, 4, 4);
    check(xfer_src_size(&b, XK_32) == 64, "32-bit size", 0, 0);
    b.width = 14;
    check(xfer_src_size(&b, XK_32) == 0, "a line too short", 0, 0);
    b.width = 16;
    b.addr = p + 2;
    check(xfer_src_size(&b, XK_32) == 64, "32 bits on a word", 0, 0);
    b.addr = p + 1;
    check(xfer_src_size(&b, XK_32) == 0, "32 bits on an odd address", 0, 0);
    b.addr = p;
    b.width = 17;
    check(xfer_src_size(&b, XK_32) == 0, "32 bits on an odd line", 0, 0);
    b.width = 16;
    b.addr = NULL;
    check(xfer_src_size(&b, XK_32) == 0, "no address", 0, 0);
    b.addr = p;
    b.bits = 16;
    check(xfer_src_size(&b, XK_32) == 0, "bits do not say 32", 0, 0);
    b.bits = 32;
    b.xmax = 0;
    check(xfer_src_size(&b, XK_32) == 0, "an empty bitmap", 0, 0);
    b.xmax = 4;
    b.xmin = -32769L;
    check(xfer_src_size(&b, XK_32) == 0, "xmin too low", 0, 0);
    b.xmin = 0;
    b.xmax = 32769L;
    check(xfer_src_size(&b, XK_32) == 0, "xmax too high", 0, 0);
    b.xmax = 4;
    b.ymin = -32769L;
    check(xfer_src_size(&b, XK_32) == 0, "ymin too low", 0, 0);
    b.ymin = 0;
    b.ymax = 32769L;
    check(xfer_src_size(&b, XK_32) == 0, "ymax too high", 0, 0);
    b.ymax = 4;
    b.width = 0x7fffffffL & ~1L;
    check(xfer_src_size(&b, XK_32) == 0, "a size over 2^31", 0, 0);
    b.width = 16;
    b.xmin = -32768L;
    b.xmax = 32768L;
    b.width = 65536L * 4;
    check(xfer_src_size(&b, XK_32) == 65536UL * 4 * 4, "the widest bitmap", 0, 0);
    b.width = 65536L * 4 - 2;
    check(xfer_src_size(&b, XK_32) == 0, "the widest bitmap, a short line", 0, 0);

    /* the other sources */
    b = mkbm(p + 1, 3, 24, 0x03021818UL, 1, 4);
    check(xfer_src_size(&b, XK_24) == 12, "24 bits on any address", 0, 0);
    b.width = 2;
    check(xfer_src_size(&b, XK_24) == 0, "24 bits, a line of 2", 0, 0);
    b = mkbm(p + 1, 1, 8, 0x01020808UL, 1, 4);
    check(xfer_src_size(&b, XK_8) == 4, "8 bits on any address", 0, 0);
    b = mkbm(p + 1, 1, 1, 0x01020101UL, 8, 4);
    check(xfer_src_size(&b, XK_1) == 4, "1 bit, eight pixels in a byte", 0, 0);
    b.xmax = 9;
    check(xfer_src_size(&b, XK_1) == 0, "1 bit, nine pixels in a byte", 0, 0);
    b.width = 2;
    check(xfer_src_size(&b, XK_1) == 8, "1 bit, nine pixels in two", 0, 0);
    b = mkbm(p, 6, 16, 0x03021010UL, 3, 2);
    check(xfer_src_size(&b, XK_16 | XK_REV) == 12, "16 bits reversed", 0, 0);
    b.bits = 15;
    check(xfer_src_size(&b, XK_15) == 0, "15 bits is stored in 16", 0, 0);

    /* destinations */
    b = mkbm(p, 8, 16, 0x03021010UL, 4, 4);
    check(xfer_dst_size(&b, XD_16) == 32, "16-bit destination", 0, 0);
    b.addr = p + 1;
    check(xfer_dst_size(&b, XD_16) == 0, "16-bit destination on an odd address", 0, 0);
    b.addr = p;
    b.width = 7;
    check(xfer_dst_size(&b, XD_16) == 0, "16-bit destination, an odd line", 0, 0);
    b = mkbm(p, 8, 4, 0x01000404UL, 16, 4);
    check(xfer_dst_size(&b, 4) == 32, "4 planes, 16 pixels", 0, 0);
    b.width = 6;
    check(xfer_dst_size(&b, 4) == 0, "4 planes, a line of 6 bytes", 0, 0);
    b.width = 8;
    b.xmax = 17;
    check(xfer_dst_size(&b, 4) == 0, "4 planes, 17 pixels in 8 bytes", 0, 0);
    b.xmax = 16;
    check(xfer_dst_size(&b, 8) == 0, "bits are not 8", 0, 0);
    check(xfer_dst_size(&b, 3) == 0, "3 planes", 0, 0);
    check(xfer_dst_size(&b, 24) == 0, "24 planes", 0, 0);
    b = mkbm(p, 2, 1, 0x01020101UL, 16, 4);
    check(xfer_dst_size(&b, 1) == 8, "1 plane", 0, 0);

    /* rectangles */
    b = mkbm(p, 16, 32, 0x03421820UL, 4, 4);
    b.xmin = -2;
    b.xmax = 2;
    b.ymin = 10;
    b.ymax = 14;
    r.x1 = -2; r.y1 = 10; r.x2 = 1; r.y2 = 13;
    check(xfer_rect_in(&b, &r), "a rectangle inside", 0, 0);
    r.x2 = 2;
    check(!xfer_rect_in(&b, &r), "a rectangle past the right", 0, 0);
    r.x2 = 1; r.y2 = 14;
    check(!xfer_rect_in(&b, &r), "a rectangle past the bottom", 0, 0);
    r.y2 = 13; r.x1 = -3;
    check(!xfer_rect_in(&b, &r), "a rectangle past the left", 0, 0);
    r.x1 = -2; r.y1 = 9;
    check(!xfer_rect_in(&b, &r), "a rectangle past the top", 0, 0);
    r.y1 = 13; r.y2 = 12;
    check(!xfer_rect_in(&b, &r), "an upside-down rectangle", 0, 0);
    r.y1 = 10; r.y2 = 13; r.x1 = 1; r.x2 = 0;
    check(!xfer_rect_in(&b, &r), "a rectangle the wrong way round", 0, 0);

    check(xfer_overlap(p, 16, p + 8, 16), "overlap, the second inside the first", 0, 0);
    check(xfer_overlap(p + 8, 16, p, 16), "overlap, the first inside the second", 0, 0);
    check(!xfer_overlap(p, 16, p + 16, 16), "no overlap, adjacent", 0, 0);
    check(!xfer_overlap(p + 16, 16, p, 16), "no overlap, adjacent, the other way", 0, 0);
    check(xfer_overlap(p, 17, p + 16, 16), "overlap by one byte", 0, 0);
    check(xfer_overlap(p, 100, p, 1), "overlap, the same start", 0, 0);
}

int main(void)
{
    test_kinds();
    test_unpack();
    test_rejected();
    test_packer();
    test_lazy();
    test_repeat();
    test_wide();
    test_extreme();
    test_clipped();
    test_sweep();
    printf("%ld checks, %d failures\n", checks, failures);
    if (failures)
    {
        printf("test_xfer FAIL\n");
        return 1;
    }
    printf("test_xfer PASS\n");
    return 0;
}
