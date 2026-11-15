/*
 * mkfonts.c - fonts that nest glyphs deeply, for test_stack.c
 *
 *   mkfonts seac IN.otf OUT.otf
 *       a copy of a CFF OpenType font in which every glyph but 'A' and
 *       'acute' is the implied seac form of endchar, "0 0 A acute
 *       endchar": FreeType draws each as an accented A, interpreting the
 *       two component charstrings one level down (about 16 KB of stack a
 *       level)
 *
 *   mkfonts chain IN.ttf OUT.ttf N [FIRST]
 *       a copy of a TrueType font in which the N glyphs from FIRST (1 if
 *       not given) form one chain of composite glyphs: glyph i is glyph
 *       i+1 moved one unit right, and the last refers to the first simple
 *       glyph with an outline after it.  FreeType loads a composite glyph one recursion a level and
 *       sets no limit but a repeated glyph.
 *
 *   mkfonts loop IN.ttf OUT.ttf
 *       a copy of a TrueType font whose prep program jumps back to its
 *       start for ever: FreeType stops it at its instruction limit, and
 *       then refuses to hint any glyph at that size
 *
 * The glyph numbers of 'A' and 'acute' come from FreeType itself, the
 * rest is done on the bytes of the file.  Table checksums are left as they
 * are (FreeType does not check them).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ft2build.h>
#include FT_FREETYPE_H

static unsigned char *data;
static long size;

static void die(const char *what)
{
    fprintf(stderr, "mkfonts: %s\n", what);
    exit(1);
}

static unsigned long be(const unsigned char *p, int n)
{
    unsigned long v = 0;

    while (n--)
        v = (v << 8) | *p++;
    return v;
}

static void put_be(unsigned char *p, unsigned long v, int n)
{
    while (n--)
    {
        p[n] = (unsigned char)v;
        v >>= 8;
    }
}

static void read_font(const char *path)
{
    FILE *f = fopen(path, "rb");

    if (!f)
        die("cannot open the input font");
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = malloc(size);
    if (!data || fread(data, 1, size, f) != (size_t)size)
        die("cannot read the input font");
    fclose(f);
}

static void write_font(const char *path)
{
    FILE *f = fopen(path, "wb");

    if (!f || fwrite(data, 1, size, f) != (size_t)size || fclose(f))
        die("cannot write the output font");
}

/* the table directory entry of a table; NULL if there is none */
static unsigned char *table(const char *tag)
{
    long n = be(data + 4, 2), i;

    for (i = 0; i < n; i++)
        if (!memcmp(data + 12 + 16 * i, tag, 4))
            return data + 12 + 16 * i;
    return NULL;
}

/*
 * ---------------------------------------------------------------------------
 * seac
 * ---------------------------------------------------------------------------
 */

/* a CFF INDEX at p: its count, and the offset of each item */
static long cff_index(long p, long *count, long **starts)
{
    long n = be(data + p, 2), i, offsize, base;

    *count = n;
    *starts = NULL;
    if (n == 0)
        return p + 2;
    offsize = data[p + 2];
    *starts = malloc((n + 1) * sizeof(long));
    base = p + 3 + (n + 1) * offsize - 1;
    for (i = 0; i <= n; i++)
        (*starts)[i] = base + (long)be(data + p + 3 + i * offsize, offsize);
    return (*starts)[n];
}

