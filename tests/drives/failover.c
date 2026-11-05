#include <stdio.h>
#include <string.h>
#include <mint/osbind.h>
static char data[20];
int main(void)
{
    FILE *log = fopen("C:\\FAILOVER.LOG", "w");
    long d = Fopen("D:\\SAME.TXT", 0), c = Fopen("C:\\SAME.TXT", 0), z = Fopen("Z:\\SAME.TXT", 0), ready, go;
    int ok = log && d >= 0 && c >= 0 && z >= 0;
    unsigned long spins;
    if (!log) return 2;
    ready = Fcreate("C:\\KILL.TAG", 0); if (ready >= 0) Fclose((int)ready); else ok = 0;
    for (spins = 0; spins < 1000000UL; spins++) {
        go = Fopen("C:\\GO.TAG", 0);
        if (go >= 0) { Fclose((int)go); break; }
    }
    ok = ok && spins < 1000000UL;
    if (d >= 0) {
        ok = Fread((int)d, 7L, data) < 0 && ok;
        Fclose((int)d); /* Must not close an overlapping handle on C or Z. */
    }
    memset(data, 0, sizeof(data));
    if (c >= 0) { ok = Fread((int)c, 7L, data) == 7 && !strcmp(data, "C root\n") && ok; Fclose((int)c); }
    memset(data, 0, sizeof(data));
    if (z >= 0) { ok = Fread((int)z, 7L, data) == 7 && !strcmp(data, "Z root\n") && ok; Fclose((int)z); }
    ok = Fopen("D:\\SAME.TXT", 0) < 0 && (Drvmap() & 8) && ok;
    fprintf(log, "%s\n", ok ? "PASS" : "FAIL"); fclose(log);
    return ok ? 0 : 1;
}
