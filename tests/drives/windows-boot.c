#include <stdio.h>
#include <string.h>
static int matches(const char *path, const char *expected)
{
    FILE *file = fopen(path, "rb");
    char data[80] = {0};
    if (!file) return 0;
    fread(data, 1, sizeof(data) - 1, file); fclose(file);
    return !strcmp(data, expected);
}
int main(void)
{
    FILE *log = fopen("C:\\WINBOOT.LOG", "w");
    FILE *out;
    int ok = matches("D:\\EXTRA.TXT", "WINDOWS FOLDER\n") &&
             matches("E:\\SAME.TXT", "WINDOWS IMAGE\n") &&
             matches("A:\\SAME.TXT", "WINDOWS FLOPPY\n");
    if (!log) return 2;
    out = fopen("D:\\WINOUT.TXT", "wb");
    if (out) { fputs("WINDOWS WRITE\n", out); fclose(out); } else ok = 0;
    fprintf(log, "WINDOWS GUEST %s\n", ok ? "PASS" : "FAIL");
    fclose(log);
    return ok ? 0 : 1;
}