/* the CharStrings offset (operator 17) from a Top DICT */
static long charstrings_offset(long s, long e)
{
    long stack[48], sp = 0, q = s, result = -1;

    while (q < e)
    {
        unsigned b0 = data[q];

        if (b0 <= 21)
        {
            unsigned op = b0;

            q++;
            if (b0 == 12)
                op = 1200 + data[q++];
            if (op == 17 && sp > 0)
                result = stack[sp - 1];
            sp = 0;
            continue;
        }
        if (sp == 48)
            die("Top DICT operand stack overflow");
        if (b0 == 28)
        {
            stack[sp++] = (short)be(data + q + 1, 2);
            q += 3;
        }
        else if (b0 == 29)
        {
            stack[sp++] = (long)(int)be(data + q + 1, 4);
            q += 5;
        }
        else if (b0 == 30)
        {
            for (q++; q < e; q++)
                if ((data[q] & 0xf) == 0xf || (data[q] >> 4) == 0xf)
                    break;
            q++;
            stack[sp++] = 0;
        }
        else if (b0 >= 32 && b0 <= 246)
        {
            stack[sp++] = (long)b0 - 139;
            q++;
        }
        else if (b0 >= 247 && b0 <= 250)
        {
            stack[sp++] = (long)(b0 - 247) * 256 + data[q + 1] + 108;
            q += 2;
        }
        else if (b0 >= 251 && b0 <= 254)
        {
            stack[sp++] = -(long)(b0 - 251) * 256 - data[q + 1] - 108;
            q += 2;
        }
        else
            q++;
    }
    return result;
}

/* fonts are given to FreeType from memory: EmuTOS's build has no streams */
static int glyph_named(const char *name)
{
    FT_Library lib;
    FT_Face face;
    int gi;

    if (FT_Init_FreeType(&lib) || FT_New_Memory_Face(lib, data, size, 0, &face))
        die("FreeType cannot open the input font");
    gi = FT_Get_Name_Index(face, name);
    FT_Done_Face(face);
    FT_Done_FreeType(lib);
    if (!gi)
        die("the font has no glyph of that name");
    return gi;
}

static void make_seac(const char *out)
{
    /* adx 0, ady 0, bchar 65 'A', achar 194 'acute', endchar */
    static const unsigned char seac[] = { 139, 139, 139 + 65, 247, 194 - 108, 14 };
    unsigned char *t = table("CFF ");
    long cff, p, count, *items, cs, n, *glyphs, gi, done = 0;
    int a, acute;

    if (!t)
        die("the font has no CFF table");
    a = glyph_named("A");
    acute = glyph_named("acute");

    cff = be(t + 8, 4);
    p = cff + data[cff + 2];                    /* past the header */
    p = cff_index(p, &count, &items);           /* Name INDEX */
    free(items);
    p = cff_index(p, &count, &items);           /* Top DICT INDEX */
    if (count < 1)
        die("the CFF table has no Top DICT");
    cs = charstrings_offset(items[0], items[1]);
    free(items);
    if (cs < 0)
        die("the Top DICT gives no CharStrings");
    cff_index(cff + cs, &n, &glyphs);

    for (gi = 1; gi < n; gi++)
    {
        if (gi == a || gi == acute)
            continue;
        if (glyphs[gi + 1] - glyphs[gi] < (long)sizeof(seac))
            continue;                           /* too short to hold it */
        memcpy(data + glyphs[gi], seac, sizeof(seac));
        done++;
    }
    free(glyphs);
    write_font(out);
    printf("%s: %ld of %ld glyphs are seac (A %d, acute %d)\n", out, done, n, a, acute);
}

/*
 * ---------------------------------------------------------------------------
 * chain
 * ---------------------------------------------------------------------------
 */

