#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include "resolution.h"

int shinogi_parse_resolution(const char *text, int *width, int *height)
{
    char *end;
    long w, h;
    errno = 0;
    w = strtol(text, &end, 10);
    if (end == text || *end != 'x' || errno) return 0;
    text = end + 1;
    h = strtol(text, &end, 10);
    if (end == text || errno) return 0;
    while (isspace((unsigned char)*end)) ++end;
    if (*end || w < 320 || w > 1920 || h < 200 || h > 1080 || w % 8)
        return 0;
    *width = (int)w;
    *height = (int)h;
    return 1;
}

static char *res_value(char *line)
{
    while (*line == ' ' || *line == '\t') ++line;
    if (strncmp(line, "res", 3)) return NULL;
    line += 3;
    while (*line == ' ' || *line == '\t') ++line;
    return *line == '=' ? line + 1 : NULL;
}

void shinogi_read_resolution(FILE *file, int *width, int *height)
{
    char line[1024], *value;
    int start = 1;
    while (fgets(line, sizeof(line), file)) {
        if (start && (value = res_value(line)) != NULL) {
            value[strcspn(value, "#;")] = '\0';
            if (shinogi_parse_resolution(value, width, height)) return;
        }
        start = strchr(line, '\n') != NULL;
    }
}

/* Preserve every unrelated byte, including long lines and unknown keys.
 * Remove duplicate res keys so both launchers see the selected size. */
int shinogi_write_resolution(FILE *input, FILE *output, int width, int height)
{
    char line[1024];
    int wrote = 0, start = 1, skip = 0, last = '\n';
    if (input) {
        while (fgets(line, sizeof(line), input)) {
            if (start) skip = res_value(line) != NULL;
            if (skip) {
                if (!wrote) fprintf(output, "res = %dx%d\r\n", width, height);
                wrote = 1;
            } else {
                fputs(line, output);
                last = (unsigned char)line[strlen(line) - 1];
            }
            start = strchr(line, '\n') != NULL;
        }
        if (ferror(input)) return 0;
    }
    if (!wrote) {
        if (last != '\n') fputs("\r\n", output);
        fprintf(output, "res = %dx%d\r\n", width, height);
    }
    return !ferror(output);
}
