#ifndef PEFIA_CSS_H
#define PEFIA_CSS_H

#include "framebuffer.h"
#include "domrt.h"

enum { DISP_INLINE = 0, DISP_BLOCK = 1, DISP_NONE = 2, DISP_LIST_ITEM = 3 };
enum { ALIGN_LEFT = 0, ALIGN_CENTER = 1, ALIGN_RIGHT = 2 };
enum { TT_NONE = 0, TT_UPPER = 1, TT_LOWER = 2 };

typedef struct {
    color_t color;
    int     bold;
    int     italic;
    int     scale;          /* glyph pixel scale 1/2 (font-size), inherited  */
    int     align;
    int     line_h;
    int     text_transform;
    int     underline;
    int     strike;
    int     is_link;
    int     display;
    int     has_bg;
    color_t bg;
    int     margin_top;
    int     margin_bottom;
    int     indent;
    int     pad_top;        /* vertical padding, folded into block spacing   */
    int     pad_bottom;
    int     border_w;       /* border width in px (0 = none)                 */
    color_t border_color;
    int     width_px;
    int     hidden;
} ComputedStyle;

void css_reset(void);
void css_add_stylesheet(const char *text, int len);

/* Fills `out` for `node`, inheriting from `parent` (NULL means document defaults). */
void css_cascade(const DomNode *node, const ComputedStyle *parent, ComputedStyle *out);

/* Parses a CSS color literal (#rgb, #rrggbb, rgb()/rgba(), hsl(), or a name).
 * Returns 1 on success, 0 for "no color here" (including transparent). */
int  css_parse_color(const char *v, color_t *out);

#endif
