/*
 * Host checks of vdi/vdi_nvdimath.c, the arithmetic of the NVDI colour
 * calls of EmuTOS's VDI.  See README.txt.
 */

#include <stdio.h>
#include <string.h>

#include "portab.h"
#include "vdi_nvdimath.h"

/* the default palettes of vdi/vdi_col.c, cut out of it by the Makefile */
#include "palettes.h"

static int failures;
static long checks;

static void check(int ok, const char *what, long a, long b)
{
    checks++;
    if (!ok)
    {
        failures++;
        printf("FAIL: %s (%ld, %ld)\n", what, a, b);
    }
}

/* 0-1000 and 0-65535 */
static void test_scale(void)
{
    WORD v;
    UWORD prev = 0;

    for (v = 0; v <= 1000; v++)
    {
        UWORD c = vdi_to_rgb16(v);

        check(rgb16_to_vdi(c) == v, "0-1000 to 0-65535 and back", v, c);
        check(v == 0 || c > prev, "the scale rises", v, c);
        prev = c;
    }
    check(vdi_to_rgb16(0) == 0, "0 is 0", 0, 0);
    check(vdi_to_rgb16(1000) == 0xffff, "1000 is ffff", 0, 0);
    check(vdi_to_rgb16(500) == 0x8000, "500 is 8000", vdi_to_rgb16(500), 0);
    check(vdi_to_rgb16(-1) == 0, "below 0 is 0", 0, 0);
    check(vdi_to_rgb16(-32768) == 0, "far below 0 is 0", 0, 0);
    check(vdi_to_rgb16(1001) == 0xffff, "above 1000 is ffff", 0, 0);
    check(vdi_to_rgb16(32767) == 0xffff, "far above 1000 is ffff", 0, 0);
    check(rgb16_to_vdi(0) == 0 && rgb16_to_vdi(0xffff) == 1000, "ends of 0-65535", 0, 0);
}

/* the Falcon 16-bit pixel */
static void test_pixel(void)
{
    ULONG p, c;
    UWORD r, g, b, px;

    for (p = 0; p < 0x10000UL; p++)
    {
        if (p & 0x20)
            continue;
        vdi_unpack16((UWORD)p, &r, &g, &b);
        check(vdi_pack16(r, g, b) == p, "pack(unpack(v)) == v", (long)p, vdi_pack16(r, g, b));
    }

    /* each component alone, over its whole range: cut, within one step, bit 5 clear */
    for (c = 0; c < 0x10000UL; c++)
    {
        UWORD cc = (UWORD)c;
        UWORD r2, g2, b2;
        int ok;

        px = vdi_pack16(cc, 0, 0);
        vdi_unpack16(px, &r2, &g2, &b2);
        ok = g2 == 0 && b2 == 0 && (r2 > cc ? r2 - cc : cc - r2) <= 0x842;
        check(ok && !(px & 0x07ff), "red alone", (long)c, px);

        px = vdi_pack16(0, cc, 0);
        vdi_unpack16(px, &r2, &g2, &b2);
        ok = r2 == 0 && b2 == 0 && (g2 > cc ? g2 - cc : cc - g2) <= 0x842;
        check(ok && !(px & 0xf83f), "green alone", (long)c, px);

        px = vdi_pack16(0, 0, cc);
        vdi_unpack16(px, &r2, &g2, &b2);
        ok = r2 == 0 && g2 == 0 && (b2 > cc ? b2 - cc : cc - b2) <= 0x842;
        check(ok && !(px & 0xffe0), "blue alone", (long)c, px);
    }
    check(vdi_pack16(0xffff, 0, 0) == 0xf800, "pack(ffff,0,0) is f800", vdi_pack16(0xffff, 0, 0), 0);
    check(vdi_pack16(0, 0xffff, 0) == 0x07c0, "pack(0,ffff,0) is 07c0", vdi_pack16(0, 0xffff, 0), 0);
    check(vdi_pack16(0, 0, 0xffff) == 0x001f, "pack(0,0,ffff) is 001f", vdi_pack16(0, 0, 0xffff), 0);
    check(vdi_pack16(0xffff, 0xffff, 0xffff) == 0xffdf, "white is ffdf", vdi_pack16(0xffff, 0xffff, 0xffff), 0);
    check(vdi_pack16(0x07ff, 0x07ff, 0x07ff) == 0, "below the top five bits is cut", 0, 0);
    check(vdi_pack16(0x0800, 0x0800, 0x0800) == 0x0841, "the top five bits are kept", vdi_pack16(0x0800, 0x0800, 0x0800), 0);
    vdi_unpack16(0xffff, &r, &g, &b);
    check(r == 0xffff && g == 0xffff && b == 0xffff, "unpack(ffff) is white", r, g);
    vdi_unpack16(0, &r, &g, &b);
    check(r == 0 && g == 0 && b == 0, "unpack(0) is black", r, g);
}

