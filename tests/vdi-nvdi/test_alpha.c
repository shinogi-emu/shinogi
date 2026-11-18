/*
 * Host checks of vdi/vdi_alpha.c, the blending of pixels with straight alpha
 * into the 16-bit pixels of EmuTOS's VDI (vr_transfer_bits, mode T_BLEND).
 * See README.txt.
 *
 * The reference is the arithmetic worked out here on its own, in 64-bit
 * integers, from the formulas of the header alpha_vectors.h; it shares no
 * code with the file under test.  The header's vectors are checked too, after
 * its checksum.
 *
 * The buffers are allocated to the exact size, with a frame of known
 * pixels around the area drawn, so that ASan sees any access past them
 * and the frame shows any write outside the area.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "portab.h"
#include "vdi_alpha.h"
#include "vdi_xfercore.h"
#include "alpha_vectors.h"

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

static uint32_t rng_state = 977;

static uint32_t rnd(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state >> 8;
}

/* ------------------------------------------------------------------ */
/* the reference */

static int64_t ref_wprime(int64_t w)
{
    return w + (w >> 15);
}

static int64_t ref_k(int64_t a, int64_t wc)
{
    int64_t wa = a + (a >> 7);

    return (wa * ref_wprime(wc) + 32768) >> 16;
}

/* one field of n bits holding d, the 8-bit component c, the weight k of 0-256 */
static int64_t ref_out(int64_t d, int64_t c, int64_t k, int n)
{
    int64_t s = c >> (8 - n);

    return (d * 256 + (s - d) * k + 128) >> 8;
}

/* a 565 pixel d with the colour rgb mixed in by k[3] */
static unsigned ref_mix(unsigned d, uint32_t rgb, const int k[3])
{
    unsigned r = (unsigned)ref_out((d >> 11) & 31, (rgb >> 16) & 255, k[0], 5);
    unsigned g = (unsigned)ref_out((d >> 5) & 63, (rgb >> 8) & 255, k[1], 6);
    unsigned b = (unsigned)ref_out(d & 31, rgb & 255, k[2], 5);

    return (r << 11) | (g << 5) | b;
}

/* the pixel as a transfer writes it: alpha 0 leaves d, any other alpha writes */
static unsigned ref_pixel(unsigned d, uint32_t argb, const unsigned w[3])
{
    int k[3];
    int a = (int)(argb >> 24);
    int c;

    if (a == 0)
        return d;
    for (c = 0; c < 3; c++)
        k[c] = (int)ref_k(a, w[c]);
    return ref_mix(d, argb & 0xffffff, k);
}

/* ------------------------------------------------------------------ */
/* the checksum of the vectors */

static uint32_t crc;

static void crc_byte(unsigned b)
{
    int i;

    crc ^= b & 255;
    for (i = 0; i < 8; i++)
        crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320UL : crc >> 1;
}

static void crc_word(long v)
{
    uint32_t u = (uint32_t)(int32_t)v;

    crc_byte(u >> 24);
    crc_byte(u >> 16);
    crc_byte(u >> 8);
    crc_byte(u);
}

static void check_vectors_sum(void)
{
    unsigned i, j, k;
    int t;

    crc = 0xffffffffUL;
    for (i = 0; i < ALPHA_NWEIGHTS; i++)
        for (j = 0; j < 3; j++)
            crc_word(alpha_weight[i][j]);
    for (i = 0; i < ALPHA_NWEIGHTS; i++)
        for (j = 0; j < 256; j++)
            for (k = 0; k < 3; k++)
                crc_word(alpha_k[i][j][k]);
    for (i = 0; i < ALPHA_NPIXELS; i++)
    {
        crc_word((long)alpha_pixel[i].argb);
        crc_word(alpha_pixel[i].d565);
        crc_word(alpha_pixel[i].e565);
        crc_word(alpha_pixel[i].d555);
        crc_word(alpha_pixel[i].e555);
        crc_word(alpha_pixel[i].weight);
    }
    for (t = 0; t < 4; t++)
    {
        crc_word(alpha_src[t].w);
        crc_word(alpha_src[t].h);
        for (j = 0; j < (unsigned)(alpha_src[t].w * alpha_src[t].h); j++)
            crc_word((long)alpha_src[t].argb[j]);
    }
    for (t = 0; t < 3; t++)
        for (j = 0; j < ALPHA_IMG_H; j++)
            for (k = 0; k < ALPHA_IMG_W; k++)
                crc_word(alpha_bg565[t][j][k]);
    for (t = 0; t < 3; t++)
        for (j = 0; j < ALPHA_IMG_H; j++)
            for (k = 0; k < ALPHA_IMG_W; k++)
                crc_word(alpha_bg555[t][j][k]);
    for (t = 0; t < ALPHA_NXFER; t++)
    {
        crc_word(alpha_xfer[t].src);
        crc_word(alpha_xfer[t].bg);
        crc_word(alpha_xfer[t].weight);
        crc_word(alpha_xfer[t].clip);
        for (j = 0; j < 4; j++)
            crc_word(alpha_xfer[t].sr[j]);
        for (j = 0; j < 4; j++)
            crc_word(alpha_xfer[t].dr[j]);
        for (j = 0; j < 4; j++)
            crc_word(alpha_xfer[t].cr[j]);
        crc_word((long)alpha_xfer[t].crc565);
        crc_word((long)alpha_xfer[t].crc555);
    }
    for (j = 0; j < 5; j++)
        for (k = 0; k < 7; k++)
            crc_word(alpha_ramp565[j][k]);
    for (t = 0; t < ALPHA_NREFUSAL; t++)
    {
        crc_word((long)alpha_refusal[t].px_format);
        crc_word(alpha_refusal[t].mode);
        crc_word(alpha_refusal[t].dest);
    }
    crc ^= 0xffffffffUL;
    check(crc == ALPHA_VECTORS_SUM, "vectors checksum", (long)crc, (long)ALPHA_VECTORS_SUM);
}

