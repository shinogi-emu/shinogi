/*
 * Host checks of vdi/vdi_textaa.c, the blending of anti-aliased outline
 * text into the 16-bit pixels of EmuTOS's VDI.  See README.txt.
 *
 * The reference is the rule the file documents, worked out here on its
 * own: a coverage c of 0-255 is step (c + 4) / 8 of 32, and each 5-bit
 * component goes from d to d + (f - d) * step / 32, rounded half up (floor
 * of the exact value plus a half), in floating point.  The pixels are
 * rrrrrggggg0bbbbb, the only direct-colour format the VDI draws in.
 *
 * The buffers are allocated to the exact size, with a frame of known
 * pixels around the area drawn, so that ASan sees any access past them
 * and the frame shows any write outside the area.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "portab.h"
#include "vdi_textaa.h"

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

/* a pixel the VDI makes: bit 5 clear */
static unsigned vdi_pixel(void)
{
    return rnd() & 0xffdf;
}

/* ------------------------------------------------------------------ */
/* the reference */

static unsigned ref_level(unsigned c)
{
    unsigned l = (unsigned)floor((c + 4) / 8.0);

    return l > 32 ? 32 : l;
}

static unsigned ref_comp(unsigned d, unsigned f, unsigned level)
{
    double v = d + ((double)f - d) * level / 32.0;

    return (unsigned)floor(v + 0.5);
}

static unsigned ref_mix(unsigned d, unsigned f, unsigned level)
{
    if (level == 0)
        return d;
    if (level >= 32)
        return f;
    return (ref_comp(d >> 11, f >> 11, level) << 11)
         | (ref_comp((d >> 6) & 31, (f >> 6) & 31, level) << 6)
         | ref_comp(d & 31, f & 31, level);
}

/* ------------------------------------------------------------------ */

static void check_levels(void)
{
    unsigned c, last = 0;

    for (c = 0; c < 256; c++)
    {
        unsigned l = VDI_AA_LEVEL(c);

        check(l == ref_level(c), "coverage to step", c, l);
        check(l >= last && l <= VDI_AA_LEVELS, "steps rise to 32", c, l);
        last = l;
    }
    check(VDI_AA_LEVEL(0) == 0 && VDI_AA_LEVEL(3) == 0, "0-3 leave a pixel", VDI_AA_LEVEL(3), 0);
    check(VDI_AA_LEVEL(255) == VDI_AA_LEVELS && VDI_AA_LEVEL(252) == VDI_AA_LEVELS,
          "252-255 are the text colour", VDI_AA_LEVEL(252), 0);
    check(VDI_AA_LEVEL(128) == 16, "half coverage is half way", VDI_AA_LEVEL(128), 0);
}

/* every pair of 5-bit components at every step, in each position */
static void check_mix(void)
{
    unsigned d, f, l, shift, i;
    static const unsigned shifts[3] = { 11, 6, 0 };

    for (i = 0; i < 3; i++)
    {
        shift = shifts[i];
        for (d = 0; d < 32; d++)
            for (f = 0; f < 32; f++)
            {
                unsigned prev = 0;

                for (l = 0; l <= 32; l++)
                {
                    unsigned other = vdi_pixel() & ~(31u << shift);
                    unsigned dp = other | (d << shift), fp = other | (f << shift);
                    unsigned r = vdi_aa_mix16((UWORD)dp, (UWORD)fp, (UWORD)l);
                    unsigned c = (r >> shift) & 31;
                    unsigned lo = d < f ? d : f, hi = d < f ? f : d;

                    check(r == ref_mix(dp, fp, l), "mix of one component", (long)(d * 32 + f), l);
                    check(c >= lo && c <= hi, "a blend lies between its colours", c, l);
                    /* the other components are equal in both, so they stay */
                    check((r & ~(31u << shift) & ~0x20u) == (other & ~0x20u), "other components kept", r, l);
                    if (l > 0)
                        check(d <= f ? c >= prev : c <= prev, "blends move towards the text colour", c, l);
                    prev = c;
                }
            }
    }

    /* whole pixels, at random, and the ends */
    for (i = 0; i < 2000000; i++)
    {
        unsigned dp = rnd() & 0xffff, fp = vdi_pixel(), lv = rnd() % 33;
        unsigned r = vdi_aa_mix16((UWORD)dp, (UWORD)fp, (UWORD)lv);

        check(r == ref_mix(dp, fp, lv), "mix of a pixel", (long)dp, (long)(fp * 64 + lv));
        if (lv > 0 && lv < 32)
            check(!(r & 0x20), "bit 5 of a blend is clear", (long)r, lv);
    }
    check(vdi_aa_mix16(0xffff, 0, 0) == 0xffff, "step 0 is the pixel, bit 5 too", 0, 0);
    check(vdi_aa_mix16(0x1234, 0xffdf, 32) == 0xffdf, "step 32 is the text colour", 0, 0);
    check(vdi_aa_mix16(0xffdf, 0x0000, 16) == ((16u << 11) | (16u << 6) | 16u),
          "white half way to black is mid grey", vdi_aa_mix16(0xffdf, 0, 16), 0);
}

