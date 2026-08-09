/*
 * setres.c -- pick shinogi's screen resolution from inside the guest.
 *
 * The mode is fixed when QEMU starts: EmuTOS asks the host for a size at
 * boot (GET_DISPLAY_INFO) and the host answers from the virtio-gpu device
 * properties the launcher passed.  Nothing running in the guest can resize
 * a scanout that is already up -- that would need a new virtio-gpu resource,
 * a VDI that can be re-initialised underneath the AES, and an AES restart.
 *
 * So this writes the choice to C:\SHINOGI.INI and leaves it there.  The
 * launcher reads that file on the way back in, which makes the round trip
 * "choose, shut down, start again" rather than an instant switch.  Saying
 * so plainly beats a dialog that appears to change the screen and does not.
 *
 * Deliberately a plain TOS program, not a GEM one: it needs a list and a
 * keypress, the desktop can run it, and a GEM dialog here would be a
 * resource file and an event loop for no gain.
 *
 * Build: m68k-atari-mint-gcc -O2 -o setres.prg setres.c
 */

#include <osbind.h>
#include <stdio.h>
#include <string.h>

#define INI_PATH "C:\\SHINOGI.INI"

struct mode {
    int  w, h;
    const char *note;
};

/*
 * Sizes, not Atari mode numbers.  A modecode means nothing to virtio-gpu,
 * which is why the AES's own resolution dialogs are useless on this machine
 * -- they offer ST and TT modes that do not exist here.
 *
 * The launcher clamps to 320x200 .. 1920x1080 and the guest rounds the width
 * down to a multiple of 8, so keep widths on that boundary and everything
 * below stays exactly what the user asked for.
 */
static const struct mode modes[] = {
    {  640,  480, "small"                    },
    {  800,  600, ""                         },
    { 1024,  768, "default"                  },
    { 1280,  720, "16:9"                     },
    { 1280,  800, ""                         },
    { 1440,  900, ""                         },
    { 1680, 1050, ""                         },
    { 1920, 1080, "largest; GEM gets tiny"   },
};
#define NMODES ((int)(sizeof(modes) / sizeof(modes[0])))

/* Read the size currently written in the file, 0 if there is none. */
static int current(int *w, int *h)
{
    FILE *f = fopen(INI_PATH, "r");
    char line[256];
    int got = 0;

    if (!f)
        return 0;

    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, " res = %dx%d", w, h) == 2 ||
            sscanf(line, " res=%dx%d", w, h) == 2) {
            got = 1;
            break;
        }
    }
    fclose(f);

    return got;
}

/*
 * Rewrite the file, preserving every line that is not ours.  It is the
 * launcher's config, not this program's, and a future setting written by
 * something else must survive being edited here.
 */
static int store(int w, int h)
{
    char (*keep)[256] = NULL;
    int nkeep = 0, i, ok = 0;
    char line[256];
    FILE *f;

    keep = (char (*)[256]) Malloc(64L * 256);
    if (!keep)
        return 0;

    if ((f = fopen(INI_PATH, "r")) != NULL) {
        while (nkeep < 64 && fgets(line, sizeof(line), f)) {
            int dw, dh;

            if (sscanf(line, " res = %dx%d", &dw, &dh) == 2 ||
                sscanf(line, " res=%dx%d", &dw, &dh) == 2)
                continue;                       /* ours; replaced below */
            strncpy(keep[nkeep], line, 255);
            keep[nkeep][255] = '\0';
            nkeep++;
        }
        fclose(f);
    }

    if ((f = fopen(INI_PATH, "w")) != NULL) {
        fprintf(f, "# shinogi settings, read by the launcher at startup.\r\n");
        fprintf(f, "res = %dx%d\r\n", w, h);
        for (i = 0; i < nkeep; i++)
            fputs(keep[i], f);
        ok = (fclose(f) == 0);
    }

    Mfree(keep);

    return ok;
}

int main(void)
{
    int w = 0, h = 0, i, c;

    Cconws("\033E");                            /* clear */
    Cconws("shinogi -- screen resolution\r\n\r\n");

    if (current(&w, &h))
        printf("Currently set: %dx%d\r\n\r\n", w, h);
    else
        printf("Nothing set yet; the launcher uses 1024x768.\r\n\r\n");

    for (i = 0; i < NMODES; i++) {
        printf("  %d) %4dx%-4d %s\r\n",
               i + 1, modes[i].w, modes[i].h, modes[i].note);
    }
    printf("\r\n  Q) leave it alone\r\n\r\n");
    printf("Choose 1-%d: ", NMODES);

    c = (int)(Cconin() & 0xff);
    printf("%c\r\n\r\n", c);

    if (c == 'q' || c == 'Q' || c < '1' || c > '0' + NMODES) {
        Cconws("Unchanged.\r\n\r\nPress a key.\r\n");
        Cconin();
        return 0;
    }

    i = c - '1';
    if (!store(modes[i].w, modes[i].h)) {
        Cconws("Could not write " INI_PATH "\r\n");
        Cconws("Drive C may be read-only.\r\n\r\nPress a key.\r\n");
        Cconin();
        return 1;
    }

    printf("Set to %dx%d.\r\n\r\n", modes[i].w, modes[i].h);
    Cconws("This takes effect the NEXT time shinogi starts --\r\n");
    Cconws("the screen cannot be resized while it is running.\r\n");
    Cconws("Shut down from the desktop, then start it again.\r\n");
    Cconws("\r\nPress a key.\r\n");
    Cconin();

    return 0;
}
