/* kernel/sysinfo.h
 * -----------------------------------------------------------------------------
 * A couple of system facts gathered at boot (from Multiboot) for `about`.
 * -----------------------------------------------------------------------------
 */
#ifndef PEFIA_SYSINFO_H
#define PEFIA_SYSINFO_H

#include <stdint.h>

extern uint32_t g_mem_kb;   /* total usable RAM in KB, 0 if unknown */

#endif /* PEFIA_SYSINFO_H */
