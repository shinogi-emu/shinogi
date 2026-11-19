/*
 * Host checks that a copy of the VDI to the screen stays on the screen:
 * vro_cpyfm (109), vrt_cpyfm (121), both scaled (bit 15 of the mode) and
 * vr_transfer_bits (170, with T_BLEND where the tree has it), with a
 * destination partly off each edge, off a corner and wholly off, with the
 * clip flag off and on.
 *
 * vdi/vdi_raster.c (made over by the Makefile so that its C blit is used, as
 * on ColdFire) and vdi/vdi_xfer.c are linked as they are; this file only
 * stands in for the rest of the VDI: the line-A variables, a 16-bit screen
 * and the few services the two files call.
 *
 * The screen is a buffer between two guard areas.  A case passes if the
 * guards are untouched (nothing outside the screen is written) and every
 * pixel of the screen is what a direct model of the copy says: the
 * destination rectangle cut to the screen, and to the clipping rectangle
 * when it is on, the source moved along with the cut.
 *
 * Every case runs in a child process, so that a stray write that a sanitizer
 * stops still leaves the other cases to report.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#include "emutos.h"
#include "vdi_defs.h"
#if CONF_WITH_VDI_NVDI_EXT
#include "vdi_xfercore.h"
#include "vdi_kept.h"
#include "vdi_nvdimath.h"
#if CONF_WITH_VDI_ALPHA
#include "vdi_alpha.h"
#endif
#endif

/* ------------------------------------------------------------------ */
/* what the two files expect of the rest of the VDI                    */

WORD *CONTRL, *INTIN, *PTSIN, *INTOUT, *PTSOUT;
WORD COPYTRAN, MFILL;
Vwk *CUR_WORK;
WORD DEV_TAB[45];
WORD MAP_COL[256], REV_MAP_COL[256];
UBYTE *v_bas_ad;
UWORD v_lin_wr;
UWORD v_planes;
UBYTE vdi_screen_swapped;
WORD vdi_bm_xres, vdi_bm_yres;

void arb_corner(Rect *r)
{
    WORD t;

    if (r->x1 > r->x2) { t = r->x1; r->x1 = r->x2; r->x2 = t; }
    if (r->y1 > r->y2) { t = r->y1; r->y1 = r->y2; r->y2 = t; }
}
WORD validate_color_index(WORD c) { return c < 0 ? 0 : c > 255 ? 255 : c; }
void *vdi_alloc_kept(LONG n) { return malloc(n); }
void *vdi_alloc_kept_blit(LONG n) { return malloc(n); }
void vdi_free_kept(void *p) { free(p); }
void *dos_alloc_anyram(LONG n) { return malloc(n); }
WORD dos_free(void *p) { free(p); return 0; }
void bzero_nobuiltin(void *p, size_t n) { memset(p, 0, n); }
#if CONF_WITH_VDI_NVDI_EXT
ULONG vdi_px_format(void) { return XF_PX_16 | XF_PX_REVERSED; }   /* host byte order */

/* the colour services are for 8-bit sources and planes: not used here */
WORD vdi_nearest_pen_in(Vwk *v, const COLOR_RGB *c) { (void)v; (void)c; return 0; }
void vdi_pen_rgb(Vwk *v, WORD p, COLOR_RGB *c) { (void)v; (void)p; memset(c, 0, sizeof(*c)); }
BOOL vdi_ctab_head(const COLOR_TAB *t, COLOR_TAB_HEAD *h) { (void)t; (void)h; return FALSE; }
BOOL vdi_ctab_prepare(Vwk *v, WORD k, BOOL i) { (void)v; (void)k; (void)i; return FALSE; }
const VdiCtab *vdi_ctab_get(Vwk *v, WORD k, BOOL i) { (void)v; (void)k; (void)i; return NULL; }
const VdiItab *vdi_itab_find(Vwk *v, ULONG r, const COLOR_TAB *t) { (void)v; (void)r; (void)t; return NULL; }
UWORD vdi_pack16(UWORD r, UWORD g, UWORD b)
{
    return (UWORD)(((r >> 11) << 11) | ((g >> 10) << 5) | (b >> 11));
}
#endif

void vdi_vro_cpyfm(Vwk *);
void vdi_vrt_cpyfm(Vwk *);
#if CONF_WITH_VDI_NVDI_EXT
void vdi_vr_transfer_bits(Vwk *);
#endif
void linea_raster(void);

/* ------------------------------------------------------------------ */

#define SW      40              /* the screen, in pixels */
#define SH      30
#define GUARD   8192            /* bytes of guard on each side */
#define MW      32              /* a memory form: 2 words of 16 pixels wide, */
#define MH      10              /* 10 high, drawn as 20 wide (fd_w) */
#define MFDBW   20

static Vwk vwk;
static UWORD palette_store[VWKEXT_PALETTE];
static VwkExt ext_store;

static UBYTE *arena;            /* guard, screen, guard */
static UWORD *screen;

static UWORD bg(int x, int y) { return (UWORD)(0x2000 + x * 37 + y * 521); }

static void fill_arena(void)
{
    memset(arena, 0xa5, GUARD + SW * SH * 2 + GUARD);
    for (int y = 0; y < SH; y++)
        for (int x = 0; x < SW; x++)
            screen[y * SW + x] = bg(x, y);
}

static int guards_intact(void)
{
    for (int i = 0; i < GUARD; i++)
        if (arena[i] != 0xa5 || arena[GUARD + SW * SH * 2 + i] != 0xa5)
            return 0;
    return 1;
}

/* the source image: 8x6, a different non-background value at every pixel */
#define IW 8
#define IH 6
static UWORD src16[IH][16];     /* a device-form MFDB of one word wide: the 8 pixels of the image, then 8 more that are as much part of its memory */
static UWORD srcmono[IH];       /* one word a row, leftmost pixel in bit 15 */
static ULONG srcargb[IH][IW];   /* bytes a, r, g, b: as the 68k lays them out, see below */
static UWORD srcpix[IH][IW];    /* what the full-weight alpha pixels come to */
static const UWORD pal16[4] = { 0x1234, 0xf81f, 0x07e0, 0x001f };   /* the colours of indices 0-3 */
static WORD vrt_mode = MD_TRANS, vrt_fg = 1, vrt_bg = 0;           /* what vrt_cpyfm is asked */

/* the pixel vrt_cpyfm leaves where the source bit is 'bit', over 'old' */
static UWORD vrt_val(int bit, UWORD old)
{
    switch (vrt_mode)
    {
    case MD_REPLACE:
        return bit ? pal16[vrt_fg] : pal16[vrt_bg];
    case MD_ERASE:
        return bit ? old : pal16[vrt_bg];
    default:
        return bit ? pal16[vrt_fg] : old;
    }
}

static UWORD img(int x, int y) { return (UWORD)(0x8000 | (x + 1) | ((y + 1) << 4) | ((x ^ y) << 8)); }
static int monobit(int x, int y) { return ((x * 3 + y * 5 + (x * y)) & 3) != 0; }

