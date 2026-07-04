/* kernel/domrt.h
 *
 * A minimal DOM: typed nodes in a parent/child/sibling tree, attribute lists,
 * id lookup, and enough mutation (set text / replace children) to let a
 * script engine and the CSS cascade both poke at the same tree.
 *
 * Everything lives in fixed-size pools instead of per-node allocation - a
 * fresh page means a fresh tree, and we'd rather fail an allocation cleanly
 * (partial page, still renders) than deal with fragmentation on every
 * navigation.
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

/* Wipes every pool and hands back a fresh, empty document root. */
void        dom_init(void);
DomNode    *dom_root(void);
/* First <body> in document order, falling back to the root if there isn't one. */
DomNode    *dom_body(void);

DomNode    *dom_create_element(const char *tag);
DomNode    *dom_create_text(const char *text, int len);
void        dom_append_child(DomNode *parent, DomNode *child);
void        dom_remove_children(DomNode *n);   /* detaches all children, doesn't free */

const char *dom_tag(const DomNode *n);
const char *dom_text(const DomNode *n);        /* "" for element nodes        */
void        dom_set_text(DomNode *n, const char *text, int len);

/* Attributes. get() returns "" when absent; set() updates in place or appends. */
const char *dom_get_attr(const DomNode *n, const char *name);
int         dom_has_attr(const DomNode *n, const char *name);
void        dom_set_attr(DomNode *n, const char *name, const char *val);

/* document.getElementById */
DomNode    *dom_get_element_by_id(const char *id);

/* String-arena helpers, shared with the parser and the script engine. */
int         dom_intern(const char *s, int len);   /* -1 on overflow          */
const char *dom_str(int off);                      /* "" for off < 0          */

#endif /* PEFIA_DOMRT_H */
