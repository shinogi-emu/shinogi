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
 * A GEM program with its own window, like every other desktop tool: a
 * text-mode program started from the desktop takes over the whole screen.
 * The window lists the sizes; a click on one, or its number key, chooses
 * it, and an alert says what happens next.  No resource file: the list is
 * drawn with the VDI.  Started where there is no AES (a shell), it falls
 * back to the same choice in text.
 *
 * Build: m68k-atari-mint-gcc -O2 -o setres.prg setres.c -lgem
 */

#include <osbind.h>
#include <stdio.h>
#include <string.h>
#include <gem.h>

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

/* The text version, for a shell with no AES. */
static int text_main(void)
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

/* ------------------------------------------------------------ GEM */

/*
 * Window contents, one text line each: a heading, the current setting,
 * a blank line, the sizes, a blank line and a hint.  FIRST_MODE is the
 * line of the first size, which is what a click is measured against.
 */
#define FIRST_MODE  3
#define NLINES      (FIRST_MODE + NMODES + 2)
#define COLS        42

static short vh, cw, ch;               /* VDI handle, character cell */
static short win = -1;
static GRECT work;                      /* window work area */
static int sel = -1;                    /* highlighted size, -1 none */
static int cur_w, cur_h, have_cur;

static void line_text(int n, char *buf)
{
    buf[0] = '\0';
    if (n == 0)
        strcpy(buf, " Screen size for the next start:");
    else if (n == 1) {
        if (have_cur)
            sprintf(buf, " now set to %dx%d", cur_w, cur_h);
        else
            strcpy(buf, " nothing set; the launcher uses 1024x768");
    } else if (n >= FIRST_MODE && n < FIRST_MODE + NMODES) {
        const struct mode *m = &modes[n - FIRST_MODE];
        sprintf(buf, "  %d  %4dx%-4d  %s", n - FIRST_MODE + 1, m->w, m->h, m->note);
    } else if (n == NLINES - 1)
        sprintf(buf, " Click a size or press 1-%d.  Esc: cancel", NMODES);
}

static void draw(const GRECT *clip)
{
    short pxy[4];
    char buf[80];
    int n;

    pxy[0] = clip->g_x; pxy[1] = clip->g_y;
    pxy[2] = clip->g_x + clip->g_w - 1; pxy[3] = clip->g_y + clip->g_h - 1;
    vs_clip(vh, 1, pxy);
    vswr_mode(vh, MD_REPLACE);
    vsf_interior(vh, FIS_SOLID);
    vsf_color(vh, G_WHITE);
    vr_recfl(vh, pxy);

    for (n = 0; n < NLINES; n++) {
        short y = work.g_y + n * ch;
        int hi = n >= FIRST_MODE && n - FIRST_MODE == sel;

        if (y > clip->g_y + clip->g_h || y + ch < clip->g_y)
            continue;
        line_text(n, buf);
        if (hi) {
            short bar[4];
            bar[0] = work.g_x; bar[1] = y;
            bar[2] = work.g_x + work.g_w - 1; bar[3] = y + ch - 1;
            vsf_color(vh, G_BLACK);
            vr_recfl(vh, bar);
            vsf_color(vh, G_WHITE);
        }
        vswr_mode(vh, MD_TRANS);
        vst_color(vh, hi ? G_WHITE : G_BLACK);
        v_gtext(vh, work.g_x, y, buf);
        vswr_mode(vh, MD_REPLACE);
    }
    vs_clip(vh, 0, pxy);
}

/* Redraw the part of AREA that is visible, through the rectangle list. */
static void redraw(const GRECT *area)
{
    GRECT r, desk;

    wind_update(BEG_UPDATE);
    graf_mouse(M_OFF, NULL);
    wind_get_grect(0, WF_WORKXYWH, &desk);
    wind_get_grect(win, WF_FIRSTXYWH, &r);
    while (r.g_w && r.g_h) {
        if (rc_intersect(area, &r) && rc_intersect(&desk, &r))
            draw(&r);
        wind_get_grect(win, WF_NEXTXYWH, &r);
    }
    graf_mouse(M_ON, NULL);
    wind_update(END_UPDATE);
}

static void highlight(int i)
{
    GRECT r;

    sel = i;
    r = work;                           /* the size lines only */
    r.g_y += FIRST_MODE * ch;
    r.g_h = NMODES * ch;
    redraw(&r);
}

