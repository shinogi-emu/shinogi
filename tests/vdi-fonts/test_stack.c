/*
 * Host checks for the outline font engine of EmuTOS's VDI on a stack of
 * its real size.
 *
 * In EmuTOS the engine and FreeType run on a font stack of
 * VDI_FT_STACK_SIZE bytes, above a canary, and vdi_ftos.c refuses memory
 * to a call that comes within VDI_FT_STACK_GUARD bytes of its bottom.
 * Here every engine call runs on such a stack, made with makecontext(),
 * and host_ftos.c refuses memory the same way.  The stack and its canary
 * are filled with a pattern first; afterwards the pattern shows how deep
 * the calls went, and the canary must be untouched.
 *
 * The fonts: a CFF OpenType font, a copy of it whose glyphs are built of
 * two others (seac, two frames of FreeType's CFF interpreter of about
 * 16 KB each), and copies of a TrueType font whose glyphs form chains of
 * composite glyphs, one recursion of FreeType's loader a level (mkfonts.c
 * makes them).  Every character of the fonts' character maps up to U+FFFF
 * is drawn at two sizes and compared with FreeType's own image of its
 * glyph, drawn on the main stack by a separate FreeType library.
 *
 * usage: test_stack <cff.otf> <seac.otf> <chain_short.ttf> <chain_long.ttf>
 *                   <chain_long_with_A.ttf> <loop.ttf>
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>

#include "vdi_ft2.h"
#include "host_ftos.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MODULE_H

#if defined(__SANITIZE_ADDRESS__)
#include <sanitizer/asan_interface.h>
#include <sanitizer/common_interface_defs.h>
#define ASAN 1

/* frames on the real stack, where their depth can be seen */
const char *__asan_default_options(void);
const char *__asan_default_options(void)
{
    return "detect_stack_use_after_return=0";
}
#endif

#define PATTERN     0xa5
#define CANARY      256                     /* bytes below the stack, as vdi_outline.c */
#define BIG_STACK   (2L * 1024 * 1024)      /* for the runs without the guard */

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


/*
 * the reference: FreeType on its own, on malloc(), on the main stack
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

/* from memory: EmuTOS's FreeType has no streams */
static int ref_open(const char *path)
{
    FILE *f;
    long size;

    if (FT_New_Library(&ref_memory, &ref_lib))
        return 0;
    FT_Add_Default_Modules(ref_lib);
    f = fopen(path, "rb");
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    ref_data = malloc(size);
    if (!ref_data || fread(ref_data, 1, size, f) != (size_t)size)
    {
        fclose(f);
        return 0;
    }
    fclose(f);
    return FT_New_Memory_Face(ref_lib, ref_data, size, 0, &ref_face) == 0;
}

static void ref_close(void)
{
    FT_Done_Face(ref_face);
    FT_Done_Library(ref_lib);
    free(ref_data);
    ref_data = NULL;
}


/*
 * running a call on a stack of its own
 */

static unsigned char *stack_mem;            /* canary, then the stack */
static long stack_size;                     /* of the stack, without the canary */
static ucontext_t main_ctx;
static void (*job)(void);

#ifdef ASAN
static const void *main_bottom;
static size_t main_size;
#endif

static void trampoline(void)
{
#ifdef ASAN
    __sanitizer_finish_switch_fiber(NULL, &main_bottom, &main_size);
#endif
    job();
#ifdef ASAN
    __sanitizer_start_switch_fiber(NULL, main_bottom, main_size);
#endif
}

/* a fresh stack of this size, filled with the pattern, the guard on or off */
static void new_stack(long size, int guard)
{
    free(stack_mem);
    stack_size = size;
    stack_mem = malloc(CANARY + size);
    if (!stack_mem)
    {
        perror("stack");
        exit(2);
    }
    memset(stack_mem, PATTERN, CANARY + size);
    if (guard)
        host_stack_guard(stack_mem, CANARY + size, CANARY + VDI_FT_STACK_GUARD);
    else
        host_stack_guard(NULL, 0, 0);
}

static void on_stack(void (*fn)(void))
{
    ucontext_t ctx;
#ifdef ASAN
    void *fake = NULL;
#endif

    job = fn;
    getcontext(&ctx);
    ctx.uc_stack.ss_sp = stack_mem + CANARY;
    ctx.uc_stack.ss_size = stack_size;
    ctx.uc_link = &main_ctx;
    makecontext(&ctx, trampoline, 0);
#ifdef ASAN
    __sanitizer_start_switch_fiber(&fake, stack_mem + CANARY, stack_size);
#endif
    swapcontext(&main_ctx, &ctx);
#ifdef ASAN
    __sanitizer_finish_switch_fiber(fake, NULL, NULL);
#endif
}

