/* kernel/css.c - stylesheet parsing + cascade. See css.h. */
#include "css.h"

/* --- string helpers ------------------------------------------------------ */
static char c_lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static int  c_ieq(const char *a, const char *b)
{
    int i = 0;
    while (a[i] && b[i]) { if (c_lc(a[i]) != c_lc(b[i])) return 0; i++; }
    return c_lc(a[i]) == c_lc(b[i]);
}
static int  c_ws(char ch) { return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f'; }
static int  c_neqn(const char *a, const char *b, int n)
{
    for (int i = 0; i < n; i++) if (c_lc(a[i]) != c_lc(b[i])) return 0;
    return 1;
}

static int hexval(char c)
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
        int n = 0; while (hexval(v[1 + n]) >= 0) n++;
        if (n >= 6) { *out = fb_rgb(hexval(v[1])*16+hexval(v[2]), hexval(v[3])*16+hexval(v[4]), hexval(v[5])*16+hexval(v[6])); return 1; }
        if (n >= 3) { *out = fb_rgb(hexval(v[1])*17, hexval(v[2])*17, hexval(v[3])*17); return 1; }
        return 0;
    }
    if (c_neqn(v, "rgb", 3)) {
        const char *p = v;
        while (*p && *p != '(') p++;
        if (*p == '(') {
            int comp[3] = {0,0,0}, ci = 0; p++;
            while (*p && *p != ')' && ci < 3) {
                while (*p == ' ' || *p == ',') p++;
                int val = 0, got = 0;
                while (*p >= '0' && *p <= '9') { val = val*10 + (*p - '0'); p++; got = 1; }
                if (got) comp[ci++] = val;
                while (*p && *p != ',' && *p != ')') p++;
            }
            *out = fb_rgb(comp[0] & 0xFF, comp[1] & 0xFF, comp[2] & 0xFF);
            return 1;
        }
        return 0;
    }
    static const struct { const char *n; uint8_t r, g, b; } names[] = {
        {"black",0,0,0},{"white",255,255,255},{"red",200,32,32},{"green",24,150,60},
        {"blue",40,90,210},{"navy",16,32,90},{"gray",128,128,128},{"grey",128,128,128},
        {"silver",192,192,192},{"orange",230,130,30},{"purple",130,50,160},{"teal",20,130,130},
        {"maroon",140,30,30},{"olive",120,120,20},{"yellow",210,190,20},{"lime",60,190,60},
        {"aqua",40,190,200},{"fuchsia",210,50,180},{"pink",235,140,170},{"brown",140,80,40},
        {"gold",210,170,30},{"darkgray",100,100,100},{"darkgrey",100,100,100},
        {"lightgray",215,215,215},{"lightgrey",215,215,215},{"whitesmoke",245,245,245},
        {"transparent",0,0,0},
    };
    for (int i = 0; i < (int)(sizeof(names)/sizeof(names[0])); i++)
        if (c_ieq(v, names[i].n)) {
            if (c_ieq(v, "transparent")) return 0;
            *out = fb_rgb(names[i].r, names[i].g, names[i].b); return 1;
        }
    return 0;
}

/* --- rule storage -------------------------------------------------------- */
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

static int specificity(const char *s, int len)
{
    int spec = 0, i = 0;
    while (i < len) {
        char ch = s[i];
        if (ch == '#') { spec += 100; i++; while (i < len && s[i] != '.' && s[i] != '#' && !c_ws(s[i])) i++; }
        else if (ch == '.') { spec += 10; i++; while (i < len && s[i] != '.' && s[i] != '#' && !c_ws(s[i])) i++; }
        else if (ch == ':') { i++; while (i < len && s[i] != '.' && s[i] != '#' && !c_ws(s[i])) i++; }
        else if (!c_ws(ch) && ch != '*' && ch != '>' && ch != '+' && ch != '~') { spec += 1; i++; while (i < len && s[i] != '.' && s[i] != '#' && s[i] != ':' && !c_ws(s[i])) i++; }
        else i++;
    }
    return spec;
}

