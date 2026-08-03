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

    if (failures == 0)
        printf("all host2atari cases passed\n");
    return failures != 0;
}
