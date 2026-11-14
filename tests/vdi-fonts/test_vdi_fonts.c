/*
 * Host checks for the outline font engine of EmuTOS's VDI (vdi/vdi_ft2.c).
 *
 * The engine, the FreeType subset and its character set tables are built
 * from the EmuTOS tree with 32-bit long, as on the target, and run against
 * host_ftos.c.  Glyph images are compared with what FreeType itself
 * renders for the glyph the character should map to, using a separate
 * FreeType library on malloc() so it neither counts as the engine's memory
 * nor fails with it.
 *
 * usage: test_vdi_fonts <sans.ttf> <second.ttf> <mono.ttf>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "vdi_ft2.h"
#include "vdi_ftmap.h"
#include "host_ftos.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MODULE_H

#define PX  16                  /* the size of the em used throughout */
#define PT  12                  /* ... given as 12 pt at 96 dpi */

static int checks, failures;

#define CHECK(cond, ...) \
    do { \
        checks++; \
        if (!(cond)) { \
            failures++; \
            printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
            printf(__VA_ARGS__); \
            printf("\n"); \
        } \
    } while (0)

static const char *sans_path, *second_path, *mono_path;
static char tmpdir[512];


/*
 * the reference: FreeType on its own, on malloc()
 */

static void *ref_alloc(FT_Memory m, long size) { (void)m; return malloc(size); }
static void ref_free(FT_Memory m, void *b) { (void)m; free(b); }
static void *ref_realloc(FT_Memory m, long cur, long size, void *b)
{
    (void)m; (void)cur;
    return realloc(b, size);
}
static struct FT_MemoryRec_ ref_memory = { NULL, ref_alloc, ref_free, ref_realloc };

static FT_Library ref_lib;
static FT_Face ref_face;
static unsigned char *ref_data;

static unsigned char *read_file(const char *path, long *size)
{
    FILE *f = fopen(path, "rb");
    unsigned char *data;
    long n;

    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = malloc(n);
    if (data && fread(data, 1, n, f) != (size_t)n)
    {
        free(data);
        data = NULL;
    }
    fclose(f);
    if (size)
        *size = n;
    return data;
}

static int ref_open(const char *path)
{
    long size;

    if (FT_New_Library(&ref_memory, &ref_lib))
        return 0;
    FT_Add_Default_Modules(ref_lib);
    ref_data = read_file(path, &size);
    if (!ref_data || FT_New_Memory_Face(ref_lib, ref_data, size, 0, &ref_face))
        return 0;
    return FT_Set_Pixel_Sizes(ref_face, 0, PX) == 0;
}

static void ref_close(void)
{
    FT_Done_Face(ref_face);
    FT_Done_Library(ref_lib);
    free(ref_data);
}

static FT_UInt ref_index(unsigned long code)
{
    return FT_Get_Char_Index(ref_face, code);
}

/* the glyph index fVDI picks for an Atari code when there is no SPDCHAR.MAP */
static FT_UInt ref_atari_index(unsigned ch, unsigned *unicode)
{
    unsigned cc = Bics2Unicode[Atari2Bics[ch]];
    FT_UInt gi = ref_index(cc == 0xffff ? (FT_ULong)-1 : cc);

    *unicode = gi ? cc : 0xffff;
    if (!gi)
    {
        if (cc == 0xffff)
            cc = ch;
        gi = ref_index(cc);
    }
    return gi;
}


/*
 * helpers
 */

static long to_words(const char *s, WORD *w)
{
    long n = 0;

    while (*s)
        w[n++] = (UBYTE)*s++;
    return n;
}

static int pixel(const VdiOutlineBitmap *b, long x, long y)
{
    const UBYTE *row = (const UBYTE *)b->data + y * b->wdwidth * 2;

    return (row[x >> 3] >> (7 - (x & 7))) & 1;
}

static long ink(const VdiOutlineBitmap *b)
{
    long x, y, n = 0;

    for (y = 0; y < b->height; y++)
        for (x = 0; x < b->width; x++)
            n += pixel(b, x, y);
    return n;
}

static long sum_advances(VdiOutlineFont *f, const char *s, long map, long effects)
{
    LONG adv[4];
    long sum = 0;

    for (; *s; s++)
    {
        if (vdi_ft_char_advance(f, (UBYTE)*s, map, effects, adv) < 0)
            return -1;
        sum += adv[0];
    }
    return sum;
}

/*
 * render one character and compare it with FreeType's own image of glyph
 * gi; returns 1 if every pixel agrees (rows outside the cell may be cut)
 */
static int same_as_reference(VdiOutlineFont *f, long ch, long map, FT_UInt gi,
                             const char *what)
{
    VdiOutlineBitmap b;
    FT_Bitmap *rb;
    WORD w = (WORD)ch;
    long x, y, bad = 0, lost = 0, left, top;

    if (vdi_ft_render(f, &w, 1, map, 0, 0, &b) < 0 || !b.data)
    {
        printf("  %s: code 0x%lx did not render\n", what, ch);
        return 0;
    }
    if (FT_Load_Glyph(ref_face, gi, FT_LOAD_DEFAULT) ||
        FT_Render_Glyph(ref_face->glyph, FT_RENDER_MODE_MONO))
    {
        vdi_ft_release(&b);
        printf("  %s: reference glyph %u did not render\n", what, gi);
        return 0;
    }
    rb = &ref_face->glyph->bitmap;
    left = b.origin_x + ref_face->glyph->bitmap_left;
    top = b.origin_y - ref_face->glyph->bitmap_top;

    for (y = 0; y < b.height; y++)
    {
        for (x = 0; x < b.width; x++)
        {
            long rx = x - left, ry = y - top;
            int want = 0;

            if (rx >= 0 && ry >= 0 && rx < (long)rb->width && ry < (long)rb->rows)
                want = (rb->buffer[ry * rb->pitch + (rx >> 3)] >> (7 - (rx & 7))) & 1;
            if (pixel(&b, x, y) != want)
                bad++;
        }
    }
    /* reference pixels that fall outside the bitmap: only above or below */
    for (y = 0; y < (long)rb->rows; y++)
        for (x = 0; x < (long)rb->width; x++)
            if ((rb->buffer[y * rb->pitch + (x >> 3)] >> (7 - (x & 7))) & 1)
            {
                long bx = x + left;

                if (bx < 0 || bx >= b.width)
                    lost++;
            }
    if (bad || lost)
        printf("  %s: code 0x%lx glyph %u: %ld pixels differ, %ld cut off at the sides\n",
               what, ch, gi, bad, lost);
    vdi_ft_release(&b);
    return !bad && !lost;
}

