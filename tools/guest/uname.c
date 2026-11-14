/*
 * uname - print system information.
 *
 * The FreeMiNT snapshot drive C is built from ships no uname, yet shell
 * scripts and people expect one. POSIX options:
 *
 *   -s system name   -n node name   -r release   -v version
 *   -m machine       -a all of those, in that order
 *   -p processor (m68k)             -o operating system (as -s)
 *
 * The fields come from mintlib's sysinfo(), but not the machine:
 * sysinfo(SI_PLATFORM), and so uname(), reads through a NULL pointer
 * when _MCH says "no ST-compatible hardware" (high word 0xffff) on a
 * non-ColdFire CPU, which hangs or kills the caller. The machine is
 * worked out here from _MCH instead.
 *
 *   m68k-atari-mint-gcc -O2 -o uname uname.c
 */
#include <stdio.h>
#include <string.h>
#include <sys/systeminfo.h>
#include <mint/cookie.h>

#define S 0x01
#define N 0x02
#define R 0x04
#define V 0x08
#define M 0x10
#define P 0x20
#define O 0x40

static int first = 1;

static void field(const char *text)
{
    printf("%s%s", first ? "" : " ", text);
    first = 0;
}

static void info(int command, const char *fallback)
{
    char buf[64];

    if (sysinfo(command, buf, sizeof(buf)) < 0 || !buf[0])
        field(fallback);
    else
        field(buf);
}

static const char *machine(void)
{
    long mch = 0, dummy;

    if (Getcookie(C_hade, &dummy) == C_FOUND)
        return "hades";
    if (Getcookie(C__MCH, &mch) != C_FOUND)
        return "atarist";
    switch ((int)(short)(mch >> 16))
    {
    case 0:  return "atarist";
    case 1:  return "atariste";
    case 2:  return "ataritt";
    case 3:  return Getcookie(C__CF_, &dummy) == C_FOUND ? "firebee" : "falcon";
    case 4:  return "milan";
    case 5:  return "aranym";
    case -1: return Getcookie(C__CF_, &dummy) == C_FOUND ? "cfevb" : "m68k";
    default: return "atariclone";
    }
}

static int usage(void)
{
    fputs("usage: uname [-amnoprsv]\n", stderr);
    return 1;
}

int main(int argc, char **argv)
{
    int want = 0, i;

    for (i = 1; i < argc; i++)
    {
        const char *a = argv[i];

        if (!strcmp(a, "--all"))
            a = "-a";
        if (a[0] != '-' || !a[1])
            return usage();
        for (a++; *a; a++)
        {
            switch (*a)
            {
            case 'a': want |= S | N | R | V | M; break;
            case 's': want |= S; break;
            case 'n': want |= N; break;
            case 'r': want |= R; break;
            case 'v': want |= V; break;
            case 'm': want |= M; break;
            case 'p': want |= P; break;
            case 'o': want |= O; break;
            default:  return usage();
            }
        }
    }
    if (!want)
        want = S;

    if (want & S) info(SI_SYSNAME, "FreeMiNT");
    if (want & N) info(SI_HOSTNAME, "localhost");
    if (want & R) info(SI_RELEASE, "unknown");
    if (want & V) info(SI_VERSION, "unknown");
    if (want & M) field(machine());
    if (want & P) field("m68k");
    if (want & O) info(SI_SYSNAME, "FreeMiNT");
    putchar('\n');
    return 0;
}
