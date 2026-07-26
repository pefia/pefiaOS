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

/* Runs script click handlers for whatever is at these document coords,
 * bubbling to ancestors. Returns 1 if a handler ran and changed the DOM
 * (the page has already been re-laid-out in that case). */
int  html_click_script(int doc_x, int doc_y);

/* Runs due script timers plus any pending load handlers. Returns 1 if the
 * document changed and was re-laid-out. Call once per frame. */
int  html_pump(void);

/* One-shot reads of script side effects: a navigation the page requested
 * (location.href = ..., window.open) and alert() text. 1 if one was taken. */
int  html_take_nav(char *out, int cap);
int  html_take_alert(char *out, int cap);

/* 1 if the document has an animation running, so the caller knows the page
 * needs repainting even when nothing else changed. */
int  html_animating(void);

enum {
    FK_NONE = 0,
    FK_TEXT = 1,
    FK_SUBMIT = 2,
    FK_CHECK = 3,
    FK_SELECT = 4
};

/* Field index at document coords, or -1. */
int  html_field_at(int doc_x, int doc_y);

int  html_field_kind(int id);
/* Focus an editable field (ignored for non-editable); -1 clears focus. */
void html_field_focus(int id);
int  html_field_focused(void);
/* Feed a character to the focused field (printable or '\b'). */
void html_field_key(char c);
/* Toggle a checkbox/radio or advance a select to its next option. */
void html_field_toggle(int id);

/* Build the navigation for the form containing field `id` (id < 0 = the
 * focused field). Returns 0 (not submittable), 1 for GET with the full URL
 * in url_out, or 2 for POST with the action in url_out and the encoded
 * body in body_out. */
int  html_field_submit(int id, char *url_out, int url_cap, char *body_out, int body_cap);

#endif
