/* kernel/puredoom.c
 * The one translation unit where PureDOOM actually gets compiled. Everywhere
 * else includes PureDOOM.h without DOOM_IMPLEMENTATION and only sees the
 * public API - define it twice and you get duplicate symbols across the link.
 *
 * None of the DOOM_IMPLEMENT_* host callbacks are defined here on purpose:
 * those default implementations assume a hosted stdio/stdlib/clock, which we
 * don't have. doom_app.c wires up doom_set_* instead, routing malloc/file
 * I/O/time/print through our own kmalloc, the embedded WAD, and clock_ms.
 */
#define DOOM_IMPLEMENTATION
#include "PureDOOM.h"