/*
 * an area of w x h pixels inside a frame of m pixels, in rows of pitch
 * pixels (pitch >= w + 2m), every pixel set by fill
 */
typedef struct {
    UWORD *buf;
    long w, h, m, pitch;
} Area;

static void area_new(Area *a, long w, long h, long m, long extra)
{
    long n;

    a->w = w; a->h = h; a->m = m;
    a->pitch = w + 2 * m + extra;
    n = a->pitch * (h + 2 * m);
    a->buf = malloc(n * sizeof(UWORD));
    while (n-- > 0)
        a->buf[n] = (UWORD)vdi_pixel();
}

static UWORD *area_at(const Area *a, long x, long y)
{
    return a->buf + (y + a->m) * a->pitch + (x + a->m);
}

/* coverage mostly blank, as text is */
static int sparse;

/* blend through the function under test, into a copy, and compare pixel by pixel */
static void check_blend_case(long w, long h, long m, long extra, long cov_extra,
                             UWORD fg, UWORD bg, int mode, int kind)
{
    Area a;
    UWORD *before;
    UBYTE *cov;
    long cpitch = w + cov_extra, x, y, n, bad = 0, outside = 0;
    long total;

    area_new(&a, w, h, m, extra);
    total = a.pitch * (h + 2 * m);
    /* the background under the text: plain, two colours, or anything */
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
        {
            if (kind == 0)
                *area_at(&a, x, y) = bg;
            else if (kind == 1)
                *area_at(&a, x, y) = ((x + y) & 1) ? 0x1234 : bg;
            else if (kind == 2)
                *area_at(&a, x, y) = (x & 4) ? fg : bg;
        }
    before = malloc(total * sizeof(UWORD));
    memcpy(before, a.buf, total * sizeof(UWORD));

    /* one byte more, so that the area starts at any alignment */
    cov = malloc(cpitch * h + 1);
    for (n = 0; n < cpitch * h + 1; n++)
    {
        unsigned r = rnd() % 8;

        /* plenty of the ends and of their neighbours; or mostly blank, as text is */
        if (sparse)
            cov[n] = (UBYTE)((rnd() % 10 < 8) ? ((rnd() & 3) ? 0 : 1 + rnd() % 3) : rnd());
        else
            cov[n] = (UBYTE)(r == 0 ? 0 : r == 1 ? 255 : r == 2 ? 3 + (rnd() & 1) : r == 3 ? 251 + (rnd() & 1) : rnd());
    }
    cov += (w + h) & 1;

    vdi_aa_blend16(area_at(&a, 0, 0), a.pitch * 2, cov, cpitch, w, h, fg, bg, (WORD)mode);

    for (y = -m; y < h + m; y++)
        for (x = -m; x < a.pitch - m; x++)
        {
            UWORD got = *area_at(&a, x, y);
            UWORD was = before[(y + m) * a.pitch + (x + m)];

            if (x < 0 || y < 0 || x >= w || y >= h)
            {
                if (got != was)
                    outside++;
            }
            else
            {
                unsigned l = ref_level(cov[y * cpitch + x]);
                unsigned want = (mode == VDI_AA_REPLACE) ? ref_mix(bg, fg, l) : ref_mix(was, fg, l);

                if (got != want)
                    bad++;
            }
        }
    check(bad == 0, "blended pixels as the reference", bad, w * 1000 + h);
    check(outside == 0, "nothing outside the area changes", outside, w * 1000 + h);

    free(cov - ((w + h) & 1));
    free(before);
    free(a.buf);
}