static void make_tmpdir(void)
{
    const char *base = getenv("TMPDIR");

    snprintf(tmpdir, sizeof(tmpdir), "%s/vdi-fonts-XXXXXX",
             (base && *base) ? base : "/tmp");
    if (!mkdtemp(tmpdir))
    {
        perror(tmpdir);
        exit(2);
    }
}

static void tmpname(char *buf, size_t len, const char *name)
{
    snprintf(buf, len, "%s/%s", tmpdir, name);
}

static int write_file(const char *path, const void *data, long size)
{
    FILE *f = fopen(path, "wb");

    if (!f)
        return 0;
    if (size && fwrite(data, 1, size, f) != (size_t)size)
    {
        fclose(f);
        return 0;
    }
    return fclose(f) == 0;
}

static VdiOutlineFont *start(const char *path)
{
    CHECK(vdi_ft_init() == 0, "FreeType did not start");
    CHECK(vdi_ft_register_file(path) == 1, "%s", path);
    return vdi_ft_face_count() > 0 ? vdi_ft_face(0) : NULL;
}

static void stop(void)
{
    vdi_ft_term();
    CHECK(host_alloc_live() == 0, "%ld blocks left after vdi_ft_term", host_alloc_live());
    /* with vdi_ftmem.c, one empty arena may be kept */
    CHECK(host_os_blocks() <= 1, "%ld system blocks left after vdi_ft_term", host_os_blocks());
}


/*
 * the checks
 */

static void check_register_and_size(void)
{
    VdiOutlineFont *face, *f;

    printf("register and size\n");
    face = start(sans_path);
    CHECK(face != NULL, "no face");
    if (!face)
        return;
    CHECK(vdi_ft_face(1) == NULL && vdi_ft_face(-1) == NULL, "face out of range");
    CHECK(strcmp(face->name, "Liberation Sans") == 0, "name '%s'", face->name);
    CHECK(face->id > 5000 && face->id <= 32767, "id %ld", face->id);
    CHECK(face->flags == VDI_FT_SCALABLE, "flags %lx", face->flags);
    CHECK(face->points == 0, "unsized face has points %ld", face->points);

    /* the same file again adds nothing */
    CHECK(vdi_ft_register_file(sans_path) == 0, "registered twice");
    CHECK(vdi_ft_face_count() == 1, "%ld faces", vdi_ft_face_count());

    f = vdi_ft_size(face, PT, PX, 0);
    CHECK(f != NULL, "12 pt at 16 px");
    if (f)
    {
        CHECK(f != face, "sized font is the face");
        CHECK(f->id == face->id, "id %ld", f->id);
        CHECK(f->points == PT, "points %ld", f->points);
        CHECK(f->top > 0 && f->bottom > 0, "top %ld bottom %ld", f->top, f->bottom);
        CHECK(f->pixel_height == f->top + f->bottom, "pixel_height %ld", f->pixel_height);
        CHECK(f->ascent == f->top && f->descent == f->bottom, "ascent/descent");
        CHECK(f->half == f->top - f->pixel_height / 2, "half %ld", f->half);
        CHECK(f->max_char_width > 0 && f->max_cell_width > 0, "widths %ld %ld",
              f->max_char_width, f->max_cell_width);
        CHECK(f->underline_size >= 1, "underline %ld", f->underline_size);
        CHECK(f->first_ade == 0 && f->last_ade == 255, "ades %ld-%ld", f->first_ade, f->last_ade);
        CHECK(vdi_ft_size(face, PT, PX, 0) == f, "same size gives another record");
        CHECK(vdi_ft_size(f, PT, PX, 0) == f, "resizing a sized font to its size");
    }
    stop();
}


static void check_render_atari(void)
{
    VdiOutlineFont *face, *f;
    VdiOutlineBitmap b;
    WORD text[16];
    long n, sum, w, h, before;
    LONG ew, eh;

    printf("render \"Atari\"\n");
    face = start(sans_path);
    f = face ? vdi_ft_size(face, PT, PX, 0) : NULL;
    CHECK(f != NULL, "no font");
    if (!f)
    {
        stop();
        return;
    }

    n = to_words("Atari", text);
    sum = sum_advances(f, "Atari", VDI_FT_MAP_ATARI, 0);
    CHECK(vdi_ft_render(f, text, n, VDI_FT_MAP_ATARI, 0, 0, &b) == 0, "render failed");
    CHECK(b.data != NULL, "no bitmap");
    if (b.data)
    {
        CHECK(b.width == sum, "width %ld, sum of advances %ld", b.width, sum);
        CHECK(b.height == f->top + f->bottom, "height %ld, top+bottom %ld",
              b.height, f->top + f->bottom);
        CHECK(b.wdwidth == (b.width + 15) / 16, "wdwidth %ld for width %ld", b.wdwidth, b.width);
        CHECK(b.origin_y == f->top, "origin_y %ld", b.origin_y);
        CHECK(b.origin_x == 0, "origin_x %ld", b.origin_x);
        CHECK(b.advance_x == sum, "advance_x %ld", b.advance_x);
        CHECK(ink(&b) > 50, "only %ld pixels set", ink(&b));
        w = b.width;
        h = b.height;
        vdi_ft_release(&b);
        CHECK(b.data == NULL, "release left data");

        CHECK(vdi_ft_text_extent(f, text, n, VDI_FT_MAP_ATARI, 0, &ew, &eh) == 0, "extent");
        CHECK(ew == w && eh == h, "extent %ldx%ld, bitmap %ldx%ld", (long)ew, (long)eh, w, h);
    }

    /* measured glyphs are drawn without loading them again: one block */
    before = host_alloc_total();
    CHECK(vdi_ft_render(f, text, n, VDI_FT_MAP_ATARI, 0, 0, &b) == 0, "second render");
    CHECK(host_alloc_total() - before == 1, "%ld allocations to draw cached text",
          host_alloc_total() - before);
    vdi_ft_release(&b);

    text[0] = 'Z';
    CHECK(vdi_ft_text_extent(f, text, 1, VDI_FT_MAP_ATARI, 0, &ew, &eh) == 0, "extent of Z");
    before = host_alloc_total();
    CHECK(vdi_ft_render(f, text, 1, VDI_FT_MAP_ATARI, 0, 0, &b) == 0, "render Z");
    CHECK(host_alloc_total() - before == 1, "%ld allocations to draw a measured glyph",
          host_alloc_total() - before);
    vdi_ft_release(&b);

    /* nothing to draw is not an error */
    CHECK(vdi_ft_render(f, text, 0, VDI_FT_MAP_ATARI, 0, 0, &b) == 0 && b.data == NULL,
          "empty string");
    stop();
}


