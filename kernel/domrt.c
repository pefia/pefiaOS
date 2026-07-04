/* kernel/domrt.c - pool-backed DOM tree. Declarations in domrt.h.
 *
 * Nothing here calls kmalloc. Nodes, attributes and interned strings all come
 * out of static arrays sized generously for the pages we actually load; once
 * a pool is full further allocations just return failure and callers are
 * expected to shrug and move on (a half-built tree beats a crash).
 */
#include "domrt.h"

#define DOM_MAX_NODES   9000
#define DOM_MAX_ATTRS   24000
#define DOM_STR_CAP     (384 * 1024)

typedef struct { int name_off; int val_off; int next; } DomAttr;

static DomNode  pool_nodes[DOM_MAX_NODES];
static int      pool_node_count;
static DomAttr  pool_attrs[DOM_MAX_ATTRS];
static int      pool_attr_count;
static char     str_arena[DOM_STR_CAP];
static int      str_arena_len;
static DomNode *document_root;

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static int  cstrlen(const char *s) { int n = 0; while (s && s[n]) n++; return n; }

static int str_ieq(const char *a, const char *b)
{
    int i = 0;
    while (a[i] && b[i]) { if (lower(a[i]) != lower(b[i])) return 0; i++; }
    return lower(a[i]) == lower(b[i]);
}

int dom_intern(const char *s, int len)
{
    if (len < 0) len = 0;
    if (str_arena_len + len + 1 > DOM_STR_CAP) return -1;
    int off = str_arena_len;
    for (int i = 0; i < len; i++) str_arena[off + i] = s[i];
    str_arena[off + len] = 0;
    str_arena_len += len + 1;
    return off;
}

const char *dom_str(int off)
{
    if (off < 0 || off >= str_arena_len) return "";
    return str_arena + off;
}

static DomNode *new_node(void)
{
    if (pool_node_count >= DOM_MAX_NODES) return 0;
    DomNode *n = &pool_nodes[pool_node_count++];
    n->type = DOM_NODE_ELEMENT;
    n->tag[0] = 0;
    n->text_off = -1;
    n->attr_head = -1;
    n->parent = n->first_child = n->last_child = n->next_sibling = 0;
    return n;
}

void dom_init(void)
{
    pool_node_count = 0;
    pool_attr_count = 0;
    str_arena_len = 0;
    document_root = new_node();
    if (document_root) {
        /* "#r" so it never collides with a real tag name in tag matching */
        document_root->tag[0] = '#'; document_root->tag[1] = 'r'; document_root->tag[2] = 0;
    }
}

DomNode *dom_root(void) { return document_root; }

DomNode *dom_create_element(const char *tag)
{
    DomNode *n = new_node();
    if (!n) return 0;
    n->type = DOM_NODE_ELEMENT;
    int i = 0;
    while (tag && tag[i] && i < 15) { n->tag[i] = lower(tag[i]); i++; }
    n->tag[i] = 0;
    return n;
}

DomNode *dom_create_text(const char *text, int len)
{
    DomNode *n = new_node();
    if (!n) return 0;
    n->type = DOM_NODE_TEXT;
    n->text_off = dom_intern(text ? text : "", len);
    return n;
}

void dom_append_child(DomNode *parent, DomNode *child)
{
    if (!parent || !child) return;
    child->parent = parent;
    child->next_sibling = 0;
    if (!parent->first_child) {
        parent->first_child = parent->last_child = child;
    } else {
        parent->last_child->next_sibling = child;
        parent->last_child = child;
    }
}

void dom_remove_children(DomNode *n)
{
    if (!n) return;
    for (DomNode *c = n->first_child; c; c = c->next_sibling) c->parent = 0;
    n->first_child = n->last_child = 0;
}

const char *dom_tag(const DomNode *n)  { return n ? n->tag : ""; }

const char *dom_text(const DomNode *n)
{
    if (!n || n->type != DOM_NODE_TEXT) return "";
    return dom_str(n->text_off);
}

void dom_set_text(DomNode *n, const char *text, int len)
{
    if (!n) return;
    if (len < 0) len = cstrlen(text);
    n->text_off = dom_intern(text ? text : "", len);
}

/* --- attributes: a singly-linked list per node, threaded through the shared
 * attribute pool (attr_head is the head index, -1 for none). --- */

const char *dom_get_attr(const DomNode *n, const char *name)
{
    if (!n) return "";
    for (int a = n->attr_head; a >= 0; a = pool_attrs[a].next)
        if (str_ieq(dom_str(pool_attrs[a].name_off), name))
            return dom_str(pool_attrs[a].val_off);
    return "";
}

int dom_has_attr(const DomNode *n, const char *name)
{
    if (!n) return 0;
    for (int a = n->attr_head; a >= 0; a = pool_attrs[a].next)
        if (str_ieq(dom_str(pool_attrs[a].name_off), name)) return 1;
    return 0;
}

void dom_set_attr(DomNode *n, const char *name, const char *val)
{
    if (!n || !name) return;
    int vlen = cstrlen(val);

    for (int a = n->attr_head; a >= 0; a = pool_attrs[a].next) {
        if (str_ieq(dom_str(pool_attrs[a].name_off), name)) {
            int off = dom_intern(val ? val : "", vlen);
            if (off >= 0) pool_attrs[a].val_off = off;
            return;
        }
    }
    if (pool_attr_count >= DOM_MAX_ATTRS) return;
    int noff = dom_intern(name, cstrlen(name));
    int voff = dom_intern(val ? val : "", vlen);
    if (noff < 0 || voff < 0) return;   /* string arena full: drop the attr */
    int idx = pool_attr_count++;
    pool_attrs[idx].name_off = noff;
    pool_attrs[idx].val_off  = voff;
    pool_attrs[idx].next = n->attr_head;
    n->attr_head = idx;
}

/* --- lookups --- */

DomNode *dom_get_element_by_id(const char *id)
{
    if (!id || !id[0]) return 0;
    for (int i = 0; i < pool_node_count; i++) {
        DomNode *n = &pool_nodes[i];
        if (n->type != DOM_NODE_ELEMENT) continue;
        const char *nid = dom_get_attr(n, "id");
        if (!nid[0]) continue;
        int j = 0;
        while (nid[j] && id[j] && nid[j] == id[j]) j++;
        if (!nid[j] && !id[j]) return n;
    }
    return 0;
}

DomNode *dom_body(void)
{
    for (int i = 0; i < pool_node_count; i++)
        if (pool_nodes[i].type == DOM_NODE_ELEMENT && str_ieq(pool_nodes[i].tag, "body"))
            return &pool_nodes[i];
    return document_root;
}
