/*
 * Host checks of vdi/vdi_bezcore.c, the points of the Bezier curves of
 * EmuTOS's VDI.  See README.txt.
 *
 * The reference is fVDI's own code (engine/bezier.c: bez_depth, do_bez4 and
 * the two loops of calc_bez), copied here as it is apart from the two
 * differences the VDI documents (a curve needs its four points; flags only
 * as far as given).  The buffers are allocated to the exact size, so that
 * ASan sees any read past the points or the flags and any write past the
 * room given.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "portab.h"
#include "vdi_bezcore.h"

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

static uint32_t rng_state = 777;

static uint32_t rnd(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state >> 8;
}

/* ------------------------------------------------------------------ */
/* fVDI's code, the reference */

struct coords { long x, y; };
struct node { struct coords anchor, in, out; short onscreen, level; };

#define POSITIVE(v)     \
{                       \
    if (v < 0)          \
        v = -v;         \
}
#define MINMAX(type, min, max, v3, v4)  \
{                                       \
    if (min > max) {                    \
        type tmp = min;                 \
        min = max;                      \
        max = tmp;                      \
    }                                   \
    if (v3 > v4) {                      \
        type tmp = v3;                  \
        v3 = v4;                        \
        v4 = tmp;                       \
    }                                   \
    if (v4 > max)                       \
        max = v4;                       \
    if (v3 < min)                       \
        min = v3;                       \
}
#define MAX3(type, max, v1, v2, v3)     \
{                                       \
    type tmp;                           \
    max = v1;                           \
    POSITIVE(max);                      \
    tmp = v2;                           \
    POSITIVE(tmp);                      \
    if (tmp > max)                      \
        max = tmp;                      \
    tmp = v3;                           \
    POSITIVE(tmp);                      \
    if (tmp > max)                      \
        max = tmp;                      \
}
#define REF_HI(x) (short)((x) >> 16)

static int ref_depth(const short *pts, short depth_scale)
{
    short base, xdiff, ydiff, depth;
    long bez_size;

    base = pts[0];
    MAX3(short, xdiff, base - pts[2], base - pts[4], base - pts[6]);
    base = pts[1];
    MAX3(short, ydiff, base - pts[3], base - pts[5], base - pts[7]);
    bez_size = (long)xdiff * ydiff;
    if (!(bez_size >>= 4 * 2 + depth_scale - 2))
        return 2;
    for (depth = 3; depth < 7; depth++)
        if (!(bez_size >>= 4))
            return depth;
    return 7;
}

static int ref_bez4(const short *xyin, short depth, short *xyout, const short clip[])
{
    int count = 0;
    struct coords mid;
    struct node *current, bez[8];

    current = bez;
    current->onscreen = 0;
    current->level = depth - 1;
    current->anchor.x = ((long)xyin[6] * 32768L) + 0x4000;
    current->anchor.y = ((long)xyin[7] * 32768L) + 0x4000;
    current->in.x = ((long)xyin[4] * 32768L) + 0x4000;
    current->in.y = ((long)xyin[5] * 32768L) + 0x4000;
    current->out.x = ((long)xyin[2] * 32768L) + 0x4000;
    current->out.y = ((long)xyin[3] * 32768L) + 0x4000;
    (current + 1)->anchor.x = ((long)xyin[0] * 32768L) + 0x4000;
    (current + 1)->anchor.y = ((long)xyin[1] * 32768L) + 0x4000;
    for (;;)
    {
        if (current->onscreen == 0)
        {
            short xmin, xmax, ymin, ymax, v3, v4;

            xmin = REF_HI(current->anchor.x);
            xmax = REF_HI(current->in.x);
            v3 = REF_HI(current->out.x);
            v4 = REF_HI((current + 1)->anchor.x);
            MINMAX(short, xmin, xmax, v3, v4);
            ymin = REF_HI(current->anchor.y);
            ymax = REF_HI(current->in.y);
            v3 = REF_HI(current->out.y);
            v4 = REF_HI((current + 1)->anchor.y);
            MINMAX(short, ymin, ymax, v3, v4);
            xmin = xmin * 2 - 16;
            ymin = ymin * 2 - 16;
            xmax = xmax * 2 + 1 + 16;
            ymax = ymax * 2 + 1 + 16;
            if ((xmax < clip[0]) || (xmin > clip[2]) || (ymax < clip[1]) || (ymin > clip[3]))
                current->level = 0;
            else if ((xmin >= clip[0]) && (xmax <= clip[2]) && (ymin >= clip[1]) && (ymax <= clip[3]))
                current->onscreen = 1;
        }
        if (current->level <= 0)
        {
            *xyout++ = (int)(((((current + 1)->anchor.x + current->in.x) >> 1) + current->out.x) >> 16);
            *xyout++ = (int)(((((current + 1)->anchor.y + current->in.y) >> 1) + current->out.y) >> 16);
            *xyout++ = (int)((((current->anchor.x + current->out.x) >> 1) + current->in.x) >> 16);
            *xyout++ = (int)((((current->anchor.y + current->out.y) >> 1) + current->in.y) >> 16);
            count += 3;
            if (current == bez)
                break;
            *xyout++ = (int)((current->anchor.x * 2) >> 16);
            *xyout++ = (int)((current->anchor.y * 2) >> 16);
            current--;
        }
        else
        {
            mid.x = (current->in.x + current->out.x) >> 1;
            mid.y = (current->in.y + current->out.y) >> 1;
            (current + 2)->anchor.x = (current + 1)->anchor.x;
            (current + 2)->anchor.y = (current + 1)->anchor.y;
            (current + 1)->out.x = (current->out.x + (current + 1)->anchor.x) >> 1;
            (current + 1)->out.y = (current->out.y + (current + 1)->anchor.y) >> 1;
            (current + 1)->in.x = (mid.x + (current + 1)->out.x) >> 1;
            (current + 1)->in.y = (mid.y + (current + 1)->out.y) >> 1;
            current->in.x = (current->in.x + current->anchor.x) >> 1;
            current->in.y = (current->in.y + current->anchor.y) >> 1;
            current->out.x = (current->in.x + mid.x) >> 1;
            current->out.y = (current->in.y + mid.y) >> 1;
            (current + 1)->anchor.x = ((current + 1)->in.x + current->out.x) >> 1;
            (current + 1)->anchor.y = ((current + 1)->in.y + current->out.y) >> 1;
            current->level--;
            (current + 1)->level = current->level;
            (current + 1)->onscreen = current->onscreen;
            current++;
        }
    }
    return count;
}