static void make_sources(void)
{
    for (int y = 0; y < IH; y++)
    {
        srcmono[y] = 0;
        for (int x = 0; x < 16; x++)
        {
            UWORD r5 = (UWORD)((x * 3 + y) & 31), g6 = (UWORD)((x * 7 + y * 9 + 1) & 63), b5 = (UWORD)((x + y * 5 + 2) & 31);

            src16[y][x] = img(x, y);
            if (x < IW && monobit(x, y))
                srcmono[y] |= (UWORD)(0x8000 >> x);
            if (x >= IW)
                continue;
            srcpix[y][x] = (UWORD)((r5 << 11) | (g6 << 5) | b5);
            /* a byte string a, r, g, b: held as bytes, not as a long */
            ((UBYTE *)&srcargb[y][x])[0] = 255;
            ((UBYTE *)&srcargb[y][x])[1] = (UBYTE)(r5 << 3);
            ((UBYTE *)&srcargb[y][x])[2] = (UBYTE)(g6 << 2);
            ((UBYTE *)&srcargb[y][x])[3] = (UBYTE)(b5 << 3);
        }
    }
}

typedef struct
{
    void *fd_addr;
    WORD fd_w, fd_h, fd_wdwidth, fd_stand, fd_nplanes, fd_r1, fd_r2, fd_r3;
} Mfdb;

static void put_ptr(WORD *p, const void *v)
{
    p[0] = (WORD)((ULONG)v >> 16);
    p[1] = (WORD)(ULONG)v;
}

/* ------------------------------------------------------------------ */

enum { OP_VRO, OP_VRT, OP_XFER, OP_BLEND, OP_LINEA };

typedef struct
{
    int op;
    int scaled;                 /* bit 15 (vro, vrt); a destination of another size (170) */
    int clip;                   /* the clip flag */
    int memdst;                 /* the destination is a memory form, not the screen */
    long dx, dy;                /* the destination rectangle's first corner */
    int dw, dh;                 /* its size */
    int exact;                  /* check every pixel against the model, not just the guards */
} Case;

static WORD mfdb_w = MFDBW;             /* the fd_w of the forms in memory: 20, or 0, or 3: not looked at */
static WORD cl[4] = { 6, 4, 30, 20 };   /* the clipping rectangle used when it is on: x1 y1 x2 y2 */
static int bigclip;                     /* a clipping rectangle that is larger than the screen */

static void report(const char *what, const Case *c, int x, int y, unsigned got, unsigned want)
{
    printf("  %s: op %d scaled %d clip %d mem %d dest %ld,%ld %dx%d at %d,%d: got %04x want %04x\n",
           what, c->op, c->scaled, c->clip, c->memdst, c->dx, c->dy, c->dw, c->dh, x, y, got, want);
}

static int inrect(const Case *c, int x, int y, int bx1, int by1, int bx2, int by2, int dw, int dh)
{
    long i = x - c->dx, j = y - c->dy;

    return !(x < bx1 || x > bx2 || y < by1 || y > by2 || i < 0 || j < 0 || i >= dw || j >= dh);
}

/* the value the model says screen pixel x, y has after the copy */
static UWORD model(const Case *c, int x, int y, int bx1, int by1, int bx2, int by2, int dw, int dh)
{
    long i = x - c->dx, j = y - c->dy;
    int sx, sy;

    if (x < bx1 || x > bx2 || y < by1 || y > by2 || i < 0 || j < 0 || i >= dw || j >= dh)
        return bg(x, y);
    sx = (int)(i * IW / dw);
    sy = (int)(j * IH / dh);
    switch (c->op)
    {
    case OP_VRO:
    case OP_LINEA:
        return src16[sy][sx];
    case OP_VRT:
        return vrt_val(monobit(sx, sy), bg(x, y));
    case OP_XFER:
        return src16[sy][sx];
    default:
        /* the blend stores the pixel as the 68k's word: this host's bytes the other way */
        return (UWORD)((srcpix[sy][sx] >> 8) | (srcpix[sy][sx] << 8));
    }
}

static int run_case(const Case *c)
{
    WORD contrl[11] = { 0 }, intin[3] = { 0 }, ptsin[8];
    Mfdb smf, dmf;
#if CONF_WITH_VDI_NVDI_EXT
    GCBITMAP sbm;
#endif
    UBYTE *marena = NULL;
    UWORD *mem = NULL;
    int dw = c->scaled ? c->dw : IW, dh = c->scaled ? c->dh : IH;
    int bad = 0;

    fill_arena();
    memset(&smf, 0, sizeof(smf));
    smf.fd_w = IW;
    smf.fd_h = IH;
    smf.fd_wdwidth = 1;
    smf.fd_nplanes = (c->op == OP_VRT) ? 1 : 16;
    smf.fd_addr = (c->op == OP_VRT) ? (void *)srcmono : (void *)src16;
    memset(&dmf, 0, sizeof(dmf));
    if (c->memdst)
    {
        size_t bytes = (size_t)MW * MH * 2;

        marena = malloc(GUARD + bytes + GUARD);
        memset(marena, 0xa5, GUARD + bytes + GUARD);
        mem = (UWORD *)(marena + GUARD);
        for (int i = 0; i < MW * MH; i++)
            mem[i] = 0x3333;
        dmf.fd_addr = mem;
        dmf.fd_w = mfdb_w;
        dmf.fd_h = MH;
        dmf.fd_wdwidth = MW / 16;
        dmf.fd_nplanes = 16;
    }

    vwk.clip = c->clip;
    vwk.xmn_clip = bigclip ? -100 : cl[0];
    vwk.ymn_clip = bigclip ? -100 : cl[1];
    vwk.xmx_clip = bigclip ? 200 : cl[2];
    vwk.ymx_clip = bigclip ? 200 : cl[3];
#if CONF_WITH_VDI_NVDI_EXT
    vwk.col.extra[3].red = vwk.col.extra[3].green = vwk.col.extra[3].blue = 0xffff;
#endif

    ptsin[0] = 0;
    ptsin[1] = 0;
    ptsin[2] = IW - 1;
    ptsin[3] = IH - 1;
    ptsin[4] = (WORD)c->dx;
    ptsin[5] = (WORD)c->dy;
    ptsin[6] = (WORD)(c->dx + dw - 1);
    ptsin[7] = (WORD)(c->dy + dh - 1);
    CONTRL = contrl;
    INTIN = intin;
    PTSIN = ptsin;
    contrl[1] = 4;
    contrl[3] = 3;

    if (c->op == OP_VRO || c->op == OP_VRT || c->op == OP_LINEA)
    {
        put_ptr(contrl + 7, &smf);
        put_ptr(contrl + 9, c->memdst ? (void *)&dmf : (void *)&(Mfdb){ 0 });
        intin[0] = (WORD)((c->op == OP_VRO ? 3 : vrt_mode) | (c->scaled ? 0x8000 : 0));
        intin[1] = vrt_fg;
        intin[2] = vrt_bg;
        if (c->op == OP_VRO)
            vdi_vro_cpyfm(&vwk);
        else if (c->op == OP_LINEA)
        {
            COPYTRAN = 0;
            MFILL = 0;
            intin[0] = 3;
            linea_raster();
        }
        else
            vdi_vrt_cpyfm(&vwk);
    }
#if CONF_WITH_VDI_NVDI_EXT
    else
    {
        memset(&sbm, 0, sizeof(sbm));
        sbm.magic = GCBITMAP_MAGIC;
        sbm.length = sizeof(sbm);
        sbm.xmax = IW;            /* one past the last pixel */
        sbm.ymax = IH;
        if (c->op == OP_XFER)
        {
            sbm.addr = (UBYTE *)src16;
            sbm.width = sizeof(src16[0]);
            sbm.bits = 16;
            sbm.px_format = XF_PX_16;
            intin[0] = 0;
        }
        else
        {
#if CONF_WITH_VDI_ALPHA
            sbm.addr = (UBYTE *)srcargb;
            sbm.width = sizeof(srcargb[0]);
            sbm.bits = 32;
            sbm.px_format = XF_PX_ARGB32;
            intin[0] = XF_T_BLEND;
#else
            return 0;
#endif
        }
        put_ptr(contrl + 7, &sbm);
        contrl[9] = contrl[10] = 0;     /* this workstation: the screen */
        vdi_vr_transfer_bits(&vwk);
    }
#endif

    if (!guards_intact())
    {
        report("wrote outside the screen buffer", c, 0, 0, 0, 0);
        bad = 1;
    }
    if (c->memdst)
    {
        size_t bytes = (size_t)MW * MH * 2;
        int ok = 1;

        for (size_t i = 0; i < GUARD; i++)
            if (marena[i] != 0xa5 || marena[GUARD + bytes + i] != 0xa5)
                ok = 0;
        if (!ok)
        {
            report("wrote outside the memory form", c, 0, 0, 0, 0);
            bad = 1;
        }
        /* a scaled copy refuses a rectangle that the form does not hold; the others draw the part inside */
        if (c->exact && !bad)
        {
            for (int y = 0; y < MH && !bad; y++)
                for (int x = 0; x < MW && !bad; x++)
                {
                    long i = x - c->dx, j = y - c->dy;
                    UWORD want = 0x3333;

                    if (!c->scaled && x < MW && i >= 0 && j >= 0 && i < IW && j < IH)
                        want = c->op != OP_VRT ? src16[j][i] : vrt_val(monobit((int)i, (int)j), 0x3333);
                    if (mem[y * MW + x] != want)
                    {
                        report("memory pixel", c, x, y, mem[y * MW + x], want);
                        bad = 1;
                    }
                }
        }
        free(marena);
        return bad;
    }
    if (c->exact)
    {
        int bx1 = 0, by1 = 0, bx2 = SW - 1, by2 = SH - 1;

        if (c->clip)
        {
            if (vwk.xmn_clip > bx1) bx1 = vwk.xmn_clip;
            if (vwk.ymn_clip > by1) by1 = vwk.ymn_clip;
            if (vwk.xmx_clip < bx2) bx2 = vwk.xmx_clip;
            if (vwk.ymx_clip < by2) by2 = vwk.ymx_clip;
        }
        for (int y = 0; y < SH && bad < 4; y++)
            for (int x = 0; x < SW && bad < 4; x++)
            {
                UWORD want = model(c, x, y, bx1, by1, bx2, by2, dw, dh);
                UWORD got = screen[y * SW + x];

                /*
                 * A scaled vrt_cpyfm stages its row as bytes, leftmost pixel in the top
                 * bit of the first byte, and the copy reads them as words: the 68k's
                 * order, not this host's.  Which pixels are drawn is then not checked,
                 * only that every one drawn is inside the destination rectangle and the
                 * bounds (the pixel that the model leaves alone must be left alone).
                 */
                if (c->op == OP_VRT && c->scaled && inrect(c, x, y, bx1, by1, bx2, by2, dw, dh))
                    want = (got == bg(x, y) || got == pal16[vrt_fg] || got == pal16[vrt_bg]) ? got : want;
                if (got != want)
                {
                    report("pixel", c, x, y, screen[y * SW + x], want);
                    bad++;
                }
            }
    }
    return bad;
}