void css_add_stylesheet(const char *text, int len)
{
    int i = 0;
    while (i < len) {
        /* skip whitespace + comments */
        while (i < len && c_ws(text[i])) i++;
        if (i + 1 < len && text[i] == '/' && text[i+1] == '*') {
            i += 2;
            while (i + 1 < len && !(text[i] == '*' && text[i+1] == '/')) i++;
            i += 2;
            continue;
        }
        if (i >= len) break;
        /* @media / @import / @font-face etc: skip the at-rule */
        if (text[i] == '@') {
            int depth = 0;
            while (i < len) {
                if (text[i] == '{') { depth++; i++; if (depth) { /* enter block */ }
                    /* For @media, recurse into block contents by NOT skipping. */
                    break;
                }
                if (text[i] == ';') { i++; break; }
                i++;
            }
            /* For simplicity treat @media block contents as normal rules: fallthrough. */
            continue;
        }

        int sel_start = i;
        while (i < len && text[i] != '{' && text[i] != '}') i++;
        if (i >= len || text[i] != '{') { if (i < len) i++; continue; }
        int sel_end = i;
        i++; /* past { */
        int decl_start = i;
        while (i < len && text[i] != '}') i++;
        int decl_end = i;
        if (i < len) i++; /* past } */

        /* copy decl block into the css arena once */
        int dlen = decl_end - decl_start;
        if (g_csslen + dlen + 1 > CSS_TEXT_CAP) continue;
        int doff = g_csslen;
        for (int z = 0; z < dlen; z++) g_css[doff + z] = text[decl_start + z];
        g_css[doff + dlen] = 0;
        g_csslen += dlen + 1;

        /* split the selector list on commas; one rule per selector */
        int s = sel_start;
        while (s < sel_end) {
            while (s < sel_end && (c_ws(text[s]) || text[s] == ',')) s++;
            int e = s;
            while (e < sel_end && text[e] != ',') e++;
            int t = e; while (t > s && c_ws(text[t-1])) t--;
            int slen = t - s;
            if (slen > 0 && g_nrules < CSS_MAX_RULES && g_csslen + slen + 1 <= CSS_TEXT_CAP) {
                int soff = g_csslen;
                for (int z = 0; z < slen; z++) g_css[soff + z] = text[s + z];
                g_css[soff + slen] = 0;
                g_csslen += slen + 1;
                CssRule *r = &g_rules[g_nrules++];
                r->sel_off = soff; r->sel_len = slen;
                r->decl_off = doff; r->decl_len = dlen;
                r->spec = specificity(g_css + soff, slen);
                r->order = g_order++;
            }
            s = e;
        }
    }
}

/* --- selector matching --------------------------------------------------- */
/* Does a single compound selector (no combinators) match node? */
static int match_simple(const DomNode *node, const char *sel, int len)
{
    if (!node || node->type != DOM_NODE_ELEMENT) return 0;
    int i = 0;
    /* type */
    if (i < len && sel[i] != '.' && sel[i] != '#' && sel[i] != ':' && sel[i] != '*' && sel[i] != '[') {
        char tag[16]; int tl = 0;
        while (i < len && sel[i] != '.' && sel[i] != '#' && sel[i] != ':' && sel[i] != '[' && tl < 15)
            tag[tl++] = sel[i++];
        tag[tl] = 0;
        if (!c_ieq(node->tag, tag)) return 0;
    } else if (i < len && sel[i] == '*') {
        i++;
    }
    /* #id and .class parts */
    const char *cls = dom_get_attr(node, "class");
    const char *id  = dom_get_attr(node, "id");
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
            /* is `want` one of the space-separated classes? */
            int found = 0, p = 0;
            while (cls[p]) {
                while (cls[p] == ' ') p++;
                int q = p; while (cls[q] && cls[q] != ' ') q++;
                int m = (q - p == wl);
                for (int z = 0; m && z < wl; z++) if (cls[p + z] != want[z]) m = 0;
                if (m) { found = 1; break; }
                p = q;
            }
            if (!found) return 0;
        } else if (ch == ':') {
            i++; while (i < len && sel[i] != '.' && sel[i] != '#') i++;   /* ignore pseudo */
        } else if (ch == '[') {
            while (i < len && sel[i] != ']') i++;                          /* ignore attr sel */
            if (i < len) i++;
        } else {
            i++;
        }
    }
    return 1;
}

/* Full selector with descendant combinators (spaces). Rightmost matches node;
 * the rest must match successive ancestors (in any positions). */
static int match_selector(const DomNode *node, const char *sel, int len)
{
    /* split into simple-selector tokens from right to left */
    int starts[16], lens[16], nt = 0;
    int i = 0;
    while (i < len && nt < 16) {
        while (i < len && (c_ws(sel[i]) || sel[i] == '>' || sel[i] == '+' || sel[i] == '~')) i++;
        if (i >= len) break;
        int s = i;
        while (i < len && !c_ws(sel[i])) i++;
        starts[nt] = s; lens[nt] = i - s; nt++;
    }
    if (nt == 0) return 0;
    /* rightmost */
    if (!match_simple(node, sel + starts[nt-1], lens[nt-1])) return 0;
    /* ancestors for the remaining (right to left) */
    const DomNode *cur = node->parent;
    for (int t = nt - 2; t >= 0; t--) {
        int matched = 0;
        while (cur) {
            if (match_simple(cur, sel + starts[t], lens[t])) { matched = 1; cur = cur->parent; break; }
            cur = cur->parent;
        }
        if (!matched) return 0;
    }
    return 1;
}

