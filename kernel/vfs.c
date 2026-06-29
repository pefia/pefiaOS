/* kernel/vfs.c
 * A fixed tree of folders/files held in RAM. Swap this out for a FAT32 reader
 * later; the rest of the OS only uses the vfs_* API.
 */
#include "vfs.h"
#include <stddef.h>

/* name, is_dir, parent, content, size */
static VNode nodes[] = {
    { "/",          1, -1, NULL, 0 },
    { "Documents",  1,  0, NULL, 0 },
    { "Pictures",   1,  0, NULL, 0 },
    { "System",     1,  0, NULL, 0 },
    { "readme.txt", 0,  0, "Welcome to pefiaOS - this is an in-memory file system.", 0 },
    { "todo.txt",   0,  1, "1. taskbar   2. file explorer   3. web browser later", 0 },
    { "notes.txt",  0,  1, "The window manager and heap allocator both work.", 0 },
    { "cat.bmp",    0,  2, "(image data placeholder)", 0 },
    { "logo.bmp",   0,  2, "(image data placeholder)", 0 },
    { "version",    0,  3, "pefiaOS 0.3 Stardance", 0 },
    { "kernel.log", 0,  3, "boot ok - heap ok - wm ok - taskbar ok", 0 },
};
#define NCOUNT ((int)(sizeof(nodes) / sizeof(nodes[0])))

void vfs_init(void) { }

int vfs_root(void)  { return 0; }
int vfs_count(void) { return NCOUNT; }

const VNode *vfs_node(int i)
{
    return (i >= 0 && i < NCOUNT) ? &nodes[i] : NULL;
}

int vfs_children(int dir, int *out, int max)
{
    int n = 0;
    for (int i = 0; i < NCOUNT && n < max; i++)
        if (nodes[i].parent == dir) out[n++] = i;
    return n;
}