static void check_bad_files(void)
{
    char trunc[600], garbage[600], empty[600], noglyf[600], noname[600];
    unsigned char *data, junk[8192];
    long size, before, i;

    printf("bad files\n");
    data = read_file(sans_path, &size);
    CHECK(data != NULL && size > 310000, "cannot read %s", sans_path);
    if (!data)
        return;
    tmpname(trunc, sizeof(trunc), "TRUNC.TTF");
    tmpname(garbage, sizeof(garbage), "GARBAGE.TTF");
    tmpname(empty, sizeof(empty), "EMPTY.TTF");
    tmpname(noglyf, sizeof(noglyf), "NOGLYF.TTF");
    tmpname(noname, sizeof(noname), "NONAME.TTF");
    for (i = 0; i < (long)sizeof(junk); i++)
        junk[i] = (UBYTE)(i * 7919 + 13);
    CHECK(write_file(trunc, data, 4096), "%s", trunc);
    CHECK(write_file(garbage, junk, sizeof(junk)), "%s", garbage);
    CHECK(write_file(empty, "", 0), "%s", empty);
    /*
     * Cut where FreeType still opens the face.  At 60000 bytes the font
     * keeps its header, metrics, cmap and loca, but glyf and name are gone:
     * a face named " " that draws nothing.  At 300000 bytes every glyph is
     * there, but the names are not.
     */
    CHECK(write_file(noglyf, data, 60000), "%s", noglyf);
    CHECK(write_file(noname, data, 300000), "%s", noname);
    free(data);

    CHECK(vdi_ft_init() == 0, "init");
    before = host_alloc_live();
    CHECK(vdi_ft_register_file(trunc) < 0, "truncated file accepted");
    CHECK(vdi_ft_register_file(garbage) < 0, "garbage accepted");
    CHECK(vdi_ft_register_file(empty) < 0, "empty file accepted");
    CHECK(vdi_ft_register_file("/nonexistent/FONT.TTF") < 0, "missing file accepted");
    CHECK(vdi_ft_register_file(noglyf) <= 0, "font without glyphs or names accepted");
    CHECK(vdi_ft_register_file(noname) <= 0, "font without names accepted");
    CHECK(vdi_ft_face_count() == 0, "%ld faces, the first named '%s'", vdi_ft_face_count(),
          vdi_ft_face_count() ? vdi_ft_face(0)->name : "");
    CHECK(host_alloc_live() == before, "%ld blocks leaked", host_alloc_live() - before);

    /* the good font still registers after them */
    CHECK(vdi_ft_register_file(sans_path) == 1, "good font after bad ones");
    stop();
    unlink(trunc);
    unlink(garbage);
    unlink(empty);
    unlink(noglyf);
    unlink(noname);
}


static void check_no_memory(void)
{
    VdiOutlineFont *face, *f, *g;
    VdiOutlineBitmap b;
    WORD text[16];
    long n, before, width;

    printf("no memory\n");
    n = to_words("Atari", text);

    /* nothing works without memory, and nothing is lost trying */
    host_alloc_fail_after(0);
    CHECK(vdi_ft_init() < 0, "init without memory");
    host_alloc_fail_after(-1);
    CHECK(host_alloc_live() == 0, "%ld blocks leaked by init", host_alloc_live());

    face = start(sans_path);
    if (!face)
        return;
    before = host_alloc_live();
    host_alloc_fail_after(0);
    CHECK(vdi_ft_register_file(second_path) < 0, "register without memory");
    CHECK(vdi_ft_size(face, PT, PX, 0) == NULL, "size without memory");
    host_alloc_fail_after(-1);
    CHECK(host_alloc_live() == before, "%ld blocks leaked", host_alloc_live() - before);

    /* a font sized before memory ran out, nothing rendered yet */
    f = vdi_ft_size(face, PT, PX, 0);
    CHECK(f != NULL, "size");
    if (!f)
    {
        stop();
        return;
    }
    before = host_alloc_live();
    host_alloc_fail_after(0);
    b.data = (UWORD *)1;
    CHECK(vdi_ft_render(f, text, n, VDI_FT_MAP_ATARI, 0, 0, &b) < 0, "render without memory");
    CHECK(b.data == NULL, "data set on failure");
    text[5] = 0x263a;           /* past the 256 cached codes */
    CHECK(vdi_ft_render(f, text + 5, 1, VDI_FT_MAP_UNICODE, 1, 0, &b) < 0,
          "render without memory, uncached code");
    host_alloc_fail_after(-1);
    CHECK(host_alloc_live() == before, "%ld blocks leaked", host_alloc_live() - before);

    /* survives: the same font draws once memory is back */
    CHECK(vdi_ft_render(f, text, n, VDI_FT_MAP_ATARI, 0, 0, &b) == 0 && b.data, "render after");
    width = b.data ? b.width : 0;
    vdi_ft_release(&b);

    /* glyphs cached: only the bitmap of the string is missing */
    before = host_alloc_live();
    host_alloc_fail_after(0);
    CHECK(vdi_ft_render(f, text, n, VDI_FT_MAP_ATARI, 0, 0, &b) < 0, "cached, no memory");
    host_alloc_fail_after(-1);
    CHECK(host_alloc_live() == before, "%ld blocks leaked", host_alloc_live() - before);
    CHECK(vdi_ft_render(f, text, n, VDI_FT_MAP_ATARI, 0, 0, &b) == 0 && b.width == width,
          "width %ld after, %ld before", b.width, width);
    vdi_ft_release(&b);

    /* a new effect needs a new instance: none without memory */
    before = host_alloc_live();
    host_alloc_fail_after(0);
    CHECK(vdi_ft_render(f, text, n, VDI_FT_MAP_ATARI, 4, 0, &b) < 0, "italic, no memory");
    host_alloc_fail_after(-1);
    CHECK(host_alloc_live() == before, "%ld blocks leaked", host_alloc_live() - before);
    g = vdi_ft_size(face, PT, PX, 0);
    CHECK(g == f, "font record changed");
    stop();
}