/* the nearest entry of a table of colours */
static void test_nearest_table(const char *name, const WORD (*tab)[3], WORD count, int quantise_ste)
{
    WORD (*q)[3] = NULL;
    WORD i, j, found, firstsame;
    LONG best;
    char what[100];
    static WORD quant[256][3];

    if (quantise_ste)
    {
        for (i = 0; i < count; i++)
            for (j = 0; j < 3; j++)
            {
                WORD col = (tab[i][j] * 3 + 100) / 200;

                quant[i][j] = (col * 1000 + 7) / 15;
            }
        q = quant;
    }
    for (i = 0; i < count; i++)
    {
        const WORD *c = q ? q[i] : tab[i];

        best = VDI_DIST_FAR;
        found = vdi_nearest_index(q ? (const WORD (*)[3])q : tab, count, c, &best);
        /* its own nearest, or the lowest of the entries equal to it */
        for (firstsame = 0; firstsame < i; firstsame++)
        {
            const WORD *o = q ? q[firstsame] : tab[firstsame];

            if (o[0] == c[0] && o[1] == c[1] && o[2] == c[2])
                break;
        }
        snprintf(what, sizeof(what), "%s entry %d is its own nearest (or the lowest equal)", name, i);
        check(found == firstsame && best == 0, what, found, firstsame);
    }
}

static void test_nearest(void)
{
    static const WORD tie[3][3] = { { 0, 0, 0 }, { 2, 0, 0 }, { 2, 0, 0 } };
    static const WORD far1[1][3] = { { 1000, 1000, 1000 } };
    static const WORD wild[2][3] = { { -500, 3000, 20 }, { 1000, 1000, 1000 } };
    static WORD tt[256][3];
    static WORD vd[256][3];
    WORD query[3] = { 1, 0, 0 };
    WORD white[3] = { 1000, 1000, 1000 };
    LONG best = VDI_DIST_FAR;

    test_nearest_table("ST", st_palette, 16, 0);
    test_nearest_table("STE", st_palette, 16, 1);
    memcpy(tt, tt_palette1, sizeof(tt_palette1));
    memcpy(tt[16], tt_palette2, sizeof(tt_palette2));
    test_nearest_table("TT", (const WORD (*)[3])tt, 256, 0);
    memcpy(vd, videl_palette1, sizeof(videl_palette1));
    memcpy(vd[16], videl_palette2, sizeof(videl_palette2));
    test_nearest_table("Falcon", (const WORD (*)[3])vd, 256, 0);

    /* equal distances: the lowest entry */
    check(vdi_nearest_index(tie, 3, query, &best) == 0 && best == 1, "a tie picks the lowest entry", best, 0);
    /* in a second table, an equal distance keeps the first */
    check(vdi_nearest_index(tie + 1, 2, query, &best) == -1 && best == 1, "an equal distance in a later table keeps the earlier", best, 0);
    best = 0;
    check(vdi_nearest_index(tie, 3, query, &best) == -1, "nothing is nearer than 0", 0, 0);
    best = VDI_DIST_FAR;
    check(vdi_nearest_index(tie, 0, query, &best) == -1 && best == VDI_DIST_FAR, "an empty table finds nothing", 0, 0);
    /* a table of a later pen, nearer */
    best = VDI_DIST_FAR;
    vdi_nearest_index(tie, 2, white, &best);
    check(vdi_nearest_index(far1, 1, white, &best) == 0 && best == 0, "a nearer entry in a later table wins", best, 0);
    /* out of range requests are clamped before they are measured */
    best = VDI_DIST_FAR;
    check(vdi_nearest_index(wild, 2, white, &best) == 1 && best == 0, "components are clamped", best, 0);
    check(vdi_color_dist(wild[0], white) == 1000L * 1000L + 980L * 980L, "distance of a clamped colour", vdi_color_dist(wild[0], white), 0);
    check(vdi_color_dist(white, white) == 0, "distance to itself is 0", 0, 0);
    {
        WORD black[3] = { 0, 0, 0 };

        check(vdi_color_dist(black, white) == 3000000L, "black to white is 3000000", vdi_color_dist(black, white), 0);
    }
}

