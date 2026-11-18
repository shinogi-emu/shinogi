/*
 * Generator of alpha_vectors.h: the shared reference vectors for blending
 * images with straight alpha onto 16-bit pixels (vr_transfer_bits, mode
 * T_BLEND).  Every implementation of the arithmetic is checked against the
 * one header this writes.
 *
 *   gen_alpha_vectors <output file>
 *
 * The reference functions below are private to this file.  They are written
 * straight from the formulas (64-bit integers, no 16-bit tricks) and share
 * no code with any VDI.  main() first checks the properties the formulas
 * must have, then writes the data.  The output depends on nothing but this
 * file (a fixed seed, no clock, no environment), so it is the same on every
 * host.
 *
 * The arithmetic, per channel c of r, g, b (all integers, >> only on
 * non-negative values):
 *
 *   Wc' = Wc + (Wc >> 15)                  Wc: the weight, 0-65535
 *   wa  = A + (A >> 7)                     A: the source alpha, 0-255
 *   kc  = (wa * Wc' + 32768) >> 16         0-256
 *   s   = C >> (8 - n)                     C: 8-bit source component, n: 5 or 6
 *   out = (d * 256 + (s - d) * kc + 128) >> 8
 *
 * A pixel with A = 0 is not touched (not even its bit 15).  Any other pixel
 * is written with 0 in every bit outside the three fields.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NWEIGHTS 6
#define NPIXELS  4096
#define IMG_W    32
#define IMG_H    24
#define NXFER    44
#define NREFUSAL 18

#define PX_ARGB32 0x03122020UL
#define PX_XRGB32 0x03421820UL

#define DEST_SCREEN 0
#define DEST_MEM16  1
#define DEST_INDEX8 2
#define DEST_ARGB32 3
#define DEST_XRGB32 4
#define DEST_PLANES 5

/* ------------------------------------------------------------------ */
/* the reference                                                       */
/* ------------------------------------------------------------------ */

/* kc for the alpha a and the weight w (one channel) */
static uint16_t ref_k(unsigned a, unsigned w)
{
    int64_t wc = (int64_t)w + ((int64_t)w >> 15);
    int64_t wa = (int64_t)a + ((int64_t)a >> 7);

    return (uint16_t)((wa * wc + 32768) >> 16);
}

/* the new field of n bits: d the old field, C the 8-bit source component */
static unsigned ref_out(unsigned d, unsigned C, unsigned n, unsigned k)
{
    int64_t s = (int64_t)(C >> (8 - n));
    int64_t num = (int64_t)d * 256 + (s - (int64_t)d) * (int64_t)k + 128;

    return (unsigned)(num >> 8);
}

/* the pixel d (layout 565, or 555 with bit 15 free) blended with argb */
static uint16_t ref_pixel(uint16_t d, uint32_t argb, const uint16_t w[3], int layout)
{
    unsigned a = argb >> 24, r = (argb >> 16) & 255, g = (argb >> 8) & 255, b = argb & 255;
    unsigned dr, dg, db, nr, ng, nb;

    if (a == 0)
        return d;
    if (layout == 565)
    {
        dr = d >> 11;
        dg = (d >> 5) & 63;
        db = d & 31;
        nr = ref_out(dr, r, 5, ref_k(a, w[0]));
        ng = ref_out(dg, g, 6, ref_k(a, w[1]));
        nb = ref_out(db, b, 5, ref_k(a, w[2]));
        return (uint16_t)((nr << 11) | (ng << 5) | nb);
    }
    dr = (d >> 10) & 31;
    dg = (d >> 5) & 31;
    db = d & 31;
    nr = ref_out(dr, r, 5, ref_k(a, w[0]));
    ng = ref_out(dg, g, 5, ref_k(a, w[1]));
    nb = ref_out(db, b, 5, ref_k(a, w[2]));
    return (uint16_t)((nr << 10) | (ng << 5) | nb);
}

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static uint32_t rng_state = 0x5eed1234UL;

static uint32_t rnd16(void)             /* a linear congruential generator, fixed seed */
{
    rng_state = rng_state * 1664525UL + 1013904223UL;
    return rng_state >> 16;
}

static uint32_t rnd24(void)
{
    uint32_t hi = rnd16() & 0xff;
    uint32_t lo = rnd16();

    return (hi << 16) | lo;
}

