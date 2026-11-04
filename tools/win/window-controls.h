#ifndef SHINOGI_WINDOW_CONTROLS_H
#define SHINOGI_WINDOW_CONTROLS_H

#include <windows.h>

/* Pump foreground notifications and the Settings shortcut until QEMU exits. */
void window_controls_wait(HANDLE process, DWORD pid, const char *launcher);

#endif
