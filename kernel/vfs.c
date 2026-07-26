#include "vfs.h"
#include "heap.h"
#include "util.h"
#include <stddef.h>

#define VFS_MAX  128
#define VFS_FREE (-2)   /* parent value marking an unused slot */

static VNode fs_table[VFS_MAX];
static int   fs_high;   /* one past the highest slot ever used */

static int str_eq(const char *a, const char *b)
{
    int i = 0;
    while (a[i] && b[i]) { if (a[i] != b[i]) return 0; i++; }
    return a[i] == b[i];
}

/* Copy into a VNode.name[32] field, always NUL-terminated. */
static void copy_name(char *dst, const char *src)
{
    int i = 0;
    while (src[i] && i < 31) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

/* Copies a NUL-terminated string body into a fresh heap buffer and points the
 * node at it. Used both when seeding and when writing. */
static void set_body(VNode *n, const char *s)
{
    if (n->content) { kfree(n->content); n->content = NULL; n->size = 0; }
    if (!s) return;
    int len = (int)kstrlen(s);
    char *buf = (char *)kmalloc((size_t)len + 1);
    if (!buf) return;
    kmemmove(buf, s, (size_t)len + 1);
    n->content = buf;
    n->size    = len;
}

static int alloc_slot(void)
{
    for (int i = 0; i < fs_high; i++)
        if (fs_table[i].parent == VFS_FREE) return i;
    if (fs_high < VFS_MAX) return fs_high++;
    return -1;
}

static void seed(int idx, const char *name, int is_dir, int parent, const char *body)
{
    VNode *n = &fs_table[idx];
    copy_name(n->name, name);
    n->is_dir  = is_dir;
    n->parent  = parent;
    n->content = NULL;
    n->size    = 0;
    if (body) set_body(n, body);
    if (idx >= fs_high) fs_high = idx + 1;
}

void vfs_init(void)
{
    for (int i = 0; i < VFS_MAX; i++) { fs_table[i].parent = VFS_FREE; fs_table[i].content = NULL; }
    fs_high = 0;

    seed(0,  "/",          1, -1, NULL);
    seed(1,  "Documents",  1,  0, NULL);
    seed(2,  "Pictures",   1,  0, NULL);
    seed(3,  "System",     1,  0, NULL);
    seed(4,  "readme.txt", 0,  0, "Welcome to pefiaOS - this is an in-memory file system.");
    seed(5,  "todo.txt",   0,  1, "1. taskbar   2. file explorer   3. web browser later");
    seed(6,  "notes.txt",  0,  1, "The window manager and heap allocator both work.");
    seed(7,  "hello.c",    0,  1, "int main(void){\n  print(6 * 7);\n  return 0;\n}\n");
    seed(8,  "cat.bmp",    0,  2, "(image data placeholder)");
    seed(9,  "logo.bmp",   0,  2, "(image data placeholder)");
    seed(10, "version",    0,  3, "pefiaOS 0.4 Stardance");
    seed(11, "kernel.log", 0,  3, "boot ok - heap ok - wm ok - taskbar ok");
}

int vfs_root(void)  { return 0; }
int vfs_count(void) { return fs_high; }

const VNode *vfs_node(int i)
{
    if (i < 0 || i >= fs_high) return NULL;
    if (fs_table[i].parent == VFS_FREE) return NULL;
    return &fs_table[i];
}

int vfs_children(int dir, int *out, int max)
{
    int found = 0;
    for (int i = 0; i < fs_high && found < max; i++)
        if (fs_table[i].parent == dir) out[found++] = i;
    return found;
}

int vfs_find_child(int dir, const char *name)
{
    for (int i = 0; i < fs_high; i++)
        if (fs_table[i].parent == dir && str_eq(fs_table[i].name, name))
            return i;
    return -1;
}

int vfs_create(int dir, const char *name, int is_dir)
{
    const VNode *d = vfs_node(dir);
    if (!d || !d->is_dir) return -1;
    if (!name[0] || vfs_find_child(dir, name) >= 0) return -1;
    int idx = alloc_slot();
    if (idx < 0) return -1;
    VNode *n = &fs_table[idx];
    copy_name(n->name, name);
    n->is_dir  = is_dir;
    n->parent  = dir;
    n->content = NULL;
    n->size    = 0;
    return idx;
}

int vfs_write(int idx, const char *data, int len)
{
    const VNode *c = vfs_node(idx);
    if (!c || c->is_dir) return -1;
    VNode *n = &fs_table[idx];
    if (n->content) { kfree(n->content); n->content = NULL; n->size = 0; }
    if (len <= 0) return 0;
    char *buf = (char *)kmalloc((size_t)len + 1);
    if (!buf) return -1;
    kmemmove(buf, data, (size_t)len);
    buf[len] = '\0';
    n->content = buf;
    n->size    = len;
    return 0;
}

int vfs_rename(int idx, const char *name)
{
    const VNode *c = vfs_node(idx);
    if (!c || idx == vfs_root() || !name[0]) return -1;
    int clash = vfs_find_child(c->parent, name);
    if (clash >= 0 && clash != idx) return -1;
    copy_name(fs_table[idx].name, name);
    return 0;
}

int vfs_delete(int idx)
{
    const VNode *c = vfs_node(idx);
    if (!c || idx == vfs_root()) return -1;
    if (c->is_dir) {                       /* refuse to delete a non-empty directory */
        int kids[4];
        if (vfs_children(idx, kids, 4) > 0) return -1;
    }
    VNode *n = &fs_table[idx];
    if (n->content) { kfree(n->content); n->content = NULL; }
    n->size   = 0;
    n->name[0] = '\0';
    n->parent = VFS_FREE;
    return 0;
}