/* run one case in a child: 0 passes, 1 fails (a crash or a sanitizer's stop counts) */
static int run_isolated(const Case *c)
{
    int status;
    pid_t pid;

    fflush(stdout);
    pid = fork();
    if (pid == 0)
    {
        int r;

        if (!getenv("CLAMP_VERBOSE") && !freopen("/dev/null", "w", stderr))
            _exit(3);    /* the sanitizer's report: only the verdict matters */
        r = run_case(c);
        fflush(stdout);
        _exit(r ? 1 : 0);
    }
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status))
    {
        printf("  killed (sanitizer or signal): op %d scaled %d clip %d mem %d dest %ld,%ld %dx%d\n",
               c->op, c->scaled, c->clip, c->memdst, c->dx, c->dy, c->dw, c->dh);
        return 1;
    }
    return WEXITSTATUS(status) != 0;
}

/* ------------------------------------------------------------------ */

static const char *opname[] = { "vro_cpyfm", "vrt_cpyfm", "vr_transfer_bits", "vr_transfer_bits T_BLEND", "line-A copy raster" };

/* the first corners to try for a destination w x h: off each edge, a corner, inside, touching, wholly off */
static void positions(int w, int h, long *xs, long *ys, int *nx, int *ny, int extreme)
{
    long x[] = { -(w / 2), -w, 0, 10, SW - w, SW - w / 2, SW + 3, -w - 7 };
    long y[] = { -(h / 2), -h, 0, 8, SH - h, SH - h / 2, SH + 3, -h - 7 };
    int i;

    for (i = 0; i < 8; i++)
    {
        xs[i] = x[i];
        ys[i] = y[i];
    }
    *nx = *ny = 8;
    if (extreme)
    {
        /* far off, and where the end of the rectangle wraps in 16 bits */
        long ex[] = { -30000, 30000, 32767 - w / 2, -32768 };
        long ey[] = { -30000, 30000, 32767 - h / 2, -32768 };

        for (i = 0; i < 4; i++)
        {
            xs[8 + i] = ex[i];
            ys[8 + i] = ey[i];
        }
        *nx = *ny = 12;
    }
}

static int total, failed;

static void run_group(int op, int scaled, int clip, int bigc, int dw, int dh)
{
    long xs[12], ys[12];
    int nx, ny, ix, iy, bad = 0, n = 0;

    positions(dw, dh, xs, ys, &nx, &ny, 1);
    bigclip = bigc;
    for (iy = 0; iy < ny; iy++)
        for (ix = 0; ix < nx; ix++)
        {
            Case c;

            c.op = op;
            c.scaled = scaled;
            c.clip = clip;
            c.memdst = 0;
            c.dx = xs[ix];
            c.dy = ys[iy];
            c.dw = dw;
            c.dh = dh;
            c.exact = (ix < 8 && iy < 8);
            n++;
            bad += run_isolated(&c);
        }
    total += n;
    failed += bad;
    printf("%-26s %-8s clip %-3s%s dest %2dx%-2d: %3d cases, %d failing\n", opname[op],
           scaled ? (dw != IW ? "scaled" : "1:1 bit15") : "plain", clip ? "on" : "off", bigc ? "+" : " ",
           dw, dh, n, bad);
}

static void run_memory(int op, int scaled)
{
    long xs[] = { -3, -9, 0, 6, MW - 8, MW - 3, MW + 4, 1000 };
    long ys[] = { -3, -9, 0, 2, MH - 6, MH - 3, MH + 4, 1000 };
    int ix, iy, bad = 0, n = 0;

    for (iy = 0; iy < 8; iy++)
        for (ix = 0; ix < 8; ix++)
        {
            Case c;

            c.op = op;
            c.scaled = scaled;
            c.clip = 0;
            c.memdst = 1;
            c.dx = xs[ix];
            c.dy = ys[iy];
            c.dw = scaled ? 13 : IW;
            c.dh = scaled ? 9 : IH;
            c.exact = !scaled;
            n++;
            bad += run_isolated(&c);
        }
    total += n;
    failed += bad;
    printf("%-26s %-8s to memory form: %3d cases, %d failing\n", opname[op], scaled ? "scaled" : "plain", n, bad);
}