/*
 * vr_clip_rects: clip_pair, bring_along and muldiv32 against a reference
 * in 64-bit arithmetic (the reference only: EmuTOS has no 64-bit type)
 */
static long long ref_span(long long a1, long long a2)
{
    long long n = a2 - a1 + 1;

    return (a1 > a2 || n > 0x7fffffffLL) ? 0 : n;
}

static void ref_bring(long long a1, long long sa, long long n1, long long n2,
                      long long b1, long long sb, long long *m1, long long *m2)
{
    if (sa == sb)
    {
        *m1 = b1 + (n1 - a1);
        *m2 = b1 + (n2 - a1);
    }
    else
    {
        *m1 = b1 + ((n1 - a1) * sb) / sa;
        *m2 = b1 + ((n2 - a1 + 1) * sb - 1) / sa;
    }
}

/* the same as clip_pair, in 64 bits */
static int ref_clip(const VDI_RECT32 *c, const VDI_RECT32 *s, const VDI_RECT32 *o,
                    long long *oc, long long *oo)
{
    long long n[4];
    long long sa, sb, tx, ty;

    n[0] = s->x1 > c->x1 ? s->x1 : c->x1;
    n[1] = s->y1 > c->y1 ? s->y1 : c->y1;
    n[2] = s->x2 < c->x2 ? s->x2 : c->x2;
    n[3] = s->y2 < c->y2 ? s->y2 : c->y2;
    if (n[0] > n[2] || n[1] > n[3])
        return 0;
    sa = ref_span(s->x1, s->x2);
    sb = ref_span(o->x1, o->x2);
    tx = ref_span(s->y1, s->y2);
    ty = ref_span(o->y1, o->y2);
    if (!sa || !sb || !tx || !ty)
        return 0;
    oc[0] = n[0]; oc[1] = n[1]; oc[2] = n[2]; oc[3] = n[3];
    ref_bring(s->x1, sa, n[0], n[2], o->x1, sb, &oo[0], &oo[2]);
    ref_bring(s->y1, tx, n[1], n[3], o->y1, ty, &oo[1], &oo[3]);
    return 1;
}

static void rect(VDI_RECT32 *r, long x1, long y1, long x2, long y2)
{
    r->x1 = x1; r->y1 = y1; r->x2 = x2; r->y2 = y2;
}

static int is_rect(const VDI_RECT32 *r, long x1, long y1, long x2, long y2)
{
    return r->x1 == x1 && r->y1 == y1 && r->x2 == x2 && r->y2 == y2;
}

/* one pair against the reference, by_dst (side = dst) or by_src */
static void compare(const VDI_RECT32 *c, const VDI_RECT32 *s, const VDI_RECT32 *o, const char *what)
{
    VDI_RECT32 got_c, got_o;
    long long rc[4], ro[4];
    int rok = ref_clip(c, s, o, rc, ro);
    int ok = clip_pair(c, s, o, &got_c, &got_o);

    check(ok == rok, what, ok, rok);
    if (ok && rok)
        check(got_c.x1 == rc[0] && got_c.y1 == rc[1] && got_c.x2 == rc[2] && got_c.y2 == rc[3] &&
              got_o.x1 == ro[0] && got_o.y1 == ro[1] && got_o.x2 == ro[2] && got_o.y2 == ro[3],
              what, got_o.x1, (long)ro[0]);
}

