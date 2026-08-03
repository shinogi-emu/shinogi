/* Native test for the 8.3 mapping. Cases come from
 * docs/hatari-gemdos-reference.md section 1. */
#include <stdio.h>
#include <string.h>

#include "hostfs_name.h"

static int failures;

static void check(const char *host, const char *want)
{
    char got[13];

    hostfs_host_to_atari(host, got);
    if (strcmp(got, want) != 0)
    {
        printf("FAIL host2atari(\"%s\") = \"%s\", want \"%s\"\n",
               host, got, want);
        failures++;
    }
}

int main(void)
{
    /* Extension is text after the LAST dot, truncated to 3. */
    check("foo.html", "FOO.HTM");
    check("readme.txt", "README.TXT");

    /* Dots before the last become '+'. */
    check("a.b.c", "A+B.C");
    check("readme.txt.bak", "README+T.BAK");

    /* Base truncated to 8. */
    check("longfilename.txt", "LONGFILE.TXT");

    /* No dot at all and longer than 8: hard truncate. */
    check("verylongname", "VERYLONG");

    /* Invalid characters become '+'; everything else upcases. */
    check("file*name.txt", "FILE+NAM.TXT");
    check("a?b.t", "A+B.T");

    /* Short names pass through, upcased. */
    check("hello.c", "HELLO.C");
    check("MiXeD.TxT", "MIXED.TXT");

    /* Leading-dot files convert oddly but predictably. */
    check(".bashrc", ".BAS");

    /* The literal ".." is the sole exception to the dot rule. */
    check("..", "..");

    /* Atari->host clipping: FIRST dot, no substitution, no upcase. */
    {
        struct { const char *in, *want; } clips[] = {
            { "a.b.c",            "a.b"      },
            { "longfilename.txt", "longfile.txt" },
            { "HELLO.C",          "HELLO.C"  },
            { "lower.txt",        "lower.txt" },
        };
        size_t i;
        char got[13];

        for (i = 0; i < sizeof(clips) / sizeof(clips[0]); i++)
        {
            hostfs_atari_clip(clips[i].in, got);
            if (strcmp(got, clips[i].want) != 0)
            {
                printf("FAIL atari_clip(\"%s\") = \"%s\", want \"%s\"\n",
                       clips[i].in, got, clips[i].want);
                failures++;
            }
        }
    }

    /* Sort is case-SENSITIVE byte order: all A-Z before all a-z. */
    if (!(hostfs_name_compare("Zebra", "apple") < 0))
    {
        printf("FAIL sort: expected \"Zebra\" before \"apple\"\n");
        failures++;
    }
    if (!(hostfs_name_compare("AUTO", "auto") < 0))
    {
        printf("FAIL sort: expected \"AUTO\" before \"auto\"\n");
        failures++;
    }

    if (failures == 0)
        printf("all name cases passed\n");
    return failures != 0;
}
