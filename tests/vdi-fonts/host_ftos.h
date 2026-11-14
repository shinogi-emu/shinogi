/*
 * Test controls of the host stand-in for EmuTOS's vdi/vdi_ftos.c.
 */

#ifndef HOST_FTOS_H
#define HOST_FTOS_H

/*
 * Let n more allocations succeed, then refuse every one after them;
 * n < 0 lets all of them succeed again.  host_alloc_fail_after(0) makes
 * every allocation fail.
 */
void host_alloc_fail_after(long n);

long host_alloc_live(void);         /* blocks allocated and not freed */
long host_alloc_total(void);        /* allocations granted so far */
long host_alloc_refused(void);      /* allocations refused so far */
long host_file_loads(void);         /* files read by vdi_font_file_load() */
long host_os_blocks(void);          /* blocks the system has given and not got back */

#endif