#if CONF_WITH_VDI_NVDI_EXT
/* ------------------------------------------------------------------ */
/*
 * The source rectangle past its form.  The source is a form of IW x IH
 * pixels in rows of 16 (fd_wdwidth 1): the form's memory is the 16 pixels of
 * a row and IH rows, whatever fd_w says; the rows past IH and the guards on
 * each side hold a poison that no copy may bring to the
 * destination.  The destination is on the screen, wholly, or (srcscreen) a
 * memory form, with the screen as the source.
 */
#define POISON 0xdead

typedef struct
{
    int op;                     /* OP_VRO, OP_VRT or OP_XFER */
    int scaled;
    int srcscreen;
    long sx, sy;                /* the first corner of the source rectangle */
    int sw, sh;                 /* its size */
    int dw, dh;                 /* the size of the destination (scaled) */
} SCase;

static int run_srccase(const SCase *c)
{
    enum { ROW = 16 };
    WORD contrl[11] = { 0 }, intin[3] = { 0 }, ptsin[8];
    size_t bytes = (size_t)ROW * IH * 2;
    UBYTE *sa = malloc(GUARD + bytes + GUARD);
    UBYTE *da = malloc(GUARD + (size_t)MW * MH * 2 + GUARD);
    UWORD *form = (UWORD *)(sa + GUARD), *mem = (UWORD *)(da + GUARD);
    Mfdb smf, dmf;
    GCBITMAP sbm;
    int dw = c->scaled ? c->dw : c->sw, dh = c->scaled ? c->dh : c->sh;
    long dx = c->srcscreen ? 2 : 10, dy = c->srcscreen ? 2 : 8;
    int bad = 0;

    memset(sa, 0xad, GUARD + bytes + GUARD);
    for (size_t i = 0; i < GUARD + bytes + GUARD; i += 2)
        *(UWORD *)(sa + i) = c->op == OP_VRT ? 0xffff : POISON;
    memset(da, 0xa5, GUARD + (size_t)MW * MH * 2 + GUARD);
    for (int i = 0; i < MW * MH; i++)
        mem[i] = 0x3333;
    fill_arena();
    if (c->op == OP_VRT)
        for (int y = 0; y < IH; y++)
        {
            form[y] = srcmono[y];
            for (int x = IW; x < ROW; x++)
                if (monobit(x, y))
                    form[y] |= (UWORD)(0x8000 >> x);
        }
    else
        for (int y = 0; y < IH; y++)
            for (int x = 0; x < ROW; x++)
                form[y * ROW + x] = src16[y][x];

    memset(&smf, 0, sizeof(smf));
    memset(&dmf, 0, sizeof(dmf));
    if (!c->srcscreen)
    {
        smf.fd_addr = form;
        smf.fd_w = mfdb_w;
        smf.fd_h = IH;
        smf.fd_wdwidth = c->op == OP_VRT ? 1 : ROW / 16;
        smf.fd_nplanes = c->op == OP_VRT ? 1 : 16;
    }
    else
    {
        dmf.fd_addr = mem;
        dmf.fd_w = mfdb_w;
        dmf.fd_h = MH;
        dmf.fd_wdwidth = MW / 16;
        dmf.fd_nplanes = 16;
    }
    vwk.clip = 0;
    ptsin[0] = (WORD)c->sx;
    ptsin[1] = (WORD)c->sy;
    ptsin[2] = (WORD)(c->sx + c->sw - 1);
    ptsin[3] = (WORD)(c->sy + c->sh - 1);
    ptsin[4] = (WORD)dx;
    ptsin[5] = (WORD)dy;
    ptsin[6] = (WORD)(dx + dw - 1);
    ptsin[7] = (WORD)(dy + dh - 1);
    CONTRL = contrl;
    INTIN = intin;
    PTSIN = ptsin;
    contrl[1] = 4;
    contrl[3] = 3;
    if (c->op == OP_XFER)
    {
        memset(&sbm, 0, sizeof(sbm));
        sbm.magic = GCBITMAP_MAGIC;
        sbm.length = sizeof(sbm);
        sbm.addr = (UBYTE *)form;
        sbm.width = ROW * 2;
        sbm.bits = 16;
        sbm.px_format = XF_PX_16;
        sbm.xmax = IW;
        sbm.ymax = IH;
        put_ptr(contrl + 7, &sbm);
        contrl[9] = contrl[10] = 0;
        vdi_vr_transfer_bits(&vwk);
    }
    else
    {
        put_ptr(contrl + 7, &smf);
        put_ptr(contrl + 9, c->srcscreen ? (void *)&dmf : (void *)&(Mfdb){ 0 });
        intin[0] = (WORD)((c->op == OP_VRO ? 3 : vrt_mode) | (c->scaled ? 0x8000 : 0));
        intin[1] = vrt_fg;
        intin[2] = vrt_bg;
        if (c->op == OP_VRO)
            vdi_vro_cpyfm(&vwk);
        else
            vdi_vrt_cpyfm(&vwk);
    }

    if (!guards_intact())
    {
        printf("  wrote outside the screen buffer: src %ld,%ld %dx%d op %d scaled %d\n", c->sx, c->sy, c->sw, c->sh, c->op, c->scaled);
        bad = 1;
    }
    /* what each destination pixel must be: the source pixel if it is in the form (or the screen), else untouched */
    for (int j = 0; j < dh; j++)
        for (int i = 0; i < dw; i++)
        {
            long sxp = c->sx + (long)i * c->sw / dw, syp = c->sy + (long)j * c->sh / dh;
            int sxmax = c->srcscreen ? SW : (c->scaled || c->op == OP_XFER) ? IW : ROW, symax = c->srcscreen ? SH : IH;
            int inside = sxp >= 0 && syp >= 0 && sxp < sxmax && syp < symax;
            UWORD got, want, base;

            if (c->srcscreen)
            {
                got = (dx + i < MW && dy + j < MH) ? mem[(dy + j) * MW + dx + i] : 0x3333;
                base = 0x3333;
                want = inside ? bg((int)sxp, (int)syp) : base;
            }
            else
            {
                got = screen[(dy + j) * SW + dx + i];
                base = bg((int)dx + i, (int)dy + j);
                want = inside ? (c->op == OP_VRT ? vrt_val(monobit((int)sxp, (int)syp), base) : src16[syp][sxp]) : base;
            }
            /* a copy that does not take the whole rectangle (scaled, 170) draws nothing: both are right */
            /* a scaled vrt_cpyfm stages its row in the 68k's byte order: see above, only fg or nothing is checked */
            if (c->op == OP_VRT && c->scaled && (got == base || got == pal16[vrt_fg] || got == pal16[vrt_bg]))
                continue;
            if (got != want && !((c->scaled || c->op == OP_XFER) && got == base))
            {
                printf("  pixel %d,%d: src %ld,%ld %dx%d op %d scaled %d srcscreen %d: got %04x want %04x\n", i, j,
                       c->sx, c->sy, c->sw, c->sh, c->op, c->scaled, c->srcscreen, got, want);
                if (++bad > 3)
                    break;
            }
        }
    /* and nothing outside the destination rectangle changed, on the screen or in the form */
    for (int y = 0; y < (c->srcscreen ? MH : SH) && bad < 4; y++)
        for (int x = 0; x < (c->srcscreen ? MW : SW); x++)
        {
            UWORD got = c->srcscreen ? mem[y * MW + x] : screen[y * SW + x];
            UWORD base = c->srcscreen ? 0x3333 : bg(x, y);

            if ((x < dx || x >= dx + dw || y < dy || y >= dy + dh) && got != base)
            {
                printf("  pixel %d,%d outside the destination changed: src %ld,%ld op %d\n", x, y, c->sx, c->sy, c->op);
                bad++;
                break;
            }
        }
    {
        size_t mb = (size_t)MW * MH * 2;

        for (size_t i = 0; i < GUARD; i++)
            if (da[i] != 0xa5 || da[GUARD + mb + i] != 0xa5)
            {
                printf("  wrote outside the memory form\n");
                bad = 1;
                break;
            }
    }
    free(sa);
    free(da);
    return bad;
}

