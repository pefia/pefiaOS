/* kernel/css.c - stylesheet parsing and the cascade. See css.h.
 *
 * Everything here works on two flat arenas: g_css holds the raw text of
 * every selector and declaration block we've seen (so rules can just store
 * offsets), and g_rules is the parsed rule list itself, kept in source
 * order with a specificity number precomputed at parse time.
 */
#include "css.h"

static char to_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static int str_ieq(const char *a, const char *b)
{
    int i = 0;
    while (a[i] && b[i]) { if (to_lower(a[i]) != to_lower(b[i])) return 0; i++; }
    return to_lower(a[i]) == to_lower(b[i]);
}

static int is_space(char ch) { return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f'; }

/* Case-insensitive compare of the first n bytes; used for prefix checks like "rgb(". */
static int prefix_ieq(const char *a, const char *b, int n)
{
    for (int i = 0; i < n; i++) if (to_lower(a[i]) != to_lower(b[i])) return 0;
    return 1;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

int css_parse_color(const char *v, color_t *out)
{
    while (*v == ' ') v++;

    if (v[0] == '#') {
        int n = 0;
        while (hex_digit(v[1 + n]) >= 0) n++;
        if (n >= 6) {
            *out = fb_rgb(hex_digit(v[1])*16+hex_digit(v[2]), hex_digit(v[3])*16+hex_digit(v[4]), hex_digit(v[5])*16+hex_digit(v[6]));
            return 1;
        }
        if (n >= 3) {
            *out = fb_rgb(hex_digit(v[1])*17, hex_digit(v[2])*17, hex_digit(v[3])*17);
            return 1;
        }
        return 0;
    }

    if (prefix_ieq(v, "rgb", 3)) {
        const char *p = v;
        while (*p && *p != '(') p++;
        if (*p != '(') return 0;
        int comp[3] = {0,0,0}, ci = 0;
        p++;
        while (*p && *p != ')' && ci < 3) {
            while (*p == ' ' || *p == ',') p++;
            int val = 0, saw_digit = 0;
            while (*p >= '0' && *p <= '9') { val = val*10 + (*p - '0'); p++; saw_digit = 1; }
            if (saw_digit) comp[ci++] = val;
            while (*p && *p != ',' && *p != ')') p++;
        }
        *out = fb_rgb(comp[0] & 0xFF, comp[1] & 0xFF, comp[2] & 0xFF);
        return 1;
    }

    static const struct { const char *n; uint8_t r, g, b; } named[] = {
        {"black",0,0,0},{"white",255,255,255},{"red",200,32,32},{"green",24,150,60},
        {"blue",40,90,210},{"navy",16,32,90},{"gray",128,128,128},{"grey",128,128,128},
        {"silver",192,192,192},{"orange",230,130,30},{"purple",130,50,160},{"teal",20,130,130},
        {"maroon",140,30,30},{"olive",120,120,20},{"yellow",210,190,20},{"lime",60,190,60},
        {"aqua",40,190,200},{"fuchsia",210,50,180},{"pink",235,140,170},{"brown",140,80,40},
        {"gold",210,170,30},{"darkgray",100,100,100},{"darkgrey",100,100,100},
        {"lightgray",215,215,215},{"lightgrey",215,215,215},{"whitesmoke",245,245,245},
        {"transparent",0,0,0},
    };
    for (int i = 0; i < (int)(sizeof(named)/sizeof(named[0])); i++) {
        if (!str_ieq(v, named[i].n)) continue;
        if (str_ieq(v, "transparent")) return 0;   /* we don't do alpha, so treat as "unset" */
        *out = fb_rgb(named[i].r, named[i].g, named[i].b);
        return 1;
    }
    return 0;
}

/* --- rule storage --- */
#define CSS_TEXT_CAP  (96 * 1024)
#define CSS_MAX_RULES 3000

typedef struct {
    int sel_off, sel_len;
    int decl_off, decl_len;
    int spec, order;
} CssRule;

static char    g_css[CSS_TEXT_CAP];
static int     g_csslen;
static CssRule g_rules[CSS_MAX_RULES];
static int     g_nrules;
static int     g_order;

void css_reset(void)
{
    g_csslen = 0;
    g_nrules = 0;
    g_order  = 0;
}

/* CSS specificity, collapsed to a single int: id=100, class/attr/pseudo-class=10,
 * type=1. Good enough since we never need to distinguish 2 ids from 3. */
static int specificity(const char *s, int len)
{
    int spec = 0, i = 0;
    while (i < len) {
        char ch = s[i];
        if (ch == '#') { spec += 100; i++; while (i < len && s[i] != '.' && s[i] != '#' && !is_space(s[i])) i++; }
        else if (ch == '.') { spec += 10; i++; while (i < len && s[i] != '.' && s[i] != '#' && !is_space(s[i])) i++; }
        else if (ch == ':') { i++; while (i < len && s[i] != '.' && s[i] != '#' && !is_space(s[i])) i++; }
        else if (!is_space(ch) && ch != '*' && ch != '>' && ch != '+' && ch != '~') {
            spec += 1; i++;
            while (i < len && s[i] != '.' && s[i] != '#' && s[i] != ':' && !is_space(s[i])) i++;
        }
        else i++;
    }
    return spec;
}

/* Appends `len` bytes of text into the css arena and returns the offset, or
 * -1 if it doesn't fit. Used for both selector and declaration text. */
static int css_arena_put(const char *text, int len)
{
    if (g_csslen + len + 1 > CSS_TEXT_CAP) return -1;
    int off = g_csslen;
    for (int z = 0; z < len; z++) g_css[off + z] = text[z];
    g_css[off + len] = 0;
    g_csslen += len + 1;
    return off;
}

void css_add_stylesheet(const char *text, int len)
{
    int i = 0;
    while (i < len) {
        while (i < len && is_space(text[i])) i++;
        if (i + 1 < len && text[i] == '/' && text[i+1] == '*') {
            i += 2;
            while (i + 1 < len && !(text[i] == '*' && text[i+1] == '/')) i++;
            i += 2;
            continue;
        }
        if (i >= len) break;

        if (text[i] == '@') {
            /* @media / @import / @font-face etc. We don't evaluate media
             * queries - just skip the "@foo(...)" prefix and let whatever is
             * inside the following {} block fall through as plain rules.
             * Wrong for conditional stylesheets, harmless for the common
             * case of "@media screen { ... }" always applying. */
            while (i < len && text[i] != '{' && text[i] != ';') i++;
            if (i < len && text[i] == '{') i++;
            else if (i < len) i++;
            continue;
        }

        int sel_start = i;
        while (i < len && text[i] != '{' && text[i] != '}') i++;
        if (i >= len || text[i] != '{') { if (i < len) i++; continue; }
        int sel_end = i;
        i++;
        int decl_start = i;
        while (i < len && text[i] != '}') i++;
        int decl_end = i;
        if (i < len) i++;

        int decl_len = decl_end - decl_start;
        int decl_off = css_arena_put(text + decl_start, decl_len);
        if (decl_off < 0) continue;

        /* one rule per comma-separated selector, all sharing the same decl block */
        int s = sel_start;
        while (s < sel_end) {
            while (s < sel_end && (is_space(text[s]) || text[s] == ',')) s++;
            int e = s;
            while (e < sel_end && text[e] != ',') e++;
            int t = e;
            while (t > s && is_space(text[t-1])) t--;
            int sel_len = t - s;
            if (sel_len > 0 && g_nrules < CSS_MAX_RULES) {
                int sel_off = css_arena_put(text + s, sel_len);
                if (sel_off >= 0) {
                    CssRule *r = &g_rules[g_nrules++];
                    r->sel_off = sel_off; r->sel_len = sel_len;
                    r->decl_off = decl_off; r->decl_len = decl_len;
                    r->spec = specificity(g_css + sel_off, sel_len);
                    r->order = g_order++;
                }
            }
            s = e;
        }
    }
}

/* --- selector matching --- */

/* Matches one compound selector (type + #id + .class*, no combinators). */
static int match_simple(const DomNode *node, const char *sel, int len)
{
    if (!node || node->type != DOM_NODE_ELEMENT) return 0;
    int i = 0;

    if (i < len && sel[i] != '.' && sel[i] != '#' && sel[i] != ':' && sel[i] != '*' && sel[i] != '[') {
        char tag[16]; int tl = 0;
        while (i < len && sel[i] != '.' && sel[i] != '#' && sel[i] != ':' && sel[i] != '[' && tl < 15)
            tag[tl++] = sel[i++];
        tag[tl] = 0;
        if (!str_ieq(node->tag, tag)) return 0;
    } else if (i < len && sel[i] == '*') {
        i++;
    }

    const char *classes = dom_get_attr(node, "class");
    const char *id      = dom_get_attr(node, "id");
    while (i < len) {
        char ch = sel[i];
        if (ch == '#') {
            i++; char want[64]; int wl = 0;
            while (i < len && sel[i] != '.' && sel[i] != '#' && sel[i] != ':' && wl < 63) want[wl++] = sel[i++];
            want[wl] = 0;
            int j = 0; while (id[j] && want[j] && id[j] == want[j]) j++;
            if (id[j] || want[j]) return 0;
        } else if (ch == '.') {
            i++; char want[64]; int wl = 0;
            while (i < len && sel[i] != '.' && sel[i] != '#' && sel[i] != ':' && wl < 63) want[wl++] = sel[i++];
            want[wl] = 0;
            int found = 0, p = 0;
            while (classes[p]) {
                while (classes[p] == ' ') p++;
                int q = p; while (classes[q] && classes[q] != ' ') q++;
                int same_len = (q - p == wl);
                for (int z = 0; same_len && z < wl; z++) if (classes[p + z] != want[z]) same_len = 0;
                if (same_len) { found = 1; break; }
                p = q;
            }
            if (!found) return 0;
        } else if (ch == ':') {
            i++; while (i < len && sel[i] != '.' && sel[i] != '#') i++;   /* pseudo-classes: ignored */
        } else if (ch == '[') {
            while (i < len && sel[i] != ']') i++;                          /* attribute selectors: ignored */
            if (i < len) i++;
        } else {
            i++;
        }
    }
    return 1;
}

/* Full selector, descendant combinators only. The rightmost compound must
 * match `node`; each compound to its left must match some ancestor, in
 * order (not necessarily the direct parent - "div p" matches p nested any
 * number of levels under div). */
static int match_selector(const DomNode *node, const char *sel, int len)
{
    int starts[16], lens[16], count = 0;
    int i = 0;
    while (i < len && count < 16) {
        while (i < len && (is_space(sel[i]) || sel[i] == '>' || sel[i] == '+' || sel[i] == '~')) i++;
        if (i >= len) break;
        int s = i;
        while (i < len && !is_space(sel[i])) i++;
        starts[count] = s; lens[count] = i - s; count++;
    }
    if (count == 0) return 0;

    if (!match_simple(node, sel + starts[count-1], lens[count-1])) return 0;

    const DomNode *cur = node->parent;
    for (int t = count - 2; t >= 0; t--) {
        int matched = 0;
        while (cur) {
            if (match_simple(cur, sel + starts[t], lens[t])) { matched = 1; cur = cur->parent; break; }
            cur = cur->parent;
        }
        if (!matched) return 0;
    }
    return 1;
}

/* --- declaration application --- */

static int parse_length_px(const char *v)
{
    while (*v == ' ') v++;
    int sign = 1;
    if (*v == '-') { sign = -1; v++; }
    int n = 0, saw_digit = 0;
    while (*v >= '0' && *v <= '9') { n = n*10 + (*v - '0'); v++; saw_digit = 1; }
    while (*v == ' ') v++;
    if (v[0] == 'e' && v[1] == 'm') n *= 16;   /* rough em -> px, assumes 16px base */
    return saw_digit ? sign * n : 0;
}

static void set_font_size(ComputedStyle *out, const char *v)
{
    while (*v == ' ') v++;
    int px;
    if (str_ieq(v, "small") || str_ieq(v, "x-small") || str_ieq(v, "smaller")) px = 13;
    else if (str_ieq(v, "medium")) px = 16;
    else if (str_ieq(v, "large") || str_ieq(v, "larger")) px = 22;
    else if (str_ieq(v, "x-large")) px = 28;
    else if (str_ieq(v, "xx-large")) px = 34;
    else px = parse_length_px(v);
    if (px <= 0) return;
    out->scale  = px >= 22 ? 2 : 1;
    out->line_h = px >= 22 ? (px + 8) : 19;
}

static void apply_decl(ComputedStyle *out, const char *prop, const char *val)
{
    color_t c;
    if (str_ieq(prop, "color")) {
        if (css_parse_color(val, &c)) out->color = c;
    } else if (str_ieq(prop, "background-color") || str_ieq(prop, "background")) {
        if (css_parse_color(val, &c)) { out->bg = c; out->has_bg = 1; }
    } else if (str_ieq(prop, "font-weight")) {
        out->bold = (str_ieq(val, "bold") || str_ieq(val, "bolder") ||
                     str_ieq(val, "700") || str_ieq(val, "800") || str_ieq(val, "900")) ? 1 : 0;
    } else if (str_ieq(prop, "font-style")) {
        out->italic = (str_ieq(val, "italic") || str_ieq(val, "oblique"));
    } else if (str_ieq(prop, "font-size")) {
        set_font_size(out, val);
    } else if (str_ieq(prop, "text-align")) {
        out->align = str_ieq(val, "center") ? ALIGN_CENTER : str_ieq(val, "right") ? ALIGN_RIGHT : ALIGN_LEFT;
    } else if (str_ieq(prop, "text-decoration") || str_ieq(prop, "text-decoration-line")) {
        out->underline = str_ieq(val, "underline") ? 1 : 0;
    } else if (str_ieq(prop, "display")) {
        if (str_ieq(val, "none")) out->display = DISP_NONE;
        else if (str_ieq(val, "inline") || str_ieq(val, "inline-block")) out->display = DISP_INLINE;
        else if (str_ieq(val, "list-item")) out->display = DISP_LIST_ITEM;
        else out->display = DISP_BLOCK;
    } else if (str_ieq(prop, "margin")) {
        int m = parse_length_px(val);
        out->margin_top = out->margin_bottom = m < 0 ? 0 : m;
    } else if (str_ieq(prop, "margin-top")) {
        int m = parse_length_px(val); out->margin_top = m < 0 ? 0 : m;
    } else if (str_ieq(prop, "margin-bottom")) {
        int m = parse_length_px(val); out->margin_bottom = m < 0 ? 0 : m;
    } else if (str_ieq(prop, "padding-left") || str_ieq(prop, "padding")) {
        int m = parse_length_px(val); if (m > 0) out->indent += m;
    }
}

static void apply_decl_block(ComputedStyle *out, const char *decl, int len)
{
    int i = 0;
    char prop[48], val[256];
    while (i < len) {
        while (i < len && (is_space(decl[i]) || decl[i] == ';')) i++;

        int pl = 0;
        while (i < len && decl[i] != ':' && decl[i] != ';' && decl[i] != '}' && pl < 47) prop[pl++] = decl[i++];
        prop[pl] = 0;
        while (pl > 0 && is_space(prop[pl-1])) prop[--pl] = 0;

        if (i >= len || decl[i] != ':') { while (i < len && decl[i] != ';') i++; continue; }
        i++;
        while (i < len && is_space(decl[i])) i++;

        int vl = 0;
        while (i < len && decl[i] != ';' && decl[i] != '}' && vl < 255) val[vl++] = decl[i++];
        while (vl > 0 && is_space(val[vl-1])) vl--;
        val[vl] = 0;

        /* strip a trailing "!important" - we don't implement importance
         * tiers, just make sure the "!" doesn't end up part of the value */
        for (int z = 0; z < vl - 1; z++) {
            if (val[z] != '!') continue;
            val[z] = 0;
            while (z > 0 && is_space(val[z-1])) val[--z] = 0;
            break;
        }

        if (prop[0] && val[0]) apply_decl(out, prop, val);
    }
}

/* --- user-agent defaults --- */

static int is_block_level_tag(const char *t)
{
    static const char *block_tags[] = {
        "p","div","section","article","header","footer","nav","main",
        "aside","ul","ol","li","dl","dt","dd","table","tr","form","blockquote","figure",
        "figcaption","h1","h2","h3","h4","h5","h6","fieldset","address","pre","hr","body",
        "html","center","details","summary","caption",0
    };
    for (int i = 0; block_tags[i]; i++) if (str_ieq(t, block_tags[i])) return 1;
    return 0;
}

static void apply_ua_defaults(const DomNode *n, ComputedStyle *out)
{
    const char *t = n->tag;

    if (str_ieq(t,"head")||str_ieq(t,"title")||str_ieq(t,"meta")||str_ieq(t,"link")||
        str_ieq(t,"script")||str_ieq(t,"style")||str_ieq(t,"base")||str_ieq(t,"#r")) {
        out->display = DISP_NONE;
        return;
    }

    out->display = is_block_level_tag(t) ? DISP_BLOCK : DISP_INLINE;

    if (t[0] == 'h' && t[1] >= '1' && t[1] <= '6' && t[2] == 0) {
        out->bold = 1;
        int level = t[1] - '0';
        out->scale  = level <= 2 ? 2 : 1;
        out->line_h = level <= 2 ? 30 : 22;
        out->margin_top = level <= 2 ? 16 : 12;
        out->margin_bottom = level <= 2 ? 10 : 6;
        out->color = fb_rgb(18, 42, 96);
    }
    else if (str_ieq(t,"p"))          { out->margin_top = 2; out->margin_bottom = 10; }
    else if (str_ieq(t,"ul")||str_ieq(t,"ol")) { out->margin_top = 6; out->margin_bottom = 6; out->indent = 24; }
    else if (str_ieq(t,"li"))         { out->display = DISP_LIST_ITEM; }
    else if (str_ieq(t,"blockquote")) { out->margin_top = 8; out->margin_bottom = 8; out->indent = 24; }
    else if (str_ieq(t,"pre"))        { out->margin_top = 8; out->margin_bottom = 8; out->color = fb_rgb(70,80,96); }
    else if (str_ieq(t,"a"))          { out->is_link = 1; out->underline = 1; out->color = fb_rgb(26, 90, 210); }
    else if (str_ieq(t,"b")||str_ieq(t,"strong")||str_ieq(t,"th")) { out->bold = 1; }
    else if (str_ieq(t,"i")||str_ieq(t,"em")||str_ieq(t,"cite")||str_ieq(t,"var")) { out->italic = 1; }
    else if (str_ieq(t,"u")||str_ieq(t,"ins"))   { out->underline = 1; }
    else if (str_ieq(t,"code")||str_ieq(t,"tt")||str_ieq(t,"kbd")||str_ieq(t,"samp")) { out->color = fb_rgb(70,80,96); }
    else if (str_ieq(t,"small"))      { out->scale = 1; }
    else if (str_ieq(t,"center"))     { out->align = ALIGN_CENTER; }
    else if (str_ieq(t,"div")||str_ieq(t,"section")||str_ieq(t,"article")||str_ieq(t,"header")||
             str_ieq(t,"footer")||str_ieq(t,"nav")||str_ieq(t,"main")||str_ieq(t,"aside")) {
        out->margin_top = 0; out->margin_bottom = 0;
    }
}

/* --- the cascade --- */
void css_cascade(const DomNode *node, const ComputedStyle *parent, ComputedStyle *out)
{
    if (parent) {
        out->color  = parent->color;
        out->bold   = parent->bold;
        out->italic = parent->italic;
        out->scale  = parent->scale;
        out->align  = parent->align;
        out->line_h = parent->line_h;
    } else {
        out->color  = fb_rgb(28, 30, 36);
        out->bold   = 0;
        out->italic = 0;
        out->scale  = 1;
        out->align  = ALIGN_LEFT;
        out->line_h = 19;
    }
    out->underline = 0;
    out->is_link = 0;
    out->display = DISP_INLINE;
    out->has_bg = 0;
    out->bg = 0;
    out->margin_top = 0;
    out->margin_bottom = 0;
    out->indent = 0;

    if (!node || node->type != DOM_NODE_ELEMENT) { out->display = DISP_INLINE; return; }

    apply_ua_defaults(node, out);
    if (out->display == DISP_NONE) return;

    /* Author rules applied in (specificity, source-order) so the winning
     * declaration lands last. We don't sort g_rules up front - pages rarely
     * have more than a few hundred rules and nodes get cascaded once per
     * layout pass, so repeatedly scanning for "next rule in order" is cheap
     * enough and avoids keeping a second sorted index around. */
    int last_spec = -1, last_order = -1;
    for (;;) {
        int best = -1, best_spec = -1, best_order = -1;
        for (int i = 0; i < g_nrules; i++) {
            CssRule *r = &g_rules[i];
            if (r->spec < last_spec || (r->spec == last_spec && r->order <= last_order)) continue;
            if (r->spec > best_spec || (r->spec == best_spec && r->order < best_order)) {
                if (match_selector(node, g_css + r->sel_off, r->sel_len)) {
                    best = i; best_spec = r->spec; best_order = r->order;
                }
            }
        }
        if (best < 0) break;
        CssRule *r = &g_rules[best];
        apply_decl_block(out, g_css + r->decl_off, r->decl_len);
        last_spec = r->spec; last_order = r->order;
    }

    /* inline style="" always wins over stylesheet rules */
    const char *inline_style = dom_get_attr(node, "style");
    if (inline_style[0]) {
        int l = 0; while (inline_style[l]) l++;
        apply_decl_block(out, inline_style, l);
    }

    if (out->line_h < 16) out->line_h = out->scale == 2 ? 30 : 19;
}
