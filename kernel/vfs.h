#ifndef PEFIA_VFS_H
#define PEFIA_VFS_H

typedef struct {
    char   name[32];
    int    is_dir;
    int    parent;     /* index of parent dir, -1 for root, -2 for a free slot */
    char  *content;    /* file body, heap-allocated (NULL for dirs/empty files) */
    int    size;       /* bytes of content, not counting a NUL terminator      */
} VNode;

void          vfs_init(void);
int           vfs_root(void);
int           vfs_count(void);
const VNode  *vfs_node(int i);
int           vfs_children(int dir, int *out, int max);

/* --- mutation, all return an index or 0/-1 as noted --- */

/* Find a direct child of `dir` by name, or -1. */
int  vfs_find_child(int dir, const char *name);
/* Create an empty file (is_dir=0) or directory (is_dir=1) under `dir`.
 * Returns the new node index, or -1 if the name is taken or the table is full. */
int  vfs_create(int dir, const char *name, int is_dir);
/* Replace a file's contents with `len` bytes of `data`. Returns 0 on success. */
int  vfs_write(int idx, const char *data, int len);
/* Delete a file, or an empty directory. Returns 0 on success, -1 otherwise. */
int  vfs_delete(int idx);
/* Rename a node in place. Returns 0, or -1 if the name is empty/taken. */
int  vfs_rename(int idx, const char *name);

#endif
