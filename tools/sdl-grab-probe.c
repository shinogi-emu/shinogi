/*
 * sdl-grab-probe -- does SDL2 still deliver mouse motion while grabbed?
 *
 * Build:  cc sdl-grab-probe.c -o sdl-grab-probe $(pkg-config --cflags --libs sdl2)
 * Run:    ./sdl-grab-probe        then focus the window and keep the mouse moving
 *
 * Why this exists
 * ---------------
 * With an absolute pointing device QEMU's SDL frontend grabs the pointer
 * as soon as it moves inside a focused window, and only lets go when the
 * pointer reaches a window edge (ui/sdl2.c, handle_mousemotion). On a host
 * where the grab also stops motion being delivered, that edge can never be
 * reached, so the grab is permanent and the guest pointer is frozen until
 * the window loses focus -- which looks exactly like a broken guest driver
 * and is not one.
 *
 * This reproduces the host behaviour on its own, with no QEMU, no guest and
 * no virtio involved: it toggles SDL_SetWindowGrab every three seconds and
 * reports how many SDL_MOUSEMOTION events arrived in each interval. A host
 * that is fine reports similar counts either way. A host that is affected
 * reports zero in every grab=ON interval.
 *
 * Measured on Ubuntu, SDL 2.32.10, x11 backend under XWayland in a GNOME
 * Remote Login session:
 *
 *     grab=OFF focus=1  motion events in last 3s: 337
 *     grab=ON  focus=1  motion events in last 3s: 0
 *     grab=OFF focus=1  motion events in last 3s: 302
 *     grab=ON  focus=1  motion events in last 3s: 0
 *
 * Intervals where the window is not focused are skipped rather than counted:
 * SDL only grabs a focused window, so an unfocused interval measures nothing.
 */

#include <SDL.h>
#include <stdio.h>

#define INTERVAL_MS 3000
#define INTERVALS   6

/*
 * argc/argv rather than void: on Windows SDL redefines main to SDL_main,
 * which is declared int(int, char **), and a void signature will not
 * compile there.
 */
int main(int argc, char *argv[])
{
    SDL_Window *win;
    Uint32 last;
    int grab = 0, motion = 0, done = 0;

    (void)argc;
    (void)argv;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    printf("SDL video driver: %s\n", SDL_GetCurrentVideoDriver());

    win = SDL_CreateWindow("sdl-grab-probe - move the mouse over me",
                           SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           640, 480, SDL_WINDOW_SHOWN);
    if (!win) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    printf("Focus the window and keep moving the mouse over it.\n");
    printf("Grab toggles every %d seconds.\n\n", INTERVAL_MS / 1000);
    last = SDL_GetTicks();

    while (done < INTERVALS) {
        SDL_Event ev;

        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_MOUSEMOTION)
                motion++;
            else if (ev.type == SDL_QUIT)
                goto out;
        }

        if (SDL_GetTicks() - last < INTERVAL_MS) {
            SDL_Delay(5);
            continue;
        }

        if (!(SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS)) {
            printf("waiting for focus - click the window\n");
            fflush(stdout);
            motion = 0;
            last = SDL_GetTicks();
            continue;
        }

        printf("grab=%-3s focus=1  motion events in last %ds: %d\n",
               grab ? "ON" : "OFF", INTERVAL_MS / 1000, motion);
        fflush(stdout);

        motion = 0;
        grab = !grab;

        /* What QEMU's sdl_grab_start()/sdl_grab_end() do. */
        SDL_SetWindowGrab(win, grab ? SDL_TRUE : SDL_FALSE);
        SDL_ShowCursor(grab ? SDL_DISABLE : SDL_ENABLE);

        last = SDL_GetTicks();
        done++;
    }

out:
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
