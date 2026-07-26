#ifndef PEFIA_DOMPARSE_H
#define PEFIA_DOMPARSE_H

#include "domrt.h"

/* Resets the DOM and parses a full document into it. Returns the new root. */
DomNode *dom_parse_document(const char *html, int len);

/* Parses an HTML fragment as children of `parent`, discarding whatever
 * children it had before (this is what innerHTML = ... calls). */
void     dom_parse_fragment(DomNode *parent, const char *html, int len);

/* Parses a fragment and appends its nodes to `parent` (document.write). */
void     dom_parse_append(DomNode *parent, const char *html, int len);

#endif
