#ifndef SHINOGI_WINDOW_CONTROLS_H
#define SHINOGI_WINDOW_CONTROLS_H

#include <windows.h>

/* Keep Windows shortcuts available when QEMU grabs its SDL mouse. */
BOOL window_controls_configure_sdl(void);

/* Pump foreground notifications and the Settings shortcut until QEMU exits. */
void window_controls_wait(HANDLE process, DWORD pid, const char *launcher);

#endif