static void test_clip_vectors(void)
{
    VDI_RECT32 clip, clipb, r100, r50, far_, o_s, o_d;

    rect(&clip, 10, 10, 50, 50);
    rect(&clipb, 10, 10, 59, 59);
    rect(&r100, 0, 0, 99, 99);
    rect(&r50, 0, 0, 49, 49);
    rect(&far_, 200, 200, 300, 300);

    /* by_dst: the clipped side is the destination */
    check(clip_pair(&clip, &r100, &r100, &o_d, &o_s) == 1 &&
          is_rect(&o_d, 10, 10, 50, 50) && is_rect(&o_s, 10, 10, 50, 50),
          "by_dst, same size: both clipped alike", 0, 0);
    check(clip_pair(&clipb, &r100, &r50, &o_d, &o_s) == 1 &&
          is_rect(&o_d, 10, 10, 59, 59) && is_rect(&o_s, 5, 5, 29, 29),
          "by_dst, half-size source: source clipped in proportion", o_s.x1, o_s.x2);
    /* by_src: the clipped side is the source */
    check(clip_pair(&clipb, &r100, &r50, &o_s, &o_d) == 1 &&
          is_rect(&o_s, 10, 10, 59, 59) && is_rect(&o_d, 5, 5, 29, 29),
          "by_src, half-size destination: destination clipped in proportion", o_d.x1, o_d.x2);
    check(clip_pair(&clip, &far_, &r100, &o_d, &o_s) == 0, "by_dst: nothing left is 0", 0, 0);
    /* 32 bits, beyond 16 */
    {
        VDI_RECT32 c, s, d;

        rect(&c, 100000L, 0, 150000L, 10);
        rect(&s, 0, 0, 199999L, 99);
        rect(&d, 0, 0, 199999L, 99);
        check(clip_pair(&c, &d, &s, &o_d, &o_s) == 1 &&
              is_rect(&o_d, 100000L, 0, 150000L, 10) && is_rect(&o_s, 100000L, 0, 150000L, 10),
              "32-bit by_dst beyond 16 bits", 0, 0);
        check(clip_pair(&c, &s, &d, &o_s, &o_d) == 1 &&
              is_rect(&o_s, 100000L, 0, 150000L, 10) && is_rect(&o_d, 100000L, 0, 150000L, 10),
              "32-bit by_src beyond 16 bits", 0, 0);
    }
    /* upside down rectangles and the empty clip are refused */
    {
        VDI_RECT32 bad;

        rect(&bad, 20, 0, 10, 5);
        check(clip_pair(&r100, &bad, &r50, &o_d, &o_s) == 0, "an upside down clipped side is refused", 0, 0);
        check(clip_pair(&r100, &r50, &bad, &o_d, &o_s) == 0, "an upside down other side is refused", 0, 0);
        check(clip_pair(&bad, &r50, &r50, &o_d, &o_s) == 0, "an upside down clip is refused", 0, 0);
    }
}

/* exhaustive: 16-bit rectangles of 1-24 pixels, offsets -8 to 32 */
static void test_clip_sweep(void)
{
    static const long clips[][2] = { { 0, 15 }, { 5, 5 }, { 10, 40 }, { -8, -1 }, { -20, 50 }, { 30, 31 } };
    static const long offs_b[] = { -8, 0, 13, 32 };
    long sa, sb, a1, k, ob;
    long before = checks;

    for (sa = 1; sa <= 24; sa++)
        for (sb = 1; sb <= 24; sb++)
            for (a1 = -8; a1 <= 32; a1++)
                for (ob = 0; ob < 4; ob++)
                    for (k = 0; k < 6; k++)
                    {
                        VDI_RECT32 c, s, o;
                        long b1 = offs_b[ob];

                        /* y: other offsets and sizes, to mix the axes */
                        rect(&c, clips[k][0], clips[5 - k][0], clips[k][1], clips[5 - k][1]);
                        rect(&s, a1, 24 - a1, a1 + sa - 1, 24 - a1 + sb - 1);
                        rect(&o, b1, -b1, b1 + sb - 1, -b1 + sa - 1);
                        compare(&c, &s, &o, "sweep by_dst/by_src, 16 bits");
                        compare(&c, &o, &s, "sweep, the other way round");
                    }
    check(checks - before > 100000L, "the sweep ran", checks - before, 0);
}