static void check_blend_set(void)
{
    static const UWORD colours[] = {
        0x0000, 0xffdf, 0xf800, 0x07c0, 0x001f, 0x8410, 0x4208, 0x7bcf
    };
    long w, h;
    int fi, bi, mode, kind;

    /* sizes from one pixel, odd and even widths, with and without padding */
    for (w = 1; w <= 19; w += 3)
        for (h = 1; h <= 7; h += 3)
            for (mode = 0; mode < 2; mode++)
                for (kind = 0; kind < 4; kind++)
                    check_blend_case(w, h, 3, (w & 1), (h & 1) * 3, colours[(w + h) % 8],
                                     colours[(w * 3 + kind) % 8], mode, kind);

    /* every pair of the basic colours, both modes, plain and busy backgrounds */
    for (fi = 0; fi < 8; fi++)
        for (bi = 0; bi < 8; bi++)
            for (mode = 0; mode < 2; mode++)
                for (kind = 0; kind < 4; kind++)
                    check_blend_case(24, 9, 2, 1, 2, colours[fi], colours[bi], mode, kind);

    /* random colours, larger areas */
    for (fi = 0; fi < 200; fi++)
        check_blend_case(1 + rnd() % 80, 1 + rnd() % 40, 1 + rnd() % 4, rnd() % 5, rnd() % 7,
                         (UWORD)vdi_pixel(), (UWORD)vdi_pixel(), (int)(rnd() & 1), (int)(rnd() % 4));
}

static void check_blend(void)
{
    /* coverage of every kind, then mostly blank as text is */
    for (sparse = 0; sparse < 2; sparse++)
        check_blend_set();

    /* nothing to blend */
    check_blend_case(0, 5, 2, 0, 0, 0x0000, 0xffdf, VDI_AA_TRANS, 0);
    check_blend_case(5, 0, 2, 0, 0, 0x0000, 0xffdf, VDI_AA_REPLACE, 0);
}

/*
 * The VDI clips a bitmap by starting it at sx,sy and drawing a part of it:
 * a part cut at any side must give what the whole gives there.
 */
static void check_windows(void)
{
    enum { W = 23, H = 11, CP = W + 3 };
    UBYTE cov[CP * H];
    UWORD whole[H][W], part[H][W];
    long n, x0, y0, x1, y1, x, y, bad = 0;
    int mode;

    for (mode = 0; mode < 4; mode++)
    {
        /* dense, then mostly blank as text is */
        for (n = 0; n < CP * H; n++)
            cov[n] = (UBYTE)((mode < 2) ? ((rnd() % 4) ? rnd() : (rnd() & 1) * 255)
                                        : ((rnd() % 8) ? 0 : rnd()));
        for (y = 0; y < H; y++)
            for (x = 0; x < W; x++)
                whole[y][x] = (UWORD)((x * 977 + y * 131) & 0xffdf);
        vdi_aa_blend16(&whole[0][0], W * 2, cov, CP, W, H, 0x0000, 0xffdf, (WORD)(mode & 1));

        for (x0 = 0; x0 < W; x0++)
            for (x1 = x0 + 1; x1 <= W; x1 += 3)
                for (y0 = 0; y0 < H; y0++)
                    for (y1 = y0 + 1; y1 <= H; y1 += 2)
                    {
                        for (y = 0; y < H; y++)
                            for (x = 0; x < W; x++)
                                part[y][x] = (UWORD)((x * 977 + y * 131) & 0xffdf);
                        vdi_aa_blend16(&part[y0][x0], W * 2, cov + y0 * CP + x0, CP,
                                       x1 - x0, y1 - y0, 0x0000, 0xffdf, (WORD)(mode & 1));
                        for (y = 0; y < H; y++)
                            for (x = 0; x < W; x++)
                            {
                                UWORD want = (x >= x0 && x < x1 && y >= y0 && y < y1) ?
                                    whole[y][x] : (UWORD)((x * 977 + y * 131) & 0xffdf);

                                if (part[y][x] != want)
                                    bad++;
                            }
                    }
    }
    check(bad == 0, "a window cut at any side blends as the whole", bad, 0);
}

