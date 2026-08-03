/*
 * hatari_config_stub.c - the one symbol Hatari's str.c needs from the
 * rest of the emulator.
 *
 * src/str.c is otherwise free-standing: its only non-libc undefined
 * symbol is ConfigureParams. Rather than invent a shape for it, this
 * pulls in Hatari's real configuration.h and defines the real type, so
 * the layout is whatever Hatari says it is. Zero initialisation gives
 * bFilenameConversion == false, which is exactly what
 * Configuration_SetDefault() sets (src/configuration.c) and what the
 * emulator runs with unless --gemdos-conv is passed.
 *
 * Nothing here reimplements or approximates any part of the conversion:
 * Str_Filename_Host2Atari() is compiled verbatim from Hatari's tree.
 *
 * configuration.h uses FILENAME_MAX without including <stdio.h>.
 */

#include <stdio.h>
#include "configuration.h"

CNF_PARAMS ConfigureParams;