static uint16_t pack565(unsigned r, unsigned g, unsigned b)
{
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

/* the same colour at 5 bits a channel, bit 15 clear */
static uint16_t to555(uint16_t v)
{
    return (uint16_t)((((v >> 11) & 31) << 10) | ((((v >> 5) & 63) >> 1) << 5) | (v & 31));
}

/* zlib's CRC-32, reflected 0xEDB88320, init and final xor 0xffffffff */
typedef struct { uint32_t c; } Crc;

static void crc_init(Crc *x)
{
    x->c = 0xffffffffUL;
}

static void crc_byte(Crc *x, unsigned b)
{
    int i;

    x->c ^= b & 255;
    for (i = 0; i < 8; i++)
        x->c = (x->c & 1) ? (x->c >> 1) ^ 0xEDB88320UL : x->c >> 1;
}

static uint32_t crc_done(const Crc *x)
{
    return x->c ^ 0xffffffffUL;
}

/* the checksum's unit: every scalar as 4 bytes, big-endian (a negative one sign-extended) */
static void crc_word(Crc *x, int64_t v)
{
    uint32_t u = (uint32_t)v;

    crc_byte(x, u >> 24);
    crc_byte(x, u >> 16);
    crc_byte(x, u >> 8);
    crc_byte(x, u);
}

/* ------------------------------------------------------------------ */
/* the data                                                            */
/* ------------------------------------------------------------------ */

typedef struct { uint32_t argb; uint16_t d565, e565, d555, e555; uint8_t weight; } Pixel;
typedef struct { uint8_t src, bg, weight, clip; int16_t sr[4], dr[4], cr[4]; uint32_t crc565, crc555; } Xfer;
typedef struct { uint32_t px_format; int16_t mode, dest; } Refusal;

static const uint16_t weight[NWEIGHTS][3] = {
    { 0xffff, 0xffff, 0xffff },
    { 0x8000, 0x8000, 0x8000 },
    { 0, 0, 0 },
    { 0xffff, 0x8000, 0 },
    { 0x7fff, 0x8001, 0xfffe },
    { 1, 2, 3 }
};

static uint16_t ktab[NWEIGHTS][256][3];
static Pixel pixels[NPIXELS];

#define ICON_W 16
#define ICON_H 12
#define RAMP_W 7
#define RAMP_H 5
#define COLUMN_H 9

static uint32_t src_icon[ICON_W * ICON_H];
static uint32_t src_ramp[RAMP_W * RAMP_H];
static uint32_t src_dot[1];
static uint32_t src_column[COLUMN_H];

static struct { int w, h; uint32_t *px; } src[4] = {
    { ICON_W, ICON_H, src_icon },
    { RAMP_W, RAMP_H, src_ramp },
    { 1, 1, src_dot },
    { 1, COLUMN_H, src_column }
};

static uint16_t bg565[3][IMG_H][IMG_W], bg555[3][IMG_H][IMG_W];
static Xfer xfers[NXFER];
static uint16_t ramp565[RAMP_H][RAMP_W];
static Refusal refusals[NREFUSAL];

static unsigned long nchecks;
static unsigned long bit15_cleared, bit15_kept;   /* the 555 write rule seen on background 2 */

#define CHECK(cond, ...) \
    do { \
        nchecks++; \
        if (!(cond)) \
        { \
            fprintf(stderr, "gen_alpha_vectors: FAILED %s:%d: %s\n  ", __FILE__, __LINE__, #cond); \
            fprintf(stderr, __VA_ARGS__); \
            fputc('\n', stderr); \
            exit(1); \
        } \
    } while (0)

static void check_properties(void)
{
    unsigned a, w, n, d, C, k;
    unsigned prev;

    /* every one of the 65536 weights, every alpha */
    for (w = 0; w < 65536; w++)
    {
        unsigned wcp = w + (w >> 15);

        prev = 0;
        CHECK(ref_k(0, w) == 0, "w=%u", w);
        for (a = 0; a < 256; a++)
        {
            unsigned kk = ref_k(a, w);

            CHECK(kk <= 256, "a=%u w=%u k=%u", a, w, kk);
            CHECK(a == 0 || kk >= prev, "a=%u w=%u k=%u prev=%u", a, w, kk, prev);
            if (wcp == 65536)
                CHECK(kk == a + (a >> 7), "a=%u w=%u k=%u", a, w, kk);
            prev = kk;
        }
    }
    CHECK(ref_k(255, 0xffff) == 256, "k(255, 0xffff)=%u", ref_k(255, 0xffff));
    CHECK(ref_k(255, 0x8000) == 128, "k(255, 0x8000)=%u", ref_k(255, 0x8000));
    for (a = 0; a < 256; a++)
        CHECK(ref_k(a, 0xffff) == a + (a >> 7), "a=%u", a);

    for (n = 5; n <= 6; n++)
        for (d = 0; d < (1u << n); d++)
            for (C = 0; C < 256; C++)
            {
                unsigned s = C >> (8 - n);

                CHECK(ref_out(d, C, n, 0) == d, "n=%u d=%u C=%u", n, d, C);
                CHECK(ref_out(d, C, n, 256) == s, "n=%u d=%u C=%u", n, d, C);
                for (k = 0; k <= 256; k++)
                {
                    int64_t num = (int64_t)d * 256 + ((int64_t)s - (int64_t)d) * (int64_t)k + 128;
                    unsigned o = ref_out(d, C, n, k);
                    unsigned lo = d < s ? d : s, hi = d < s ? s : d;

                    CHECK(num >= 128 && num <= 16256, "n=%u d=%u C=%u k=%u num=%ld", n, d, C, k, (long)num);
                    CHECK(lo <= o && o <= hi, "n=%u d=%u C=%u k=%u out=%u", n, d, C, k, o);
                    /* the same numerator written the other way */
                    CHECK(o == (unsigned)(((int64_t)d * (256 - k) + (int64_t)s * k + 128) >> 8),
                          "n=%u d=%u C=%u k=%u out=%u", n, d, C, k, o);
                }
            }
}

static void make_ktab(void)
{
    unsigned i, a, c;

    for (i = 0; i < NWEIGHTS; i++)
        for (a = 0; a < 256; a++)
            for (c = 0; c < 3; c++)
                ktab[i][a][c] = ref_k(a, weight[i][c]);
}

static void make_pixels(void)
{
    static const uint32_t fixed[4] = { 0xff2010, 0x10e040, 0x2030ff, 0x808080 };
    static const uint16_t fixed_d[4] = { 0x8410, 0x2d6b, 0xd69a, 0x07e0 };
    unsigned i = 0, a, j;

    /* every alpha with fixed colours, onto fixed destinations */
    for (a = 0; a < 256; a++)
        for (j = 0; j < 4; j++, i++)
        {
            pixels[i].argb = ((uint32_t)a << 24) | fixed[j];
            pixels[i].d565 = fixed_d[j];
            pixels[i].d555 = to555(fixed_d[j]) | (uint16_t)((rnd16() & 1) << 15);
            pixels[i].weight = (uint8_t)((a + j) % NWEIGHTS);
        }
    /* black onto 0x0000 and white onto 0xffff, every alpha */
    for (a = 0; a < 256; a++)
        for (j = 0; j < 2; j++, i++)
        {
            unsigned flag = rnd16() & 1;

            pixels[i].weight = (uint8_t)((a + j) % NWEIGHTS);

            if (j == 0)
            {
                pixels[i].argb = (uint32_t)a << 24;
                pixels[i].d565 = 0x0000;
                pixels[i].d555 = (uint16_t)(flag << 15);
            }
            else
            {
                pixels[i].argb = ((uint32_t)a << 24) | 0xffffff;
                pixels[i].d565 = 0xffff;
                pixels[i].d555 = (uint16_t)(0x7fff | (flag << 15));
            }
        }
    /* 565 destinations with green's lowest bit set, every alpha twice */
    for (j = 0; j < 2; j++)
        for (a = 0; a < 256; a++, i++)
        {
            unsigned r = rnd16() & 31, g = (rnd16() & 63) | 1, b = rnd16() & 31;

            pixels[i].weight = (uint8_t)(i % NWEIGHTS);
            pixels[i].argb = ((uint32_t)a << 24) | rnd24();
            pixels[i].d565 = (uint16_t)((r << 11) | (g << 5) | b);
            pixels[i].d555 = (uint16_t)((rnd16() & 0x7fff) | ((rnd16() & 1) << 15));
        }
    /* the rest random; every fourth with one of the awkward alphas */
    for (j = 0; i < NPIXELS; i++, j++)
    {
        static const unsigned awkward[7] = { 0, 1, 127, 128, 129, 254, 255 };
        uint32_t c = rnd24();
        unsigned al = rnd16() & 255;

        if ((j & 3) == 0)
            al = awkward[(j >> 2) % 7];
        pixels[i].weight = (uint8_t)(i % NWEIGHTS);
        pixels[i].argb = ((uint32_t)al << 24) | c;
        pixels[i].d565 = (uint16_t)rnd16();
        pixels[i].d555 = (uint16_t)rnd16();
    }
    for (i = 0; i < NPIXELS; i++)
    {
        pixels[i].e565 = ref_pixel(pixels[i].d565, pixels[i].argb, weight[pixels[i].weight], 565);
        pixels[i].e555 = ref_pixel(pixels[i].d555, pixels[i].argb, weight[pixels[i].weight], 555);
    }
}

static void make_sources(void)
{
    static const unsigned ramp_alpha[RAMP_W] = { 0, 1, 127, 128, 129, 254, 255 };
    static const uint32_t ramp_rgb[RAMP_H] = { 0xff2010, 0x10e040, 0x2030ff, 0xffffff, 0x000000 };
    int x, y, sx, sy, i;

    /* the icon: a disc, centre (7.5, 5.5), radius 5.5.  Pixel (x,y) has its centre at (x,y)
       and covers [x-0.5, x+0.5) in each direction, so the disc sits in the middle of the
       16 x 12 frame.  The alpha is the coverage of the pixel, 16 x 16 sample points on a
       grid (points on the circle count as inside) */
    for (y = 0; y < ICON_H; y++)
        for (x = 0; x < ICON_W; x++)
        {
            int cnt = 0;
            unsigned a, r, g, b;

            for (sy = 0; sy < 16; sy++)
                for (sx = 0; sx < 16; sx++)
                {
                    /* in 1/32 pixel: the centre is (240, 176), the radius 176 */
                    int64_t px = (int64_t)x * 32 - 16 + 2 * sx + 1 - 240;
                    int64_t py = (int64_t)y * 32 - 16 + 2 * sy + 1 - 176;

                    if (px * px + py * py <= 176 * 176)
                        cnt++;
                }
            a = (unsigned)((cnt * 255 + 128) / 256);
            r = (unsigned)(17 * x);
            g = 0x80;
            b = (unsigned)(255 - 17 * x);
            src_icon[y * ICON_W + x] = ((uint32_t)a << 24) | (r << 16) | (g << 8) | b;
        }
    for (y = 0; y < RAMP_H; y++)
        for (x = 0; x < RAMP_W; x++)
            src_ramp[y * RAMP_W + x] = ((uint32_t)ramp_alpha[x] << 24) | ramp_rgb[y];
    src_dot[0] = 0xc83060c0UL;
    for (i = 0; i < COLUMN_H; i++)
        src_column[i] = ((uint32_t)(i * 255 / 8) << 24) | 0xe0a020;
}

static void make_backgrounds(void)
{
    /* two colours whose 565 green fields are odd */
    const uint16_t c0 = pack565(0x12 << 3, (0x2b << 2), 0x0d << 3);
    const uint16_t c1 = pack565(0x1a << 3, (0x15 << 2), 0x06 << 3);
    int x, y;

    for (y = 0; y < IMG_H; y++)
        for (x = 0; x < IMG_W; x++)
        {
            bg565[0][y][x] = 0x8410;
            bg565[1][y][x] = ((x ^ y) & 1) ? c1 : c0;
            bg565[2][y][x] = (uint16_t)rnd16();
            bg555[0][y][x] = to555(bg565[0][y][x]);
            bg555[1][y][x] = to555(bg565[1][y][x]);
            bg555[2][y][x] = to555(bg565[2][y][x]);
        }
    /* bit 15 of background 2 in 555, at random in about half of its pixels: transfers onto
       it show the write rule (written pixels clear it, untouched ones keep it) */
    for (y = 0; y < IMG_H; y++)
        for (x = 0; x < IMG_W; x++)
            bg555[2][y][x] |= (uint16_t)((rnd16() & 1) << 15);
}

/* the destination 'dst' (IMG_W x IMG_H) with the source si drawn, nearest sampling, clipped */
static void ref_transfer(uint16_t dst[IMG_H][IMG_W], int layout, const Xfer *t)
{
    int sw = src[t->src].w, sh = src[t->src].h;
    int dw = t->dr[2] - t->dr[0] + 1, dh = t->dr[3] - t->dr[1] + 1;
    int x, y;

    CHECK(t->sr[2] - t->sr[0] + 1 == sw && t->sr[3] - t->sr[1] + 1 == sh, "source rectangle");
    CHECK(dw > 0 && dh > 0 && t->dr[0] >= 0 && t->dr[1] >= 0 && t->dr[2] < IMG_W && t->dr[3] < IMG_H,
          "destination rectangle");
    for (y = 0; y < dh; y++)
        for (x = 0; x < dw; x++)
        {
            int px = t->dr[0] + x, py = t->dr[1] + y;
            int sx = (int)((int64_t)x * sw / dw), sy = (int)((int64_t)y * sh / dh);

            if (t->clip && (px < t->cr[0] || py < t->cr[1] || px > t->cr[2] || py > t->cr[3]))
                continue;
            {
                uint32_t argb = src[t->src].px[sy * sw + sx];
                uint16_t before = dst[py][px];

                dst[py][px] = ref_pixel(before, argb, weight[t->weight], layout);
                if (layout == 555 && t->bg == 2 && (before & 0x8000))
                {
                    if (argb >> 24)
                        bit15_cleared++;
                    else
                        bit15_kept++;
                }
            }
        }
}

static uint32_t crc_image(uint16_t dst[IMG_H][IMG_W])
{
    Crc c;
    int x, y;

    crc_init(&c);
    for (y = 0; y < IMG_H; y++)
        for (x = 0; x < IMG_W; x++)
        {
            crc_byte(&c, dst[y][x] >> 8);
            crc_byte(&c, dst[y][x]);
        }
    return crc_done(&c);
}

static int nxfer;

static void add_xfer(int s, int bg, int wt, int clip, int sx2, int sy2,
                     int dx1, int dy1, int dx2, int dy2, int cx1, int cy1, int cx2, int cy2)
{
    Xfer *t = &xfers[nxfer];
    uint16_t d[IMG_H][IMG_W];

    CHECK(nxfer < NXFER, "too many transfers");
    t->src = (uint8_t)s;
    t->bg = (uint8_t)bg;
    t->weight = (uint8_t)wt;
    t->clip = (uint8_t)clip;
    t->sr[0] = 0; t->sr[1] = 0; t->sr[2] = (int16_t)sx2; t->sr[3] = (int16_t)sy2;
    t->dr[0] = (int16_t)dx1; t->dr[1] = (int16_t)dy1; t->dr[2] = (int16_t)dx2; t->dr[3] = (int16_t)dy2;
    t->cr[0] = (int16_t)cx1; t->cr[1] = (int16_t)cy1; t->cr[2] = (int16_t)cx2; t->cr[3] = (int16_t)cy2;
    memcpy(d, bg565[bg], sizeof d);
    ref_transfer(d, 565, t);
    t->crc565 = crc_image(d);
    if (bg == 0 && wt == 0 && s == 1 && dx1 == 0 && dy1 == 0 && dx2 == 6 && dy2 == 4)
    {
        int x, y;

        for (y = 0; y < RAMP_H; y++)
            for (x = 0; x < RAMP_W; x++)
                ramp565[y][x] = d[y][x];
    }
    memcpy(d, bg555[bg], sizeof d);
    ref_transfer(d, 555, t);
    t->crc555 = crc_image(d);
    nxfer++;
}

static void make_xfers(void)
{
    int bg, s, wt;

    for (bg = 0; bg < 3; bg++)
    {
        for (s = 0; s < 4; s++)         /* every source 1:1 at (0,0) */
            add_xfer(s, bg, 0, 0, src[s].w - 1, src[s].h - 1,
                     0, 0, src[s].w - 1, src[s].h - 1, 0, 0, IMG_W - 1, IMG_H - 1);
        for (s = 0; s < 4; s++)         /* and at (3,1) */
            add_xfer(s, bg, 0, 0, src[s].w - 1, src[s].h - 1,
                     3, 1, 3 + src[s].w - 1, 1 + src[s].h - 1, 0, 0, IMG_W - 1, IMG_H - 1);
        /* the icon 2x, unclipped and clipped */
        add_xfer(0, bg, 0, 0, 15, 11, 0, 0, 31, 23, 0, 0, IMG_W - 1, IMG_H - 1);
        add_xfer(0, bg, 0, 1, 15, 11, 0, 0, 31, 23, 5, 3, 20, 17);
        /* the ramp 3x2 */
        add_xfer(1, bg, 0, 0, 6, 4, 2, 2, 22, 11, 0, 0, IMG_W - 1, IMG_H - 1);
        /* the icon to 7 x 5 */
        add_xfer(0, bg, 0, 0, 15, 11, 4, 4, 10, 8, 0, 0, IMG_W - 1, IMG_H - 1);
        /* the column 1 x 9 to 5 x 18 */
        add_xfer(3, bg, 0, 0, 0, 8, 1, 1, 5, 18, 0, 0, IMG_W - 1, IMG_H - 1);
    }
    for (wt = 1; wt < NWEIGHTS; wt++)   /* background 0 only: the icon with the other weights */
        add_xfer(0, 0, wt, 0, 15, 11, 0, 0, 15, 11, 0, 0, IMG_W - 1, IMG_H - 1);
}

static void set_refusal(int i, uint32_t px, int mode, int dest)
{
    refusals[i].px_format = px;
    refusals[i].mode = (int16_t)mode;
    refusals[i].dest = (int16_t)dest;
}

static void make_refusals(void)
{
    int i = 0, m;

    set_refusal(i++, 0x03922020UL, 64, DEST_SCREEN);
    set_refusal(i++, 0x03822020UL, 64, DEST_SCREEN);
    set_refusal(i++, 0x04122020UL, 64, DEST_SCREEN);
    set_refusal(i++, 0x03922020UL, 64, DEST_MEM16);
    set_refusal(i++, 0x01020101UL, 64, DEST_SCREEN);
    set_refusal(i++, 0x01020808UL, 64, DEST_SCREEN);
    for (m = 65; m <= 71; m++)
        set_refusal(i++, PX_ARGB32, m, DEST_SCREEN);
    set_refusal(i++, PX_ARGB32, 80, DEST_SCREEN);
    set_refusal(i++, PX_ARGB32, 64, DEST_INDEX8);
    set_refusal(i++, PX_ARGB32, 64, DEST_ARGB32);
    set_refusal(i++, PX_XRGB32, 64, DEST_XRGB32);
    set_refusal(i++, PX_ARGB32, 64, DEST_PLANES);
    CHECK(i == NREFUSAL, "refusals %d", i);
}

/* ------------------------------------------------------------------ */
/* the checksum and the output                                         */
/* ------------------------------------------------------------------ */

/* every array of the header in declaration order, every scalar as 4 bytes big-endian */
static uint32_t vectors_sum(void)
{
    Crc c;
    unsigned i, j, k;
    int x, y, t;

    crc_init(&c);
    for (i = 0; i < NWEIGHTS; i++)
        for (j = 0; j < 3; j++)
            crc_word(&c, weight[i][j]);
    for (i = 0; i < NWEIGHTS; i++)
        for (j = 0; j < 256; j++)
            for (k = 0; k < 3; k++)
                crc_word(&c, ktab[i][j][k]);
    for (i = 0; i < NPIXELS; i++)
    {
        crc_word(&c, pixels[i].argb);
        crc_word(&c, pixels[i].d565);
        crc_word(&c, pixels[i].e565);
        crc_word(&c, pixels[i].d555);
        crc_word(&c, pixels[i].e555);
        crc_word(&c, pixels[i].weight);
    }
    for (t = 0; t < 4; t++)
    {
        crc_word(&c, src[t].w);
        crc_word(&c, src[t].h);
        for (j = 0; j < (unsigned)(src[t].w * src[t].h); j++)
            crc_word(&c, src[t].px[j]);
    }
    for (t = 0; t < 3; t++)
        for (y = 0; y < IMG_H; y++)
            for (x = 0; x < IMG_W; x++)
                crc_word(&c, bg565[t][y][x]);
    for (t = 0; t < 3; t++)
        for (y = 0; y < IMG_H; y++)
            for (x = 0; x < IMG_W; x++)
                crc_word(&c, bg555[t][y][x]);
    for (t = 0; t < NXFER; t++)
    {
        crc_word(&c, xfers[t].src);
        crc_word(&c, xfers[t].bg);
        crc_word(&c, xfers[t].weight);
        crc_word(&c, xfers[t].clip);
        for (j = 0; j < 4; j++)
            crc_word(&c, xfers[t].sr[j]);
        for (j = 0; j < 4; j++)
            crc_word(&c, xfers[t].dr[j]);
        for (j = 0; j < 4; j++)
            crc_word(&c, xfers[t].cr[j]);
        crc_word(&c, xfers[t].crc565);
        crc_word(&c, xfers[t].crc555);
    }
    for (y = 0; y < RAMP_H; y++)
        for (x = 0; x < RAMP_W; x++)
            crc_word(&c, ramp565[y][x]);
    for (t = 0; t < NREFUSAL; t++)
    {
        crc_word(&c, refusals[t].px_format);
        crc_word(&c, refusals[t].mode);
        crc_word(&c, refusals[t].dest);
    }
    return crc_done(&c);
}

static void emit_src_array(FILE *f, const char *name, const uint32_t *p, int n)
{
    int i;

    fprintf(f, "static const unsigned long %s[%d] = {", name, n);
    for (i = 0; i < n; i++)
        fprintf(f, "%s0x%08lxUL,", (i % 4) == 0 ? "\n    " : " ", (unsigned long)p[i]);
    fprintf(f, "\n};\n\n");
}

static void emit_grid(FILE *f, const char *name, uint16_t g[3][IMG_H][IMG_W])
{
    int t, x, y;

    fprintf(f, "static const unsigned short %s[3][ALPHA_IMG_H][ALPHA_IMG_W] = {\n", name);
    for (t = 0; t < 3; t++)
    {
        fprintf(f, "  {\n");
        for (y = 0; y < IMG_H; y++)
        {
            fprintf(f, "    {");
            for (x = 0; x < IMG_W; x++)
                fprintf(f, "%s0x%04x", x ? "," : "", g[t][y][x]);
            fprintf(f, "},\n");
        }
        fprintf(f, "  },\n");
    }
    fprintf(f, "};\n\n");
}

static void emit_rect(FILE *f, const int16_t *r)
{
    fprintf(f, "{%d,%d,%d,%d}", r[0], r[1], r[2], r[3]);
}

static void emit(FILE *f, uint32_t sum)
{
    int i, j, k;

    fprintf(f,
"/*\n"
" * alpha_vectors.h: the shared reference vectors for blending images with\n"
" * straight alpha onto 16-bit pixels (vr_transfer_bits, mode T_BLEND).\n"
" * Written by gen_alpha_vectors.c (tests/vdi-nvdi); do not edit.  The same\n"
" * file is committed in every tree that checks against it.  It is included,\n"
" * never compiled on its own.\n"
" *\n"
" * The arithmetic, per channel c of r, g, b (integers, >> only on non-negative\n"
" * values).  Wc is the weight (0-65535), A the source alpha (0-255), n the bits\n"
" * of the destination field (5 or 6), d its value, C the 8-bit source component:\n"
" *\n"
" *     Wc' = Wc + (Wc >> 15)\n"
" *     wa  = A + (A >> 7)\n"
" *     kc  = (wa * Wc' + 32768) >> 16                  0-256\n"
" *     s   = C >> (8 - n)\n"
" *     out = (d * 256 + (s - d) * kc + 128) >> 8\n"
" *\n"
" * A = 0: the pixel is not touched.  Any other A: the pixel is written, with\n"
" * 0 in every bit outside the three fields (bit 15 of a 555 pixel).\n"
" *\n"
" * Layouts: 565 is rrrrrggggggbbbbb, 555 is xrrrrrgggggbbbbb.  A 555 source\n"
" * value d555 may have bit 15 set; e555 follows the write rule above.  A\n"
" * 555 background holds the colours of the 565 one at 5 bits a channel;\n"
" * background 2 in 555 also has bit 15 set in about half of its pixels.\n"
" * The icon is a disc of centre (7.5, 5.5) and radius 5.5; pixel (x,y) has its\n"
" * centre at (x,y) and covers [x-0.5, x+0.5), so the disc sits in the middle\n"
" * of the 16 x 12 frame.\n"
" * Rectangles are x1, y1, x2, y2, inclusive.  The pixel of a source image\n"
" * is the long 0xAARRGGBB (PX_RGBA32 cases rotate its bytes).  Images are\n"
" * rows top to bottom.  A transfer draws source rectangle sr into dr with\n"
" * the nearest source pixel, sx = (x - dr.x1) * sw / dw and likewise sy\n"
" * (integer division, the phase does not move when clipped); clip = 1 skips\n"
" * the destination pixels outside cr; the weight is alpha_weight[weight].\n"
" * crc565/crc555: zlib CRC-32 of the whole 32 x 24 destination, each pixel\n"
" * 2 bytes big-endian, rows top to bottom, after drawing onto\n"
" * alpha_bg565[bg] / alpha_bg555[bg].  alpha_ramp565 is the ramp drawn 1:1\n"
" * at (0,0) onto background 0 at full weight.\n"
" *\n"
" * ALPHA_VECTORS_SUM: CRC-32 (as above) of every array in declaration order\n"
" * (alpha_weight, alpha_k, alpha_pixel, alpha_src, alpha_bg565, alpha_bg555,\n"
" * alpha_xfer, alpha_ramp565, alpha_refusal), every scalar element, whatever\n"
" * its width, appended as 4 bytes big-endian (a negative one sign-extended),\n"
" * the fields of a struct in order; for alpha_src its w, h and then the\n"
" * pixels it points to (never the pointer).  A consumer recomputes it and\n"
" * compares before anything else.\n"
" */\n"
"#ifndef ALPHA_VECTORS_H\n"
"#define ALPHA_VECTORS_H\n\n"
"#define ALPHA_PX_ARGB32   0x03122020UL\n"
"#define ALPHA_PX_RGBA32   0x03022020UL\n"
"#define ALPHA_T_BLEND     64\n"
"#define ALPHA_NWEIGHTS    %d\n"
"#define ALPHA_NPIXELS     %d\n"
"#define ALPHA_IMG_W       %d\n"
"#define ALPHA_IMG_H       %d\n"
"#define ALPHA_DEST_SCREEN 0\n"
"#define ALPHA_DEST_MEM16  1\n"
"#define ALPHA_DEST_INDEX8 2\n"
"#define ALPHA_DEST_ARGB32 3\n"
"#define ALPHA_DEST_XRGB32 4\n"
"#define ALPHA_DEST_PLANES 5\n"
"#define ALPHA_NXFER       %d\n"
"#define ALPHA_NREFUSAL    %d\n\n"
"typedef struct { unsigned long argb; unsigned short d565, e565, d555, e555; unsigned char weight; } AlphaPixel;\n"
"typedef struct { unsigned char src, bg, weight, clip; short sr[4], dr[4], cr[4]; unsigned long crc565, crc555; } AlphaXfer;\n"
"typedef struct { unsigned long px_format; short mode; short dest; } AlphaRefusal;\n\n",
            NWEIGHTS, NPIXELS, IMG_W, IMG_H, NXFER, NREFUSAL);

    fprintf(f, "static const unsigned short alpha_weight[ALPHA_NWEIGHTS][3] = {\n");
    for (i = 0; i < NWEIGHTS; i++)
        fprintf(f, "    {0x%04x, 0x%04x, 0x%04x},\n", weight[i][0], weight[i][1], weight[i][2]);
    fprintf(f, "};\n\n");

    fprintf(f, "static const unsigned short alpha_k[ALPHA_NWEIGHTS][256][3] = {\n");
    for (i = 0; i < NWEIGHTS; i++)
    {
        fprintf(f, "  {\n");
        for (j = 0; j < 256; j++)
            fprintf(f, "%s{%d,%d,%d},%s", (j % 6) == 0 ? "    " : " ",
                    ktab[i][j][0], ktab[i][j][1], ktab[i][j][2],
                    (j % 6) == 5 || j == 255 ? "\n" : "");
        fprintf(f, "  },\n");
    }
    fprintf(f, "};\n\n");

    fprintf(f, "static const AlphaPixel alpha_pixel[ALPHA_NPIXELS] = {\n");
    for (i = 0; i < NPIXELS; i++)
        fprintf(f, "    {0x%08lxUL, 0x%04x, 0x%04x, 0x%04x, 0x%04x, %d},\n", (unsigned long)pixels[i].argb,
                pixels[i].d565, pixels[i].e565, pixels[i].d555, pixels[i].e555, pixels[i].weight);
    fprintf(f, "};\n\n");

    emit_src_array(f, "alpha_src_icon", src_icon, ICON_W * ICON_H);
    emit_src_array(f, "alpha_src_ramp", src_ramp, RAMP_W * RAMP_H);
    emit_src_array(f, "alpha_src_dot", src_dot, 1);
    emit_src_array(f, "alpha_src_column", src_column, COLUMN_H);
    fprintf(f,
"/* icon 16x12, ramp 7x5, dot 1x1, column 1x9 */\n"
"static const struct { short w, h; const unsigned long *argb; } alpha_src[4] = {\n"
"    {%d, %d, alpha_src_icon},\n"
"    {%d, %d, alpha_src_ramp},\n"
"    {1, 1, alpha_src_dot},\n"
"    {1, %d, alpha_src_column}\n"
"};\n\n", ICON_W, ICON_H, RAMP_W, RAMP_H, COLUMN_H);

    emit_grid(f, "alpha_bg565", bg565);
    emit_grid(f, "alpha_bg555", bg555);

    fprintf(f, "static const AlphaXfer alpha_xfer[ALPHA_NXFER] = {\n");
    for (i = 0; i < NXFER; i++)
    {
        fprintf(f, "    {%d, %d, %d, %d, ", xfers[i].src, xfers[i].bg, xfers[i].weight, xfers[i].clip);
        emit_rect(f, xfers[i].sr);
        fprintf(f, ", ");
        emit_rect(f, xfers[i].dr);
        fprintf(f, ", ");
        emit_rect(f, xfers[i].cr);
        fprintf(f, ", 0x%08lxUL, 0x%08lxUL},\n", (unsigned long)xfers[i].crc565, (unsigned long)xfers[i].crc555);
    }
    fprintf(f, "};\n\n");

    fprintf(f, "static const unsigned short alpha_ramp565[%d][%d] = {\n", RAMP_H, RAMP_W);
    for (j = 0; j < RAMP_H; j++)
    {
        fprintf(f, "    {");
        for (k = 0; k < RAMP_W; k++)
            fprintf(f, "%s0x%04x", k ? ", " : "", ramp565[j][k]);
        fprintf(f, "},\n");
    }
    fprintf(f, "};\n\n");

    fprintf(f, "static const AlphaRefusal alpha_refusal[ALPHA_NREFUSAL] = {\n");
    for (i = 0; i < NREFUSAL; i++)
        fprintf(f, "    {0x%08lxUL, %d, %d},\n", (unsigned long)refusals[i].px_format, refusals[i].mode, refusals[i].dest);
    fprintf(f, "};\n\n");

    fprintf(f, "#define ALPHA_VECTORS_SUM 0x%08lxUL\n\n#endif\n", (unsigned long)sum);
}

/* sanity of the data itself */
static void check_data(void)
{
    int i, zero = 0, zero15 = 0, full = 0, part = 0, bit15 = 0, icon_zero = 0;

    for (i = 0; i < NPIXELS; i++)
    {
        unsigned a = pixels[i].argb >> 24;

        if (a == 0)
        {
            zero++;
            if (pixels[i].d555 & 0x8000)
                zero15++;
            CHECK(pixels[i].e565 == pixels[i].d565 && pixels[i].e555 == pixels[i].d555, "A=0 touched, pixel %d", i);
        }
        else
            CHECK((pixels[i].e555 & 0x8000) == 0, "bit 15 written, pixel %d", i);
        if (pixels[i].d555 & 0x8000)
            bit15++;
    }
    CHECK(zero > 0 && zero15 > 0, "no A=0 pixels (%d, %d)", zero, zero15);
    CHECK(bit15 > NPIXELS * 2 / 5 && bit15 < NPIXELS * 3 / 5, "bit 15 in %d", bit15);
    for (i = 0; i < ICON_W * ICON_H; i++)
    {
        unsigned a = src_icon[i] >> 24;

        if (a == 0)
            icon_zero++;
        else if (a == 255)
            full++;
        else
            part++;
    }
    CHECK(icon_zero > 0 && full > 0 && part > 0, "icon alpha classes (%d, %d, %d)", icon_zero, full, part);
    CHECK(bit15_cleared > 0 && bit15_kept > 0, "555 write rule on background 2 (%lu, %lu)", bit15_cleared, bit15_kept);
    for (i = 0; i < 6; i++)             /* each fixed colour meets every weight */
    {
        int j, seen[4] = { 0, 0, 0, 0 };

        for (j = 0; j < 1024; j++)
            if (pixels[j].weight == i)
                seen[j & 3]++;
        CHECK(seen[0] && seen[1] && seen[2] && seen[3], "weight %d of the fixed colours", i);
    }
    CHECK(nxfer == NXFER, "%d transfers", nxfer);
}

int main(int argc, char **argv)
{
    FILE *f;
    uint32_t sum;

    if (argc != 2)
    {
        fprintf(stderr, "usage: gen_alpha_vectors <output file>\n");
        return 2;
    }
    check_properties();
    printf("gen_alpha_vectors: reference properties hold (%lu checks)\n", nchecks);

    make_ktab();
    make_pixels();
    make_sources();
    make_backgrounds();
    make_xfers();
    make_refusals();
    check_data();
    sum = vectors_sum();

    f = fopen(argv[1], "w");
    if (!f)
    {
        perror(argv[1]);
        return 1;
    }
    emit(f, sum);
    if (fclose(f) != 0)
    {
        perror(argv[1]);
        return 1;
    }
    printf("gen_alpha_vectors: wrote %s, ALPHA_VECTORS_SUM 0x%08lx\n", argv[1], (unsigned long)sum);
    return 0;
}
