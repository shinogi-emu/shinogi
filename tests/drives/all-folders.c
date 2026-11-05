#include <stdio.h>
#include <string.h>
#include <mint/osbind.h>
int main(int argc, char **argv)
{
    FILE *log = fopen(argc > 1 ? "C:\\ALLMINT.LOG" : "C:\\ALLEMUTO.LOG", "w");
    int d, errors = 0;
    (void)argv;
    if (!log) return 2;
    for (d = 2; d < 26; d++) {
        char path[40], text[20] = {0}, expected[20];
        long h;
        if (d == 20) continue; /* FreeMiNT owns U */
        sprintf(path, "%c:\\SAME.TXT", 'A' + d);
        sprintf(expected, "%c root\n", 'A' + d);
        h = Fopen(path, 0);
        if (!(Drvmap() & (1L << d)) || h < 0 ||
            Fread((int)h, 19L, text) != (long)strlen(expected) || strcmp(text, expected)) {
            fprintf(log, "FAIL %s handle=%ld data=%s\n", path, h, text); errors++;
        }
        if (h >= 0) Fclose((int)h);
    }
    fprintf(log, "%s\n", errors ? "FAIL" : "PASS");
    fclose(log);
    return errors ? 1 : 0;
}
