/* kernel/css.h
 *
 * A CSS engine sized for what real pages actually use: it parses <style>
 * blocks and inline style="" into a flat rule list, then cascades user-agent
 * defaults + author rules + inline style into a ComputedStyle per node.
 * Selectors support type/.class/#id/descendant combinators with correct
 * specificity ordering, plus basic property inheritance.
 */
#ifndef PEFIA_CSS_H
#define PEFIA_CSS_H

#include "framebuffer.h"
#include "domrt.h"

enum { DISP_INLINE = 0, DISP_BLOCK = 1, DISP_NONE = 2, DISP_LIST_ITEM = 3 };
enum { ALIGN_LEFT = 0, ALIGN_CENTER = 1, ALIGN_RIGHT = 2 };

typedef struct {
    color_t color;          /* inherited                                     */
    int     bold;           /* inherited                                     */
    int     italic;         /* inherited                                     */
    int     scale;          /* glyph pixel scale 1/2 (font-size), inherited  */
    int     align;          /* inherited                                     */
    int     line_h;         /* derived from scale                            */
    int     underline;      /* not inherited                                 */
    int     is_link;        /* not inherited                                 */
    int     display;        /* not inherited                                 */
    int     has_bg;         /* not inherited                                 */
    color_t bg;
    int     margin_top;     /* not inherited                                 */
    int     margin_bottom;
    int     indent;         /* extra left indent (padding-left), not inherited */
} ComputedStyle;

void css_reset(void);
void css_add_stylesheet(const char *text, int len);

/* Fills `out` for `node`, inheriting from `parent` (NULL means document defaults). */
void css_cascade(const DomNode *node, const ComputedStyle *parent, ComputedStyle *out);

/* Parses a CSS color literal (#rgb, #rrggbb, rgb(), or a name). 1 on success. */
int  css_parse_color(const char *v, color_t *out);

#endif /* PEFIA_CSS_H */
