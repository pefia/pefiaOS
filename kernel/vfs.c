/* Fake filesystem, baked in at compile time. Everything below is just
 * flat data - no directories are actually built at runtime, "parent" is
 * simply an index into this same array. Whenever a real disk backend
 * shows up, this whole table gets deleted and nothing outside this file
 * should notice. */
#include "vfs.h"
#include <stddef.h>

/* name, is_dir, parent index, content, size */
static VNode fs_table[] = {
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
#define FS_NODE_COUNT ((int)(sizeof(fs_table) / sizeof(fs_table[0])))

void vfs_init(void) { } /* nothing to set up, the table above is already "mounted" */

int vfs_root(void)  { return 0; }
int vfs_count(void) { return FS_NODE_COUNT; }

const VNode *vfs_node(int i)
{
    return (i >= 0 && i < FS_NODE_COUNT) ? &fs_table[i] : NULL;
}

int vfs_children(int dir, int *out, int max)
{
    int found = 0;
    for (int i = 0; i < FS_NODE_COUNT && found < max; i++)
        if (fs_table[i].parent == dir) out[found++] = i;
    return found;
}