/* --- declaration application -------------------------------------------- */
static int parse_px(const char *v)
{
    while (*v == ' ') v++;
    int sign = 1; if (*v == '-') { sign = -1; v++; }
    int n = 0, got = 0;
    while (*v >= '0' && *v <= '9') { n = n*10 + (*v - '0'); v++; got = 1; }
    /* em ~ 16px */
    while (*v == ' ') v++;
    if (v[0] == 'e' && v[1] == 'm') n *= 16;
    return got ? sign * n : 0;
}

static void set_font_size(ComputedStyle *out, const char *v)
{
    while (*v == ' ') v++;
    int px = 0;
    if (c_ieq(v, "small") || c_ieq(v, "x-small") || c_ieq(v, "smaller")) px = 13;
    else if (c_ieq(v, "medium")) px = 16;
    else if (c_ieq(v, "large") || c_ieq(v, "larger")) px = 22;
    else if (c_ieq(v, "x-large")) px = 28;
    else if (c_ieq(v, "xx-large")) px = 34;
    else px = parse_px(v);
    if (px <= 0) return;
    out->scale  = px >= 22 ? 2 : 1;
    out->line_h = px >= 22 ? (px + 8) : 19;
}

static void apply_decl(ComputedStyle *out, const char *prop, const char *val)
{
    color_t c;
    if (c_ieq(prop, "color")) { if (css_parse_color(val, &c)) out->color = c; }
    else if (c_ieq(prop, "background-color") || c_ieq(prop, "background")) {
        if (css_parse_color(val, &c)) { out->bg = c; out->has_bg = 1; }
    }
    else if (c_ieq(prop, "font-weight")) {
        out->bold = (c_ieq(val, "bold") || c_ieq(val, "bolder") ||
                     c_ieq(val, "700") || c_ieq(val, "800") || c_ieq(val, "900")) ? 1 : 0;
    }
    else if (c_ieq(prop, "font-style")) out->italic = (c_ieq(val, "italic") || c_ieq(val, "oblique"));
    else if (c_ieq(prop, "font-size")) set_font_size(out, val);
    else if (c_ieq(prop, "text-align")) {
        out->align = c_ieq(val, "center") ? ALIGN_CENTER : c_ieq(val, "right") ? ALIGN_RIGHT : ALIGN_LEFT;
    }
    else if (c_ieq(prop, "text-decoration") || c_ieq(prop, "text-decoration-line")) {
        out->underline = c_ieq(val, "underline") ? 1 : 0;
    }
    else if (c_ieq(prop, "display")) {
        if (c_ieq(val, "none")) out->display = DISP_NONE;
        else if (c_ieq(val, "inline") || c_ieq(val, "inline-block")) out->display = DISP_INLINE;
        else if (c_ieq(val, "list-item")) out->display = DISP_LIST_ITEM;
        else out->display = DISP_BLOCK;
    }
    else if (c_ieq(prop, "margin")) { int m = parse_px(val); out->margin_top = out->margin_bottom = m < 0 ? 0 : m; }
    else if (c_ieq(prop, "margin-top")) { int m = parse_px(val); out->margin_top = m < 0 ? 0 : m; }
    else if (c_ieq(prop, "margin-bottom")) { int m = parse_px(val); out->margin_bottom = m < 0 ? 0 : m; }
    else if (c_ieq(prop, "padding-left") || c_ieq(prop, "padding")) { int m = parse_px(val); if (m > 0) out->indent += m; }
}

static void apply_block(ComputedStyle *out, const char *decl, int len)
{
    int i = 0;
    char prop[48], val[256];
    while (i < len) {
        while (i < len && (c_ws(decl[i]) || decl[i] == ';')) i++;
        int pl = 0;
        while (i < len && decl[i] != ':' && decl[i] != ';' && decl[i] != '}' && pl < 47) prop[pl++] = decl[i++];
        prop[pl] = 0;
        /* trim trailing ws on prop */
        while (pl > 0 && c_ws(prop[pl-1])) prop[--pl] = 0;
        if (i >= len || decl[i] != ':') { while (i < len && decl[i] != ';') i++; continue; }
        i++; /* past : */
        while (i < len && c_ws(decl[i])) i++;
        int vl = 0;
        while (i < len && decl[i] != ';' && decl[i] != '}' && vl < 255) val[vl++] = decl[i++];
        while (vl > 0 && c_ws(val[vl-1])) vl--;
        val[vl] = 0;
        /* strip !important */
        for (int z = 0; z < vl - 1; z++) if (val[z] == '!') { val[z] = 0; while (z > 0 && c_ws(val[z-1])) val[--z] = 0; break; }
        if (prop[0] && val[0]) apply_decl(out, prop, val);
    }
}