/* ------------------------------------------------------------------ */

static void check_turn_case(long w, long h, long sextra, long dextra)
{
    static const WORD angles[3] = { 900, 1800, 2700 };
    long spitch = w + sextra, x, y, i;
    UBYTE *src = malloc(spitch * h), *back;

    for (i = 0; i < spitch * h; i++)
        src[i] = (UBYTE)rnd();

    for (i = 0; i < 3; i++)
    {
        WORD a = angles[i];
        long nw = (a == 1800) ? w : h, nh = (a == 1800) ? h : w;
        long dpitch = nw + dextra, bad = 0;
        UBYTE *dst = malloc(dpitch * nh);

        memset(dst, 0xee, dpitch * nh);
        vdi_aa_turn(src, spitch, w, h, a, dst, dpitch);
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++)
            {
                long tx, ty;

                if (a == 900)
                    tx = y, ty = w - 1 - x;
                else if (a == 1800)
                    tx = w - 1 - x, ty = h - 1 - y;
                else
                    tx = h - 1 - y, ty = x;
                if (dst[ty * dpitch + tx] != src[y * spitch + x])
                    bad++;
            }
        for (y = 0; y < nh; y++)
            for (x = nw; x < dpitch; x++)
                if (dst[y * dpitch + x] != 0xee)
                    bad++;
        check(bad == 0, "turned as one-bit text is turned", bad, w * 10000 + h * 10 + i);
        free(dst);
    }

    /* four quarter turns, and a quarter and three quarters, give it back */
    {
        long cw = w, ch = h, k, bad = 0;
        UBYTE *cur = malloc(w * h), *next;

        for (y = 0; y < h; y++)
            memcpy(cur + y * w, src + y * spitch, w);
        for (k = 0; k < 4; k++)
        {
            next = malloc(cw * ch);
            vdi_aa_turn(cur, cw, cw, ch, 900, next, ch);
            free(cur);
            cur = next;
            x = cw; cw = ch; ch = x;
        }
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++)
                if (cur[y * w + x] != src[y * spitch + x])
                    bad++;
        back = malloc(h * w);
        next = malloc(w * h);
        vdi_aa_turn(src, spitch, w, h, 900, back, h);
        vdi_aa_turn(back, h, h, w, 2700, next, w);
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++)
                if (next[y * w + x] != src[y * spitch + x])
                    bad++;
        check(bad == 0, "turned round comes back", bad, w * 1000 + h);
        free(back);
        free(next);
        free(cur);
    }
    free(src);
}

static void check_turn(void)
{
    long w, h;

    for (w = 1; w <= 17; w += 2)
        for (h = 1; h <= 13; h += 3)
            check_turn_case(w, h, w & 3, h & 1);
    check_turn_case(300, 40, 0, 0);
    check_turn_case(1, 1, 0, 0);
}

int main(void)
{
    check_levels();
    check_mix();
    check_blend();
    check_windows();
    check_turn();

    printf("test_textaa: %ld checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
