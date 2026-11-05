/* Run once from AUTO (EmuTOS), then from MINT.CNF (FreeMiNT).
 * All paths point at the disposable fixture supplied by check.py. */
#include <stdio.h>
#include <string.h>
#include <mint/osbind.h>
#include <mint/mintbind.h>
static FILE *logfile;
static int errors;
static int previous_run;
static unsigned char sector[512] __attribute__((aligned(4)));
static unsigned char payload[24577], actual[24577];
#define CHECK(x) do { if (!(x)) { fprintf(logfile, "FAIL line %d: %s\n", __LINE__, #x); fflush(logfile); errors++; } } while (0)

static int content(const char *path, const char *expected)
{
    char data[80] = {0};
    long h = Fopen(path, 0), n;
    if (h < 0) return 0;
    n = Fread((int)h, sizeof(data) - 1, data);
    Fclose((int)h);
    return n == (long)strlen(expected) && !strcmp(data, expected);
}

static void writable(char drive)
{
    char path[100], renamed[100], dir[40];
    long h;
    size_t i;
    sprintf(path, "%c:\\RESULT.BIN", drive);
    h = Fopen(path, 0);
    /* A prior run must still be readable through the reopened filesystem. */
    if (previous_run) CHECK(h >= 0);
    if (h >= 0) {
        CHECK(Fread((int)h, sizeof(actual), actual) == sizeof(actual));
        CHECK(!memcmp(actual, payload, sizeof(actual)));
        CHECK(Fclose((int)h) == 0);
    }
    h = Fcreate(path, 0); CHECK(h >= 0);
    if (h < 0) return;
    CHECK(Fwrite((int)h, sizeof(payload), payload) == sizeof(payload));
    CHECK(Fclose((int)h) == 0);
    h = Fopen(path, 2); CHECK(h >= 0);
    if (h >= 0) {
        CHECK(Fseek(7L, (int)h, 0) == 7);
        CHECK(Fwrite((int)h, 701L, payload + 7) == 701);
        CHECK(Fseek(0L, (int)h, 0) == 0);
        CHECK(Fread((int)h, sizeof(actual), actual) == sizeof(actual));
        CHECK(!memcmp(actual, payload, sizeof(actual)));
        Fclose((int)h);
    }
    sprintf(dir, "%c:\\MUTATE", drive);
    sprintf(path, "%s\\TEMP.BIN", dir);
    sprintf(renamed, "%s\\RENAMED.BIN", dir);
    CHECK(Dcreate(dir) == 0);
    h = Fcreate(path, 0); CHECK(h >= 0);
    if (h >= 0) { CHECK(Fwrite((int)h, sizeof(payload), payload) == sizeof(payload)); Fclose((int)h); }
    CHECK(Frename(0, path, renamed) == 0);
    CHECK(Fdelete(renamed) == 0);
    CHECK(Ddelete(dir) == 0);
    /* Delete a short file, then write a different long file: vvfat regression. */
    sprintf(path, "%c:\\SHORT.BIN", drive);
    h = Fcreate(path, 0); CHECK(h >= 0);
    if (h >= 0) { CHECK(Fwrite((int)h, 3L, payload) == 3); Fclose((int)h); }
    CHECK(Fdelete(path) == 0);
    sprintf(path, "%c:\\SECOND.BIN", drive);
    h = Fcreate(path, 0); CHECK(h >= 0);
    if (h >= 0) { CHECK(Fwrite((int)h, sizeof(payload), payload) == sizeof(payload)); Fclose((int)h); }
    for (i = 0; i < sizeof(actual); i++) actual[i] = 0;
}

static void readonly(char drive)
{
    char path[40], other[40];
    long h;
    sprintf(path, "%c:\\SAME.TXT", drive);
    sprintf(other, "%c:\\DENIED.TXT", drive);
    CHECK(Fattrib(path, 0, 0) & 1);
    h = Fopen(path, 2); CHECK(h < 0); if (h >= 0) Fclose((int)h);
    h = Fcreate(other, 0); CHECK(h < 0); if (h >= 0) Fclose((int)h);
    CHECK(Fdelete(path) < 0);
    CHECK(Frename(0, path, other) < 0);
    CHECK(Dcreate(other) < 0);
    CHECK(Fattrib(path, 1, 0) < 0);
}