/* ------------------------------------------------------------------ */

static void check_k(void)
{
    unsigned w, a, c;
    long i, bad = 0;

    for (w = 0; w < ALPHA_NWEIGHTS; w++)
        for (a = 0; a < 256; a++)
            for (c = 0; c < 3; c++)
                check(vdi_alpha_k((UWORD)a, vdi_alpha_weight(alpha_weight[w][c])) == alpha_k[w][a][c],
                      "k against the vectors", w * 256 + a, c);
    /* the weight: 0-65535 to 0-65536 */
    check(vdi_alpha_weight(0) == 0, "weight 0", 0, 0);
    check(vdi_alpha_weight(0x7fff) == 0x7fff, "weight 7fff", 0, 0);
    check(vdi_alpha_weight(0x8000) == 0x8001, "weight 8000", 0, 0);
    check(vdi_alpha_weight(0xffff) == VDI_ALPHA_FULL, "weight ffff", 0, 0);
    for (i = 0; i < 65536; i++)
        if (vdi_alpha_weight((UWORD)i) != (ULONG)ref_wprime(i))
            bad++;
    check(bad == 0, "weight, every value", bad, 0);
    bad = 0;
    for (i = 0; i < 1000000L; i++)
    {
        uint32_t r = rnd();
        unsigned aa = r & 255;
        unsigned ww = (rnd() & 0xffff);

        if (i & 1)
            ww = (i & 2) ? 0xffff : (ww & 0xff);
        if (vdi_alpha_k((UWORD)aa, vdi_alpha_weight((UWORD)ww)) != (UWORD)ref_k(aa, ww))
            bad++;
    }
    check(bad == 0, "k, random", bad, 0);
}

/* every d, C and k in one field, the other two with k = 0 */
static void check_channel(void)
{
    int ch, n, d, c, k;
    long bad = 0, bad_other = 0;

    for (ch = 0; ch < 3; ch++)
    {
        int sh = ch == 0 ? 11 : ch == 1 ? 5 : 0;

        n = ch == 1 ? 6 : 5;
        for (d = 0; d < (1 << n); d++)
            for (c = 0; c < 256; c++)
            {
                uint32_t rgb;
                unsigned other;
                UWORD kk[3];
                unsigned in;

                /* the other fields: random, and the colour's other components random */
                other = rnd() & 0xffff;
                other &= ~((unsigned)((1 << n) - 1) << sh);
                in = other | ((unsigned)d << sh);
                rgb = rnd() & 0xffffff;
                rgb &= ~(0xffUL << (ch == 0 ? 16 : ch == 1 ? 8 : 0));
                rgb |= (uint32_t)c << (ch == 0 ? 16 : ch == 1 ? 8 : 0);
                for (k = 0; k <= 256; k++)
                {
                    unsigned out, fld, expect;

                    kk[0] = kk[1] = kk[2] = 0;
                    kk[ch] = (UWORD)k;
                    out = vdi_alpha_mix565((UWORD)in, rgb, kk[0], kk[1], kk[2]);
                    fld = (out >> sh) & ((1u << n) - 1);
                    expect = (unsigned)ref_out(d, c, k, n);
                    if (fld != expect)
                        bad++;
                    if ((out & ~((((1u << n) - 1)) << sh)) != other)
                        bad_other++;
                    checks += 2;
                }
            }
    }
    if (bad)
        failures++, printf("FAIL: channel mix, %ld wrong fields\n", bad);
    if (bad_other)
        failures++, printf("FAIL: channel mix, %ld changed other fields\n", bad_other);
}

