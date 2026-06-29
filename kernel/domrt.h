/* kernel/domrt.h
 * -----------------------------------------------------------------------------
 * A real (if small) DOM tree: typed nodes, a parent/child/sibling structure,
 * arbitrary attributes, id lookup, and live mutation (set text / replace
 * children) so a script engine and a CSS cascade can both work against it.
 *
 * All storage comes from fixed pools (no per-node malloc) so the tree is cheap
 * to build and tear down on every navigation. When a pool fills, allocation
 * fails gracefully and the partial tree still renders.
 * -----------------------------------------------------------------------------
 */
#ifndef PEFIA_DOMRT_H
#define PEFIA_DOMRT_H

#include <stdint.h>

typedef enum {
    DOM_NODE_ELEMENT = 1,
    DOM_NODE_TEXT    = 2
} DomNodeType;

typedef struct DomNode {
    uint8_t  type;
    char     tag[16];          /* lowercased tag name (elements only)        */
    int      text_off;         /* offset into the string arena, or -1        */
    int      attr_head;        /* index into the attribute pool, or -1       */
    struct DomNode *parent;
    struct DomNode *first_child;
    struct DomNode *last_child;
    struct DomNode *next_sibling;
} DomNode;

/* Reset all pools and create a fresh empty document root. */
void        dom_init(void);
DomNode    *dom_root(void);
/* First <body> in document order, or the root if none. */
DomNode    *dom_body(void);

DomNode    *dom_create_element(const char *tag);
DomNode    *dom_create_text(const char *text, int len);
void        dom_append_child(DomNode *parent, DomNode *child);
void        dom_remove_children(DomNode *n);   /* detaches all children       */

const char *dom_tag(const DomNode *n);
const char *dom_text(const DomNode *n);        /* "" for element nodes        */
void        dom_set_text(DomNode *n, const char *text, int len);

/* Attributes. get returns "" when absent. set updates in place or appends. */
const char *dom_get_attr(const DomNode *n, const char *name);
int         dom_has_attr(const DomNode *n, const char *name);
void        dom_set_attr(DomNode *n, const char *name, const char *val);

/* document.getElementById */
DomNode    *dom_get_element_by_id(const char *id);

/* String-arena helpers (used by the parser and the script engine). */
int         dom_intern(const char *s, int len);   /* -1 on overflow          */
const char *dom_str(int off);                      /* "" for off < 0          */

#endif /* PEFIA_DOMRT_H */
