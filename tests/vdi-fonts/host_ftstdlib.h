/*
 * The C library FreeType uses in the host build: the host's own, through
 * FreeType's ftstdlib.h, but without the 64-bit integer limits.  The
 * EmuTOS build (freetype/config/vdi_ftstdlib.h) leaves those out too, so
 * FreeType takes the same 32-bit arithmetic paths here as on the target
 * and renders the same pixels.
 */

#ifndef HOST_FTSTDLIB_H
#define HOST_FTSTDLIB_H

#include <freetype/config/ftstdlib.h>

#undef FT_LLONG_MAX
#undef FT_LLONG_MIN
#undef FT_ULLONG_MAX

#endif
