/* kernel/htmlrender.h
 * -----------------------------------------------------------------------------
 * A compact HTML5/CSS-aware layout + paint engine for the pefiaOS browser.
 * It tokenizes HTML, lays out word-wrapped flowed text with headings, links,
 * lists and rules, decodes entities, applies a little inline CSS colour, and
 * degrades JavaScript gracefully (rendering <noscript> and simple
 * document.write output). One active document at a time.
 * -----------------------------------------------------------------------------
 */
#ifndef PEFIA_HTMLRENDER_H
#define PEFIA_HTMLRENDER_H

/* Set the active page. Cheap no-op if (id,gen) is unchanged. The HTML is copied
 * internally, so the caller's buffer need not outlive this call. */
void html_set_page(const void *id, unsigned gen, const char *html, int len);

/* Lay the active page out for a content width in pixels (cached per width).
 * Fills the page title if a <title> was present. */
void html_layout(int width, char *title_out, int title_cap);

/* Total laid-out document height in pixels. */
int  html_doc_height(void);

/* Paint the document into the rectangle (x,y,w,h) scrolled down by `scroll`. */
void html_paint(int x, int y, int w, int h, int scroll);

/* Set base URL used to resolve relative resource URLs (img/script src). */
void html_set_base_url(const char *url);

/* Hit-test a link at document coordinates; returns its href or 0. */
const char *html_link_at(int doc_x, int doc_y);

#endif /* PEFIA_HTMLRENDER_H */
