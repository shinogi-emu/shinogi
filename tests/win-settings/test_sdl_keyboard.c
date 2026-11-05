/* Link the shipped SDL2.dll: QEMU sets this same hint before grabbing. */
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <stdio.h>
#include <string.h>
#include "../../tools/win/window-controls.h"

int main(int argc, char **argv)
{
    SDL_Window *window;
    SDL_bool keyboard, mouse;
    SDL_version version;
    int baseline = argc == 2 && !strcmp(argv[1], "--baseline");
    SetEnvironmentVariableA("SDL_GRAB_KEYBOARD", NULL);
    if (!baseline && !window_controls_configure_sdl()) return 2;
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO)) { puts(SDL_GetError()); return 2; }
    SDL_GetVersion(&version);
    window = SDL_CreateWindow("Shinogi SDL shortcut regression", 20, 20, 640, 480, SDL_WINDOW_SHOWN);
    if (!window) return 2;
    SDL_SetHint(SDL_HINT_GRAB_KEYBOARD, "1");
    SDL_SetWindowGrab(window, SDL_TRUE);
    keyboard = SDL_GetWindowKeyboardGrab(window);
    mouse = SDL_GetWindowMouseGrab(window);
    printf("SDL %u.%u.%u: keyboard grab=%d mouse grab=%d\n", version.major, version.minor, version.patch, keyboard, mouse);
    SDL_SetWindowGrab(window, SDL_FALSE);
    SDL_DestroyWindow(window); SDL_Quit();
    if (keyboard || !mouse) { puts("FAIL: SDL captures host shortcut modifiers"); return 1; }
    puts("PASS: host shortcut modifiers available; SDL mouse grab retained");
    return 0;
}