static int run_isolated_src(const SCase *c)
{
    int status;
    pid_t pid;

    fflush(stdout);
    pid = fork();
    if (pid == 0)
    {
        int r;

        if (!getenv("CLAMP_VERBOSE") && !freopen("/dev/null", "w", stderr))
            _exit(3);
        r = run_srccase(c);
        fflush(stdout);
        _exit(r ? 1 : 0);
    }
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status))
        printf("  killed: src %ld,%ld %dx%d op %d scaled %d srcscreen %d\n", c->sx, c->sy, c->sw, c->sh, c->op, c->scaled,
               c->srcscreen);
    return !WIFEXITED(status) || WEXITSTATUS(status) != 0;
}

static void run_source_group(int op, int scaled, int srcscreen)
{
    long fw = srcscreen ? SW : IW, fh = srcscreen ? SH : IH;
    long xs[] = { -3, -9, 0, 2, fw - 5, fw - 2, fw + 3, -(long)30000, 30000 };
    long ys[] = { -2, -9, 0, 1, fh - 4, fh - 1, fh + 3, -(long)30000, 30000 };
    int ix, iy, n = 0, bad = 0;

    for (iy = 0; iy < 9; iy++)
        for (ix = 0; ix < 9; ix++)
        {
            SCase c = { op, scaled, srcscreen, xs[ix], ys[iy], 5, 4, 7, 6 };

            n++;
            bad += run_isolated_src(&c);
        }
    total += n;
    failed += bad;
    printf("%-26s %-8s source %-6s past its edges: %3d cases, %d failing\n", opname[op], scaled ? "scaled" : "plain",
           srcscreen ? "screen" : "form", n, bad);
}


#endif

/* ------------------------------------------------------------------ */
/* the rest runs in a child too, with a time limit (a loop that does not end is a failure) */

static int in_child(int (*fn)(const void *), const void *arg, const char *what)
{
    int status;
    pid_t pid;

    fflush(stdout);
    pid = fork();
    if (pid == 0)
    {
        int r;

        if (!getenv("CLAMP_VERBOSE") && !freopen("/dev/null", "w", stderr))
            _exit(3);
        alarm(60);
        r = fn(arg);
        fflush(stdout);
        _exit(r > 100 ? 100 : r);
    }
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status))
    {
        printf("  killed (sanitizer, signal or time): %s\n", what);
        return 1;
    }
    return WEXITSTATUS(status);
}

/* ------------------------------------------------------------------ */
/* forms of 1 to 8 planes: the planar blit of the C code */

typedef struct
{
    int planes;
    int op;                     /* OP_VRO or OP_VRT */
    int mode;                   /* vrt_cpyfm's */
} PCase;

#define PSW 16                  /* the source form: one word wide */
#define PSH 6
#define PDW 32                  /* the destination form: two words wide */
#define PDH 10

static unsigned pget(const UWORD *f, int wd, int planes, int x, int y)
{
    unsigned v = 0;

    for (int pl = 0; pl < planes; pl++)
        if (f[(y * wd + x / 16) * planes + pl] & (0x8000 >> (x % 16)))
            v |= 1u << pl;
    return v;
}

static void pset(UWORD *f, int wd, int planes, int x, int y, unsigned v)
{
    for (int pl = 0; pl < planes; pl++)
    {
        UWORD *w = &f[(y * wd + x / 16) * planes + pl];

        if (v & (1u << pl))
            *w |= (UWORD)(0x8000 >> (x % 16));
        else
            *w &= (UWORD)~(0x8000 >> (x % 16));
    }
}

static int run_planar(const void *vc)
{
    const PCase *c = vc;
    int p = c->planes, mask = (1 << p) - 1, bad = 0, n = 0;
    int fg = p == 1 ? 1 : mask, bgi = p == 1 ? 0 : 1;
    size_t sbytes = (size_t)PSW / 16 * PSH * p * 2, dbytes = (size_t)PDW / 16 * PDH * p * 2;
    size_t sm = c->op == OP_VRT ? (size_t)PSW / 16 * PSH * 2 : sbytes;
    UBYTE *sa = malloc(GUARD + sm + GUARD), *da = malloc(GUARD + dbytes + GUARD);
    UWORD *sf = (UWORD *)(sa + GUARD), *df = (UWORD *)(da + GUARD);
    UWORD *dsave = malloc(dbytes);
    static const long dxs[] = { -3, 0, 10, 28, 31, 35, -8 }, dys[] = { -2, 0, 4, 8, 9, 12, -5 };
#if CONF_WITH_VDI_NVDI_EXT
    static const long sxs[] = { -2, 0, 12, 20 }, sys[] = { 0, 3, 8 };
#else
    static const long sxs[] = { 0, 11, 0, 0 }, sys[] = { 0, 2, 0 };    /* the forms are trusted: the source is in them */
#endif

    memset(sa, 0xad, GUARD + sm + GUARD);
    memset(da, 0xa5, GUARD + dbytes + GUARD);
    memset(sf, 0, sm);
    for (int y = 0; y < PSH; y++)
        for (int x = 0; x < PSW; x++)
            if (c->op == OP_VRT)
            {
                if (monobit(x, y))
                    sf[y] |= (UWORD)(0x8000 >> x);
            }
            else
                pset(sf, PSW / 16, p, x, y, (unsigned)(x * 7 + y * 2 + 3) & mask);

    for (size_t ix = 0; ix < 7; ix++)
        for (size_t iy = 0; iy < 7; iy++)
            for (size_t jx = 0; jx < 4; jx++)
                for (size_t jy = 0; jy < 3; jy++)
                {
                    long dx = dxs[ix], dy = dys[iy], sx = sxs[jx], sy = sys[jy];
                    WORD contrl[11] = { 0 }, intin[3] = { 0 }, ptsin[8];
                    Mfdb smf, dmf;

                    memset(dsave, 0, dbytes);
                    memset(df, 0, dbytes);
                    for (int y = 0; y < PDH; y++)
                        for (int x = 0; x < PDW; x++)
                            pset(df, PDW / 16, p, x, y, (unsigned)(x * 5 + y * 3 + 1) & mask);
                    memcpy(dsave, df, dbytes);
                    memset(&smf, 0, sizeof(smf));
                    memset(&dmf, 0, sizeof(dmf));
                    smf.fd_addr = sf;
                    smf.fd_w = PSW;
                    smf.fd_h = PSH;
                    smf.fd_wdwidth = PSW / 16;
                    smf.fd_nplanes = c->op == OP_VRT ? 1 : p;
                    dmf.fd_addr = df;
                    dmf.fd_w = 0;               /* not what bounds it */
                    dmf.fd_h = PDH;
                    dmf.fd_wdwidth = PDW / 16;
                    dmf.fd_nplanes = p;
                    ptsin[0] = (WORD)sx;
                    ptsin[1] = (WORD)sy;
                    ptsin[2] = (WORD)(sx + 4);
                    ptsin[3] = (WORD)(sy + 3);
                    ptsin[4] = (WORD)dx;
                    ptsin[5] = (WORD)dy;
                    ptsin[6] = (WORD)(dx + 4);
                    ptsin[7] = (WORD)(dy + 3);
                    CONTRL = contrl;
                    INTIN = intin;
                    PTSIN = ptsin;
                    contrl[1] = 4;
                    contrl[3] = 3;
                    put_ptr(contrl + 7, &smf);
                    put_ptr(contrl + 9, &dmf);
                    vwk.clip = 0;
                    if (c->op == OP_VRO)
                    {
                        intin[0] = 3;
                        vdi_vro_cpyfm(&vwk);
                    }
                    else
                    {
                        intin[0] = (WORD)c->mode;
                        intin[1] = (WORD)fg;
                        intin[2] = (WORD)bgi;
                        vdi_vrt_cpyfm(&vwk);
                    }
                    n++;
                    for (int y = 0; y < PDH; y++)
                        for (int x = 0; x < PDW; x++)
                        {
                            unsigned old = pget(dsave, PDW / 16, p, x, y), want = old;
                            long i = x - dx, j = y - dy;

                            if (i >= 0 && j >= 0 && i < 5 && j < 4 && sx + i >= 0 && sx + i < PSW && sy + j >= 0 &&
                                sy + j < PSH)
                            {
                                if (c->op == OP_VRO)
                                    want = (unsigned)((sx + i) * 7 + (sy + j) * 2 + 3) & mask;
                                else
                                {
                                    int bit = monobit((int)(sx + i), (int)(sy + j));

                                    want = c->mode == MD_REPLACE ? (bit ? fg : bgi) :
                                           c->mode == MD_ERASE ? (bit ? old : bgi) : (bit ? fg : old);
                                }
                            }
                            if (pget(df, PDW / 16, p, x, y) != want && bad++ < 3)
                                printf("  planar %d op %d mode %d dest %ld,%ld src %ld,%ld pixel %d,%d: got %x want %x\n", p,
                                       c->op, c->mode, dx, dy, sx, sy, x, y, pget(df, PDW / 16, p, x, y), want);
                        }
                    for (size_t i = 0; i < GUARD; i++)
                        if (da[i] != 0xa5 || da[GUARD + dbytes + i] != 0xa5 || sa[i] != 0xad ||
                            sa[GUARD + sm + i] != 0xad)
                        {
                            printf("  planar %d: wrote outside a form (dest %ld,%ld src %ld,%ld)\n", p, dx, dy, sx, sy);
                            return 100;
                        }
                }
    free(sa);
    free(da);
    free(dsave);
    total += n;
    return bad;
}