/* Store size I and say what happens next.  Returns 1 to quit. */
static int choose(int i)
{
    char msg[200];

    highlight(i);
    if (!store(modes[i].w, modes[i].h)) {
        form_alert(1, "[3][Could not write C:\\SHINOGI.INI.|"
                      "Drive C may be read-only.][ OK ]");
        return 0;
    }
    sprintf(msg, "[1][Screen size set to %dx%d.|"
                 "It takes effect the next time|"
                 "Shinogi starts: shut down from|"
                 "the desktop, then start again.][ OK ]",
            modes[i].w, modes[i].h);
    form_alert(1, msg);
    return 1;
}

static int gem_main(void)
{
    short work_in[11] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2 }, work_out[57];
    short msg[8], mx, my, mb, ks, key, clicks, ev, dummy, i;
    GRECT desk, outer;
    int quit = 0;

    vh = graf_handle(&cw, &ch, &dummy, &dummy);
    v_opnvwk(work_in, &vh, work_out);
    if (vh <= 0) {
        form_alert(1, "[3][No VDI workstation.][ OK ]");
        return 1;
    }
    vst_alignment(vh, TA_LEFT, TA_TOP, &dummy, &dummy);
    have_cur = current(&cur_w, &cur_h);
    for (i = 0; i < NMODES; i++)
        if (have_cur && modes[i].w == cur_w && modes[i].h == cur_h)
            sel = i;

    wind_get_grect(0, WF_WORKXYWH, &desk);
    work.g_w = COLS * cw;
    work.g_h = NLINES * ch;
    wind_calc_grect(WC_BORDER, NAME | CLOSER | MOVER, &work, &outer);
    outer.g_x = desk.g_x + (desk.g_w - outer.g_w) / 2;
    outer.g_y = desk.g_y + (desk.g_h - outer.g_h) / 3;
    win = wind_create_grect(NAME | CLOSER | MOVER, &outer);
    if (win < 0) {
        form_alert(1, "[3][No window available.|Close a window and try again.][ OK ]");
        v_clsvwk(vh);
        return 1;
    }
    wind_set_str(win, WF_NAME, " Screen resolution ");
    wind_open_grect(win, &outer);
    wind_set(win, WF_TOP, 0, 0, 0, 0);      /* keys come here, not to the desktop */
    wind_get_grect(win, WF_WORKXYWH, &work);

    while (!quit) {
        ev = evnt_multi(MU_MESAG | MU_KEYBD | MU_BUTTON, 1, 1, 1,
                        0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                        msg, 0, &mx, &my, &mb, &ks, &key, &clicks);
        if (ev & MU_MESAG) {
            switch (msg[0]) {
            case WM_REDRAW:
                redraw((GRECT *)&msg[4]);
                break;
            case WM_TOPPED:
                wind_set(win, WF_TOP, 0, 0, 0, 0);
                break;
            case WM_MOVED:
                wind_set_grect(win, WF_CURRXYWH, (GRECT *)&msg[4]);
                wind_get_grect(win, WF_WORKXYWH, &work);
                break;
            case WM_CLOSED:
            case AP_TERM:
                quit = 1;
                break;
            }
        }
        if (!quit && (ev & MU_KEYBD)) {
            int c = key & 0xff;

            if (c >= '1' && c < '1' + NMODES)
                quit = choose(c - '1');
            else if (c == 27 || c == 'q' || c == 'Q')
                quit = 1;
            else if ((key >> 8) == 0x48 && sel > 0)           /* up */
                highlight(sel - 1);
            else if ((key >> 8) == 0x50 && sel < NMODES - 1)  /* down */
                highlight(sel + 1);
            else if ((c == '\r' || c == '\n') && sel >= 0)
                quit = choose(sel);
        }
        if (!quit && (ev & MU_BUTTON) && wind_find(mx, my) == win) {
            int n = (my - work.g_y) / ch - FIRST_MODE;

            if (my >= work.g_y && n >= 0 && n < NMODES)
                quit = choose(n);
        }
    }

    wind_close(win);
    wind_delete(win);
    v_clsvwk(vh);
    return 0;
}

int main(void)
{
    int rc;

    if (appl_init() < 0)
        return text_main();
    rc = gem_main();
    appl_exit();
    return rc;
}
