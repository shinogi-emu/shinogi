#ifndef SHINOGI_SETTINGS_H
#define SHINOGI_SETTINGS_H

#define IDD_SETTINGS 100
#define IDC_DRIVEC 101
#define IDC_BROWSE 102
#define IDC_RESOLUTION 103
#define IDC_FULLSCREEN 104

#ifndef RC_INVOKED
#include <windows.h>

/* Leave room for filenames below drive C within the Win32 path limit. */
#define DRIVE_PATH_MAX 220

typedef struct {
    char file[MAX_PATH];
    char drivec[MAX_PATH];
    int width, height, fullscreen;
} Settings;

int settings_load(Settings *settings, const char *program_dir);
void settings_resolution(Settings *settings);
int settings_dialog(HINSTANCE instance, Settings *settings);
int settings_drive_valid(const char *path);
#endif
#endif