static void run_planar_groups(void)
{
    static const int planes[] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    static const int modes[] = { 0, MD_TRANS, MD_REPLACE, MD_ERASE };

    for (int k = 0; k < 8; k++)
        for (int m = 0; m < 4; m++)
        {
            PCase c = { planes[k], m ? OP_VRT : OP_VRO, modes[m] };
            int bad = in_child(run_planar, &c, "planar");

            failed += bad;
            printf("%d planes %-9s mode %d: %d failing\n", c.planes, m ? "vrt_cpyfm" : "vro_cpyfm", c.mode, bad);
        }
}

#if CONF_WITH_VDI_NVDI_EXT
/* ------------------------------------------------------------------ */
/*
 * Forms that are not what they should be, from a program that does not care
 * (nothing a user must be able to do to the kernel with one): each must draw
 * nothing and touch nothing outside the forms, or at least not run on.
 */
#define RB 70000                /* bytes of the form's own memory */
#define RG 131072               /* bytes of guard on each side */

typedef struct
{
    const char *name;
    int op;                     /* OP_VRO or OP_VRT */
    int scaled;
    int sform, dform;           /* 0: the screen, 1: a memory form, 2: a NULL pointer to a form */
    Mfdb s, d;
    int sodd, dodd;             /* add one to the address */
    WORD pts[8];
    int nothing;                /* nothing may change; else only safety (the guards, and ending) */
} RCase;

static int run_refusal(const void *vc)
{
    const RCase *c = vc;
    WORD contrl[11] = { 0 }, intin[3] = { 0 };
    UBYTE *sa = malloc(RG + RB + RG), *da = malloc(RG + RB + RG), *sb = sa + RG, *db = da + RG;
    Mfdb s = c->s, d = c->d, zero = { 0 };
    int bad = 0;

    fill_arena();
    for (size_t i = 0; i < RG + RB + RG; i += 2)
        *(UWORD *)(sa + i) = c->op == OP_VRT ? 0xffff : 0xdead;
    memset(da, 0xa5, RG + RB + RG);
    memset(db, 0x33, RB);
    s.fd_addr = c->sform == 1 ? sb + c->sodd : NULL;
    d.fd_addr = c->dform == 1 ? db + c->dodd : NULL;
    CONTRL = contrl;
    INTIN = intin;
    PTSIN = (WORD *)c->pts;
    contrl[1] = 4;
    contrl[3] = 3;
    vwk.clip = 0;
    if (c->sform == 2)
        put_ptr(contrl + 7, NULL);
    else
        put_ptr(contrl + 7, c->sform ? (void *)&s : (void *)&zero);
    if (c->dform == 2)
        put_ptr(contrl + 9, NULL);
    else
        put_ptr(contrl + 9, c->dform ? (void *)&d : (void *)&zero);
    intin[0] = (WORD)((c->op == OP_VRO ? 3 : MD_REPLACE) | (c->scaled ? 0x8000 : 0));
    intin[1] = 1;
    intin[2] = 2;
    if (c->op == OP_VRO)
        vdi_vro_cpyfm(&vwk);
    else
        vdi_vrt_cpyfm(&vwk);

    if (!guards_intact())
    {
        printf("  %s: wrote outside the screen buffer\n", c->name);
        bad++;
    }
    for (size_t i = 0; i < RG; i++)
        if (da[i] != 0xa5 || da[RG + RB + i] != 0xa5)
        {
            printf("  %s: wrote outside the destination form\n", c->name);
            bad++;
            break;
        }
    if (c->nothing)
    {
        for (int y = 0; y < SH; y++)
            for (int x = 0; x < SW; x++)
                if (screen[y * SW + x] != bg(x, y) && bad++ < 3)
                    printf("  %s: the screen changed at %d,%d\n", c->name, x, y);
        for (size_t i = 0; i < RB; i++)
            if (db[i] != 0x33)
            {
                printf("  %s: the destination form changed at byte %zu\n", c->name, i);
                bad++;
                break;
            }
    }
    free(sa);
    free(da);
    return bad;
}