int main(int argc, char **argv)
{
    char saved[120], dta1[64], dta2[64], name[14];
    void *dta = (void *)Fgetdta();
    long a, b;
    int swapped = 0;
    int i, stage = argc > 1 && !strcmp(argv[1], "mint");
    logfile = fopen(stage ? "C:\\MINTDRV.LOG" : "C:\\EMUDRV.LOG", "w");
    if (!logfile) return 3;
    a = Fopen("C:\\SWAP.TAG", 0);
    swapped = a >= 0;
    if (a >= 0) Fclose((int)a);
    a = Fopen("C:\\PRIOR.TAG", 0);
    previous_run = a >= 0;
    if (a >= 0) Fclose((int)a);
    for (i = 0; i < (int)sizeof(payload); i++) payload[i] = (unsigned char)(i * 37 + i / 251);
    CHECK((Drvmap() & 0x200007fL) == 0x200007fL); /* A..G and Z */
    CHECK(content("C:\\SAME.TXT", "C root\n"));
    CHECK(content("D:\\SAME.TXT", "D root\n"));
    CHECK(content("E:\\SAME.TXT", "E root\n"));
    CHECK(content("Z:\\SAME.TXT", "Z root\n"));
    CHECK(content("F:\\SAME.TXT", "F root\n"));
    CHECK(content("G:\\SAME.TXT", "G root\n"));
    CHECK(content("A:\\SAME.TXT", swapped ? "A swap\n" : "A root\n"));
    CHECK(content("B:\\SAME.TXT", swapped ? "B swap\n" : "B root\n"));
    CHECK(Floprd(sector, 0L, 0, swapped ? 18 : 9, 79, 1, 1) == 0);
    CHECK(Floprd(sector, 0L, 1, swapped ? 9 : 18, 79, 1, 1) == 0);
    CHECK(Floprd(sector, 0L, 0, 1, 80, 0, 1) < 0);
    CHECK(Flopwr(sector, 0L, 1, 1, 0, 0, 1) == -13L);
    a = Fopen("D:\\SAME.TXT", 0); b = Fopen("Z:\\SAME.TXT", 0);
    CHECK(a >= 0 && b >= 0 && a != b);
    if (a >= 0 && b >= 0) {
        char x[8] = {0}, y[8] = {0};
        CHECK(Fread((int)a, 7L, x) == 7); CHECK(Fread((int)b, 7L, y) == 7);
        CHECK(!strcmp(x, "D root\n") && !strcmp(y, "Z root\n"));
    }
    if (a >= 0) Fclose((int)a);
    if (b >= 0) Fclose((int)b);
    Dsetdrv(3);
    CHECK(Dsetpath("D:\\SUB") == 0); Dsetdrv(25); CHECK(Dsetpath("Z:\\OTHER") == 0);
    CHECK(Dgetpath(saved, 4) == 0 && !stricmp(saved, "\\SUB"));
    CHECK(Dgetpath(saved, 26) == 0 && !stricmp(saved, "\\OTHER"));
    Dsetdrv(3); CHECK(Dgetdrv() == 3); CHECK(content("LOCAL.TXT", "D sub\n"));
    Dsetdrv(25); CHECK(Dgetdrv() == 25); CHECK(content("LOCAL.TXT", "Z other\n"));
    Dsetdrv(2);
    Fsetdta(dta1); CHECK(Fsfirst("D:\\*.TXT", 0) == 0);
    memcpy(name, dta1 + 30, 14);
    Fsetdta(dta2); CHECK(Fsfirst("Z:\\*.TXT", 0) == 0);
    Fsetdta(dta1); CHECK(Fsnext() == 0); CHECK(strcmp(name, dta1 + 30) != 0);
    Fsetdta(dta2); CHECK(Fsnext() == 0);
    Fsetdta(dta);
    CHECK(Frename(0, "D:\\SAME.TXT", "Z:\\STOLEN.TXT") < 0);
    writable('D'); writable('Z'); writable('F'); writable('A');
    readonly('E'); readonly('G'); readonly('B');
    a = Fcreate("C:\\PRIOR.TAG", 0); CHECK(a >= 0);
    if (a >= 0) Fclose((int)a);
    if (stage) Sync();
    fprintf(logfile, "%s %s\n", stage ? "MINT" : "EMUTOS", errors ? "FAIL" : "PASS");
    fclose(logfile);
    if (stage) Sync();
    return errors ? 1 : 0;
}
