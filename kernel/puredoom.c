/* kernel/puredoom.c
 * -----------------------------------------------------------------------------
 * The single translation unit that compiles id Software's DOOM, by way of
 * Daivuk's PureDOOM (a zero-dependency, single-header port). We define
 * DOOM_IMPLEMENTATION here and *nowhere else*; the rest of the kernel includes
 * PureDOOM.h without it, getting only the public API.
 *
 * We deliberately do NOT define any DOOM_IMPLEMENT_* macros: those would pull in
 * the host stdio/stdlib/sys-time default callbacks. Instead the whole platform
 * layer (malloc, file I/O over the embedded WAD, time, print) is supplied at
 * runtime from doom_app.c via doom_set_*.
 * -----------------------------------------------------------------------------
 */
#define DOOM_IMPLEMENTATION
#include "PureDOOM.h"
