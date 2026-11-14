/*
 * Host stand-in for EmuTOS's vdi/vdi_ftos.c and the FreeType memory
 * manager of vdi/vdi_ftlibc.c: memory from malloc(), files from stdio.
 *
 * Every block is counted, so the tests can see a leak as a number, and
 * allocations can be made to fail on demand (host_ftos.h).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vdi_ftos.h"
#include "host_ftos.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SYSTEM_H
#include <freetype/internal/ftobjs.h>

/* like vdi_ftos.c, each block starts with its size */
#define BLOCK_HEADER    8

static long fail_after = -1;
static long live, total, refused, loads;

void host_alloc_fail_after(long n)
{
    fail_after = n;
}

long host_alloc_live(void)
{
    return live;
}

long host_alloc_total(void)
{
    return total;
}

long host_alloc_refused(void)
{
    return refused;
}

long host_file_loads(void)
{
    return loads;
}


void *vdi_ft_alloc(ULONG size)
{
    unsigned char *block;

    if (fail_after == 0)
    {
        refused++;
        return NULL;
    }
    if (size > 0x7fffffffUL - BLOCK_HEADER)
        return NULL;

    block = malloc(size + BLOCK_HEADER);
    if (!block)
        return NULL;
    if (fail_after > 0)
        fail_after--;

    memcpy(block, &size, sizeof(size));
    live++;
    total++;
    return block + BLOCK_HEADER;
}


void vdi_ft_free(void *block)
{
    if (block)
    {
        free((unsigned char *)block - BLOCK_HEADER);
        live--;
    }
}


void *vdi_ft_realloc(void *block, ULONG size)
{
    ULONG oldsize;
    void *newblock;

    if (!block)
        return vdi_ft_alloc(size);

    if (size == 0)
    {
        vdi_ft_free(block);
        return NULL;
    }

    memcpy(&oldsize, (unsigned char *)block - BLOCK_HEADER, sizeof(oldsize));
    newblock = vdi_ft_alloc(size);
    if (!newblock)
        return NULL;

    memcpy(newblock, block, (oldsize < size) ? oldsize : size);
    vdi_ft_free(block);

    return newblock;
}


void *vdi_font_file_load(const char *path, ULONG *size)
{
    FILE *f;
    long length;
    void *data = NULL;

    f = fopen(path, "rb");
    if (!f)
        return NULL;

    if (fseek(f, 0, SEEK_END) == 0 && (length = ftell(f)) > 0 &&
        fseek(f, 0, SEEK_SET) == 0)
    {
        data = vdi_ft_alloc(length);
        if (data)
        {
            if (fread(data, 1, length, f) == (size_t)length)
            {
                if (size)
                    *size = length;
                loads++;
            }
            else
            {
                vdi_ft_free(data);
                data = NULL;
            }
        }
    }
    fclose(f);

    return data;
}


void vdi_font_file_free(void *data)
{
    vdi_ft_free(data);
}


/* the engine never scans directories itself */
LONG vdi_font_dir_scan(const char *dir,
                       void (*found)(const char *path, void *ctx), void *ctx)
{
    (void)dir;
    (void)found;
    (void)ctx;
    return -1;
}


/* FreeType's memory manager, as in vdi_ftlibc.c */

static void *ft_alloc_cb(FT_Memory memory, long size)
{
    (void)memory;
    return vdi_ft_alloc(size);
}

static void ft_free_cb(FT_Memory memory, void *block)
{
    (void)memory;
    vdi_ft_free(block);
}

static void *ft_realloc_cb(FT_Memory memory, long cur_size, long new_size,
                           void *block)
{
    (void)memory;
    (void)cur_size;
    return vdi_ft_realloc(block, new_size);
}

static const struct FT_MemoryRec_ host_memory =
{
    NULL,
    ft_alloc_cb,
    ft_free_cb,
    ft_realloc_cb
};

FT_BASE_DEF(FT_Memory) FT_New_Memory(void)
{
    return (FT_Memory)&host_memory;
}

FT_BASE_DEF(void) FT_Done_Memory(FT_Memory memory)
{
    (void)memory;
}
