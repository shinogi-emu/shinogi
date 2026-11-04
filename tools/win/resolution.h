#ifndef SHINOGI_RESOLUTION_H
#define SHINOGI_RESOLUTION_H

#include <stdio.h>

int shinogi_parse_resolution(const char *text, int *width, int *height);
void shinogi_read_resolution(FILE *file, int *width, int *height);
int shinogi_write_resolution(FILE *input, FILE *output, int width, int height);

#endif