/*
 * The engine holds no reference to a font file it does not use: once the
 * sans font's instances have all been pushed out by another font's, and
 * the engine has dropped its idle file buffers (it does when a file cannot
 * be read for lack of memory or because it is gone), sizing the sans font
 * again must read its file again.  gone is a face whose file is missing.
 */
static int file_released(VdiOutlineFont *sans, VdiOutlineFont *other, VdiOutlineFont *gone,
                         long size)
{
    long i, loads;

    for (i = 0; i < 12; i++)
        if (!vdi_ft_size(other, 20 + i, 20 + i, 0))
            return 0;
    if (vdi_ft_size(gone, PT, PX, 0))
        return 0;
    loads = host_file_loads();
    if (!vdi_ft_size(sans, size, size, 0))
        return 0;
    return host_file_loads() == loads + 1;
}

/*
 * every allocation in turn fails, through the whole life of the engine;
 * each time nothing may leak, whatever was set up must still work once
 * memory is back, and no reference to the font file may be left behind
 */
static void check_every_failure_point(void)
{
    VdiOutlineBitmap b;
    VdiXfntInfo info;
    WORD text[16], xy[512];
    BYTE flags[256];
    LONG adv[4], w, h;
    long n, i, good_width = 0, runs = 0, bad_after = 0, held = 0;
    int finished = 0;
    char gone[600], hidden[600];
    unsigned char *data;
    long size;

    printf("every allocation failing in turn\n");
    n = to_words("Atari", text);
    tmpname(gone, sizeof(gone), "GONE.TTF");
    tmpname(hidden, sizeof(hidden), "GONE.OFF");
    data = read_file(sans_path, &size);
    CHECK(data && write_file(gone, data, size), "%s", gone);
    free(data);

    /* the result without failures */
    {
        VdiOutlineFont *face = start(sans_path);
        VdiOutlineFont *f = face ? vdi_ft_size(face, PT, PX, 0) : NULL;
        VdiOutlineFont *other, *lost;

        if (f && vdi_ft_render(f, text, n, VDI_FT_MAP_ATARI, 1, 0, &b) == 0 && b.data)
        {
            good_width = b.width;
            vdi_ft_release(&b);
        }
        CHECK(good_width > 0, "no reference width");
        CHECK(vdi_ft_register_file(second_path) == 1 && vdi_ft_register_file(gone) == 1,
              "more fonts");
        other = vdi_ft_face(1);
        lost = vdi_ft_face(2);
        CHECK(rename(gone, hidden) == 0, "%s", gone);
        CHECK(face && other && lost && file_released(face, other, lost, 13),
              "the font file stays in memory with nothing using it");
        CHECK(rename(hidden, gone) == 0, "%s", hidden);
        stop();
    }

    for (i = 0; i < 20000 && !finished; i++)
    {
        VdiOutlineFont *face = NULL, *f = NULL;
        long refused = host_alloc_refused();
        int started;

        runs++;
        host_alloc_fail_after(i);
        started = vdi_ft_init() == 0;
        if (started)
        {
            if (vdi_ft_register_file(sans_path) == 1)
                face = vdi_ft_face(0);
            if (face)
                f = vdi_ft_size(face, PT, PX, 0);
            if (f)
            {
                if (vdi_ft_render(f, text, n, VDI_FT_MAP_ATARI, 1, 0, &b) == 0)
                    vdi_ft_release(&b);
                vdi_ft_text_extent(f, text, n, VDI_FT_MAP_ATARI, 0x15, &w, &h);
                vdi_ft_char_advance(f, 0x263a, VDI_FT_MAP_UNICODE, 0, adv);
                vdi_ft_get_outline(f, 'g', VDI_FT_MAP_ATARI, xy, flags, 256);
                vdi_ft_xfntinfo(f, 0x7ff, &info);
            }
        }
        finished = host_alloc_refused() == refused;
        host_alloc_fail_after(-1);

        /* what exists still works */
        if (f)
        {
            if (vdi_ft_render(f, text, n, VDI_FT_MAP_ATARI, 1, 0, &b) == 0 && b.data)
            {
                if (b.width != good_width)
                    bad_after++;
                vdi_ft_release(&b);
            }
            else
                bad_after++;
        }
        else if (face)
        {
            if (!vdi_ft_size(face, PT, PX, 0))
                bad_after++;
        }
        else if (started)
        {
            if (vdi_ft_register_file(sans_path) == 1)
                face = vdi_ft_face(0);
            else
                bad_after++;
        }
        else if (vdi_ft_init() != 0)
            bad_after++;

        /* and holds on to nothing it does not use */
        if (face)
        {
            VdiOutlineFont *other, *lost;

            if (vdi_ft_register_file(second_path) == 1 && vdi_ft_register_file(gone) == 1 &&
                (other = vdi_ft_face(1)) != NULL && (lost = vdi_ft_face(2)) != NULL &&
                rename(gone, hidden) == 0)
            {
                if (!file_released(face, other, lost, 13))
                    held++;
                rename(hidden, gone);
            }
            else
                bad_after++;
        }

        vdi_ft_term();
        if (host_alloc_live() != 0)
        {
            CHECK(host_alloc_live() == 0, "allocation %ld failing leaks %ld blocks",
                  i, host_alloc_live());
            break;
        }
    }
    CHECK(finished, "still failing after %ld allocations", i);
    CHECK(bad_after == 0, "%ld runs did not recover", bad_after);
    CHECK(held == 0, "%ld runs left the font file referenced", held);
    printf("  %ld failure points\n", runs);
    unlink(gone);
}