/*
 * calc_bez's two loops; flags are given (maxin of them) and a curve needs
 * its four points (fVDI: i >= maxin - 1).  out: big enough; brk: a byte a point.
 */
static long ref_calc(const short *points, int count, const unsigned char *flags, int maxin,
                     int scale, int close, const short *clip, short *out, unsigned char *brk,
                     long *maxpnt_out, int *moves, int *used)
{
    long maxpnt = count;
    int i, movecnt = 0;
    const short *pts_ptr, *last_pnt;
    short *pts_out;

    *used = 0;
    if (close)
        maxpnt++;
    for (pts_ptr = points, i = 0; i < maxin; pts_ptr += 2, i++)
    {
        if (flags[i] & 2)
        {
            movecnt++;
            *used = 1;
        }
        if (flags[i] & 1)
        {
            if (i >= maxin - 2)
                break;
            *used = 1;
            maxpnt += (3 << (ref_depth(pts_ptr, scale) - 1)) - 3;
            pts_ptr += 4;
            i += 2;
        }
    }
    if (close)
        maxpnt += movecnt;
    *maxpnt_out = maxpnt;
    *moves = movecnt;

    memset(brk, 0, maxpnt + 8);
    pts_ptr = points;
    last_pnt = pts_ptr;
    pts_out = out;
    for (i = 0; i < count; i++)
    {
        if (i < maxin)
        {
            if (flags[i] & 2)
            {
                if (close)
                {
                    *pts_out++ = last_pnt[0];
                    *pts_out++ = last_pnt[1];
                    last_pnt = pts_ptr;
                }
                brk[(pts_out - out) / 2] = 1;
            }
            *pts_out++ = *pts_ptr++;
            *pts_out++ = *pts_ptr++;
            if (flags[i] & 1)
            {
                int cnt;

                if (i >= maxin - 2)
                    break;
                pts_ptr -= 2;
                cnt = ref_bez4(pts_ptr, ref_depth(pts_ptr, scale), pts_out, clip);
                pts_out += 2 * (cnt - 1);
                pts_ptr += 6;
                i += 2;
            }
        }
        else
        {
            *pts_out++ = *pts_ptr++;
            *pts_out++ = *pts_ptr++;
        }
    }
    if (close)
    {
        *pts_out++ = last_pnt[0];
        *pts_out++ = last_pnt[1];
    }
    return (pts_out - out) / 2;
}

/* ------------------------------------------------------------------ */

static long cases;

