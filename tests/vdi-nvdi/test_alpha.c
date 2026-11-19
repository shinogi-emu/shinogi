/*
 * Host checks of vdi/vdi_alpha.c, the blending of pixels with straight alpha
 * into the 16-bit pixels of EmuTOS's VDI (vr_transfer_bits, mode T_BLEND),
 * and of that mode through the raster transfer core, vdi/vdi_xfercore.c
 * built with CONF_WITH_VDI_ALPHA.  See README.txt.
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

/* ------------------------------------------------------------------ */
/* the raster transfer core with T_BLEND (vdi_xfercore.c, CONF_WITH_VDI_ALPHA) */

/* the colour of a source pixel of a direct kind, from the format as written down */
static uint32_t ref_src_rgb(const UBYTE *p, int kind)
{
    int rev = (kind & XK_REV) != 0;
    uint32_t v, r, g, b;

    switch (kind & XK_MASK)
    {
    case XK_ARGB32:
    case XK_32:
        return ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    case XK_RGBA32:
        return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
    case XK_24:
        return rev ? (((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0])
                   : (((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]);
    default:
        v = rev ? p[1] * 256u + p[0] : p[0] * 256u + p[1];
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

/* its alpha: 255 for the kinds without one */
static unsigned ref_src_alpha(const UBYTE *p, int kind)
{
    switch (kind & XK_MASK)
    {
    case XK_ARGB32:
        return p[0];
    case XK_RGBA32:
        return p[3];
    default:
        return 255;
    }
}

static int kind_bytes(int kind)
{
    switch (kind & XK_MASK)
    {
    case XK_15:
    case XK_16:
        return 2;
    case XK_24:
        return 3;
    default:
        return 4;
    }
}

static int has_alpha(int kind)
{
    return (kind & XK_MASK) == XK_ARGB32 || (kind & XK_MASK) == XK_RGBA32;
}

/* plenty of 0 and 255 and their neighbours, as images have them */
static UBYTE rnd_alpha(void)
{
    uint32_t t = rnd() & 15;

    return (UBYTE)(t < 3 ? 0 : t < 6 ? 255 : t == 6 ? 1 : t == 7 ? 254 : t == 8 ? 127 : t == 9 ? 128 : rnd());
}

/* a source of sw x sh pixels of a kind in exactly pitch * sh bytes; 'opaque': alpha 255 */
static UBYTE *make_src(int kind, long sw, long sh, long pitch, int opaque)
{
    UBYTE *s = malloc((size_t)(pitch * sh));
    long i, x, y;

    for (i = 0; i < pitch * sh; i++)
        s[i] = (UBYTE)rnd();
    if (has_alpha(kind))
        for (y = 0; y < sh; y++)
            for (x = 0; x < sw; x++)
                s[y * pitch + x * 4 + ((kind & XK_MASK) == XK_ARGB32 ? 0 : 3)] = opaque ? 255 : rnd_alpha();
    return s;
}

/* a job onto 16-bit words: the source rectangle at src, the part (skipx, skipy, count, rows) drawn */
typedef struct
{
    int kind;
    const UBYTE *src;
    long pitch;
    long sw, sh, dw, dh, skipx, skipy, count, rows;
    unsigned w[3];
    int blend;
} Blend;

static void set_weight(Blend *b, int wi)
{
    int c;

    for (c = 0; c < 3; c++)
        b->w[c] = wi < 0 ? 0xffff : alpha_weight[wi][c];
}

/* run b into the rows at d, dpitch bytes apart, each row given the one above as prev (as vdi_xfer.c) */
static void run_job(const Blend *b, UBYTE *d, long dpitch)
{
    XferJob job;
    XferState st;
    long y;
    int c;

    memset(&job, 0, sizeof(job));
    job.kind = (WORD)b->kind;
    job.planes = XD_16;
    job.src = b->src;
    job.pitch = b->pitch;
    job.sw = (ULONG)b->sw;
    job.sh = (ULONG)b->sh;
    job.dw = (ULONG)b->dw;
    job.dh = (ULONG)b->dh;
    job.skipx = (ULONG)b->skipx;
    job.skipy = (ULONG)b->skipy;
    job.count = b->count;
    job.rows = b->rows;
    job.blend = b->blend ? TRUE : FALSE;
    for (c = 0; c < 3; c++)
        job.wp[c] = (ULONG)ref_wprime(b->w[c]);
    xfer_begin(&st, &job);
    for (y = 0; y < b->rows; y++)
        xfer_row16(&st, d + y * dpitch, y ? d + (y - 1) * dpitch : NULL);
}

/* the pixel T_BLEND makes at (dx, dy) of the destination rectangle over d: the nearest source pixel */
static unsigned ref_blend_at(const Blend *b, long dx, long dy, unsigned d)
{
    long sx = (long)((int64_t)dx * b->sw / b->dw), sy = (long)((int64_t)dy * b->sh / b->dh);
    const UBYTE *p = b->src + sy * b->pitch + sx * kind_bytes(b->kind);
    uint32_t argb = ((uint32_t)ref_src_alpha(p, b->kind) << 24) | ref_src_rgb(p, b->kind);

    return ref_pixel(d, argb, b->w);
}

/* run b into d (dsize bytes, the part drawn from its start) and compare every byte with the reference */
static int blend_check(const Blend *b, UBYTE *d, long dpitch, long dsize)
{
    UBYTE *e = malloc((size_t)dsize);
    long x, y;
    int ok;

    memcpy(e, d, (size_t)dsize);
    for (y = 0; y < b->rows; y++)
        for (x = 0; x < b->count; x++)
        {
            UBYTE *q = e + y * dpitch + x * 2;

            put16(q, ref_blend_at(b, b->skipx + x, b->skipy + y, get16(q)));
        }
    run_job(b, d, dpitch);
    ok = memcmp(d, e, (size_t)dsize) == 0;
    free(e);
    return ok;
}

/* one job over a random background with a frame after each row and after the last */
static void blend_case(int kind, long sw, long sh, long dw, long dh, long skipx, long skipy,
                       long count, long rows, int wi, int shift)
{
    Blend b;
    long pitch = sw * kind_bytes(kind) + (shift & 3) * 2;
    long dpitch = count * 2 + 4, dsize = dpitch * rows + 4, i;
    UBYTE *src = make_src(kind, sw, sh, pitch, 0);
    UBYTE *d = malloc((size_t)dsize);

    memset(&b, 0, sizeof(b));
    b.kind = kind;
    b.src = src;
    b.pitch = pitch;
    b.sw = sw;
    b.sh = sh;
    b.dw = dw;
    b.dh = dh;
    b.skipx = skipx;
    b.skipy = skipy;
    b.count = count;
    b.rows = rows;
    b.blend = 1;
    set_weight(&b, wi);
    for (i = 0; i < dsize; i++)
        d[i] = (UBYTE)rnd();
    if (!blend_check(&b, d, dpitch, dsize))
    {
        check(0, "blend rows against the reference", kind, wi);
        printf("  kind %d %ldx%ld -> %ldx%ld skip %ld,%ld part %ldx%ld weight %d\n",
               kind, sw, sh, dw, dh, skipx, skipy, count, rows, wi);
    }
    else
        check(1, "blend rows against the reference", kind, wi);
    free(d);
    free(src);
}

static const int blend_kinds[] = {
    XK_ARGB32, XK_RGBA32, XK_16, XK_24, XK_32, XK_15, XK_15 | XK_REV, XK_16 | XK_REV, XK_24 | XK_REV
};
#define NBLEND ((int)(sizeof(blend_kinds) / sizeof(blend_kinds[0])))

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

static void check_kinds(void)
{
    static const int kinds[] = {
        XK_NONE, XK_1, XK_8, XK_15, XK_16, XK_15 | XK_REV, XK_16 | XK_REV, XK_24, XK_24 | XK_REV,
        XK_32, XK_ARGB32, XK_RGBA32
    };
    static const int dests[] = { XD_16, 1, 2, 4, 8, XD_INDEX8 };
    UBYTE p[4] = { 0x12, 0x34, 0x56, 0x78 };
    unsigned i, j;
    long n;

    check(XF_PX_ARGB32 == ALPHA_PX_ARGB32 && XF_PX_RGBA32 == ALPHA_PX_RGBA32, "the formats of the vectors", 0, 0);
    check(XF_T_BLEND == ALPHA_T_BLEND, "T_BLEND", XF_T_BLEND, ALPHA_T_BLEND);
    check(XK_ARGB32 == 7 && XK_RGBA32 == 8 && (XK_RGBA32 & XK_MASK) == XK_RGBA32, "the kinds", XK_ARGB32, XK_RGBA32);
    check(xfer_kind(0x03122020UL) == XK_ARGB32, "kind ARGB32", xfer_kind(0x03122020UL), 0);
    check(xfer_kind(0x03022020UL) == XK_RGBA32, "kind RGBA32", xfer_kind(0x03022020UL), 0);
    check(xfer_kind(0x03922020UL) == XK_NONE, "ARGB32 is never reversed", 0, 0);
    check(xfer_kind(0x03822020UL) == XK_NONE, "RGBA32 is never reversed", 0, 0);
    check(xfer_kind(0x04122020UL) == XK_NONE, "four components are none", 0, 0);
    check(xfer_kind(0x03122021UL) == XK_NONE && xfer_kind(0x03022120UL) == XK_NONE, "kinds near them", 0, 0);
    check(xfer_kind(0x03421820UL) == XK_32, "xRGB stays", 0, 0);
    check(xfer_kind_bits(XK_ARGB32) == 32 && xfer_kind_bits(XK_RGBA32) == 32, "32 bits", 0, 0);
    check(xfer_alpha(p, XK_ARGB32) == 0x12, "alpha first", xfer_alpha(p, XK_ARGB32), 0x12);
    check(xfer_alpha(p, XK_RGBA32) == 0x78, "alpha last", xfer_alpha(p, XK_RGBA32), 0x78);
    check(xfer_alpha(p, XK_32) == 255, "xRGB has no alpha", xfer_alpha(p, XK_32), 255);
    check(xfer_alpha(p, XK_24) == 255 && xfer_alpha(p, XK_16) == 255 && xfer_alpha(p, XK_15) == 255 &&
          xfer_alpha(p, XK_16 | XK_REV) == 255 && xfer_alpha(p, XK_24 | XK_REV) == 255,
          "the other kinds have no alpha", 0, 0);
    check(xfer_unpack(p, XK_ARGB32) == 0x345678UL, "ARGB colour", (long)xfer_unpack(p, XK_ARGB32), 0x345678);
    check(xfer_unpack(p, XK_RGBA32) == 0x123456UL, "RGBA colour", (long)xfer_unpack(p, XK_RGBA32), 0x123456);
    for (n = 0; n < 100000L; n++)
    {
        p[0] = (UBYTE)rnd();
        p[1] = (UBYTE)rnd();
        p[2] = (UBYTE)rnd();
        p[3] = (UBYTE)rnd();
        check(xfer_unpack(p, XK_ARGB32) == ref_src_rgb(p, XK_ARGB32) &&
              xfer_unpack(p, XK_RGBA32) == ref_src_rgb(p, XK_RGBA32) &&
              xfer_alpha(p, XK_ARGB32) == p[0] && xfer_alpha(p, XK_RGBA32) == p[3],
              "unpack and alpha, random", n, 0);
    }

    /* T_BLEND: direct sources onto 16 bits only */
    for (i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++)
        for (j = 0; j < sizeof(dests) / sizeof(dests[0]); j++)
        {
            int k = kinds[i] & XK_MASK;
            int expect = k != XK_NONE && k != XK_1 && k != XK_8 && dests[j] == XD_16;

            check(!xfer_blend_ok((WORD)kinds[i], (WORD)dests[j]) == !expect, "what T_BLEND takes", kinds[i], dests[j]);
        }

    /*
     * the refusals of the vectors, decided as vdi_xfer.c does: a format that
     * is no kind, a mode other than 64, a memory destination of another
     * format than the screen's (ARGB32, xRGB32), or what xfer_blend_ok()
     * refuses (8-bit indices, planes of the screen or of memory)
     */
    for (i = 0; i < ALPHA_NREFUSAL; i++)
    {
        const AlphaRefusal *r = &alpha_refusal[i];
        WORD kind = xfer_kind(r->px_format);
        int dest = r->dest;
        int refused;

        if (kind == XK_NONE || r->mode != XF_T_BLEND || dest == ALPHA_DEST_ARGB32 || dest == ALPHA_DEST_XRGB32)
            refused = 1;
        else if (dest == ALPHA_DEST_PLANES)
            refused = !xfer_blend_ok(kind, 1) && !xfer_blend_ok(kind, 2) &&
                      !xfer_blend_ok(kind, 4) && !xfer_blend_ok(kind, 8);
        else
            refused = !xfer_blend_ok(kind, dest == ALPHA_DEST_INDEX8 ? XD_INDEX8 : XD_16);
        check(refused, "a refusal of the vectors", (long)i, (long)r->px_format);
    }
}

static void check_sizes(void)
{
    static const unsigned long fmt[2] = { 0x03122020UL, 0x03022020UL };
    static const int kinds[2] = { XK_ARGB32, XK_RGBA32 };
    static ULONG storage[64];
    UBYTE *p = (UBYTE *)storage;
    GCBITMAP b;
    int t;

    for (t = 0; t < 2; t++)
    {
        int kind = kinds[t];
        long w, h, extra;

        b = mkbm(p, 16, 32, fmt[t], 4, 4);
        check(xfer_src_size(&b, (WORD)kind) == 64, "ARGB/RGBA size", t, (long)xfer_src_size(&b, (WORD)kind));
        b.addr = p + 2;
        check(xfer_src_size(&b, (WORD)kind) == 64, "ARGB/RGBA on a word", t, 0);
        b.addr = p + 1;
        check(xfer_src_size(&b, (WORD)kind) == 0, "ARGB/RGBA on an odd address", t, 0);
        b.addr = p + 3;
        check(xfer_src_size(&b, (WORD)kind) == 0, "ARGB/RGBA on an odd address, 3", t, 0);
        b.addr = p;
        b.width = 17;
        check(xfer_src_size(&b, (WORD)kind) == 0, "ARGB/RGBA, an odd line", t, 0);
        b.width = 14;
        check(xfer_src_size(&b, (WORD)kind) == 0, "ARGB/RGBA, a line shorter than 4 w", t, 0);
        b.width = 18;
        check(xfer_src_size(&b, (WORD)kind) == 72, "ARGB/RGBA, a longer line", t, 0);
        b.width = 16;
        b.bits = 24;
        check(xfer_src_size(&b, (WORD)kind) == 0, "ARGB/RGBA, bits 24", t, 0);
        b.bits = 16;
        check(xfer_src_size(&b, (WORD)kind) == 0, "ARGB/RGBA, bits 16", t, 0);
        b.bits = 32;
        b.addr = NULL;
        check(xfer_src_size(&b, (WORD)kind) == 0, "ARGB/RGBA, no address", t, 0);

        /*
         * memory of exactly width * h bytes, the destination of exactly its
         * size: drawn whole, 1:1, enlarged and reduced, nothing past either is
         * read (ASan) and every pixel is the reference's
         */
        for (w = 1; w <= 9; w++)
            for (h = 1; h <= 5; h++)
                for (extra = 0; extra <= 2; extra += 2)
                {
                    long width = 4 * w + extra, dims[3][2], i, n;
                    UBYTE *src = make_src(kind, w, h, width, 0);
                    Blend bl;

                    dims[0][0] = w;
                    dims[0][1] = h;
                    dims[1][0] = 3 * w;
                    dims[1][1] = 2 * h + 1;
                    dims[2][0] = (w + 1) / 2;
                    dims[2][1] = (h + 1) / 2;
                    b = mkbm(src, width, 32, fmt[t], w, h);
                    check(xfer_src_size(&b, (WORD)kind) == (ULONG)(width * h), "exact size", w, h);
                    for (n = 0; n < 3; n++)
                    {
                        long dw = dims[n][0], dh = dims[n][1], dsize = dw * dh * 2;
                        UBYTE *d = malloc((size_t)dsize);

                        for (i = 0; i < dsize; i++)
                            d[i] = (UBYTE)rnd();
                        memset(&bl, 0, sizeof(bl));
                        bl.kind = kind;
                        bl.src = src;
                        bl.pitch = width;
                        bl.sw = w;
                        bl.sh = h;
                        bl.dw = bl.count = dw;
                        bl.dh = bl.rows = dh;
                        bl.blend = 1;
                        set_weight(&bl, (int)((w + h + n) % 7) - 1);
                        check(blend_check(&bl, d, dw * 2, dsize), "exact buffers, the whole rectangle", w * 100 + h, n);
                        free(d);
                    }
                    free(src);
                }
    }
}

static void check_transfer(void)
{
    static const long sizes[] = { 1, 2, 3, 7, 16, 19, 33, 40 };
    long sw, dw, n = 0;
    int k, a, bb, c;

    /* every size 1-40 by 1-40, two heights for each pair of widths, every direct kind, the weights in turn */
    for (sw = 1; sw <= 40; sw++)
        for (dw = 1; dw <= 40; dw++)
        {
            long h1s = (sw * 7 + dw * 3) % 40 + 1, h1d = (sw + dw * 5) % 40 + 1;
            long h2s = dw, h2d = sw;

            for (k = 0; k < NBLEND; k++)
            {
                int wi = (int)(n % 7) - 1;

                blend_case(blend_kinds[k], sw, h1s, dw, h1d, 0, 0, dw, h1d, wi, (int)n);
                blend_case(blend_kinds[k], sw, h2s, dw, h2d, 0, 0, dw, h2d, (int)((n + 3) % 7) - 1, (int)n + 1);
                n++;
            }
        }

    /* windows starting inside the rectangle: the same pixels as the whole run there, nothing else */
    n = 0;
    for (a = 0; a < 8; a++)
        for (bb = 0; bb < 8; bb++)
            for (c = 0; c < 8; c++)
                for (k = 0; k < NBLEND; k++)
                {
                    long sw2 = sizes[a], sh = sizes[bb], dw2 = sizes[c], dh = sizes[(a + bb + c) % 8];
                    long sx = dw2 > 2 ? 1 + (n % 2) : 0, sy = dh > 2 ? 1 : 0;
                    long cx = dw2 - sx - (dw2 > 3 ? 1 : 0), cy = dh - sy - (dh > 3 ? 2 : 0);
                    int kind = blend_kinds[k];
                    long pitch = sw2 * kind_bytes(kind), dpitch = dw2 * 2, dsize = dpitch * dh, x, y;
                    UBYTE *src, *bg, *whole, *win;
                    Blend b;
                    int ok = 1;

                    if (cx < 1 || cy < 1)
                        continue;
                    src = make_src(kind, sw2, sh, pitch, 0);
                    bg = malloc((size_t)dsize);
                    whole = malloc((size_t)dsize);
                    win = malloc((size_t)dsize);
                    for (x = 0; x < dsize; x++)
                        bg[x] = (UBYTE)rnd();
                    memcpy(whole, bg, (size_t)dsize);
                    memcpy(win, bg, (size_t)dsize);
                    memset(&b, 0, sizeof(b));
                    b.kind = kind;
                    b.src = src;
                    b.pitch = pitch;
                    b.sw = sw2;
                    b.sh = sh;
                    b.dw = b.count = dw2;
                    b.dh = b.rows = dh;
                    b.blend = 1;
                    set_weight(&b, (int)(n % 7) - 1);
                    run_job(&b, whole, dpitch);
                    b.skipx = sx;
                    b.skipy = sy;
                    b.count = cx;
                    b.rows = cy;
                    run_job(&b, win + sy * dpitch + sx * 2, dpitch);
                    for (y = 0; y < dh; y++)
                        for (x = 0; x < dw2; x++)
                        {
                            long o = y * dpitch + x * 2;
                            int inside = x >= sx && x < sx + cx && y >= sy && y < sy + cy;
                            const UBYTE *e = inside ? whole + o : bg + o;

                            if (win[o] != e[0] || win[o + 1] != e[1])
                                ok = 0;
                        }
                    check(ok, "a window is the whole run there", kind, n);
                    if (!ok)
                        printf("  kind %d %ldx%ld -> %ldx%ld window %ld,%ld %ldx%ld\n",
                               kind, sw2, sh, dw2, dh, sx, sy, cx, cy);
                    free(win);
                    free(whole);
                    free(bg);
                    free(src);
                    n++;
                }

    /* sources and destinations of 65536 pixels, and skips at the ends of 16-bit coordinates */
    for (k = 0; k < NBLEND; k++)
    {
        int kind = blend_kinds[k];

        blend_case(kind, 65536L, 1, 1, 1, 0, 0, 1, 1, -1, k);
        blend_case(kind, 1, 1, 65536L, 1, 0, 0, 65536L, 1, 1, k);
        blend_case(kind, 1, 1, 65536L, 1, 65535L, 0, 1, 1, 3, k);
        blend_case(kind, 65536L, 1, 65536L, 1, 65535L, 0, 1, 1, 4, k);
        blend_case(kind, 65536L, 1, 65536L, 1, 0, 0, 65536L, 1, 1, k);
        blend_case(kind, 65536L, 1, 65535L, 1, 65534L, 0, 1, 1, 0, k);
        blend_case(kind, 65535L, 1, 65536L, 1, 65535L, 0, 1, 1, 5, k);
        blend_case(kind, 3, 65536L, 5, 7, 2, 0, 3, 7, 1, k);
        blend_case(kind, 3, 7, 5, 65536L, 2, 65530L, 3, 6, -1, k);
        blend_case(kind, 2, 65536L, 2, 65536L, 0, 65533L, 2, 3, 3, k);
    }

    /*
     * an enlarged row is blended, not copied from the row above: the rows
     * that take the same source row differ where the background does
     */
    {
        static const unsigned half[3] = { 0x8000, 0x8000, 0x8000 };
        long pitch = 8 * 4, dpitch = 16 * 2, y, x;
        UBYTE *src = make_src(XK_ARGB32, 8, 2, pitch, 0);
        UBYTE d[16 * 2 * 6];
        Blend b;
        int ok = 1, differ = 0;

        for (x = 0; x < 8; x++)
        {
            src[x * 4] = 128;               /* every alpha of both rows half way */
            src[pitch + x * 4] = 128;
        }
        for (y = 0; y < 6; y++)
            for (x = 0; x < 16; x++)
                put16(d + y * dpitch + x * 2, (unsigned)(y * 0x2104 + x * 0x0841) & 0xffff);
        memset(&b, 0, sizeof(b));
        b.kind = XK_ARGB32;
        b.src = src;
        b.pitch = pitch;
        b.sw = 8;
        b.sh = 2;
        b.dw = b.count = 16;
        b.dh = b.rows = 6;
        b.blend = 1;
        memcpy(b.w, half, sizeof(half));
        ok = blend_check(&b, d, dpitch, (long)sizeof(d));
        for (y = 1; y < 3; y++)
            if (memcmp(d + y * dpitch, d, (size_t)dpitch) != 0)
                differ = 1;
        check(ok, "an enlarged row is blended", 0, 0);
        check(differ, "enlarged rows differ by their background", 0, 0);
        free(src);
    }
}

/* A = 255 at full weight is mode 32: the same job without blending, wherever mode 32 does not filter */
static void check_mode32_equal(void)
{
    static const long sizes[] = { 1, 2, 3, 5, 8, 13, 21, 40 };
    int a, b, c, d, k;
    long n = 0;

    for (a = 0; a < 8; a++)
        for (b = a; b < 8; b++)
            for (c = 0; c < 8; c++)
                for (d = c; d < 8; d++)
                    for (k = 0; k < NBLEND; k++)
                    {
                        int kind = blend_kinds[k];
                        long sw = sizes[a], dw = sizes[b], sh = sizes[c], dh = sizes[d];
                        long pitch = sw * kind_bytes(kind) + (n & 1) * 2, dsize = dw * dh * 2, i;
                        UBYTE *src = make_src(kind, sw, sh, pitch, 1);
                        UBYTE *blended = malloc((size_t)dsize), *copied = malloc((size_t)dsize);
                        Blend bl;

                        for (i = 0; i < dsize; i++)
                        {
                            blended[i] = (UBYTE)rnd();
                            copied[i] = (UBYTE)rnd();
                        }
                        memset(&bl, 0, sizeof(bl));
                        bl.kind = kind;
                        bl.src = src;
                        bl.pitch = pitch;
                        bl.sw = sw;
                        bl.sh = sh;
                        bl.dw = bl.count = dw;
                        bl.dh = bl.rows = dh;
                        set_weight(&bl, -1);
                        bl.blend = 1;
                        run_job(&bl, blended, dw * 2);
                        bl.blend = 0;
                        run_job(&bl, copied, dw * 2);
                        check(memcmp(blended, copied, (size_t)dsize) == 0, "opaque at full weight is mode 32", kind, n);
                        free(copied);
                        free(blended);
                        free(src);
                        n++;
                    }
}

/* the test's own pixel value of a colour on 4 planes */
static ULONG pen4(void *ctx, ULONG rgb)
{
    (void)ctx;
    return (((rgb >> 16) * 3 + ((rgb >> 8) & 255) * 5 + (rgb & 255) * 7) >> 7) & 15;
}

/*
 * modes 0 and 32 take ARGB32 and RGBA32 wherever they take xRGB: the alpha
 * is not read, the output is that of the xRGB source of the same colours
 * (whose x byte differs), onto 16 bits (1:1, enlarged, filtered when
 * reduced), onto planes and as 8-bit indices with and without dithering
 */
static void check_replace(void)
{
    static const long dims[][4] = {
        { 7, 5, 7, 5 }, { 7, 5, 19, 11 }, { 19, 11, 7, 5 }, { 40, 3, 9, 17 }, { 1, 1, 13, 2 }, { 33, 9, 1, 1 }
    };
    static const int kinds[2] = { XK_ARGB32, XK_RGBA32 };
    COLOR_ENTRY pal[16];
    UBYTE *map = malloc(XFER_ITAB_CELLS(4));
    void *work = malloc(XFER_ITAB_WORK(16));
    unsigned t, i;
    int kk, path;

    for (i = 0; i < 16; i++)
    {
        memset(&pal[i], 0, sizeof(pal[i]));
        pal[i].rgb.red = (UWORD)rnd();
        pal[i].rgb.green = (UWORD)rnd();
        pal[i].rgb.blue = (UWORD)rnd();
    }
    xfer_itab_fill(pal, 16, 4, map, work);
    for (kk = 0; kk < 2; kk++)
        for (t = 0; t < sizeof(dims) / sizeof(dims[0]); t++)
            for (path = 0; path < 4; path++)
            {
                long sw = dims[t][0], sh = dims[t][1], dw = dims[t][2], dh = dims[t][3];
                long pitch = sw * 4, x, y, size, line;
                int kind = kinds[kk];
                UBYTE *src = make_src(kind, sw, sh, pitch, 0);
                UBYTE *x32 = malloc((size_t)(pitch * sh));
                UBYTE *out[2];
                int s;

                /* the xRGB source: the same colours, a random x byte */
                for (y = 0; y < sh; y++)
                    for (x = 0; x < sw; x++)
                    {
                        const UBYTE *p = src + y * pitch + x * 4;
                        UBYTE *q = x32 + y * pitch + x * 4;
                        uint32_t rgb = ref_src_rgb(p, kind);

                        q[0] = (UBYTE)rnd();
                        q[1] = (UBYTE)(rgb >> 16);
                        q[2] = (UBYTE)(rgb >> 8);
                        q[3] = (UBYTE)rgb;
                    }
                line = path == 0 ? dw * 2 : path == 1 ? ((dw + 15) / 16) * 2 * 4 : dw;
                size = line * dh;
                for (s = 0; s < 2; s++)
                {
                    XferJob job;
                    XferState st;
                    UBYTE *pix = malloc((size_t)dw);
                    WORD *err = calloc((size_t)(3 * dw), sizeof(WORD));

                    out[s] = malloc((size_t)size);
                    memset(out[s], 0x5a, (size_t)size);
                    memset(&job, 0, sizeof(job));
                    job.kind = (WORD)(s ? XK_32 : kind);
                    job.planes = path == 0 ? XD_16 : path == 1 ? 4 : XD_INDEX8;
                    job.src = s ? x32 : src;
                    job.pitch = pitch;
                    job.sw = (ULONG)sw;
                    job.sh = (ULONG)sh;
                    job.dw = (ULONG)dw;
                    job.dh = (ULONG)dh;
                    job.count = dw;
                    job.rows = dh;
                    job.pen_rgb = pen4;
                    if (path >= 2)
                    {
                        job.index = TRUE;
                        job.imap = map;
                        job.ibits = 4;
                        job.dcol = pal;
                        if (path == 3)
                        {
                            job.dither = TRUE;
                            job.err = err;
                        }
                    }
                    xfer_begin(&st, &job);
                    for (y = 0; y < dh; y++)
                    {
                        UBYTE *row = out[s] + y * line;

                        if (path == 0)
                            xfer_row16(&st, row, y ? row - line : NULL);
                        else if (path == 1)
                        {
                            xfer_row_pix(&st, pix, NULL);
                            xfer_store_planar(row, 4, 0, dw, pix, NULL);
                        }
                        else
                            xfer_row_index(&st, row);
                    }
                    free(err);
                    free(pix);
                }
                check(memcmp(out[0], out[1], (size_t)size) == 0, "modes 0 and 32 draw ARGB/RGBA as xRGB",
                      kind * 100 + path, (long)t);
                free(out[0]);
                free(out[1]);
                free(x32);
                free(src);
            }
    free(work);
    free(map);
}

/* every transfer of the vectors, in both orders of bytes, through xfer_begin and xfer_row16 */
static void check_images(void)
{
    static const int kinds[2] = { XK_ARGB32, XK_RGBA32 };
    int t, kk, ramp = -1;

    for (t = 0; t < ALPHA_NXFER; t++)
    {
        const AlphaXfer *x = &alpha_xfer[t];

        if (x->src == 1 && x->bg == 0 && x->weight == 0 && !x->clip &&
            x->sr[0] == 0 && x->sr[1] == 0 && x->dr[0] == 0 && x->dr[1] == 0 &&
            x->dr[2] == x->sr[2] && x->dr[3] == x->sr[3])
            ramp = t;
    }
    check(ramp >= 0, "the ramp entry is in the vectors", ramp, 0);
    for (kk = 0; kk < 2; kk++)
        for (t = 0; t < ALPHA_NXFER; t++)
        {
            const AlphaXfer *x = &alpha_xfer[t];
            int kind = kinds[kk];
            long w = alpha_src[x->src].w, h = alpha_src[x->src].h, pitch = w * 4, i, dpitch = ALPHA_IMG_W * 2;
            long cx1 = 0, cy1 = 0, cx2 = ALPHA_IMG_W - 1, cy2 = ALPHA_IMG_H - 1, dx1, dy1, dx2, dy2;
            UBYTE *src = malloc((size_t)(pitch * h));
            UBYTE *d = malloc((size_t)(dpitch * ALPHA_IMG_H));
            Blend b;
            int c;

            for (i = 0; i < w * h; i++)
            {
                uint32_t argb = (uint32_t)alpha_src[x->src].argb[i];

                put_be32(src + i * 4, kind == XK_RGBA32 ? rotate_rgba(argb) : argb);
            }
            for (i = 0; i < ALPHA_IMG_W * ALPHA_IMG_H; i++)
                put16(d + i * 2, alpha_bg565[x->bg][i / ALPHA_IMG_W][i % ALPHA_IMG_W]);
            if (x->clip)
            {
                cx1 = x->cr[0] > cx1 ? x->cr[0] : cx1;
                cy1 = x->cr[1] > cy1 ? x->cr[1] : cy1;
                cx2 = x->cr[2] < cx2 ? x->cr[2] : cx2;
                cy2 = x->cr[3] < cy2 ? x->cr[3] : cy2;
            }
            dx1 = x->dr[0] > cx1 ? x->dr[0] : cx1;
            dy1 = x->dr[1] > cy1 ? x->dr[1] : cy1;
            dx2 = x->dr[2] < cx2 ? x->dr[2] : cx2;
            dy2 = x->dr[3] < cy2 ? x->dr[3] : cy2;
            if (dx1 <= dx2 && dy1 <= dy2)
            {
                memset(&b, 0, sizeof(b));
                b.kind = kind;
                b.src = src + x->sr[1] * pitch + x->sr[0] * 4;
                b.pitch = pitch;
                b.sw = x->sr[2] - x->sr[0] + 1;
                b.sh = x->sr[3] - x->sr[1] + 1;
                b.dw = x->dr[2] - x->dr[0] + 1;
                b.dh = x->dr[3] - x->dr[1] + 1;
                b.skipx = dx1 - x->dr[0];
                b.skipy = dy1 - x->dr[1];
                b.count = dx2 - dx1 + 1;
                b.rows = dy2 - dy1 + 1;
                b.blend = 1;
                for (c = 0; c < 3; c++)
                    b.w[c] = alpha_weight[x->weight][c];
                run_job(&b, d + dy1 * dpitch + dx1 * 2, dpitch);
            }
            crc = 0xffffffffUL;
            for (i = 0; i < dpitch * ALPHA_IMG_H; i++)
                crc_byte(d[i]);
            crc ^= 0xffffffffUL;
            check(crc == x->crc565, "a transfer of the vectors", kind * 100 + t, (long)crc);
            if (t == ramp)
            {
                long y, xx, bad = 0;

                for (y = 0; y < 5; y++)
                    for (xx = 0; xx < 7; xx++)
                        if (get16(d + y * dpitch + xx * 2) != alpha_ramp565[y][xx])
                            bad++;
                check(bad == 0, "the ramp in full", kind, bad);
            }
            free(d);
            free(src);
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
    check_kinds();
    check_sizes();
    check_transfer();
    check_mode32_equal();
    check_replace();
    check_images();

    printf("test_alpha: %ld checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