/* a NULL destination is the screen: the copy is drawn there */
static int run_nulldst(const void *vc)
{
    WORD contrl[11] = { 0 }, intin[3] = { 0 }, ptsin[8] = { 0, 0, 7, 0, 3, 2, 10, 2 };
    Mfdb s;
    int bad = 0;

    (void)vc;
    fill_arena();
    memset(&s, 0, sizeof(s));
    s.fd_addr = src16;
    s.fd_w = IW;
    s.fd_h = IH;
    s.fd_wdwidth = 1;
    s.fd_nplanes = 16;
    CONTRL = contrl;
    INTIN = intin;
    PTSIN = ptsin;
    contrl[1] = 4;
    contrl[3] = 3;
    put_ptr(contrl + 7, &s);
    put_ptr(contrl + 9, NULL);
    intin[0] = 3;
    vwk.clip = 0;
    vdi_vro_cpyfm(&vwk);
    for (int y = 0; y < SH; y++)
        for (int x = 0; x < SW; x++)
        {
            UWORD want = (y == 2 && x >= 3 && x <= 10) ? src16[0][x - 3] : bg(x, y);

            if (screen[y * SW + x] != want && bad++ < 3)
                printf("  NULL destination: pixel %d,%d got %04x want %04x\n", x, y, screen[y * SW + x], want);
        }
    return bad + !guards_intact();
}

static void run_refusals(void)
{
    static RCase rc[40];
    int n = 0, bad = 0, i;
    Mfdb ok16 = { 0, 16, 1, 1, 0, 16, 0, 0, 0 };

#define ADD(...) do { rc[n] = (RCase){ __VA_ARGS__ }; n++; } while (0)
#define PTS8 { 0, 0, 7, 0, 0, 0, 7, 0 }
    /* C1: a plane count that is none of 1 to 8 and 16 */
    for (int k = 0; k < 4; k++)
    {
        static const WORD pl[] = { 0, -1, 15, 9 };
        Mfdb f = ok16, m = { 0, 16, 1, 1, 0, 1, 0, 0, 0 };

        f.fd_nplanes = pl[k];
        ADD("planes both", OP_VRO, 0, 1, 1, f, f, 0, 0, PTS8, 1);
        ADD("planes vrt dest", OP_VRT, 0, 1, 1, m, f, 0, 0, PTS8, 1);
    }
    /* C2: a row of 32768 bytes (the stride is a WORD) */
    {
        Mfdb big = ok16;

        big.fd_wdwidth = 1024;
        big.fd_h = 2;
        big.fd_w = 16384;
        ADD("stride dest", OP_VRO, 0, 0, 1, ok16, big, 0, 0, { 0, 0, 7, 0, 0, 1, 7, 1 }, 1);
        ADD("stride dest scaled", OP_VRO, 1, 0, 1, ok16, big, 0, 0, { 0, 0, 7, 0, 0, 1, 15, 1 }, 1);
        ADD("stride source", OP_VRO, 0, 1, 0, big, ok16, 0, 0, { 0, 1, 7, 1, 0, 0, 7, 0 }, 1);
    }
    /* I1: a form with no size, or a negative one */
    for (int k = 0; k < 6; k++)
    {
        static const WORD wd[] = { 1, 1, 1, -2048, -1, 0 }, h[] = { -32768, -1, 0, 1, 1, 1 };
        Mfdb f = ok16;

        f.fd_wdwidth = wd[k];
        f.fd_h = h[k];
        ADD("size source", OP_VRO, 0, 1, 0, f, ok16, 0, 0, PTS8, 1);
        ADD("size dest", OP_VRO, 0, 0, 1, ok16, f, 0, 0, PTS8, 1);
    }
    /* I2: no source */
    ADD("NULL source", OP_VRO, 0, 2, 0, ok16, ok16, 0, 0, PTS8, 1);
    ADD("NULL source", OP_VRO, 1, 2, 0, ok16, ok16, 0, 0, { 0, 0, 7, 0, 0, 0, 15, 0 }, 1);
    /* M5: an odd address */
    ADD("odd source", OP_VRO, 0, 1, 0, ok16, ok16, 1, 0, PTS8, 1);
    ADD("odd dest", OP_VRO, 0, 0, 1, ok16, ok16, 0, 1, PTS8, 1);
    ADD("odd dest scaled", OP_VRO, 1, 0, 1, ok16, ok16, 0, 1, { 0, 0, 7, 0, 0, 0, 15, 0 }, 1);
    /* C3: wide forms and the far ends of the coordinates: whatever is drawn stays inside the forms and ends */
    {
        Mfdb m1 = { 0, 32767, 1, 2048, 0, 1, 0, 0, 0 }, m2 = { 0, 16, 1, 4400, 0, 1, 0, 0, 0 };

        ADD("wide source", OP_VRO, 0, 1, 1, m1, m2, 0, 0, { -10000, 0, 30000, 0, 30000, 0, 30000, 0 }, 0);
        ADD("wide mono source", OP_VRT, 0, 1, 0, m1, ok16, 0, 0, { 32740, 0, 32767, 0, 0, 0, 27, 0 }, 0);
        ADD("wide mono source to form", OP_VRT, 0, 1, 1, m1, m2, 0, 0, { 32740, 0, 32767, 0, 0, 0, 27, 0 }, 0);
    }
#undef ADD
    for (i = 0; i < n; i++)
    {
        int r = in_child(run_refusal, &rc[i], rc[i].name);

        total++;
        if (r)
            printf("  ^ refusal case %d: %s op %d scaled %d\n", i, rc[i].name, rc[i].op, rc[i].scaled);
        bad += r != 0;
    }
    if (in_child(run_nulldst, NULL, "NULL destination"))
        bad++;
    total++;
    failed += bad;
    printf("forms that are not what they should be: %d cases, %d failing\n", n + 1, bad);
}

#endif

/* ------------------------------------------------------------------ */
/* a copy within one form: the screen scrolled, or a form onto itself */

typedef struct
{
    int memory, clip;
} SameCase;