/* the deepest the calls since new_stack() went into the stack, in bytes */
static long depth(void)
{
    long i;

#ifdef ASAN
    __asan_unpoison_memory_region(stack_mem, CANARY + stack_size);
#endif
    for (i = 0; i < CANARY + stack_size && stack_mem[i] == PATTERN; i++)
        ;
    return CANARY + stack_size - i;
}


/*
 * the engine calls, with their arguments and results in statics
 */

static const char *arg_path;
static VdiOutlineFont *arg_face, *arg_font;
static long arg_px, ret;
static WORD arg_code;
static VdiOutlineBitmap ret_bm;

static void job_register(void)
{
    ret = vdi_ft_init();
    if (ret == 0)
        ret = vdi_ft_register_file(arg_path);
}

static void job_size(void)
{
    arg_font = vdi_ft_size(arg_face, (arg_px * 72 + 48) / 96, arg_px, 0);
}

static void job_render(void)
{
    memset(&ret_bm, 0, sizeof(ret_bm));
    ret = vdi_ft_render(arg_font, &arg_code, 1, VDI_FT_MAP_UNICODE, 0, 0, &ret_bm);
}

static void job_term(void)
{
    vdi_ft_term();
}


/*
 * comparing with the reference
 */

static int pixel(const VdiOutlineBitmap *b, long x, long y)
{
    const unsigned char *row = (const unsigned char *)b->data + y * b->wdwidth * 2;

    return (row[x >> 3] >> (7 - (x & 7))) & 1;
}

static int ref_pixel(const FT_Bitmap *rb, long x, long y)
{
    return (rb->buffer[y * rb->pitch + (x >> 3)] >> (7 - (x & 7))) & 1;
}

/*
 * ret_bm against FreeType's image of glyph gi at the reference size: 1 if
 * every pixel agrees, as test_vdi_fonts.c compares (the engine's image is
 * the character cell, so rows above or below it may be cut, not columns)
 */
static int same_as_reference_with(FT_UInt gi, FT_Int32 load_flags)
{
    FT_Bitmap *rb;
    long x, y, left, top;

    if (FT_Load_Glyph(ref_face, gi, load_flags) ||
        FT_Render_Glyph(ref_face->glyph, FT_RENDER_MODE_MONO))
        return 0;
    rb = &ref_face->glyph->bitmap;
    if (!ret_bm.data)
    {
        for (y = 0; y < (long)rb->rows; y++)
            for (x = 0; x < (long)rb->width; x++)
                if (ref_pixel(rb, x, y))
                    return 0;
        return 1;
    }

    left = ret_bm.origin_x + ref_face->glyph->bitmap_left;
    top = ret_bm.origin_y - ref_face->glyph->bitmap_top;
    for (y = 0; y < ret_bm.height; y++)
        for (x = 0; x < ret_bm.width; x++)
        {
            long rx = x - left, ry = y - top;
            int want = 0;

            if (rx >= 0 && ry >= 0 && rx < (long)rb->width && ry < (long)rb->rows)
                want = ref_pixel(rb, rx, ry);
            if (pixel(&ret_bm, x, y) != want)
                return 0;
        }
    for (y = 0; y < (long)rb->rows; y++)
        for (x = 0; x < (long)rb->width; x++)
            if (ref_pixel(rb, x, y) && (x + left < 0 || x + left >= ret_bm.width))
                return 0;
    return 1;
}


static int same_as_reference(FT_UInt gi)
{
    return same_as_reference_with(gi, FT_LOAD_DEFAULT);
}


/*
 * a font: registered and drawn character by character on the stack
 */

typedef struct {
    long registered;            /* faces registered */
    long codes, drawn, same;    /* characters tried, drawn, drawn as the reference */
    long not_drawn;             /* characters the engine could not draw */
    long simple_not_drawn;      /* of those, glyphs that are no composite */
    long refused;               /* allocations refused by the stack guard */
    long deepest;               /* bytes of stack */
} Result;

/* glyph gi of the reference is a TrueType composite glyph */
static int composite(FT_UInt gi)
{
    return !FT_Load_Glyph(ref_face, gi, FT_LOAD_NO_RECURSE | FT_LOAD_NO_SCALE) &&
           ref_face->glyph->format == FT_GLYPH_FORMAT_COMPOSITE;
}

