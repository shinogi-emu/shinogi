#include <stdint.h>
#ifndef GUEST
#include <stdio.h>
#include <stdlib.h>
#endif

#ifdef GUEST
typedef unsigned short UWORD;
typedef short WORD;
typedef unsigned long ULONG;
#else
typedef uint16_t UWORD;
typedef int16_t WORD;
typedef uint32_t ULONG;
#endif
#ifndef GPU_DAMAGE_HEADER
#define GPU_DAMAGE_HEADER "../emutos/bios/gpu_damage.h"
#endif
#include GPU_DAMAGE_HEADER

#define WIDTH 1920UL
#define HEIGHT 1080UL
#ifdef GUEST
/* mshort limits individual C objects to 64KiB; use reserved guest RAM. */
#define pixels ((UWORD *)0x01000000UL)
#define previous ((UWORD *)0x01400000UL)
#define argb ((ULONG *)0x01800000UL)
#define reference ((ULONG *)0x02100000UL)
#define colours ((ULONG *)0x02a00000UL)
#else
static UWORD pixels[WIDTH * HEIGHT], previous[WIDTH * HEIGHT];
static ULONG argb[WIDTH * HEIGHT], reference[WIDTH * HEIGHT], colours[65536UL];
#endif
static ULONG random_state = 0x12345678;
static unsigned failure_line;
static ULONG detail[8];

static ULONG next_random(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}

static void finish(int ok)
{
#ifdef GUEST
    if (!ok) {
        unsigned entry;
        unsigned shift;
        for (shift = 12; ; shift -= 4) {
            *(volatile ULONG *)0xff008000UL = "0123456789ABCDEF"[(failure_line >> shift) & 15];
            if (!shift) break;
        }
        for (entry = 0; entry < 8; entry++) {
            *(volatile ULONG *)0xff008000UL = ' ';
            for (shift = 28; ; shift -= 4) {
                *(volatile ULONG *)0xff008000UL = "0123456789ABCDEF"[(detail[entry] >> shift) & 15];
                if (!shift) break;
            }
        }
    }
    *(volatile ULONG *)0xff008000UL = ok ? 'P' : 'F';
    *(volatile ULONG *)0xff009004UL = 2;
    for (;;) __asm__ volatile ("stop #0x2700");
#else
    if (ok) puts("GPU damage: complete reference frames, bounds and idle passed");
    else printf("GPU damage FAIL at line %u\n", failure_line);
    exit(ok ? 0 : 1);
#endif
}

#define require(ok) do { if (!(ok)) { failure_line = __LINE__; finish(0); } } while (0)

static void check(UWORD width, UWORD height)
{
    GPU_DAMAGE damage;
    ULONG count = (ULONG)width * height, i;
    int run;
    for (i = 0; i < count; i++) {
        pixels[i] = (UWORD)next_random();
        previous[i] = (UWORD)~pixels[i];
        reference[i] = colours[pixels[i]];
    }
    require(gpu_damage_update(pixels, previous, argb, colours, width, height, 1, &damage));
    require(damage.x == 0 && damage.y == 0 && damage.w == width && damage.h == height);
    for (i = 0; i < count; i++) require(argb[i] == reference[i]);
    require(!gpu_damage_update(pixels, previous, argb, colours, width, height, 0, &damage));
    for (run = 0; run < 12; run++) {
        ULONG changes[24];
        unsigned changed[24];
        unsigned j;
        for (j = 0; j < 24; j++) {
            ULONG index = j == 0 ? 0 : j == 1 ? count - 1 : next_random() % count;
            changes[j] = index;
            pixels[index] ^= (UWORD)(1U << (j % 16));
            reference[index] = colours[pixels[index]];
        }
        for (j = 0; j < 24; j++)
            changed[j] = pixels[changes[j]] != previous[changes[j]];
        require(gpu_damage_update(pixels, previous, argb, colours, width, height, 0, &damage));
        require(damage.w && damage.h && damage.x + damage.w <= width && damage.y + damage.h <= height);
        for (j = 0; j < 24; j++) {
            ULONG x = changes[j] % width, y = changes[j] / width;
            if (!changed[j]) continue;
            detail[0] = width; detail[1] = height;
            detail[2] = damage.x; detail[3] = damage.y;
            detail[4] = damage.w; detail[5] = damage.h;
            detail[6] = x; detail[7] = y;
            require(x >= damage.x && x < damage.x + damage.w &&
                    y >= damage.y && y < damage.y + damage.h);
        }
        for (i = 0; i < count; i++) {
            require(argb[i] == reference[i]);
            require(previous[i] == pixels[i]);
        }
        require(!gpu_damage_update(pixels, previous, argb, colours, width, height, 0, &damage));
    }
    /* One pixel at each edge must not force an unrelated whole row. */
    for (i = 0; i < 4; i++) {
        ULONG index = i == 0 ? 0 : i == 1 ? (ULONG)width - 1 : i == 2 ? count - width : count - 1;
        pixels[index] ^= 1;
        require(gpu_damage_update(pixels, previous, argb, colours, width, height, 0, &damage));
        require(damage.w == 2 && damage.h == 1);
    }
}

int main(void)
{
    ULONG i;
    for (i = 0; i < 65536; i++) colours[i] = i * 0x010101UL;
    check(8, 1);
    check(320, 200);
    check(1024, 768);
    check(WIDTH, HEIGHT);
    finish(1);
    return 0;
}

#ifdef GUEST
__asm__(".text\n.globl _start\n_start:\nmove.l #0x03f00000,%sp\njsr main\n");
#endif
