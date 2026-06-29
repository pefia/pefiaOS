/* kernel/domparse.h - build a DOM tree from an HTML byte stream. */
#ifndef PEFIA_DOMPARSE_H
#define PEFIA_DOMPARSE_H

#include "domrt.h"

/* Reset the DOM and parse a full document into it. Returns the document root. */
DomNode *dom_parse_document(const char *html, int len);

/* Parse an HTML fragment as children of `parent`, replacing its current
 * children (used by innerHTML = ...). */
void     dom_parse_fragment(DomNode *parent, const char *html, int len);

/* Parse an HTML fragment and append its nodes to `parent` (document.write). */
void     dom_parse_append(DomNode *parent, const char *html, int len);

#endif /* PEFIA_DOMPARSE_H */
