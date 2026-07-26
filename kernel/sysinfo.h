#ifndef PEFIA_SYSINFO_H
#define PEFIA_SYSINFO_H

#include <stdint.h>

/* Single source of truth for the OS version string (about/ver/terminal). */
#define PEFIA_VERSION "pefiaOS 0.3 (Stardance)"

extern uint32_t g_mem_kb;   /* total usable RAM in KB, 0 if unknown */

#endif
