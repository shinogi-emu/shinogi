/*
 * WTEST.PRG - can a program running on the guest create files on C:?
 *
 * Drop it in the AUTO folder, after MINT.PRG, and boot:
 *
 *   m68k-atari-mint-gcc -O2 -o WTEST.PRG tools/guest/wtest.c
 *   cp WTEST.PRG ~/shinogi-drive-c/AUTO/
 *
 * It answers a question the host side cannot: whether a GEMDOS call
 * that should reach drive C actually gets there.  Watching the host
 * helper is not enough, because the interesting failures never reach
 * it -- shin-jb9 was exactly that, an Fcreate rejected inside MiNT
 * with no CREATE ever going out on the wire, which from the host
 * looked indistinguishable from an application that simply never
 * tried to save anything.
 *
 * Fcreate and Dcreate are both here because they take different paths
 * and shin-jb9 broke only one of them: making folders worked, so the
 * drive looked writable while no file on it could be created.  Any
 * future "settings do not save" report should start here.
 *
 * Output goes to BIOS device 1, the serial line, so it lands in the
 * harness log next to the kernel's own KERN_DEBUG output rather than
 * on a screen that XaAES is about to clear.  The files it leaves
 * behind are the other half of the evidence: they show up on the host
 * or they do not.
 */
#include <osbind.h>

static void sput(const char *s)
{
    while (*s) Bconout(1, *s++);
}

static void shex(long v)
{
    int i;

    sput("0x");
    for (i = 7; i >= 0; i--)
    {
        int nib = (int)((v >> (i * 4)) & 0xF);
        Bconout(1, nib < 10 ? '0' + nib : 'A' + nib - 10);
    }
    sput("\r\n");
}

static void try_create(const char *path)
{
    long h;

    sput("WTEST: Fcreate "); sput(path); sput(" -> ");
    h = Fcreate(path, 0);
    shex(h);

    /* Creating the handle is only half of it: a drive that hands out
     * handles it cannot write to would still fail every save. */
    if (h >= 0)
    {
        sput("WTEST:   Fwrite -> "); shex(Fwrite((int)h, 6L, "hello\n"));
        Fclose((int)h);
    }
}

int main(void)
{
    long h;

    sput("\r\n=== WTEST start ===\r\n");

    try_create("C:\\WTEST.TXT");                /* drive root      */
    try_create("C:\\HIGHWIRE\\WTEST2.TXT");     /* a subdirectory  */

    sput("WTEST: Fopen(rw) HIGHWIRE.CFG -> ");
    h = Fopen("C:\\HIGHWIRE\\HIGHWIRE.CFG", 2);
    shex(h);
    if (h >= 0) Fclose((int)h);

    /* EACCDN here just means a previous run left the folder behind. */
    sput("WTEST: Dcreate C:\\WTESTDIR -> ");
    shex(Dcreate("C:\\WTESTDIR"));

    sput("=== WTEST end ===\r\n");
    return 0;
}