static Result walk(const char *path)
{
    static const long sizes[] = { 16, 72 };
    Result r;
    long refused_before = host_stack_refused();
    unsigned i;

    memset(&r, 0, sizeof(r));
    if (!ref_open(path))
    {
        printf("  %s: the reference cannot open it\n", path);
        return r;
    }

    arg_path = path;
    on_stack(job_register);
    r.registered = (ret > 0) ? ret : 0;
    arg_face = (r.registered > 0) ? vdi_ft_face(0) : NULL;

    for (i = 0; arg_face && i < sizeof(sizes) / sizeof(sizes[0]); i++)
    {
        FT_ULong code;
        FT_UInt gi;

        arg_px = sizes[i];
        on_stack(job_size);
        if (!arg_font || FT_Set_Pixel_Sizes(ref_face, 0, arg_px))
            continue;
        for (code = FT_Get_First_Char(ref_face, &gi); gi; code = FT_Get_Next_Char(ref_face, code, &gi))
        {
            if (code > 0xffff)
                continue;
            arg_code = (WORD)code;
            on_stack(job_render);
            r.codes++;
            if (ret < 0)
            {
                r.not_drawn++;
                if (!composite(gi))
                    r.simple_not_drawn++;
                continue;
            }
            r.drawn++;
            r.same += same_as_reference(gi);
            vdi_ft_release(&ret_bm);
        }
    }

    on_stack(job_term);
    r.deepest = depth();
    r.refused = host_stack_refused() - refused_before;
    CHECK(host_alloc_live() == 0, "%s: %ld blocks left after vdi_ft_term", path, host_alloc_live());
    ref_close();
    return r;
}

/* without the guard, on a large stack: how deep loading one glyph goes */
static long natural_depth(const char *path, FT_ULong code)
{
    new_stack(BIG_STACK, 0);
    arg_path = path;
    on_stack(job_register);
    if (ret > 0 && (arg_face = vdi_ft_face(0)) != NULL)
    {
        arg_px = 16;
        on_stack(job_size);
        if (arg_font)
        {
            arg_code = (WORD)code;
            on_stack(job_render);
            if (ret >= 0)
                vdi_ft_release(&ret_bm);
        }
    }
    on_stack(job_term);
    return depth();
}

static void report(const char *what, const Result *r)
{
    printf("  %s: %ld face(s), %ld characters, %ld drawn, %ld as FreeType draws them, "
           "%ld not drawn (%ld simple glyphs), %ld allocations refused, deepest %ld bytes of %ld\n",
           what, r->registered, r->codes, r->drawn, r->same, r->not_drawn, r->simple_not_drawn,
           r->refused, r->deepest, stack_size);
}

/* the stack is the size of EmuTOS's font stack, with the guard */
static void real_stack(void)
{
    new_stack(VDI_FT_STACK_SIZE, 1);
}

/*
 * the lowest glyph of the chain glyphs first..last that a character up to
 * U+FFFF has, and that character; 0 if none
 */
static FT_UInt deepest_mapped(const char *path, FT_UInt first, FT_UInt last, FT_ULong *found)
{
    FT_ULong code;
    FT_UInt g, best = 0;

    *found = 0;
    if (!ref_open(path))
        return 0;
    for (code = FT_Get_First_Char(ref_face, &g); g; code = FT_Get_Next_Char(ref_face, code, &g))
        if (code <= 0xffff && g >= first && g <= last && (!best || g < best))
        {
            best = g;
            *found = code;
        }
    ref_close();
    return best;
}


/*
 * a font whose prep program never ends: FreeType refuses to hint it at
 * any size, and its glyphs are drawn unhinted instead of not at all
 */
static void check_loop(const char *path)
{
    static const char text[] = "Abg8";
    long i, same = 0;

    printf("a prep program that never ends\n");
    real_stack();
    if (!ref_open(path))
    {
        CHECK(0, "the reference cannot open %s", path);
        return;
    }
    CHECK(FT_Set_Pixel_Sizes(ref_face, 0, 16) == 0, "reference size");
    CHECK(FT_Load_Glyph(ref_face, FT_Get_Char_Index(ref_face, 'A'), FT_LOAD_DEFAULT) != 0,
          "FreeType hints a glyph of the font");
    arg_path = path;
    on_stack(job_register);
    CHECK(ret == 1, "%s not registered", path);
    arg_face = (ret == 1) ? vdi_ft_face(0) : NULL;
    if (arg_face)
    {
        arg_px = 16;
        on_stack(job_size);
        for (i = 0; arg_font && text[i]; i++)
        {
            arg_code = text[i];
            on_stack(job_render);
            if (ret >= 0)
            {
                same += same_as_reference_with(FT_Get_Char_Index(ref_face, (FT_ULong)text[i]),
                                               FT_LOAD_NO_HINTING);
                vdi_ft_release(&ret_bm);
            }
        }
        CHECK(arg_font && same == (long)sizeof(text) - 1, "%ld of %ld drawn unhinted, as FreeType draws them",
              same, (long)sizeof(text) - 1);
    }
    on_stack(job_term);
    CHECK(host_alloc_live() == 0, "%s: %ld blocks left after vdi_ft_term", path, host_alloc_live());
    ref_close();
}


