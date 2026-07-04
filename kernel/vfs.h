/* Read-only in-memory filesystem. The file explorer only ever talks to
 * this API, so the day someone writes a real FAT32 driver, it can slot in
 * underneath without the explorer noticing. */
#ifndef PEFIA_VFS_H
#define PEFIA_VFS_H

typedef struct {
    char         name[32];
    int          is_dir;
    int          parent;     /* index of parent dir, -1 for root */
    const char  *content;    /* file body text (NULL for dirs)   */
    int          size;
} VNode;

void          vfs_init(void);
int           vfs_root(void);
int           vfs_count(void);
const VNode  *vfs_node(int i);
int           vfs_children(int dir, int *out, int max);  /* fills out[], returns count */

#endif /* PEFIA_VFS_H */