static int run_same(const void *vc)
{
    const SameCase *c = vc;
    static const long oxs[] = { -9, -3, 0, 2, 5, 25, -20 }, oys[] = { -5, -1, 0, 2, 3, 8, -8 };
    int bad = 0;
    int fw = c->memory ? MW : SW, fh = c->memory ? MH : SH;
    UWORD *snap = malloc(sizeof(UWORD) * 1200);
    UBYTE *fa = malloc(GUARD + (size_t)MW * MH * 2 + GUARD);
    UWORD *mem = (UWORD *)(fa + GUARD);

    for (int ix = 0; ix < 7; ix++)
        for (int iy = 0; iy < 7; iy++)
        {
            long ox = oxs[ix], oy = oys[iy];
            WORD contrl[11] = { 0 }, intin[3] = { 0 }, ptsin[8];
            int sx1 = 2, sy1 = 1, w = c->memory ? 12 : 20, h = c->memory ? 6 : 12;
            Mfdb f;
            UWORD *form;
            int bx1 = 0, by1 = 0, bx2 = fw - 1, by2 = fh - 1;

            fill_arena();
            memset(fa, 0xa5, GUARD + (size_t)MW * MH * 2 + GUARD);
            for (int y = 0; y < MH; y++)
                for (int x = 0; x < MW; x++)
                    mem[y * MW + x] = bg(x, y);
            form = c->memory ? mem : screen;
            memset(&f, 0, sizeof(f));
            if (c->memory)
            {
                f.fd_addr = mem;
                f.fd_w = MW;
                f.fd_h = MH;
                f.fd_wdwidth = MW / 16;
                f.fd_nplanes = 16;
            }
            for (int y = 0; y < fh; y++)
                for (int x = 0; x < fw; x++)
                    snap[y * fw + x] = form[y * fw + x];
            vwk.clip = c->clip && !c->memory;
            vwk.xmn_clip = cl[0];
            vwk.ymn_clip = cl[1];
            vwk.xmx_clip = cl[2];
            vwk.ymx_clip = cl[3];
            if (vwk.clip)
            {
                bx1 = cl[0]; by1 = cl[1]; bx2 = cl[2]; by2 = cl[3];
            }
            ptsin[0] = sx1; ptsin[1] = sy1; ptsin[2] = (WORD)(sx1 + w - 1); ptsin[3] = (WORD)(sy1 + h - 1);
            ptsin[4] = (WORD)(sx1 + ox); ptsin[5] = (WORD)(sy1 + oy);
            ptsin[6] = (WORD)(sx1 + ox + w - 1); ptsin[7] = (WORD)(sy1 + oy + h - 1);
            CONTRL = contrl; INTIN = intin; PTSIN = ptsin;
            contrl[1] = 4; contrl[3] = 3;
            put_ptr(contrl + 7, &f);
            put_ptr(contrl + 9, &f);
            intin[0] = 3;
            vdi_vro_cpyfm(&vwk);
            for (int y = 0; y < fh; y++)
                for (int x = 0; x < fw; x++)
                {
                    long i = x - (sx1 + ox), j = y - (sy1 + oy);
                    UWORD want = snap[y * fw + x];

                    if (i >= 0 && j >= 0 && i < w && j < h && x >= bx1 && x <= bx2 && y >= by1 && y <= by2)
                        want = snap[(sy1 + j) * fw + sx1 + i];
                    if (form[y * fw + x] != want && bad++ < 4)
                        printf("  same form %s clip %d offset %ld,%ld pixel %d,%d: got %04x want %04x\n",
                               c->memory ? "memory" : "screen", c->clip, ox, oy, x, y, form[y * fw + x], want);
                }
            if (!guards_intact())
            {
                printf("  same form: wrote outside the screen buffer\n");
                bad++;
            }
            for (size_t i = 0; i < GUARD; i++)
                if (fa[i] != 0xa5 || fa[GUARD + (size_t)MW * MH * 2 + i] != 0xa5)
                {
                    printf("  same form: wrote outside the form\n");
                    bad++;
                    break;
                }
        }
    free(snap);
    free(fa);
    return bad;
}

static void run_same_groups(void)
{
    for (int m = 0; m < 2; m++)
        for (int clip = 0; clip < 2; clip++)
        {
            SameCase c = { m, clip };
            int bad;

            if (m && clip)
                continue;
            bad = in_child(run_same, &c, "same form");
            total += 49;
            failed += bad;
            printf("vro_cpyfm onto its own %s%s: 49 cases, %d failing\n", m ? "memory form" : "screen",
                   clip ? ", clip on" : "", bad);
        }
}

/* three ways to ask vrt_cpyfm: the writing mode and the two colour indices */
static void set_vrt(int v)
{
    static const WORD t[3][3] = { { MD_TRANS, 3, 0 }, { MD_REPLACE, 1, 2 }, { MD_ERASE, 2, 3 } };

    vrt_mode = t[v][0];
    vrt_fg = t[v][1];
    vrt_bg = t[v][2];
    printf("-- vrt_cpyfm mode %d, colours %d and %d\n", vrt_mode, vrt_fg, vrt_bg);
}

#if CONF_WITH_VDI_NVDI_EXT
#define LAST_OP OP_BLEND
#else
#define LAST_OP OP_VRT          /* the targets without the NVDI extensions: no scaling, no vr_transfer_bits */
#endif

int main(void)
{
    int op, cl_on;

    arena = malloc(GUARD + SW * SH * 2 + GUARD);
    screen = (UWORD *)(arena + GUARD);
    make_sources();
    v_bas_ad = (UBYTE *)screen;
    v_lin_wr = SW * 2;
    v_planes = 16;
    DEV_TAB[0] = SW - 1;
    DEV_TAB[1] = SH - 1;
    for (int i = 0; i < 256; i++)
        MAP_COL[i] = REV_MAP_COL[i] = (WORD)i;
    memset(&vwk, 0, sizeof(vwk));
    memset(palette_store, 0, sizeof(palette_store));
    for (int i = 0; i < 4; i++)
        palette_store[i] = pal16[i];
    memcpy(ext_store.palette, palette_store, sizeof(palette_store));
    vwk.ext = &ext_store;
    CUR_WORK = &vwk;

    /* sanity: a copy wholly on the screen, before any of the rest means anything */
    {
        Case c = { OP_VRO, 0, 0, 0, 10, 8, IW, IH, 1 };

        if (run_isolated(&c))
        {
            printf("the copy of reference does not work even on the screen: the test is wrong\n");
            return 2;
        }
    }

    for (op = OP_VRO; op <= LAST_OP; op++)
        for (int v = 0; v < (op == OP_VRT ? 3 : 1); v++)
    {
        if (op == OP_VRT)
            set_vrt(v);
        /* vr_transfer_bits has no bit 15; its destination may be of another size.  A reduction of a
           direct source onto 16 bits is filtered, so it is only enlarged here. */
        for (cl_on = 0; cl_on < 3; cl_on++)
        {
            int clip = cl_on != 0, bigc = cl_on == 2;

            run_group(op, 0, clip, bigc, IW, IH);
#if CONF_WITH_VDI_NVDI_EXT
            if (op == OP_VRO || op == OP_VRT)
            {
                run_group(op, 1, clip, bigc, 13, 9);
                run_group(op, 1, clip, bigc, 5, 4);
            }
            else
                run_group(op, 1, clip, bigc, 13, 9);
#endif
        }
    }
    bigclip = 0;
    /* fd_w is not what bounds a form: its memory is fd_wdwidth words a row and fd_h rows */
    {
        static const WORD fws[] = { MFDBW, 0, 3 };

        for (int k = 0; k < 3; k++)
        {
            mfdb_w = fws[k];
            printf("-- fd_w %d\n", mfdb_w);
            run_memory(OP_VRO, 0);
            for (int v = 0; v < 3; v++)
            {
                set_vrt(v);
                run_memory(OP_VRT, 0);
#if CONF_WITH_VDI_NVDI_EXT
                run_memory(OP_VRT, 1);
#endif
            }
#if CONF_WITH_VDI_NVDI_EXT
            run_memory(OP_VRO, 1);
#endif
        }
#if CONF_WITH_VDI_NVDI_EXT
        for (int k = 0; k < 3; k++)
        {
            static const WORD sfws[] = { IW, 0, 3 };

            mfdb_w = sfws[k];
            printf("-- fd_w %d\n", mfdb_w);
            for (op = OP_VRO; op <= OP_XFER; op++)
                for (int v = 0; v < (op == OP_VRT ? 3 : 1); v++)
                {
                    if (op == OP_VRT)
                        set_vrt(v);
                    run_source_group(op, 0, 0);
                    if (op != OP_XFER)
                        run_source_group(op, 1, 0);
                }
            set_vrt(0);
            run_source_group(OP_VRO, 0, 1);
        }
#endif
        mfdb_w = MFDBW;
    }
    /* line-A: the same copy, without a clip flag */
    run_group(OP_LINEA, 0, 0, 0, IW, IH);

    run_same_groups();
    run_planar_groups();
#if CONF_WITH_VDI_NVDI_EXT
    run_refusals();
#endif

    printf("test_clamp: %d cases, %d failures\n", total, failed);
    return failed ? 1 : 0;
}
