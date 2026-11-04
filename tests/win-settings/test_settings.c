#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../tools/win/resolution.h"
#include "../../tools/hostfsd/windows-path.h"

static FILE *contents(const char *text)
{
    FILE *f = tmpfile();
    assert(f);
    assert(fputs(text, f) >= 0);
    rewind(f);
    return f;
}

static void rewrite(const char *before, const char *expected)
{
    FILE *in = contents(before), *out = tmpfile();
    char actual[10000];
    size_t n;
    assert(out);
    assert(shinogi_write_resolution(in, out, 1280, 720));
    rewind(out);
    n = fread(actual, 1, sizeof(actual) - 1, out);
    actual[n] = '\0';
    assert(!strcmp(actual, expected));
    fclose(in);
    fclose(out);
}

int main(void)
{
    int w = 1024, h = 768;
    char path[512], before[6000], expected[6000];
    FILE *f;
    const char *bad[] = {"0x0", "319x200", "320x199", "1928x1080",
                         "1920x1081", "1001x768", "1280x720oops",
                         "999999999999999999999999x720", "1280x", "x720"};
    size_t i;
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        assert(!shinogi_parse_resolution(bad[i], &w, &h));
        assert(w == 1024 && h == 768);
    }
    assert(shinogi_parse_resolution(" 320x200\r\n", &w, &h) && w == 320 && h == 200);
    assert(shinogi_parse_resolution("1920x1080", &w, &h) && w == 1920 && h == 1080);
    f = contents("# res = 320x200\r\nres = invalid\r\n res = 800x600 # chosen\r\n");
    shinogi_read_resolution(f, &w, &h);
    assert(w == 800 && h == 600);
    fclose(f);
    rewrite("# keep\r\nres=640x480\r\nnet = on\r\nres = 800x600\r\n",
            "# keep\r\nres = 1280x720\r\nnet = on\r\n");
    rewrite("net = off", "net = off\r\nres = 1280x720\r\n");
    rewrite("", "res = 1280x720\r\n");
    /* A res-looking fragment in a long comment is still a comment. */
    memset(before, '#', 1023);
    strcpy(before + 1023, "res=640x480\nunknown=value\n");
    strcpy(expected, before);
    strcat(expected, "res = 1280x720\r\n");
    rewrite(before, expected);
    hostfs_windows_path(path, "\\\\?\\UNC\\server\\share\\Drive C");
    assert(!strcmp(path, "\\\\server\\share\\Drive C"));
    hostfs_windows_path(path, "\\\\?\\C:\\Atari\\Drive C");
    assert(!strcmp(path, "C:\\Atari\\Drive C"));
    hostfs_windows_path(path, "\\\\?\\C:\\");
    assert(!strcmp(path, "C:\\"));
    hostfs_windows_path(path, "\\\\server\\share");
    assert(!strcmp(path, "\\\\server\\share"));
    puts("Windows settings: resolution bounds, file preservation and UNC paths passed");
    return 0;
}
