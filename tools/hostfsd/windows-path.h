#ifndef SHINOGI_WINDOWS_PATH_H
#define SHINOGI_WINDOWS_PATH_H

#include <string.h>

/* OUT must have room for FINAL. Removing an extended UNC prefix still
 * requires preserving the two slashes that make a share path absolute. */
static void hostfs_windows_path(char *out, const char *final)
{
    if (strncmp(final, "\\\\?\\UNC\\", 8) == 0) {
        strcpy(out, "\\\\");
        strcat(out, final + 8);
    } else {
        strcpy(out, strncmp(final, "\\\\?\\", 4) == 0 ? final + 4 : final);
    }
}

#endif