/* sizes the VDI cannot represent are refused, and hold nothing */
static void check_refused_sizes(void)
{
    char gone[600], hidden[600];
    VdiOutlineFont *face, *other, *lost, *f;
    unsigned char *data;
    long size;

    printf("refused sizes\n");
    tmpname(gone, sizeof(gone), "GONE.TTF");
    tmpname(hidden, sizeof(hidden), "GONE.OFF");
    data = read_file(sans_path, &size);
    CHECK(data && write_file(gone, data, size), "%s", gone);
    free(data);

    face = start(sans_path);
    CHECK(vdi_ft_register_file(second_path) == 1 && vdi_ft_register_file(gone) == 1,
          "more fonts");
    other = vdi_ft_face(1);
    lost = vdi_ft_face(2);
    if (!face || !other || !lost)
    {
        stop();
        return;
    }

    /* the widest cell of a 32000 pixel em does not fit in 32000 */
    CHECK(vdi_ft_size(face, 32000, 32000, 0) == NULL, "32000 pixels accepted");
    f = vdi_ft_size(face, 1000, 1000, 0);
    CHECK(f != NULL && vdi_ft_size(face, 1000, 1000, 32000) == NULL, "width 32000 accepted");
    CHECK(rename(gone, hidden) == 0, "%s", gone);
    /* only the 1000 pixel size may keep the file */
    CHECK(f && vdi_ft_size(face, 1000, 1000, 0) == f, "1000 pixels");
    CHECK(file_released(face, other, lost, 13), "a refused size keeps the font file");
    CHECK(rename(hidden, gone) == 0, "%s", hidden);
    stop();
    unlink(gone);
}


static void check_atari_map(void)
{
    VdiOutlineFont *face, *f;
    long ch, bad_index = 0, bad_image = 0, missing = 0;
    unsigned u;
    FT_UInt gi;

    printf("Atari codes 0x80-0xff\n");
    face = start(sans_path);
    f = face ? vdi_ft_size(face, PT, PX, 0) : NULL;
    CHECK(f != NULL, "no font");
    if (!f)
    {
        stop();
        return;
    }

    /* fVDI's (NVDI's) table, not the Atari character set's Hebrew */
    CHECK(vdi_ft_char_index(f, 0x80, VDI_FT_MAP_ATARI, VDI_FT_MAP_UNICODE) == 0x00c7, "0x80");
    CHECK(vdi_ft_char_index(f, 0x81, VDI_FT_MAP_ATARI, VDI_FT_MAP_UNICODE) == 0x00fc, "0x81");
    CHECK(vdi_ft_char_index(f, 0x84, VDI_FT_MAP_ATARI, VDI_FT_MAP_UNICODE) == 0x00e4, "0x84");
    CHECK(vdi_ft_char_index(f, 0x9e, VDI_FT_MAP_ATARI, VDI_FT_MAP_UNICODE) == 0x00df, "0x9e");
    CHECK(vdi_ft_char_index(f, 0xc2, VDI_FT_MAP_ATARI, VDI_FT_MAP_UNICODE) == 0x00ca, "0xc2");
    CHECK(vdi_ft_char_index(f, 0xd3, VDI_FT_MAP_ATARI, VDI_FT_MAP_UNICODE) == 0x2018, "0xd3");
    CHECK(vdi_ft_char_index(f, 0xde, VDI_FT_MAP_ATARI, VDI_FT_MAP_UNICODE) == 0x2030, "0xde");

    for (ch = 0x80; ch <= 0xff; ch++)
    {
        gi = ref_atari_index(ch, &u);
        if (vdi_ft_char_index(f, ch, VDI_FT_MAP_ATARI, VDI_FT_MAP_UNICODE) != (LONG)u)
        {
            bad_index++;
            printf("  0x%02lx: index %ld, expected 0x%04x\n", ch,
                   vdi_ft_char_index(f, ch, VDI_FT_MAP_ATARI, VDI_FT_MAP_UNICODE), u);
        }
        if (!gi)
            missing++;
        if (!same_as_reference(f, ch, VDI_FT_MAP_ATARI, gi, "atari"))
            bad_image++;
    }
    CHECK(bad_index == 0, "%ld codes map to other characters", bad_index);
    CHECK(bad_image == 0, "%ld codes draw other glyphs", bad_image);
    printf("  %ld of 128 codes not in the font (drawn as .notdef)\n", missing);

    /* a character the font lacks is its .notdef glyph */
    CHECK(ref_index(0xe000) == 0, "the font has U+E000");
    CHECK(vdi_ft_char_index(f, 0xe000, VDI_FT_MAP_UNICODE, VDI_FT_MAP_UNICODE) == 0xffff,
          "U+E000 has an index");
    CHECK(same_as_reference(f, 0xe000, VDI_FT_MAP_UNICODE, 0, "notdef"), "U+E000 is not .notdef");

    /* the other maps */
    CHECK(same_as_reference(f, 0x00c2, VDI_FT_MAP_UNICODE, ref_index(0xc2), "unicode"),
          "U+00C2");
    CHECK(same_as_reference(f, 0x263a, VDI_FT_MAP_UNICODE, ref_index(0x263a), "unicode"),
          "U+263A");
    CHECK(same_as_reference(f, 33, VDI_FT_MAP_BICS, ref_index('A'), "bics"), "BICS 33");
    CHECK(vdi_ft_char_index(f, 'A', VDI_FT_MAP_UNICODE, VDI_FT_MAP_ATARI) == 'A', "U+0041 to Atari");
    CHECK(vdi_ft_char_index(f, 0xdf, VDI_FT_MAP_UNICODE, VDI_FT_MAP_ATARI) == 0x9e, "U+00DF to Atari");
    CHECK(vdi_ft_char_index(f, 33, VDI_FT_MAP_BICS, VDI_FT_MAP_UNICODE) == 'A', "BICS 33");
    CHECK(vdi_ft_char_index(f, 'A', VDI_FT_MAP_UNICODE, VDI_FT_MAP_BICS) == 33, "U+0041 to BICS");

    /* the same code under another map, on the same font: no stale glyph */
    CHECK(same_as_reference(f, 0xc2, VDI_FT_MAP_ATARI, ref_index(0xca), "switch"), "0xc2 Atari");
    CHECK(same_as_reference(f, 0xc2, VDI_FT_MAP_UNICODE, ref_index(0xc2), "switch"), "0xc2 Unicode");
    CHECK(same_as_reference(f, 0xc2, VDI_FT_MAP_ATARI, ref_index(0xca), "switch"), "0xc2 Atari again");

    /* codes above 0x7fff are unsigned */
    {
        WORD text[3] = { (WORD)0xffff, (WORD)0x8000, (WORD)0xfb01 };
        VdiOutlineBitmap b;
        LONG w, h, adv[4];

        CHECK(vdi_ft_render(f, text, 3, VDI_FT_MAP_UNICODE, 0, 0, &b) == 0, "high codes");
        vdi_ft_release(&b);
        CHECK(vdi_ft_text_extent(f, text, 3, VDI_FT_MAP_ATARI, 0, &w, &h) == 0, "high codes, Atari");
        vdi_ft_char_advance(f, 0xffff, VDI_FT_MAP_BICS, 0, adv);
        vdi_ft_char_advance(f, 0xffff, VDI_FT_MAP_ATARI, 0, adv);
    }
    stop();
}


