/* Native test for the Unix-epoch -> GEMDOS packed date/time
 * conversion.  Cases were cross-checked against python3's
 * calendar.timegm() / datetime, independently of this codebase.
 *
 * GEMDOS packing:
 *   date = (year - 1980) << 9 | month << 5 | day       (month 1-12, day 1-31)
 *   time = hour << 11 | minute << 5 | second / 2        (2-second resolution)
 *
 * Anything before 1980 clamps only the year field to 0 -- month and day
 * stay the real month/day of that date.  This matches the reference
 * implementation (Hatari gemdos.c) and is deliberately NOT 1980-01-01.
 */
#include <stdio.h>

#include "epochtime.h"

static int failures;

static void check(const char *name, ULONG secs, UWORD want_date, UWORD want_time)
{
    UWORD got_date, got_time;

    epoch_to_dos(secs, &got_date, &got_time);
    if (got_date != want_date || got_time != want_time)
    {
        printf("FAIL %s: epoch_to_dos(%lu) = date 0x%04x time 0x%04x, "
               "want date 0x%04x time 0x%04x\n",
               name, (unsigned long)secs,
               got_date, got_time, want_date, want_time);
        failures++;
    }
}

int main(void)
{
    /* The epoch itself: 1970-01-01 00:00:00, below the 1980 floor. */
    check("epoch", 0UL, 0x0021U, 0x0000U);

    /* A known recent timestamp: 2026-08-03 12:34:56 UTC. */
    check("recent", 1785760496UL, 0x5d03U, 0x645cU);

    /* Leap year, start: 2024-01-01 00:00:00. */
    check("leap-start", 1704067200UL, 0x5821U, 0x0000U);

    /* Leap year, end: 2024-12-31 23:59:59 -- also exercises hour/minute
     * packing at the top of their ranges. */
    check("leap-end", 1735689599UL, 0x599fU, 0xbf7dU);

    /* 29 February itself, mid-leap-year: 2024-02-29 12:00:00. */
    check("feb29", 1709208000UL, 0x585dU, 0x6000U);

    /* Below the 1980 floor: 1975-06-15 10:20:30. Year field clamps to
     * 0 but month/day are the real 6/15, NOT 1/1. */
    check("pre1980", 172059630UL, 0x00cfU, 0x528fU);

    /* Late in the day, non-leap year: 2025-03-10 23:59:59. */
    check("late-day", 1741651199UL, 0x5a6aU, 0xbf7dU);

    if (failures == 0)
        printf("all epochtime cases passed\n");
    return failures != 0;
}
