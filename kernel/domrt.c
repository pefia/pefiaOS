/* kernel/domrt.c - DOM tree backed by fixed pools. See domrt.h. */
#include "domrt.h"

#define DOM_MAX_NODES   9000
#define DOM_MAX_ATTRS   24000
#define DOM_STR_CAP     (384 * 1024)

typedef struct { int name_off; int val_off; int next; } DomAttr;

static DomNode  g_nodes[DOM_MAX_NODES];
static int      g_nnodes;
static DomAttr  g_attrs[DOM_MAX_ATTRS];
static int      g_nattrs;
static char     g_str[DOM_STR_CAP];
static int      g_strlen;
static DomNode *g_root;

/* --- small helpers (freestanding, no libc) ------------------------------- */
static char d_lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static int  d_slen(const char *s) { int n = 0; while (s && s[n]) n++; return n; }
static int  d_ieq(const char *a, const char *b)
{
    int i = 0;
    while (a[i] && b[i]) { if (d_lc(a[i]) != d_lc(b[i])) return 0; i++; }
    return d_lc(a[i]) == d_lc(b[i]);
}

int dom_intern(const char *s, int len)
{
    if (len < 0) len = 0;
    if (g_strlen + len + 1 > DOM_STR_CAP) return -1;
    int off = g_strlen;
    for (int i = 0; i < len; i++) g_str[off + i] = s[i];
    g_str[off + len] = 0;
    g_strlen += len + 1;
    return off;
}

const char *dom_str(int off)
{
    if (off < 0 || off >= g_strlen) return "";
    return g_str + off;
}

static DomNode *alloc_node(void)
{
    if (g_nnodes >= DOM_MAX_NODES) return 0;
    DomNode *n = &g_nodes[g_nnodes++];
    n->type = DOM_NODE_ELEMENT;
    n->tag[0] = 0;
    n->text_off = -1;
    n->attr_head = -1;
    n->parent = n->first_child = n->last_child = n->next_sibling = 0;
    return n;
}

void dom_init(void)
{
    g_nnodes = 0;
    g_nattrs = 0;
    g_strlen = 0;
    g_root = alloc_node();
    if (g_root) {
        g_root->type = DOM_NODE_ELEMENT;
        g_root->tag[0] = '#'; g_root->tag[1] = 'r'; g_root->tag[2] = 0;
    }
}

DomNode *dom_root(void) { return g_root; }

DomNode *dom_create_element(const char *tag)
{
    DomNode *n = alloc_node();
    if (!n) return 0;
    n->type = DOM_NODE_ELEMENT;
    int i = 0;
    while (tag && tag[i] && i < 15) { n->tag[i] = d_lc(tag[i]); i++; }
    n->tag[i] = 0;
    return n;
}

DomNode *dom_create_text(const char *text, int len)
{
    DomNode *n = alloc_node();
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
    DomNode *c = n->first_child;
    while (c) { c->parent = 0; c = c->next_sibling; }
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
    if (len < 0) len = d_slen(text);
    n->text_off = dom_intern(text ? text : "", len);
}

/* --- attributes ---------------------------------------------------------- */
const char *dom_get_attr(const DomNode *n, const char *name)
{
    if (!n) return "";
    for (int a = n->attr_head; a >= 0; a = g_attrs[a].next)
        if (d_ieq(dom_str(g_attrs[a].name_off), name))
            return dom_str(g_attrs[a].val_off);
    return "";
}

int dom_has_attr(const DomNode *n, const char *name)
{
    if (!n) return 0;
    for (int a = n->attr_head; a >= 0; a = g_attrs[a].next)
        if (d_ieq(dom_str(g_attrs[a].name_off), name)) return 1;
    return 0;
}

void dom_set_attr(DomNode *n, const char *name, const char *val)
{
    if (!n || !name) return;
    int vlen = d_slen(val);
    /* update in place if present */
    for (int a = n->attr_head; a >= 0; a = g_attrs[a].next) {
        if (d_ieq(dom_str(g_attrs[a].name_off), name)) {
            int off = dom_intern(val ? val : "", vlen);
            if (off >= 0) g_attrs[a].val_off = off;
            return;
        }
    }
    if (g_nattrs >= DOM_MAX_ATTRS) return;
    int noff = dom_intern(name, d_slen(name));
    int voff = dom_intern(val ? val : "", vlen);
    if (noff < 0 || voff < 0) return;
    int idx = g_nattrs++;
    g_attrs[idx].name_off = noff;
    g_attrs[idx].val_off  = voff;
    g_attrs[idx].next = n->attr_head;
    n->attr_head = idx;
}

/* --- queries ------------------------------------------------------------- */
DomNode *dom_get_element_by_id(const char *id)
{
    if (!id || !id[0]) return 0;
    for (int i = 0; i < g_nnodes; i++) {
        DomNode *n = &g_nodes[i];
        if (n->type != DOM_NODE_ELEMENT) continue;
        const char *nid = dom_get_attr(n, "id");
        if (nid[0]) {
            int j = 0; while (nid[j] && id[j] && nid[j] == id[j]) j++;
            if (!nid[j] && !id[j]) return n;
        }
    }
    return 0;
}

DomNode *dom_body(void)
{
    for (int i = 0; i < g_nnodes; i++)
        if (g_nodes[i].type == DOM_NODE_ELEMENT && d_ieq(g_nodes[i].tag, "body"))
            return &g_nodes[i];
    return g_root;
}