int main(int argc, char **argv)
{
    const char *cff, *seac, *chain_short, *chain_long, *chain_a, *loop;
    Result plain, sr, cs, cl, ca;
    long need_plain, need_seac, need_chain;
    FT_UInt deep;
    FT_ULong deep_code;

    if (argc != 7)
    {
        fprintf(stderr, "usage: %s <cff.otf> <seac.otf> <chain_short.ttf> "
                "<chain_long.ttf> <chain_long_with_A.ttf> <loop.ttf>\n", argv[0]);
        return 2;
    }
    cff = argv[1];
    seac = argv[2];
    chain_short = argv[3];
    chain_long = argv[4];
    chain_a = argv[5];
    loop = argv[6];

    /* the long chain, as the Makefile has mkfonts make it */
#define LONG_FIRST  200
#define LONG_LAST   (200 + 1200 - 1)

    printf("stack of %ld bytes, guard %ld bytes, canary %d bytes\n",
           (long)VDI_FT_STACK_SIZE, (long)VDI_FT_STACK_GUARD, CANARY);

    /* first, what the glyphs need without the guard */
    need_plain = natural_depth(cff, 'A');
    need_seac = natural_depth(seac, 'a');
    deep = deepest_mapped(chain_long, LONG_FIRST, LONG_LAST, &deep_code);
    need_chain = natural_depth(chain_long, deep_code);
    printf("  without the guard: 'A' of the CFF font %ld bytes, seac 'a' %ld bytes, "
           "glyph %u, %u levels down the long chain, %ld bytes\n",
           need_plain, need_seac, deep, LONG_LAST + 1 - deep, need_chain);
    CHECK(need_seac > need_plain + 8192, "the seac glyph is not deeper (%ld, %ld)", need_seac, need_plain);
    CHECK(need_chain > VDI_FT_STACK_SIZE, "the long chain fits the stack (%ld)", need_chain);

    printf("CFF font\n");
    real_stack();
    plain = walk(cff);
    report("CFF", &plain);
    CHECK(plain.registered == 1, "%s", cff);
    CHECK(plain.codes > 200 && plain.same == plain.codes, "%ld of %ld", plain.same, plain.codes);
    CHECK(plain.refused == 0, "%ld refused", plain.refused);
    CHECK(plain.deepest <= stack_size, "into the canary: %ld", plain.deepest);

    printf("seac\n");
    real_stack();
    sr = walk(seac);
    report("seac", &sr);
    CHECK(sr.registered == 1, "%s", seac);
    CHECK(sr.codes > 200 && sr.same == sr.codes, "%ld of %ld", sr.same, sr.codes);
    CHECK(sr.refused == 0, "%ld refused", sr.refused);
    CHECK(sr.deepest <= stack_size, "into the canary: %ld", sr.deepest);
    CHECK(sr.deepest > plain.deepest, "seac %ld, plain %ld", sr.deepest, plain.deepest);

    printf("short chain\n");
    real_stack();
    cs = walk(chain_short);
    report("short chain", &cs);
    CHECK(cs.registered == 1, "%s", chain_short);
    CHECK(cs.codes > 1000 && cs.same == cs.codes, "%ld of %ld", cs.same, cs.codes);
    CHECK(cs.refused == 0, "%ld refused", cs.refused);
    CHECK(cs.deepest <= stack_size, "into the canary: %ld", cs.deepest);

    /* what can be drawn is drawn right, what is nested too deep is refused */
    printf("long chain\n");
    real_stack();
    cl = walk(chain_long);
    report("long chain", &cl);
    CHECK(cl.registered == 1, "%s", chain_long);
    CHECK(cl.drawn > 1000 && cl.same == cl.drawn, "%ld of %ld", cl.same, cl.drawn);
    CHECK(cl.not_drawn > 0, "every glyph of the chain was drawn");
    CHECK(cl.simple_not_drawn == 0, "%ld simple glyphs not drawn", cl.simple_not_drawn);
    CHECK(cl.refused > 0, "the guard refused nothing");
    CHECK(cl.deepest <= stack_size, "into the canary: %ld", cl.deepest);

    printf("long chain through 'A'\n");
    real_stack();
    ca = walk(chain_a);
    report("chain through 'A'", &ca);
    CHECK(ca.registered == 0, "a face whose 'A' cannot be loaded registered");
    CHECK(ca.refused > 0, "the guard refused nothing");
    CHECK(ca.deepest <= stack_size, "into the canary: %ld", ca.deepest);

    /* and the engine goes on as before */
    check_loop(loop);

    printf("the CFF font again\n");
    real_stack();
    plain = walk(cff);
    CHECK(plain.registered == 1 && plain.same == plain.codes && plain.codes > 200,
          "%ld of %ld", plain.same, plain.codes);

    host_stack_guard(NULL, 0, 0);
    free(stack_mem);
    printf("vdi-fonts stack: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