static void one_case(int count, int nflags, int scale, int close, const short *clip, int spread)
{
    BezPoint *pts = malloc(sizeof(BezPoint) * (count ? count : 1));
    unsigned char *flags = malloc(nflags ? nflags : 1);
    short *refout = malloc(sizeof(short) * 2 * 20000);
    unsigned char *refbrk = malloc(20000), *brk;
    BezPoint *out;
    long refmax, need;
    int i, refmoves, refused, refn, n, room;
    WORD moves;
    BOOL used;

    for (i = 0; i < count; i++)
    {
        pts[i].x = (WORD)((int)(rnd() % (2 * spread)) - spread / 2);
        pts[i].y = (WORD)((int)(rnd() % (2 * spread)) - spread / 2);
    }
    for (i = 0; i < nflags; i++)
    {
        uint32_t r = rnd() % 10;

        flags[i] = r < 3 ? 1 : r < 4 ? 2 : r < 5 ? 3 : 0;
    }
    refn = (int)ref_calc((const short *)pts, count, flags, nflags, scale, close, clip,
                         refout, refbrk, &refmax, &refmoves, &refused);
    need = bez_count(pts, (WORD)count, flags, (WORD)nflags, (WORD)scale, (BOOL)close, &moves, &used);
    check(need == refmax, "count as fVDI's", need, refmax);
    check(moves == refmoves && (used != 0) == (refused != 0), "moves and use as fVDI's", moves, refmoves);
    check(refn <= refmax, "the estimate is an upper bound", refn, refmax);

    /* all the room it needs: the points of fVDI */
    room = (int)need;
    out = malloc(sizeof(BezPoint) * (room ? room : 1));
    brk = malloc((room + 8) / 8);
    n = bez_generate(pts, (WORD)count, flags, (WORD)nflags, (WORD)scale, (BOOL)close, clip,
                     out, brk, (WORD)room);
    check(n == refn, "points as fVDI's", n, refn);
    if (n == refn)
        for (i = 0; i < n; i++)
        {
            int b = (brk[i >> 3] >> (7 - (i & 7))) & 1;

            if (out[i].x != refout[2 * i] || out[i].y != refout[2 * i + 1] || b != refbrk[i])
            {
                check(0, "point or move as fVDI's", i, n);
                break;
            }
        }
    free(out);
    free(brk);

    /* less room than it needs: refused, nothing written beyond it */
    if (refn > 0)
    {
        int rooms[3] = { refn - 1, refn / 2, 0 };
        int k;

        for (k = 0; k < 3; k++)
        {
            room = rooms[k];
            out = malloc(sizeof(BezPoint) * (room ? room : 1));
            brk = malloc((room + 8) / 8);
            n = bez_generate(pts, (WORD)count, flags, (WORD)nflags, (WORD)scale, (BOOL)close, clip,
                             out, brk, (WORD)room);
            check(n == -1, "short room is refused", n, room);
            free(out);
            free(brk);
        }
    }
    cases++;
    free(pts);
    free(flags);
    free(refout);
    free(refbrk);
}

int main(void)
{
    static const short clips[3][4] = { { 0, 0, 639, 479 }, { 100, 80, 300, 200 }, { -32768, -32768, 32767, 32767 } };
    int round, count, nflags, scale, close;

    for (round = 0; round < 6000; round++)
    {
        count = 1 + rnd() % 63;
        nflags = count ? (int)(rnd() % count) : 0;      /* up to count - 1 */
        if (rnd() & 1)
            nflags = count ? count - 1 : 0;
        scale = rnd() % 10;
        close = rnd() & 1;
        one_case(count, nflags, scale, close, clips[round % 3], round % 5 == 0 ? 30000 : 700);
    }

    /* the edges: no points; a curve without its four points is not one */
    {
        BezPoint none[1] = { { 5, 5 } }, out[2];
        UBYTE brk[2];

        check(bez_generate(none, 0, NULL, 0, 0, TRUE, clips[0], out, brk, 2) == 0, "no points, none made", 0, 0);
    }
    {
        BezPoint p[3] = { { 0, 0 }, { 100, 100 }, { 200, 0 } };
        unsigned char f[2] = { 1, 0 };
        BezPoint out[8];
        UBYTE brk[2];
        WORD moves;
        BOOL used;

        check(bez_count(p, 3, f, 2, 0, FALSE, &moves, &used) == 3 && !used, "three points and a curve flag", 0, 0);
        check(bez_generate(p, 3, f, 2, 0, FALSE, clips[0], out, brk, 8) == 1, "the making stops at it", 0, 0);
    }
    printf("%ld checks (%ld cases), %d failures\n", checks, cases, failures);
    printf("test_bezier %s\n", failures ? "FAIL" : "PASS");
    return failures != 0;
}