static uint32_t rotate_rgba(uint32_t argb)
{
    /* the bytes A R G B as R G B A */
    return (argb << 8) | (argb >> 24);
}

static void put_be32(UBYTE *p, uint32_t v)
{
    p[0] = (UBYTE)(v >> 24);
    p[1] = (UBYTE)(v >> 16);
    p[2] = (UBYTE)(v >> 8);
    p[3] = (UBYTE)v;
}

static unsigned get16(const UBYTE *p)
{
    return ((unsigned)p[0] << 8) | p[1];
}

static void put16(UBYTE *p, unsigned v)
{
    p[0] = (UBYTE)(v >> 8);
    p[1] = (UBYTE)v;
}

static unsigned mix_k(unsigned d, uint32_t argb, const unsigned w[3])
{
    UWORD k[3];
    int c;

    for (c = 0; c < 3; c++)
        k[c] = vdi_alpha_k((UWORD)(argb >> 24), vdi_alpha_weight((UWORD)w[c]));
    return vdi_alpha_mix565((UWORD)d, argb & 0xffffff, k[0], k[1], k[2]);
}

/* one pixel through the row function, as a row of one */
static unsigned row_one(unsigned d, uint32_t argb, int rgba, const unsigned w[3])
{
    ULONG wp[3];
    UBYTE dst[2];
    UBYTE src[4];
    int c;

    for (c = 0; c < 3; c++)
        wp[c] = vdi_alpha_weight((UWORD)w[c]);
    put16(dst, d);
    put_be32(src, rgba ? rotate_rgba(argb) : argb);
    vdi_alpha_row565(dst, src, 1, rgba ? FALSE : TRUE, wp);
    return get16(dst);
}

static void check_pixels(void)
{
    static const unsigned full[3] = {0xffff, 0xffff, 0xffff};
    long i, bad = 0, bad_row = 0;

    for (i = 0; i < ALPHA_NPIXELS; i++)
    {
        const AlphaPixel *p = &alpha_pixel[i];
        unsigned wt[3];
        unsigned e;
        int c;

        for (c = 0; c < 3; c++)
            wt[c] = alpha_weight[p->weight][c];
        e = mix_k(p->d565, p->argb, wt);
        if ((p->argb >> 24) == 0)
            check(e == p->d565, "pixel, alpha 0 is d", i, e);
        else
            check(e == p->e565, "pixel against the vectors", i, e);
        e = row_one(p->d565, p->argb, 0, wt);
        check(e == p->e565, "row of one against the vectors, ARGB", i, e);
        e = row_one(p->d565, p->argb, 1, wt);
        check(e == p->e565, "row of one against the vectors, RGBA", i, e);
    }
    for (i = 0; i < 2000000L; i++)
    {
        uint32_t argb = rnd() << 8 ^ rnd();
        unsigned d = rnd() & 0xffff;
        unsigned w[3];
        unsigned e, r;
        int c;

        switch (rnd() & 7)
        {
        case 0:
            argb &= 0x00ffffff;
            break;
        case 1:
            argb |= 0xff000000UL;
            break;
        case 2:
            argb = (argb & 0x00ffffffUL) | ((uint32_t)(rnd() & 1 ? 1 : 254 + (rnd() & 1)) << 24);
            break;
        default:
            break;
        }
        for (c = 0; c < 3; c++)
        {
            unsigned t = rnd() & 7;

            w[c] = t == 0 ? 0xffff : t == 1 ? 0x8000 : t == 2 ? 0 : rnd() & 0xffff;
        }
        if (rnd() & 1)
            w[1] = w[2] = w[0];
        e = ref_pixel(d, argb, w);
        if (mix_k(d, argb, w) != e && (argb >> 24) != 0)
            bad++;
        r = row_one(d, argb, 0, w);
        if (r != e)
            bad_row++;
        r = row_one(d, argb, 1, w);
        if (r != e)
            bad_row++;
        checks += 3;
    }
    check(bad == 0, "random pixels, mix", bad, 0);
    check(bad_row == 0, "random pixels, rows of one", bad_row, 0);
    /* A = 255 at full weight is mode 32's pixel */
    for (i = 0; i < 100000L; i++)
    {
        uint32_t rgb = rnd() & 0xffffff;

        check(row_one(rnd() & 0xffff, 0xff000000UL | rgb, 0, full) == xfer_pack565(rgb), "opaque is mode 32", i, 0);
        check(row_one(rnd() & 0xffff, 0xff000000UL | rgb, 1, full) == xfer_pack565(rgb), "opaque is mode 32, RGBA", i, 0);
    }
}

/* full weight, every alpha of 128-255, every field value against every component:
   an opaque shortcut may take alphas 253-255 (they give the opaque pixel) but
   not 252 or lower, which differ from it where a field and the component's
   top bits are 43 or more apart (green at 0 against 252-255 and the like) */
