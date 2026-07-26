#ifndef STUB_VFS
#define STUB_VFS
typedef struct { char name[32]; int is_dir; int parent; char *content; int size; } VNode;
int vfs_count(void);
const VNode *vfs_node(int i);
#endif