static void check_spdchar_map(void)
{
    char path[600], shortpath[600];
    unsigned char map[3600];
    VdiOutlineFont *face, *f;
    long i;

    printf("SPDCHAR.MAP\n");
    memset(map, 0, sizeof(map));
    map[0] = '0';
    map[1] = '0';
    for (i = 32; i < 256; i++)
    {
        int bics = (i == 'A') ? Atari2Bics['B'] : Atari2Bics[i];

        map[2 + (i - 32) * 2] = (UBYTE)(bics >> 8);
        map[3 + (i - 32) * 2] = (UBYTE)bics;
    }
    tmpname(path, sizeof(path), "SPDCHAR.MAP");
    tmpname(shortpath, sizeof(shortpath), "SHORT");
    CHECK(write_file(path, map, sizeof(map)), "%s", path);

    /* too short to hold the first table: ignored */
    CHECK(mkdir(shortpath, 0700) == 0, "%s", shortpath);
    strcat(shortpath, "/spdchar.map");
    CHECK(write_file(shortpath, map, 449), "%s", shortpath);
    CHECK(vdi_ft_init() == 0, "init");
    CHECK(vdi_ft_register_file(shortpath) == 0, "short map");
    CHECK(vdi_ft_register_file(sans_path) == 1, "font");
    face = vdi_ft_face(0);
    f = face ? vdi_ft_size(face, PT, PX, 0) : NULL;
    CHECK(f && vdi_ft_char_index(f, 'A', VDI_FT_MAP_ATARI, VDI_FT_MAP_UNICODE) == 'A',
          "short map was used");
    stop();

    CHECK(vdi_ft_init() == 0, "init");
    CHECK(vdi_ft_register_file(path) == 0, "map");
    CHECK(vdi_ft_register_file(sans_path) == 1, "font");
    face = vdi_ft_face(0);
    f = face ? vdi_ft_size(face, PT, PX, 0) : NULL;
    CHECK(f != NULL, "no font");
    if (f)
    {
        CHECK(vdi_ft_char_index(f, 'A', VDI_FT_MAP_ATARI, VDI_FT_MAP_UNICODE) == 'B',
              "'A' maps to %ld", vdi_ft_char_index(f, 'A', VDI_FT_MAP_ATARI, VDI_FT_MAP_UNICODE));
        CHECK(same_as_reference(f, 'A', VDI_FT_MAP_ATARI, ref_index('B'), "spdchar"), "'A' is not 'B'");
        CHECK(same_as_reference(f, 'C', VDI_FT_MAP_ATARI, ref_index('C'), "spdchar"), "'C'");
        CHECK(vdi_ft_char_index(f, 'B', VDI_FT_MAP_UNICODE, VDI_FT_MAP_ATARI) == 'A',
              "U+0042 back to Atari");
    }
    stop();
    unlink(path);
    unlink(shortpath);
    shortpath[strlen(shortpath) - strlen("/spdchar.map")] = 0;
    rmdir(shortpath);
}


