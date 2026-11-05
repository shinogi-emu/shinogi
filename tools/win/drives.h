#ifndef SHINOGI_DRIVES_H
#define SHINOGI_DRIVES_H

#define IDC_DRIVES 130
#define IDD_DRIVES 131
#define IDC_DRIVE_SLOT 132
#define IDC_DRIVE_PATH 133
#define IDC_DRIVE_BROWSE 134
#define IDC_DRIVE_READONLY 135
#define IDC_DRIVE_CLEAR 136
#define IDC_DRIVE_DESCRIPTION 137

#ifndef RC_INVOKED
#include <windows.h>
#define EXTRA_FOLDERS 23
#define HARD_IMAGES 4
#define FLOPPY_IMAGES 2
#define EXTRA_DRIVES (EXTRA_FOLDERS + HARD_IMAGES + FLOPPY_IMAGES)
/* Includes space for a folder-relative GEMDOS filename. */
#define DRIVE_PATH_MAX 220
#define DRIVE_OPTIONS_MAX 16384

typedef struct {
    char path[MAX_PATH];
    int readonly;
} DriveMapping;

typedef struct {
    DriveMapping mapping[EXTRA_DRIVES];
} DriveSettings;

int drives_load(DriveSettings *drives, const char *file);
int drives_save(const DriveSettings *drives, const char *file);
int drives_valid(const DriveSettings *drives, const char *drivec, char *error, size_t size);
int drives_dialog(HINSTANCE instance, HWND owner, DriveSettings *drives, const char *drivec);
/* Append raw block backends safely. Caller supplies the folder channels. */
int drives_image_options(const DriveSettings *drives, char *out, size_t size);
int drives_folder_valid(const char *path);
#endif
#endif