/* --- user-agent defaults ------------------------------------------------- */
static int is_block_tag(const char *t)
{
    static const char *b[] = { "p","div","section","article","header","footer","nav","main",
        "aside","ul","ol","li","dl","dt","dd","table","tr","form","blockquote","figure",
        "figcaption","h1","h2","h3","h4","h5","h6","fieldset","address","pre","hr","body",
        "html","center","details","summary","caption",0 };
    for (int i = 0; b[i]; i++) if (c_ieq(t, b[i])) return 1;
    return 0;
}

static void ua_defaults(const DomNode *n, ComputedStyle *out)
{
    const char *t = n->tag;

    /* not-displayed metadata containers */
    if (c_ieq(t,"head")||c_ieq(t,"title")||c_ieq(t,"meta")||c_ieq(t,"link")||
        c_ieq(t,"script")||c_ieq(t,"style")||c_ieq(t,"base")||c_ieq(t,"#r")) {
        out->display = DISP_NONE;
        return;
    }

    out->display = is_block_tag(t) ? DISP_BLOCK : DISP_INLINE;

    if (t[0] == 'h' && t[1] >= '1' && t[1] <= '6' && t[2] == 0) {
        out->bold = 1;
        int lv = t[1] - '0';
        out->scale  = lv <= 2 ? 2 : 1;
        out->line_h = lv <= 2 ? 30 : 22;
        out->margin_top = lv <= 2 ? 16 : 12;
        out->margin_bottom = lv <= 2 ? 10 : 6;
        out->color = fb_rgb(18, 42, 96);
    }
    else if (c_ieq(t,"p"))          { out->margin_top = 2; out->margin_bottom = 10; }
    else if (c_ieq(t,"ul")||c_ieq(t,"ol")) { out->margin_top = 6; out->margin_bottom = 6; out->indent = 24; }
    else if (c_ieq(t,"li"))         { out->display = DISP_LIST_ITEM; }
    else if (c_ieq(t,"blockquote")) { out->margin_top = 8; out->margin_bottom = 8; out->indent = 24; }
    else if (c_ieq(t,"pre"))        { out->margin_top = 8; out->margin_bottom = 8; out->color = fb_rgb(70,80,96); }
    else if (c_ieq(t,"a"))          { out->is_link = 1; out->underline = 1; out->color = fb_rgb(26, 90, 210); }
    else if (c_ieq(t,"b")||c_ieq(t,"strong")||c_ieq(t,"th")) { out->bold = 1; }
    else if (c_ieq(t,"i")||c_ieq(t,"em")||c_ieq(t,"cite")||c_ieq(t,"var")) { out->italic = 1; }
    else if (c_ieq(t,"u")||c_ieq(t,"ins"))   { out->underline = 1; }
    else if (c_ieq(t,"code")||c_ieq(t,"tt")||c_ieq(t,"kbd")||c_ieq(t,"samp")) { out->color = fb_rgb(70,80,96); }
    else if (c_ieq(t,"small"))      { out->scale = 1; }
    else if (c_ieq(t,"center"))     { out->align = ALIGN_CENTER; }
    else if (c_ieq(t,"div")||c_ieq(t,"section")||c_ieq(t,"article")||c_ieq(t,"header")||
             c_ieq(t,"footer")||c_ieq(t,"nav")||c_ieq(t,"main")||c_ieq(t,"aside")) {
        out->margin_top = 0; out->margin_bottom = 0;
    }
}

/* --- the cascade --------------------------------------------------------- */
void css_cascade(const DomNode *node, const ComputedStyle *parent, ComputedStyle *out)
{
    /* inherited */
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
    /* non-inherited reset */
    out->underline = 0;
    out->is_link = 0;
    out->display = DISP_INLINE;
    out->has_bg = 0;
    out->bg = 0;
    out->margin_top = 0;
    out->margin_bottom = 0;
    out->indent = 0;

    if (!node || node->type != DOM_NODE_ELEMENT) { out->display = DISP_INLINE; return; }

    ua_defaults(node, out);
    if (out->display == DISP_NONE) return;

    /* author rules, applied in (specificity, source-order) order so the
     * winning declaration is applied last. Simple insertion-ranked pass. */
    int applied;
    int last_spec = -1, last_order = -1;
    do {
        applied = 0;
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
        if (best >= 0) {
            CssRule *r = &g_rules[best];
            apply_block(out, g_css + r->decl_off, r->decl_len);
            last_spec = r->spec; last_order = r->order;
            applied = 1;
        }
    } while (applied);

    /* inline style attribute wins over author rules */
    const char *inl = dom_get_attr(node, "style");
    if (inl[0]) {
        int l = 0; while (inl[l]) l++;
        apply_block(out, inl, l);
    }

    /* keep line height sensible for the chosen scale */
    if (out->line_h < 16) out->line_h = out->scale == 2 ? 30 : 19;
}