/* 32-bit values up to 0x7fffffff and sizes across 2^16 and 2^31 */
static void test_clip_wide(void)
{
    static const long sizes[] = { 1, 2, 3, 255, 256, 65535L, 65536L, 65537L, 0x7ffffffeL, 0x7fffffffL };
    static const long starts[] = { 0, 1, 65535L, 65536L, -65536L, 0x7fff0000L, -0x7fffffffL - 1 };
    int i, j, k, m;

    for (i = 0; i < 10; i++)
        for (j = 0; j < 10; j++)
            for (k = 0; k < 7; k++)
                for (m = 0; m < 7; m++)
                {
                    VDI_RECT32 c, s, o;
                    long long a2 = (long long)starts[k] + sizes[i] - 1;
                    long long b2 = (long long)starts[m] + sizes[j] - 1;
                    long cx1 = starts[k] / 2 + starts[m] / 4;
                    long cx2;

                    if (a2 > 0x7fffffffLL || b2 > 0x7fffffffLL)
                        continue;
                    cx2 = (long)(a2 / 2 + b2 / 4);
                    rect(&s, starts[k], 0, (long)a2, 7);
                    rect(&o, starts[m], 3, (long)b2, 3 + 100000L);
                    rect(&c, cx1, 1, cx2, 5);
                    compare(&c, &s, &o, "wide, by_dst");
                    compare(&c, &o, &s, "wide, by_src");
                    rect(&c, 0x7ffffffeL - 9, 0, 0x7fffffffL, 9);
                    compare(&c, &s, &o, "wide, clip at the top");
                    rect(&c, -0x7fffffffL - 1, 0, -0x7fffffffL + 8, 9);
                    compare(&c, &s, &o, "wide, clip at the bottom");
                }
    /* the whole of 32 bits is more than a size can say: refused */
    {
        VDI_RECT32 c, s, o, oc, oo;

        rect(&c, 0, 0, 10, 10);
        rect(&s, -0x7fffffffL - 1, 0, 0x7fffffffL, 10);
        rect(&o, 0, 0, 10, 10);
        check(clip_pair(&c, &s, &o, &oc, &oo) == 0, "a side of 2^32 pixels is refused", 0, 0);
        rect(&s, -1, 0, 0x7ffffffeL, 10);    /* 0x7fffffff + 1 pixels */
        check(clip_pair(&c, &s, &o, &oc, &oo) == 0, "a side of 2^31 pixels is refused", 0, 0);
        rect(&s, -1, 0, 0x7ffffffdL, 10);    /* 0x7fffffff pixels: the most */
        check(clip_pair(&c, &s, &o, &oc, &oo) == 1, "a side of 2^31 - 1 pixels is taken", 0, 0);
    }
}

static ULONG rnd_state = 12345UL;

static ULONG rnd(void)
{
    rnd_state ^= rnd_state << 13;
    rnd_state ^= rnd_state >> 17;
    rnd_state ^= rnd_state << 5;
    return rnd_state;
}

static void test_muldiv(void)
{
    long i;
    long bad = 0;

    check(muldiv32(0, 5, 3) == 0, "0 * 5 / 3", 0, 0);
    check(muldiv32(7, 3, 2) == 10, "7 * 3 / 2", 0, 0);
    check(muldiv32(0x7fffffffL, 0x7fffffffL, 0x7fffffffL) == 0x7fffffffL, "max * max / max", 0, 0);
    check(muldiv32(0x7fffffffL, 0x7fffffffL, 1) == 0x7fffffffL, "a quotient over a LONG is capped", 0, 0);
    check(muldiv32(65536L, 65536L, 2) == 0x7fffffffL, "2^31 is capped", 0, 0);
    check(muldiv32(65536L, 65535L, 2) == 0x7fff8000L, "just under 2^31", muldiv32(65536L, 65535L, 2), 0);
    check(muldiv32(-1, 1, 1) == 0 && muldiv32(1, 0, 1) == 0 && muldiv32(1, 1, 0) == 0 &&
          muldiv32(1, -1, 1) == 0 && muldiv32(1, 1, -1) == 0, "operands out of range give 0", 0, 0);
    for (i = 0; i < 1000000L; i++)
    {
        ULONG ra = rnd(), rb = rnd(), rc = rnd();
        long a = (long)((ra >> (rnd() & 31)) & 0x7fffffffUL);
        long b = (long)((rb >> (rnd() & 31)) & 0x7fffffffUL);
        long c = (long)((rc >> (rnd() & 31)) & 0x7fffffffUL);
        long long q;

        if (b == 0)
            b = 1;
        if (c == 0)
            c = 1;
        q = (long long)a * b / c;
        if (q > 0x7fffffffLL)
            q = 0x7fffffffLL;
        if (muldiv32(a, b, c) != q)
        {
            if (bad++ < 5)
                printf("muldiv32(%ld, %ld, %ld) = %ld, not %lld\n", a, b, c, muldiv32(a, b, c), q);
        }
    }
    check(bad == 0, "muldiv32 against 64 bits, 10^6 triples", bad, 0);
}

int main(void)
{
    test_scale();
    test_pixel();
    test_nearest();
    test_clip_vectors();
    test_clip_sweep();
    test_clip_wide();
    test_muldiv();
    printf("%ld checks, %d failures\n", checks, failures);
    if (failures)
    {
        printf("test_nvdimath FAIL\n");
        return 1;
    }
    printf("test_nvdimath PASS\n");
    return 0;
}
