#include <stdio.h>
#include <string.h>
#include <mint/osbind.h>

int main(void)
{
    FILE *log = fopen("C:\\DRIVE.LOG", "w");
    FILE *disk = fopen("D:\\HELLO.TXT", "r");
    char data[80] = {0};
    int ok;
    if (!log) return 2;
    if (disk) { fgets(data, sizeof(data), disk); fclose(disk); }
    ok = disk && !strcmp(data, "SHINOGI RAW DISK\n") && (Drvmap() & 8L);
    fprintf(log, "%s map=%lx data=%s\n", ok ? "IMAGE DRIVE PASS" : "IMAGE DRIVE FAIL", Drvmap(), data);
    fclose(log);
    return ok ? 0 : 1;
}
