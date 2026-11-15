/*
 * render_crc.c - a checksum of every glyph image FreeType draws
 *
 * For each font, size and render mode (monochrome, as the engine draws,
 * and grey), every glyph is loaded and rendered, and the image, its
 * placement and its advance go into one CRC-32, printed as a line.
 *
 * The Makefile builds this twice: against EmuTOS's FreeType configuration
 * and against FreeType's own values of the settings EmuTOS changes (the
 * render pool and the bytecode instruction limit), and compares the two
 * outputs: EmuTOS's settings must not change a pixel.
 *
 * usage: render_crc <font>...
 */

#include <stdio.h>
#include <stdlib.h>

#include <ft2build.h>
#include FT_FREETYPE_H

static unsigned long crc_table[256];

static void crc_init(void)
{
    unsigned long c;
    int n, k;

    for (n = 0; n < 256; n++)
    {
        c = (unsigned long)n;
        for (k = 0; k < 8; k++)
            c = (c & 1) ? 0xedb88320UL ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}

static unsigned long crc(unsigned long c, const void *data, size_t len)
{
    const unsigned char *p = data;

    c ^= 0xffffffffUL;
    while (len--)
        c = crc_table[(c ^ *p++) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffUL;
}

static unsigned long crc_long(unsigned long c, long v)
{
    unsigned char b[4];

    b[0] = (unsigned char)v;
    b[1] = (unsigned char)(v >> 8);
    b[2] = (unsigned char)(v >> 16);
    b[3] = (unsigned char)(v >> 24);
    return crc(c, b, 4);
}

int main(int argc, char **argv)
{
    static const int sizes[] = { 8, 12, 16, 24, 48, 120, 300 };
    static const FT_Render_Mode modes[] = { FT_RENDER_MODE_MONO, FT_RENDER_MODE_NORMAL };
    FT_Library lib;
    FT_Face face;
    int a, s, m;

    crc_init();
    if (FT_Init_FreeType(&lib))
        return 2;
    for (a = 1; a < argc; a++)
    {
        FILE *f = fopen(argv[a], "rb");
        unsigned char *data = NULL;
        long size = 0;

        /* from memory: EmuTOS's FreeType has no streams */
        if (f)
        {
            fseek(f, 0, SEEK_END);
            size = ftell(f);
            fseek(f, 0, SEEK_SET);
            data = malloc(size);
            if (data && fread(data, 1, size, f) != (size_t)size)
                size = 0;
            fclose(f);
        }
        if (!data || !size || FT_New_Memory_Face(lib, data, size, 0, &face))
        {
            printf("%s: cannot open\n", argv[a]);
            free(data);
            continue;
        }
        for (s = 0; s < (int)(sizeof(sizes) / sizeof(sizes[0])); s++)
        {
            if (FT_Set_Pixel_Sizes(face, 0, sizes[s]))
                continue;
            for (m = 0; m < 2; m++)
            {
                unsigned long c = 0;
                long gi, failed = 0;

                for (gi = 0; gi < face->num_glyphs; gi++)
                {
                    FT_GlyphSlot g = face->glyph;
                    FT_Bitmap *b = &g->bitmap;
                    unsigned row;

                    if (FT_Load_Glyph(face, (FT_UInt)gi, FT_LOAD_DEFAULT) ||
                        FT_Render_Glyph(g, modes[m]))
                    {
                        failed++;
                        c = crc_long(c, -1);
                        continue;
                    }
                    c = crc_long(c, b->width);
                    c = crc_long(c, b->rows);
                    c = crc_long(c, g->bitmap_left);
                    c = crc_long(c, g->bitmap_top);
                    c = crc_long(c, g->advance.x);
                    for (row = 0; row < b->rows; row++)
                        c = crc(c, b->buffer + (long)row * b->pitch,
                                (m == 0) ? (b->width + 7) / 8 : b->width);
                }
                printf("%s %d px %s: %ld glyphs, %ld failed, crc %08lx\n", argv[a], sizes[s],
                       m ? "grey" : "mono", face->num_glyphs, failed, c);
            }
        }
        FT_Done_Face(face);
        free(data);
    }
    FT_Done_FreeType(lib);
    return 0;
}