static void make_chain(const char *out, long n, long first)
{
    unsigned char *head = table("head"), *maxp = table("maxp");
    unsigned char *loca = table("loca"), *glyf = table("glyf");
    long loff, goff, numg, i, end = -1, pos, newsize, glyf_at, loca_at, last;
    int long_loca;
    long *start, *len;
    unsigned char *out_data;

    if (!head || !maxp || !loca || !glyf)
        die("the font has no TrueType glyphs");
    long_loca = (short)be(data + be(head + 8, 4) + 50, 2);
    numg = be(data + be(maxp + 8, 4) + 4, 2);
    loff = be(loca + 8, 4);
    goff = be(glyf + 8, 4);
    last = first + n - 1;
    if (n < 1 || first < 1 || last + 1 >= numg)
        die("the chain is longer than the font");

    start = malloc(numg * sizeof(long));
    len = malloc(numg * sizeof(long));
    for (i = 0; i < numg; i++)
    {
        long a, b;

        if (long_loca)
        {
            a = be(data + loff + 4 * i, 4);
            b = be(data + loff + 4 * i + 4, 4);
        }
        else
        {
            a = 2 * be(data + loff + 2 * i, 2);
            b = 2 * be(data + loff + 2 * i + 2, 2);
        }
        start[i] = goff + a;
        len[i] = b - a;
    }
    for (i = last + 1; i < numg; i++)
        if (len[i] >= 10 && (short)be(data + start[i], 2) > 0)
        {
            end = i;
            break;
        }
    if (end < 0)
        die("no simple glyph to end the chain on");

    /* the new glyf and loca tables go at the end of the file */
    newsize = (size + 3) & ~3L;
    glyf_at = newsize;
    for (i = 0; i < numg; i++)
        newsize += ((i >= first && i <= last) ? 20 : ((len[i] + 3) & ~3L));
    loca_at = newsize;
    newsize += 4 * (numg + 1);

    out_data = calloc(1, newsize);
    memcpy(out_data, data, size);
    pos = glyf_at;
    for (i = 0; i < numg; i++)
    {
        put_be(out_data + loca_at + 4 * i, pos - glyf_at, 4);
        if (i >= first && i <= last)
        {
            unsigned char *g = out_data + pos;

            put_be(g, 0xffff, 2);               /* a composite glyph */
            put_be(g + 2, 0, 2);
            put_be(g + 4, 0, 2);
            put_be(g + 6, 1000, 2);
            put_be(g + 8, 1000, 2);
            put_be(g + 10, 0x0003, 2);          /* word x,y arguments, one component */
            put_be(g + 12, (i < last) ? i + 1 : end, 2);
            put_be(g + 14, 1, 2);
            put_be(g + 16, 0, 2);
            pos += 20;
        }
        else
        {
            memcpy(out_data + pos, data + start[i], len[i]);
            pos += (len[i] + 3) & ~3L;
        }
    }
    put_be(out_data + loca_at + 4 * numg, pos - glyf_at, 4);

    free(data);
    data = out_data;
    size = newsize;
    glyf = table("glyf");
    loca = table("loca");
    head = table("head");
    put_be(glyf + 8, glyf_at, 4);
    put_be(glyf + 12, loca_at - glyf_at, 4);
    put_be(loca + 8, loca_at, 4);
    put_be(loca + 12, 4 * (numg + 1), 4);
    put_be(data + be(head + 8, 4) + 50, 1, 2);  /* long loca */
    free(start);
    free(len);
    write_font(out);
    printf("%s: glyphs %ld..%ld chain to glyph %ld of %ld\n", out, first, last, end, numg);
}

/*
 * ---------------------------------------------------------------------------
 * loop
 * ---------------------------------------------------------------------------
 */

static void make_loop(const char *out)
{
    /* PUSHW[0] -3, JMPR: back to the PUSHW */
    static const unsigned char loop[] = { 0xb8, 0xff, 0xfd, 0x1c };
    unsigned char *t = table("prep");

    if (!t || be(t + 12, 4) < sizeof(loop))
        die("the font has no prep program to replace");
    memcpy(data + be(t + 8, 4), loop, sizeof(loop));
    put_be(t + 12, sizeof(loop), 4);
    write_font(out);
    printf("%s: its prep program loops\n", out);
}

int main(int argc, char **argv)
{
    if (argc == 4 && !strcmp(argv[1], "seac"))
    {
        read_font(argv[2]);
        make_seac(argv[3]);
        return 0;
    }
    if ((argc == 5 || argc == 6) && !strcmp(argv[1], "chain"))
    {
        read_font(argv[2]);
        make_chain(argv[3], atol(argv[4]), (argc == 6) ? atol(argv[5]) : 1);
        return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "loop"))
    {
        read_font(argv[2]);
        make_loop(argv[3]);
        return 0;
    }
    fprintf(stderr, "usage: mkfonts seac IN.otf OUT.otf | mkfonts chain IN.ttf OUT.ttf N [FIRST]"
            " | mkfonts loop IN.ttf OUT.ttf\n");
    return 2;
}
