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

int main(void)
{
    test_scale();
    test_pixel();
    test_nearest();
    printf("%ld checks, %d failures\n", checks, failures);
    if (failures)
    {
        printf("test_nvdimath FAIL\n");
        return 1;
    }
    printf("test_nvdimath PASS\n");
    return 0;
}