static void check_effects_and_width(void)
{
    VdiOutlineFont *face, *f, *narrow;
    VdiOutlineBitmap b, plain;
    WORD text[16];
    long n, e;
    LONG w, h;

    printf("effects and width\n");
    face = start(sans_path);
    f = face ? vdi_ft_size(face, PT, PX, 0) : NULL;
    CHECK(f != NULL, "no font");
    if (!f)
    {
        stop();
        return;
    }
    n = to_words("Hamburgefonts", text);
    CHECK(vdi_ft_render(f, text, n, VDI_FT_MAP_ATARI, 0, 0, &plain) == 0, "plain");

    for (e = 0; e < 32; e++)
    {
        CHECK(vdi_ft_render(f, text, n, VDI_FT_MAP_ATARI, e, 0, &b) == 0 && b.data,
              "effects %ld", e);
        if (!b.data)
            continue;
        CHECK(b.height == plain.height, "effects %ld: height %ld", e, b.height);
        CHECK(vdi_ft_text_extent(f, text, n, VDI_FT_MAP_ATARI, e, &w, &h) == 0 &&
              w == b.width && h == b.height,
              "effects %ld: extent %ldx%ld, bitmap %ldx%ld", e, (long)w, (long)h, b.width, b.height);
        if (e == 1)
            CHECK(b.width > plain.width, "bold not wider");
        if (e == 2)
            CHECK(ink(&b) < ink(&plain), "light not lighter");
        if (e == 4)
            CHECK(b.width > plain.width, "italic not wider");
        if (e == 8)
        {
            long x, row, full = 0;

            for (row = 0; row < b.height; row++)
            {
                for (x = 0; x < b.width && pixel(&b, x, row); x++)
                    ;
                if (x == b.width)
                    full++;
            }
            CHECK(full == f->underline_size, "%ld underline rows, %ld wanted", full, f->underline_size);
        }
        if (e == 16)
            CHECK(ink(&b) != ink(&plain), "outlined is the same");
        vdi_ft_release(&b);
    }

    /* skew */
    CHECK(vdi_ft_render(f, text, n, VDI_FT_MAP_ATARI, 0, 200, &b) == 0 && b.width > plain.width,
          "skew 20 degrees not wider");
    vdi_ft_release(&b);
    CHECK(vdi_ft_render(f, text, n, VDI_FT_MAP_ATARI, 0, -200, &b) == 0 && b.width > plain.width,
          "skew -20 degrees not wider");
    vdi_ft_release(&b);
    CHECK(vdi_ft_render(f, text, n, VDI_FT_MAP_ATARI, 0, 30000, &b) == 0, "absurd skew");
    vdi_ft_release(&b);

    /* width independent of height */
    narrow = vdi_ft_size(face, PT, PX, f->max_char_width / 2);
    CHECK(narrow != NULL && narrow != f, "half width");
    if (narrow)
    {
        CHECK(narrow->max_char_width == f->max_char_width / 2, "max_char_width %ld",
              narrow->max_char_width);
        CHECK(narrow->top == f->top && narrow->bottom == f->bottom, "height changed");
        CHECK(vdi_ft_render(narrow, text, n, VDI_FT_MAP_ATARI, 0, 0, &b) == 0 && b.data,
              "narrow render");
        CHECK(b.width < plain.width * 3 / 4 && b.width > plain.width / 4,
              "narrow width %ld, plain %ld", b.width, plain.width);
        vdi_ft_release(&b);
    }
    CHECK(vdi_ft_size(face, PT, PX, 40000) == f, "absurd width is ignored");
    CHECK(vdi_ft_size(face, 0, 0, 0) != NULL, "zero size");
    CHECK(vdi_ft_size(face, 100000, 100000, 0) == NULL ||
          vdi_ft_size(face, 100000, 100000, 0)->points <= 32000, "huge size");

    /* an unsized face is used at 10 points */
    CHECK(vdi_ft_render(face, text, n, VDI_FT_MAP_ATARI, 0, 0, &b) == 0 && b.data &&
          b.height == vdi_ft_size(face, 10, 10, 0)->pixel_height, "unsized face");
    vdi_ft_release(&b);
    vdi_ft_release(&plain);
    stop();
}


static void check_outline(void)
{
    VdiOutlineFont *face, *f;
    WORD xy[2 * 400];
    BYTE flags[400];
    long n, i, jumps = 0, beziers = 0;

    printf("outlines\n");
    face = start(sans_path);
    f = face ? vdi_ft_size(face, PT, PX, 0) : NULL;
    CHECK(f != NULL, "no font");
    if (!f)
    {
        stop();
        return;
    }
    n = vdi_ft_get_outline(f, 'O', VDI_FT_MAP_ATARI, xy, flags, 400);
    CHECK(n > 4, "%ld points", n);
    CHECK(n > 0 && (flags[0] & 2), "first flag %d", n > 0 ? flags[0] : -1);
    for (i = 0; i < n && i < 400; i++)
    {
        if (flags[i] & 2)
            jumps++;
        if (flags[i] & 1)
            beziers++;
        CHECK(xy[2 * i] >= -2 && xy[2 * i] <= f->max_cell_width + 2 &&
              xy[2 * i + 1] >= -f->bottom - 2 && xy[2 * i + 1] <= f->top + 2,
              "point %ld at %d,%d", i, xy[2 * i], xy[2 * i + 1]);
    }
    CHECK(jumps == 2, "'O' has %ld contours", jumps);
    CHECK(beziers > 0, "no curves");
    CHECK(vdi_ft_get_outline(f, 'O', VDI_FT_MAP_ATARI, xy, flags, 3) == -1, "overflow");
    CHECK(vdi_ft_get_outline(f, 0xe000, VDI_FT_MAP_UNICODE, xy, flags, 400) == 0, "missing");
    CHECK(vdi_ft_get_outline(f, 0x263a, VDI_FT_MAP_UNICODE, xy, flags, 400) > 0, "U+263A");
    CHECK(vdi_ft_get_outline(f, ' ', VDI_FT_MAP_ATARI, xy, flags, 400) == 0, "space");
    stop();
}


