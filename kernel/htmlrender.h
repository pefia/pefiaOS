/* kernel/htmlrender.h
 *
 * The layout + paint engine for the in-OS browser. Tokenizes/parses HTML
 * into a DOM, cascades CSS, runs scripts, then flows the result into
 * word-wrapped text with headings, links, lists, rules and images -
 * decoding entities and applying inline/author CSS color and layout along
 * the way. JavaScript degrades gracefully: <noscript> renders, and simple
 * document.write output gets spliced in. Only one document is active at a
 * time.
 */
#ifndef PEFIA_HTMLRENDER_H
#define PEFIA_HTMLRENDER_H

/* Sets the active page. A cheap no-op if (id, gen) matches what's already
 * loaded. The HTML is copied internally, so the caller's buffer doesn't need
 * to outlive this call. */
void html_set_page(const void *id, unsigned gen, const char *html, int len);

/* Lays the active page out for a given content width in pixels (the result
 * is cached per width). Fills title_out if the page has a <title>. */
void html_layout(int width, char *title_out, int title_cap);

/* Total laid-out document height in pixels. */
int  html_doc_height(void);

/* Paints the document into (x, y, w, h), scrolled down by `scroll`. */
void html_paint(int x, int y, int w, int h, int scroll);

/* Sets the base URL used to resolve relative resource URLs (img/script src). */
void html_set_base_url(const char *url);

/* Hit-tests a link at document coordinates; returns its href, or 0. */
const char *html_link_at(int doc_x, int doc_y);

#endif /* PEFIA_HTMLRENDER_H */