static void check_full_alpha(void)
{
    static const unsigned full[3] = {0xffff, 0xffff, 0xffff};
    unsigned a, dd, c;
    long bad = 0;

    for (a = 128; a < 256; a++)
        for (dd = 0; dd < 64; dd++)
            for (c = 0; c < 256; c++)
            {
                unsigned d = ((dd >> 1) << 11) | (dd << 5) | (dd >> 1);
                uint32_t argb = ((uint32_t)a << 24) | (c << 16) | (c << 8) | c;
                unsigned e = ref_pixel(d, argb, full);

                if (row_one(d, argb, 0, full) != e || row_one(d, argb, 1, full) != e)
                    bad++;
                checks += 2;
            }
    check(bad == 0, "full weight, every alpha, field and component", bad, 0);
}

/* ------------------------------------------------------------------ */
/* rows */

static void fill_alpha(uint32_t *px, long count, int kind)
{
    long i;

    for (i = 0; i < count; i++)
    {
        uint32_t a;

        switch (kind)
        {
        case 0:
            a = 0;
            break;
        case 1:
            a = 255;
            break;
        case 2:
        {
            uint32_t t = rnd() & 15;

            a = t < 3 ? 0 : t < 6 ? 255 : t == 6 ? 1 : t == 7 ? 254 : t == 8 ? 127 : t == 9 ? 128 : rnd() & 255;
            break;
        }
        default:
            a = (uint32_t)((i * 255) / (count > 1 ? count - 1 : 1));
            break;
        }
        px[i] = (a << 24) | (rnd() & 0xffffff);
    }
}

static void check_rows(void)
{
    static const unsigned full[3] = {0xffff, 0xffff, 0xffff};
    long count, bad = 0, bad_frame = 0, bad_hole = 0;
    int off, kind, rgba, wi;

    for (count = 1; count <= 80; count++)
        for (off = 0; off < 4; off++)
            for (kind = 0; kind < 4; kind++)
                for (rgba = 0; rgba < 2; rgba++)
                    for (wi = -1; wi < ALPHA_NWEIGHTS; wi++)
                    {
                        unsigned w[3];
                        long total = 8 + count, start = 2 + off, i;
                        UBYTE *buf = malloc((size_t)total * 2);
                        UBYTE *orig = malloc((size_t)total * 2);
                        UBYTE *src = malloc((size_t)count * 4);
                        uint32_t *px = malloc((size_t)count * sizeof *px);
                        ULONG wp[3];
                        int c;

                        for (c = 0; c < 3; c++)
                            w[c] = wi < 0 ? full[c] : alpha_weight[wi][c];
                        fill_alpha(px, count, kind);
                        for (i = 0; i < count; i++)
                            put_be32(src + i * 4, rgba ? rotate_rgba(px[i]) : px[i]);
                        for (i = 0; i < total; i++)
                            put16(orig + i * 2, (unsigned)(0x5a31 + i * 0x0707) & 0xffff);
                        memcpy(buf, orig, (size_t)total * 2);
                        for (c = 0; c < 3; c++)
                            wp[c] = vdi_alpha_weight((UWORD)w[c]);
                        vdi_alpha_row565(buf + start * 2, src, count, rgba ? FALSE : TRUE, wp);
                        for (i = 0; i < total; i++)
                        {
                            unsigned got = get16(buf + i * 2);
                            unsigned was = get16(orig + i * 2);

                            checks++;
                            if (i < start || i >= start + count)
                            {
                                if (got != was)
                                    bad_frame++;
                            }
                            else
                            {
                                unsigned e = ref_pixel(was, px[i - start], w);

                                if (got != e)
                                    bad++;
                                if ((px[i - start] >> 24) == 0 && got != was)
                                    bad_hole++;
                                if (wi < 0 && (px[i - start] >> 24) == 255 &&
                                    got != xfer_pack565(px[i - start] & 0xffffff))
                                    bad++;
                            }
                        }
                        free(px);
                        free(src);
                        free(orig);
                        free(buf);
                    }
    check(bad == 0, "rows, pixels", bad, 0);
    check(bad_frame == 0, "rows, frame intact", bad_frame, 0);
    check(bad_hole == 0, "rows, alpha 0 untouched", bad_hole, 0);
    /* no pixels: nothing read or written */
    {
        ULONG wp[3] = {VDI_ALPHA_FULL, VDI_ALPHA_FULL, VDI_ALPHA_FULL};

        vdi_alpha_row565((UBYTE *)0, (const UBYTE *)0, 0, TRUE, wp);
        check(1, "rows, count 0", 0, 0);
    }
}

int main(void)
{
    check_vectors_sum();
    check_k();
    check_channel();
    check_pixels();
    check_full_alpha();
    check_rows();

    printf("test_alpha: %ld checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