static void check_info(void)
{
    VdiOutlineFont *face, *f, *mono, *second;
    VdiXfntInfo info;
    VdiFontHeader fh;
    WORD text[8];
    LONG w, h;
    long n, i;

    printf("font information\n");
    face = start(sans_path);
    CHECK(vdi_ft_register_file(second_path) == 1, "%s", second_path);
    CHECK(vdi_ft_register_file(mono_path) == 1, "%s", mono_path);
    CHECK(vdi_ft_face_count() == 3, "%ld faces", vdi_ft_face_count());
    second = vdi_ft_face(1);
    mono = vdi_ft_face(2);
    if (!face || !second || !mono)
    {
        stop();
        return;
    }
    CHECK(face->id != second->id && face->id != mono->id && second->id != mono->id,
          "ids %ld %ld %ld", face->id, second->id, mono->id);
    CHECK(strcmp(second->name, "DejaVu Sans") == 0, "name '%s'", second->name);
    CHECK(mono->flags == (VDI_FT_SCALABLE | VDI_FT_MONOSPACE), "mono flags %lx", mono->flags);

    memset(&info, 0x55, sizeof(info));
    info.font_flags = 0;
    vdi_ft_xfntinfo(face, 0x7ff, &info);
    CHECK(strcmp(info.font_name, "Liberation Sans") == 0, "font_name '%s'", info.font_name);
    CHECK(strcmp(info.family_name, "Liberation Sans") == 0, "family '%s'", info.family_name);
    CHECK(strcmp(info.style_name, "Regular") == 0, "style '%s'", info.style_name);
    CHECK(strcmp(info.file_name1, sans_path) == 0, "file '%s'", info.file_name1);
    CHECK(info.file_name2[0] == 0 && info.file_name3[0] == 0, "file names 2/3");
    CHECK(info.pt_cnt == 10, "pt_cnt %d", info.pt_cnt);
    CHECK(info.pt_sizes[0] == 8 && info.pt_sizes[9] == 48, "sizes %d..%d",
          info.pt_sizes[0], info.pt_sizes[9]);
    for (i = info.pt_cnt; i < 64; i++)
        if (info.pt_sizes[i] != 0x5555)
            break;
    CHECK(i == 64, "pt_sizes written past pt_cnt");
    CHECK(info.font_flags == VDI_FFLAG_VALID, "font_flags %x", info.font_flags);
    CHECK(info.font_class == VDI_FCLASS_SANS, "class %d", info.font_class);
    CHECK(info.size == 0x55555555 && info.format == 0x5555 && info.id == 0x5555,
          "caller's fields written");

    memset(&info, 0, sizeof(info));
    vdi_ft_xfntinfo(mono, VDI_XFNT_CLASS, &info);
    CHECK(info.font_flags == (VDI_FFLAG_VALID | VDI_FFLAG_MONOSPACED), "mono flags %x",
          info.font_flags);
    CHECK(info.font_name[0] == 0, "font name filled without being asked");

    /* fields not asked for are left alone */
    memset(&info, 0x55, sizeof(info));
    vdi_ft_xfntinfo(face, VDI_XFNT_FONT_NAME, &info);
    CHECK(info.family_name[0] == 0x55 && info.font_class == 0x5555, "unasked fields written");

    f = vdi_ft_size(face, PT, PX, 0);
    memset(&fh, 0x55, sizeof(fh));
    if (f)
        vdi_ft_fontheader(f, &fh);
    CHECK(memcmp(fh.fh_fmver, "D1.0\r\n\0\0", 8) == 0, "format id");
    CHECK(strcmp(fh.fh_fntnm, "Liberation Sans") == 0, "fh_fntnm '%s'", fh.fh_fntnm);
    CHECK(fh.fh_orupm == ref_face->units_per_EM, "orupm %u", fh.fh_orupm);
    CHECK(fh.fh_nchrf == (UWORD)ref_face->num_glyphs, "nchrf %u", fh.fh_nchrf);
    CHECK(fh.fh_hedsz == sizeof(VdiFontHeader), "hedsz %u", fh.fh_hedsz);
    CHECK(strcmp(fh.fh_sfntn, "LiberationSans") == 0, "ps name '%.32s'", fh.fh_sfntn);
    CHECK(fh.fh_cflgs == 0 && fh.fh_frmcl == 0x68, "cflgs %d frmcl %x", fh.fh_cflgs, fh.fh_frmcl);

    /* monospaced: the extent is the sum of the advances */
    f = vdi_ft_size(mono, PT, PX, 0);
    n = to_words("il.W", text);
    CHECK(f && vdi_ft_text_extent(f, text, n, VDI_FT_MAP_ATARI, 0, &w, &h) == 0 &&
          w == sum_advances(f, "il.W", VDI_FT_MAP_ATARI, 0) && w == 4 * f->max_char_width,
          "mono extent %ld", (long)w);
    stop();
}


static void check_many_sizes(void)
{
    VdiOutlineFont *face, *first, *f;
    VdiOutlineBitmap b;
    WORD text[8];
    long n, i, live12 = 0, loads;

    printf("many sizes\n");
    face = start(sans_path);
    if (!face)
    {
        stop();
        return;
    }
    n = to_words("Atari", text);
    loads = host_file_loads();
    first = vdi_ft_size(face, 8, 8, 0);
    CHECK(first != NULL, "first size");
    CHECK(host_file_loads() == loads + 1, "%ld file reads", host_file_loads() - loads);
    for (i = 9; i < 60; i++)
    {
        f = vdi_ft_size(face, i, i, 0);
        CHECK(f != NULL && f->points == i, "size %ld", i);
        if (i == 20)
            live12 = host_alloc_live();
    }
    /* open faces are bounded: only the size records add up */
    CHECK(host_alloc_live() - live12 <= 2 * (60 - 21),
          "%ld more blocks for %d more sizes", host_alloc_live() - live12, 60 - 21);
    /* the file was read once */
    CHECK(host_file_loads() == loads + 1, "%ld file reads", host_file_loads() - loads);
    /* the first is still usable */
    CHECK(first && vdi_ft_render(first, text, n, VDI_FT_MAP_ATARI, 0, 0, &b) == 0 && b.data &&
          b.height == first->pixel_height, "first size after many others");
    vdi_ft_release(&b);
    stop();
}


int main(int argc, char **argv)
{
    if (argc != 4)
    {
        fprintf(stderr, "usage: %s <sans.ttf> <second.ttf> <mono.ttf>\n", argv[0]);
        return 2;
    }
    sans_path = argv[1];
    second_path = argv[2];
    mono_path = argv[3];

    if (!ref_open(sans_path))
    {
        fprintf(stderr, "cannot open %s\n", sans_path);
        return 2;
    }
    make_tmpdir();

    check_register_and_size();
    check_render_atari();
    check_bad_files();
    check_no_memory();
    check_every_failure_point();
    check_refused_sizes();
    check_atari_map();
    check_spdchar_map();
    check_effects_and_width();
    check_outline();
    check_info();
    check_many_sizes();

    rmdir(tmpdir);
    ref_close();

    printf("vdi-fonts: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
