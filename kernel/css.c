#include "css.h"

static char to_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static int str_ieq(const char *a, const char *b)
{
    int i = 0;
    while (a[i] && b[i]) { if (to_lower(a[i]) != to_lower(b[i])) return 0; i++; }
    return to_lower(a[i]) == to_lower(b[i]);
}

/* n-byte case-insensitive compare where a is not NUL-terminated */
static int mem_ieq(const char *a, int alen, const char *b)
{
    int i = 0;
    for (; i < alen; i++) { if (!b[i] || to_lower(a[i]) != to_lower(b[i])) return 0; }
    return b[i] == 0;
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

/* hsl() -> rgb, integer math. h in degrees, s/l in percent. */
static void hsl_to_rgb(int h, int s, int l, int *r, int *g, int *b)
{
    h = ((h % 360) + 360) % 360;
    if (s < 0) s = 0;
    if (s > 100) s = 100;
    if (l < 0) l = 0;
    if (l > 100) l = 100;
    /* scaled by 1000 to keep the intermediate math in ints */
    int c = (255 * s * (100 - (l >= 50 ? (2*l - 100) : (100 - 2*l)))) / 10000;
    int region = h / 60;
    int rem = h % 60;
    int x = (c * (60 - (region & 1 ? rem : 60 - rem))) / 60;
    int m = (255 * l) / 100 - c / 2;
    int rr = 0, gg = 0, bb = 0;
    switch (region) {
        case 0: rr = c; gg = x; break;
        case 1: rr = x; gg = c; break;
        case 2: gg = c; bb = x; break;
        case 3: gg = x; bb = c; break;
        case 4: rr = x; bb = c; break;
        default: rr = c; bb = x; break;
    }
    rr += m; gg += m; bb += m;
    *r = rr < 0 ? 0 : rr > 255 ? 255 : rr;
    *g = gg < 0 ? 0 : gg > 255 ? 255 : gg;
    *b = bb < 0 ? 0 : bb > 255 ? 255 : bb;
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

    if (prefix_ieq(v, "rgb", 3) || prefix_ieq(v, "hsl", 3)) {
        int is_hsl = prefix_ieq(v, "hsl", 3);
        const char *p = v;
        while (*p && *p != '(') p++;
        if (*p != '(') return 0;
        int comp[4] = {0,0,0,255}, ci = 0;
        p++;
        while (*p && *p != ')' && ci < 4) {
            while (*p == ' ' || *p == ',' || *p == '/') p++;
            int neg = 0;
            if (*p == '-') { neg = 1; p++; }
            int val = 0, saw_digit = 0;
            while (*p >= '0' && *p <= '9') { val = val*10 + (*p - '0'); p++; saw_digit = 1; }
            if (*p == '.') { p++; while (*p >= '0' && *p <= '9') p++; }
            if (saw_digit) comp[ci++] = neg ? -val : val;
            while (*p && *p != ',' && *p != ')' && *p != ' ' && *p != '/') p++;
        }
        /* rgba/hsla with alpha 0 is invisible: report "no color" rather than
         * painting an opaque black rectangle over the content. */
        if (ci >= 4 && comp[3] == 0) return 0;
        if (is_hsl) {
            int r, g, b;
            hsl_to_rgb(comp[0], comp[1], comp[2], &r, &g, &b);
            *out = fb_rgb((uint8_t)r, (uint8_t)g, (uint8_t)b);
        } else {
            *out = fb_rgb(comp[0] & 0xFF, comp[1] & 0xFF, comp[2] & 0xFF);
        }
        return 1;
    }

    static const struct { const char *n; uint8_t r, g, b; } named[] = {
        {"black",0,0,0},{"white",255,255,255},{"red",200,32,32},{"green",24,150,60},
        {"blue",40,90,210},{"navy",16,32,90},{"gray",128,128,128},{"grey",128,128,128},
        {"silver",192,192,192},{"orange",230,130,30},{"purple",130,50,160},{"teal",20,130,130},
        {"maroon",140,30,30},{"olive",120,120,20},{"yellow",210,190,20},{"lime",60,190,60},
        {"aqua",40,190,200},{"cyan",40,190,200},{"magenta",210,50,180},{"fuchsia",210,50,180},
        {"pink",235,140,170},{"brown",140,80,40},{"gold",210,170,30},
        {"darkgray",100,100,100},{"darkgrey",100,100,100},{"dimgray",105,105,105},
        {"lightgray",215,215,215},{"lightgrey",215,215,215},{"whitesmoke",245,245,245},
        {"gainsboro",220,220,220},{"beige",245,245,220},{"ivory",255,255,240},
        {"lightblue",173,216,230},{"skyblue",135,206,235},{"steelblue",70,130,180},
        {"royalblue",65,105,225},{"dodgerblue",30,144,255},{"midnightblue",25,25,112},
        {"darkblue",0,0,139},{"cornflowerblue",100,149,237},{"lavender",230,230,250},
        {"darkgreen",0,100,0},{"forestgreen",34,139,34},{"seagreen",46,139,87},
        {"limegreen",50,205,50},{"olivedrab",107,142,35},{"darkolivegreen",85,107,47},
        {"crimson",220,20,60},{"firebrick",178,34,34},{"darkred",139,0,0},
        {"tomato",255,99,71},{"coral",255,127,80},{"salmon",250,128,114},
        {"orangered",255,69,0},{"darkorange",255,140,0},{"goldenrod",218,165,32},
        {"khaki",240,230,140},{"tan",210,180,140},{"peru",205,133,63},
        {"chocolate",210,105,30},{"sienna",160,82,45},{"wheat",245,222,179},
        {"indigo",75,0,130},{"violet",238,130,238},{"orchid",218,112,214},
        {"plum",221,160,221},{"thistle",216,191,216},{"slateblue",106,90,205},
        {"slategray",112,128,144},{"slategrey",112,128,144},{"lightslategray",119,136,153},
        {"turquoise",64,224,208},{"darkcyan",0,139,139},{"cadetblue",95,158,160},
        {"azure",240,255,255},{"aliceblue",240,248,255},{"snow",255,250,250},
        {"linen",250,240,230},{"seashell",255,245,238},{"honeydew",240,255,240},
        {"transparent",0,0,0},{"currentcolor",0,0,0},{"inherit",0,0,0},{"initial",0,0,0},
    };
    for (int i = 0; i < (int)(sizeof(named)/sizeof(named[0])); i++) {
        if (!str_ieq(v, named[i].n)) continue;
        /* these mean "leave whatever was inherited alone", not a real color */
        if (str_ieq(v, "transparent") || str_ieq(v, "currentcolor") ||
            str_ieq(v, "inherit") || str_ieq(v, "initial")) return 0;
        *out = fb_rgb(named[i].r, named[i].g, named[i].b);
        return 1;
    }
    return 0;
}

#define CSS_TEXT_CAP  (320 * 1024)
#define CSS_MAX_RULES 8000

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
static short   g_sorted[CSS_MAX_RULES];
static int     g_sorted_n;                /* -1 when the index needs rebuild */

void css_reset(void)
{
    g_csslen = 0;
    g_nrules = 0;
    g_order  = 0;
    g_sorted_n = -1;
}

/* CSS specificity, collapsed to a single int: id=100, class/attr/pseudo-class=10,
 * type=1. Good enough since we never need to distinguish 2 ids from 3. */
static int specificity(const char *s, int len)
{
    int spec = 0, i = 0;
    while (i < len) {
        char ch = s[i];
        if (ch == '#') { spec += 100; i++; while (i < len && s[i] != '.' && s[i] != '#' && s[i] != ':' && s[i] != '[' && !is_space(s[i])) i++; }
        else if (ch == '.') { spec += 10; i++; while (i < len && s[i] != '.' && s[i] != '#' && s[i] != ':' && s[i] != '[' && !is_space(s[i])) i++; }
        else if (ch == '[') { spec += 10; while (i < len && s[i] != ']') i++; if (i < len) i++; }
        else if (ch == ':') {
            /* ::before and friends are element selectors (1), :hover is a
             * class-level one (10) */
            int dbl = (i + 1 < len && s[i+1] == ':');
            spec += dbl ? 1 : 10;
            i += dbl ? 2 : 1;
            while (i < len && s[i] != '.' && s[i] != '#' && s[i] != ':' && s[i] != '[' && !is_space(s[i])) i++;
        }
        else if (!is_space(ch) && ch != '*' && ch != '>' && ch != '+' && ch != '~' && ch != ',') {
            spec += 1; i++;
            while (i < len && s[i] != '.' && s[i] != '#' && s[i] != ':' && s[i] != '[' && !is_space(s[i])) i++;
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

/* Does this at-rule's prelude apply to us? We render one screen-ish
 * viewport, so "screen" and width/min-width queries apply and "print" does
 * not - the common case that used to leak print styles into the page. */
static int media_applies(const char *s, int len)
{
    int has_print = 0, has_screen = 0;
    for (int i = 0; i + 4 < len; i++) {
        if (mem_ieq(s + i, 5, "print")) has_print = 1;
        if (i + 5 < len && mem_ieq(s + i, 6, "screen")) has_screen = 1;
    }
    if (has_print && !has_screen) return 0;
    return 1;
}

/* Skips the balanced {...} block starting at text[i] (which must be '{').
 * Returns the index just past the closing brace. */
static int skip_block(const char *text, int len, int i)
{
    int depth = 0;
    while (i < len) {
        if (text[i] == '{') depth++;
        else if (text[i] == '}') { depth--; i++; if (depth <= 0) return i; continue; }
        i++;
    }
    return i;
}

static void add_rules(const char *text, int sel_start, int sel_end, int decl_off, int decl_len)
{
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

static void parse_rules(const char *text, int len, int depth);

/* An at-rule: @media/@supports wrap a nested rule list we recurse into,
 * everything else (@import, @font-face, @keyframes, @charset) is skipped
 * whole rather than having its inner declarations leak out as page rules. */
static int handle_at_rule(const char *text, int len, int i, int depth)
{
    int name_start = i + 1;
    int name_end = name_start;
    while (name_end < len && !is_space(text[name_end]) && text[name_end] != '{' && text[name_end] != ';') name_end++;

    int prelude_start = name_end;
    int j = name_end;
    while (j < len && text[j] != '{' && text[j] != ';') j++;

    int nested = mem_ieq(text + name_start, name_end - name_start, "media") ||
                 mem_ieq(text + name_start, name_end - name_start, "supports") ||
                 mem_ieq(text + name_start, name_end - name_start, "layer");

    if (j >= len) return len;
    if (text[j] == ';') return j + 1;

    int block_end = skip_block(text, len, j);
    if (!nested || depth >= 3) return block_end;
    if (!media_applies(text + prelude_start, j - prelude_start)) return block_end;

    /* recurse into the block's contents (block_end-1 is the closing brace) */
    parse_rules(text + j + 1, block_end - j - 2 > 0 ? block_end - j - 2 : 0, depth + 1);
    return block_end;
}

static void parse_rules(const char *text, int len, int depth)
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

        if (text[i] == '@') { i = handle_at_rule(text, len, i, depth); continue; }
        if (text[i] == '}') { i++; continue; }

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

        add_rules(text, sel_start, sel_end, decl_off, decl_len);
    }
}

void css_add_stylesheet(const char *text, int len)
{
    parse_rules(text, len, 0);
    g_sorted_n = -1;   /* new rules: the cascade index is stale */
}

/* nth-child style position of an element among its element siblings (1-based),
 * plus whether it is the last one. */
static void child_position(const DomNode *node, int *index, int *is_last)
{
    *index = 1; *is_last = 1;
    if (!node || !node->parent) return;
    int seen = 0, found = 0;
    for (DomNode *c = node->parent->first_child; c; c = c->next_sibling) {
        if (c->type != DOM_NODE_ELEMENT) continue;
        seen++;
        if (c == node) { *index = seen; found = 1; }
        else if (found) { *is_last = 0; return; }
    }
}

/* Does one class name (want, wlen bytes) appear in a space-separated list? */
static int has_class(const char *classes, const char *want, int wlen)
{
    int p = 0;
    while (classes[p]) {
        while (classes[p] == ' ') p++;
        int q = p; while (classes[q] && classes[q] != ' ') q++;
        if (q - p == wlen) {
            int same = 1;
            for (int z = 0; z < wlen; z++) if (classes[p + z] != want[z]) { same = 0; break; }
            if (same) return 1;
        }
        if (q == p) break;
        p = q;
    }
    return 0;
}

/* [attr], [attr=v], [attr~=v], [attr^=v], [attr$=v], [attr*=v], [attr|=v].
 * `sel` points just past '[', len covers up to (not including) ']'. */
static int match_attr(const DomNode *node, const char *sel, int len)
{
    char name[48]; int nl = 0;
    int i = 0;
    while (i < len && sel[i] != '=' && sel[i] != '~' && sel[i] != '^' && sel[i] != '$' &&
           sel[i] != '*' && sel[i] != '|' && !is_space(sel[i]) && nl < 47) name[nl++] = sel[i++];
    name[nl] = 0;
    if (!nl) return 0;
    if (!dom_has_attr(node, name)) return 0;

    while (i < len && is_space(sel[i])) i++;
    if (i >= len) return 1;

    char op = sel[i];
    if (op == '=') i++;
    else if (i + 1 < len && sel[i+1] == '=') i += 2;
    else return 1;

    while (i < len && is_space(sel[i])) i++;
    char q = 0;
    if (i < len && (sel[i] == '"' || sel[i] == '\'')) { q = sel[i]; i++; }
    char want[96]; int wl = 0;
    while (i < len && wl < 95 && (q ? sel[i] != q : (!is_space(sel[i]) && sel[i] != ']'))) want[wl++] = sel[i++];
    want[wl] = 0;

    const char *val = dom_get_attr(node, name);
    int vl = 0; while (val[vl]) vl++;

    switch (op) {
        case '=': return mem_ieq(val, vl, want);
        case '~': return has_class(val, want, wl);
        case '^': { if (wl > vl) return 0; for (int z = 0; z < wl; z++) if (val[z] != want[z]) return 0; return 1; }
        case '$': { if (wl > vl) return 0; for (int z = 0; z < wl; z++) if (val[vl-wl+z] != want[z]) return 0; return 1; }
        case '|': { if (wl > vl) return 0; for (int z = 0; z < wl; z++) if (val[z] != want[z]) return 0; return val[wl] == 0 || val[wl] == '-'; }
        case '*':
        default:  { if (!wl) return 1; for (int s = 0; s + wl <= vl; s++) { int m = 1; for (int z = 0; z < wl; z++) if (val[s+z] != want[z]) { m = 0; break; } if (m) return 1; } return 0; }
    }
}

/* Matches one compound selector (type + #id + .class + [attr] + :pseudo,
 * no combinators). */
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
            while (i < len && sel[i] != '.' && sel[i] != '#' && sel[i] != ':' && sel[i] != '[' && wl < 63) want[wl++] = sel[i++];
            want[wl] = 0;
            int j = 0; while (id[j] && want[j] && id[j] == want[j]) j++;
            if (id[j] || want[j]) return 0;
        } else if (ch == '.') {
            i++; char want[64]; int wl = 0;
            while (i < len && sel[i] != '.' && sel[i] != '#' && sel[i] != ':' && sel[i] != '[' && wl < 63) want[wl++] = sel[i++];
            want[wl] = 0;
            if (!has_class(classes, want, wl)) return 0;
        } else if (ch == '[') {
            i++;
            int start = i;
            while (i < len && sel[i] != ']') i++;
            if (!match_attr(node, sel + start, i - start)) return 0;
            if (i < len) i++;
        } else if (ch == ':') {
            i++;
            if (i < len && sel[i] == ':') { i++; return 0; }
            int start = i;
            while (i < len && sel[i] != '.' && sel[i] != '#' && sel[i] != ':' && sel[i] != '[') i++;
            int plen = i - start;
            const char *p = sel + start;
            /* structural pseudo-classes we can answer honestly; the
             * interaction ones (:hover/:focus/:active) never match since
             * nothing here tracks pointer or focus state, and matching them
             * unconditionally would paint every link in its hover style. */
            if (mem_ieq(p, plen, "first-child")) { int idx, last; child_position(node, &idx, &last); if (idx != 1) return 0; }
            else if (mem_ieq(p, plen, "last-child")) { int idx, last; child_position(node, &idx, &last); if (!last) return 0; }
            else if (mem_ieq(p, plen, "only-child")) { int idx, last; child_position(node, &idx, &last); if (idx != 1 || !last) return 0; }
            else if (mem_ieq(p, plen, "root")) { if (node->parent && node->parent->parent) return 0; }
            else if (mem_ieq(p, plen, "checked")) { if (!dom_has_attr(node, "checked")) return 0; }
            else if (mem_ieq(p, plen, "disabled")) { if (!dom_has_attr(node, "disabled")) return 0; }
            else if (mem_ieq(p, plen, "empty")) { if (node->first_child) return 0; }
            else if (mem_ieq(p, plen, "link") || mem_ieq(p, plen, "any-link")) { if (!dom_has_attr(node, "href")) return 0; }
            else if (plen >= 9 && mem_ieq(p, 9, "nth-child")) {

                int idx, last; child_position(node, &idx, &last);
                int q = 9; while (q < plen && p[q] != '(') q++;
                q++;
                while (q < plen && is_space(p[q])) q++;
                if (mem_ieq(p + q, 3, "odd") || (q + 2 < plen && to_lower(p[q]) == 'o')) { if (!(idx & 1)) return 0; }
                else if (q + 3 < plen && to_lower(p[q]) == 'e') { if (idx & 1) return 0; }
                else { int want = 0, saw = 0; while (q < plen && p[q] >= '0' && p[q] <= '9') { want = want*10 + (p[q]-'0'); q++; saw = 1; } if (saw && idx != want) return 0; }
            }
            else return 0;
        } else {
            i++;
        }
    }
    return 1;
}

/* Full selector with real combinators. The selector is split right-to-left
 * into compounds plus the combinator that precedes each one; the rightmost
 * compound must match `node`, then we walk leftward:
 *   ' ' any ancestor · '>' the direct parent
 *   '+' the immediately preceding element sibling · '~' any preceding sibling
 * Descendant and general-sibling are "try each candidate"; since we don't
 * backtrack across compounds, a pathological selector can miss a match that
 * a real engine finds. That costs a style, never a crash. */
#define SEL_MAX 12

static const DomNode *prev_element_sibling(const DomNode *n)
{
    if (!n || !n->parent) return 0;
    const DomNode *prev = 0;
    for (DomNode *c = n->parent->first_child; c; c = c->next_sibling) {
        if (c == n) return prev;
        if (c->type == DOM_NODE_ELEMENT) prev = c;
    }
    return 0;
}

static int match_selector(const DomNode *node, const char *sel, int len)
{
    int starts[SEL_MAX], lens[SEL_MAX], combi[SEL_MAX], count = 0;
    int i = 0;
    int pending = ' ';
    while (i < len && count < SEL_MAX) {
        while (i < len && is_space(sel[i])) i++;
        while (i < len && (sel[i] == '>' || sel[i] == '+' || sel[i] == '~')) {
            pending = sel[i]; i++;
            while (i < len && is_space(sel[i])) i++;
        }
        if (i >= len) break;
        int s = i;
        while (i < len && !is_space(sel[i]) && sel[i] != '>' && sel[i] != '+' && sel[i] != '~') {
            if (sel[i] == '[') { while (i < len && sel[i] != ']') i++; }
            i++;
        }
        starts[count] = s; lens[count] = i - s; combi[count] = pending; count++;
        pending = ' ';
    }
    if (count == 0) return 0;

    if (!match_simple(node, sel + starts[count-1], lens[count-1])) return 0;

    const DomNode *cur = node;
    for (int t = count - 2; t >= 0; t--) {
        int comb = combi[t + 1];
        const char *cs = sel + starts[t];
        int cl = lens[t];
        if (comb == '>') {
            cur = cur->parent;
            if (!cur || !match_simple(cur, cs, cl)) return 0;
        } else if (comb == '+') {
            cur = prev_element_sibling(cur);
            if (!cur || !match_simple(cur, cs, cl)) return 0;
        } else if (comb == '~') {
            const DomNode *p = prev_element_sibling(cur);
            while (p && !match_simple(p, cs, cl)) p = prev_element_sibling(p);
            if (!p) return 0;
            cur = p;
        } else {
            const DomNode *p = cur->parent;
            while (p && !match_simple(p, cs, cl)) p = p->parent;
            if (!p) return 0;
            cur = p;
        }
    }
    return 1;
}

static int parse_length_px(const char *v)
{
    while (*v == ' ') v++;
    int sign = 1;
    if (*v == '-') { sign = -1; v++; }
    int n = 0, saw_digit = 0;
    while (*v >= '0' && *v <= '9') { n = n*10 + (*v - '0'); v++; saw_digit = 1; }
    if (*v == '.') { v++; while (*v >= '0' && *v <= '9') v++; }
    while (*v == ' ') v++;
    if (v[0] == 'e' && v[1] == 'm') n *= 16;
    else if (v[0] == 'r' && v[1] == 'e' && v[2] == 'm') n *= 16;
    else if (v[0] == 'p' && v[1] == 't') n = n * 4 / 3;
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
    else {
        px = parse_length_px(v);
        /* A percentage is relative to the inherited size, not a pixel count:
         * reading "107.1%" as 107px is how a search results page ended up
         * rendered at double size with 115px line spacing. */
        int is_pct = 0;
        for (int i = 0; v[i] && v[i] != ' ' && v[i] != ';'; i++) if (v[i] == '%') { is_pct = 1; break; }
        if (is_pct) px = (px >= 140) ? 24 : 16;
    }
    if (px <= 0) return;
    /* two rasterized sizes exist, so anything wilder than this is either a
     * unit we don't understand or a value we misread - clamp rather than
     * hand layout a line height nothing can read */
    if (px > 48) px = 48;
    if (px < 8) px = 8;
    out->scale  = px >= 22 ? 2 : 1;
    out->line_h = px >= 22 ? (px + 8) : 19;
}

/* Splits a shorthand value into up to 4 space-separated components,
 * returning how many were found. Used by margin/padding. */
static int split_values(const char *val, int *out4)
{
    int n = 0, i = 0;
    while (val[i] && n < 4) {
        while (val[i] == ' ') i++;
        if (!val[i]) break;
        int start = i;
        while (val[i] && val[i] != ' ') i++;
        char tmp[32]; int tl = 0;
        for (int z = start; z < i && tl < 31; z++) tmp[tl++] = val[z];
        tmp[tl] = 0;
        out4[n++] = parse_length_px(tmp);
    }
    return n;
}

/* CSS box shorthand order: 1 value = all, 2 = v/h, 3 = t/h/b, 4 = t/r/b/l */
static void expand_box(const char *val, int *top, int *right, int *bottom, int *left)
{
    int v[4] = {0,0,0,0};
    int n = split_values(val, v);
    if (n <= 0) { *top = *right = *bottom = *left = 0; return; }
    if (n == 1) { *top = *right = *bottom = *left = v[0]; return; }
    if (n == 2) { *top = *bottom = v[0]; *right = *left = v[1]; return; }
    if (n == 3) { *top = v[0]; *right = *left = v[1]; *bottom = v[2]; return; }
    *top = v[0]; *right = v[1]; *bottom = v[2]; *left = v[3];
}

/* Pulls a color out of a multi-value shorthand ("background: #fff url(x)",
 * "border: 1px solid red") by trying each space-separated token. */
static int shorthand_color(const char *val, color_t *out)
{
    int i = 0;
    while (val[i]) {
        while (val[i] == ' ') i++;
        if (!val[i]) break;
        int start = i;
        int depth = 0;
        while (val[i] && (depth > 0 || val[i] != ' ')) {
            if (val[i] == '(') depth++;
            else if (val[i] == ')') depth--;
            i++;
        }
        char tok[64]; int tl = 0;
        for (int z = start; z < i && tl < 63; z++) tok[tl++] = val[z];
        tok[tl] = 0;
        if (css_parse_color(tok, out)) return 1;
    }
    return 0;
}

static void apply_decl(ComputedStyle *out, const char *prop, const char *val)
{
    color_t c;
    int t, r, b, l;

    if (str_ieq(prop, "color")) {
        if (css_parse_color(val, &c)) out->color = c;
    } else if (str_ieq(prop, "background-color")) {
        if (css_parse_color(val, &c)) { out->bg = c; out->has_bg = 1; }
    } else if (str_ieq(prop, "background")) {
        /* shorthand: pick the color out of "#fff url(...) no-repeat" */
        if (shorthand_color(val, &c)) { out->bg = c; out->has_bg = 1; }
    } else if (str_ieq(prop, "font-weight")) {
        out->bold = (str_ieq(val, "bold") || str_ieq(val, "bolder") ||
                     str_ieq(val, "600") || str_ieq(val, "700") ||
                     str_ieq(val, "800") || str_ieq(val, "900")) ? 1 : 0;
    } else if (str_ieq(prop, "font-style")) {
        out->italic = (str_ieq(val, "italic") || str_ieq(val, "oblique"));
    } else if (str_ieq(prop, "font-size")) {
        set_font_size(out, val);
    } else if (str_ieq(prop, "font")) {
        /* shorthand: only the size component is worth recovering */
        int px = parse_length_px(val);
        if (px > 0) set_font_size(out, val);
        if (!str_ieq(val, "inherit")) {
            for (int i = 0; val[i]; i++)
                if (to_lower(val[i]) == 'b' && str_ieq(val + i, "bold")) { out->bold = 1; break; }
        }
    } else if (str_ieq(prop, "line-height")) {
        int n = parse_length_px(val);
        int is_pct = 0;
        for (int i = 0; val[i]; i++) if (val[i] == '%') { is_pct = 1; break; }
        if (is_pct) n = 19 * n / 100;
        else if (n > 0 && n < 8) n *= 16;
        if (n >= 12 && n <= 60) out->line_h = n;
    } else if (str_ieq(prop, "text-align")) {
        out->align = (str_ieq(val, "center") || str_ieq(val, "-webkit-center")) ? ALIGN_CENTER :
                     str_ieq(val, "right") ? ALIGN_RIGHT : ALIGN_LEFT;
    } else if (str_ieq(prop, "text-decoration") || str_ieq(prop, "text-decoration-line")) {
        out->underline = 0; out->strike = 0;
        for (int i = 0; val[i]; i++) {
            if (str_ieq(val + i, "underline") || (val[i] == 'u' && val[i+1] == 'n')) { out->underline = 1; break; }
        }
        if (str_ieq(val, "line-through")) { out->strike = 1; out->underline = 0; }
        if (str_ieq(val, "none")) { out->underline = 0; out->strike = 0; }
    } else if (str_ieq(prop, "text-transform")) {
        out->text_transform = str_ieq(val, "uppercase") ? TT_UPPER :
                              str_ieq(val, "lowercase") ? TT_LOWER : TT_NONE;
    } else if (str_ieq(prop, "visibility")) {
        out->hidden = (str_ieq(val, "hidden") || str_ieq(val, "collapse"));
    } else if (str_ieq(prop, "display")) {
        if (str_ieq(val, "none")) out->display = DISP_NONE;
        else if (str_ieq(val, "inline") || str_ieq(val, "inline-block") ||
                 str_ieq(val, "inline-flex")) out->display = DISP_INLINE;
        else if (str_ieq(val, "list-item")) out->display = DISP_LIST_ITEM;
        else out->display = DISP_BLOCK;
    } else if (str_ieq(prop, "margin")) {
        expand_box(val, &t, &r, &b, &l);
        out->margin_top = t < 0 ? 0 : t;
        out->margin_bottom = b < 0 ? 0 : b;
        if (l > 0) out->indent += l;
    } else if (str_ieq(prop, "margin-top")) {
        int m = parse_length_px(val); out->margin_top = m < 0 ? 0 : m;
    } else if (str_ieq(prop, "margin-bottom")) {
        int m = parse_length_px(val); out->margin_bottom = m < 0 ? 0 : m;
    } else if (str_ieq(prop, "margin-left")) {
        int m = parse_length_px(val); if (m > 0) out->indent += m;
    } else if (str_ieq(prop, "padding")) {
        expand_box(val, &t, &r, &b, &l);
        if (l > 0) out->indent += l;
        if (t > 0) out->pad_top += t;
        if (b > 0) out->pad_bottom += b;
    } else if (str_ieq(prop, "padding-left")) {
        int m = parse_length_px(val); if (m > 0) out->indent += m;
    } else if (str_ieq(prop, "padding-top")) {
        int m = parse_length_px(val); if (m > 0) out->pad_top += m;
    } else if (str_ieq(prop, "padding-bottom")) {
        int m = parse_length_px(val); if (m > 0) out->pad_bottom += m;
    } else if (str_ieq(prop, "border") || str_ieq(prop, "border-top") ||
               str_ieq(prop, "border-bottom") || str_ieq(prop, "border-left") ||
               str_ieq(prop, "border-right")) {
        if (str_ieq(val, "none") || str_ieq(val, "0")) out->border_w = 0;
        else {
            int w = parse_length_px(val);
            out->border_w = w > 0 ? (w > 8 ? 8 : w) : 1;
            if (shorthand_color(val, &c)) out->border_color = c;
        }
    } else if (str_ieq(prop, "border-color")) {
        if (css_parse_color(val, &c)) { out->border_color = c; if (!out->border_w) out->border_w = 1; }
    } else if (str_ieq(prop, "border-width")) {
        int w = parse_length_px(val); out->border_w = w < 0 ? 0 : w > 8 ? 8 : w;
    } else if (str_ieq(prop, "width") || str_ieq(prop, "max-width")) {
        int w = parse_length_px(val);
        /* percentages and auto stay "unset" - we have no containing-block
         * width at cascade time, so the layout pass decides */
        for (int i = 0; val[i]; i++) if (val[i] == '%') { w = 0; break; }
        if (w > 0) out->width_px = w;
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

        /* a value can contain ';' inside url(...) or a data: URI, so track
         * parens and only break on a top-level separator */
        int vl = 0, depth = 0;
        while (i < len && vl < 255) {
            char ch = decl[i];
            if (ch == '(') depth++;
            else if (ch == ')') { if (depth > 0) depth--; }
            else if ((ch == ';' || ch == '}') && depth == 0) break;
            val[vl++] = ch; i++;
        }
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

static int is_block_level_tag(const char *t)
{
    static const char *block_tags[] = {
        "p","div","section","article","header","footer","nav","main",
        "aside","ul","ol","li","dl","dt","dd","table","tr","form","blockquote","figure",
        "figcaption","h1","h2","h3","h4","h5","h6","fieldset","address","pre","hr","body",
        "html","center","details","summary","caption","video","audio","iframe","canvas",
        "legend","output","dialog",0
    };
    for (int i = 0; block_tags[i]; i++) if (str_ieq(t, block_tags[i])) return 1;
    return 0;
}

static void apply_ua_defaults(const DomNode *n, ComputedStyle *out)
{
    const char *t = n->tag;

    if (str_ieq(t,"head")||str_ieq(t,"title")||str_ieq(t,"meta")||str_ieq(t,"link")||
        str_ieq(t,"script")||str_ieq(t,"style")||str_ieq(t,"base")||str_ieq(t,"#r")||
        str_ieq(t,"template")||str_ieq(t,"option")||str_ieq(t,"datalist")||
        /* we do run scripts, so <noscript> content is not for us - showing it
         * puts "turn on JavaScript" banners in the middle of working pages */
        str_ieq(t,"noscript")) {
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
    else if (str_ieq(t,"dd"))         { out->indent = 24; }
    else if (str_ieq(t,"dt"))         { out->bold = 1; }
    else if (str_ieq(t,"blockquote")) { out->margin_top = 8; out->margin_bottom = 8; out->indent = 24; }
    else if (str_ieq(t,"pre"))        { out->margin_top = 8; out->margin_bottom = 8; out->color = fb_rgb(70,80,96); }
    else if (str_ieq(t,"a"))          { out->is_link = 1; out->underline = 1; out->color = fb_rgb(26, 90, 210); }
    else if (str_ieq(t,"b")||str_ieq(t,"strong")||str_ieq(t,"th")) { out->bold = 1; }
    else if (str_ieq(t,"i")||str_ieq(t,"em")||str_ieq(t,"cite")||str_ieq(t,"var")) { out->italic = 1; }
    else if (str_ieq(t,"u")||str_ieq(t,"ins"))   { out->underline = 1; }
    else if (str_ieq(t,"s")||str_ieq(t,"del")||str_ieq(t,"strike")) { out->strike = 1; }
    else if (str_ieq(t,"mark"))       { out->has_bg = 1; out->bg = fb_rgb(255, 240, 130); }
    else if (str_ieq(t,"code")||str_ieq(t,"tt")||str_ieq(t,"kbd")||str_ieq(t,"samp")) { out->color = fb_rgb(70,80,96); }
    else if (str_ieq(t,"small"))      { out->scale = 1; }
    else if (str_ieq(t,"center"))     { out->align = ALIGN_CENTER; }
    else if (str_ieq(t,"hr"))         { out->margin_top = 6; out->margin_bottom = 6; }
    else if (str_ieq(t,"table"))      { out->margin_top = 6; out->margin_bottom = 6; }
    else if (str_ieq(t,"fieldset"))   { out->margin_top = 8; out->margin_bottom = 8; out->border_w = 1; out->border_color = fb_rgb(190,196,206); }
    else if (str_ieq(t,"div")||str_ieq(t,"section")||str_ieq(t,"article")||str_ieq(t,"header")||
             str_ieq(t,"footer")||str_ieq(t,"nav")||str_ieq(t,"main")||str_ieq(t,"aside")) {
        out->margin_top = 0; out->margin_bottom = 0;
    }
}

/* Builds g_sorted: rule indices in ascending (spec, order). Insertion sort
 * over the index array - rule counts are in the hundreds-to-thousands and
 * this runs once per document, not once per node (which is what the old
 * repeated-rescan cascade effectively did, at O(nodes * rules^2)). */
static void build_sorted(void)
{
    int n = g_nrules < CSS_MAX_RULES ? g_nrules : CSS_MAX_RULES;
    for (int i = 0; i < n; i++) {
        CssRule *r = &g_rules[i];
        int j = i - 1;
        while (j >= 0) {
            CssRule *s = &g_rules[g_sorted[j]];
            if (s->spec < r->spec || (s->spec == r->spec && s->order < r->order)) break;
            g_sorted[j + 1] = g_sorted[j];
            j--;
        }
        g_sorted[j + 1] = (short)i;
    }
    g_sorted_n = n;
}

void css_cascade(const DomNode *node, const ComputedStyle *parent, ComputedStyle *out)
{
    if (parent) {
        out->color  = parent->color;
        out->bold   = parent->bold;
        out->italic = parent->italic;
        out->scale  = parent->scale;
        out->align  = parent->align;
        out->line_h = parent->line_h;
        out->text_transform = parent->text_transform;
        out->hidden = parent->hidden;
    } else {
        out->color  = fb_rgb(28, 30, 36);
        out->bold   = 0;
        out->italic = 0;
        out->scale  = 1;
        out->align  = ALIGN_LEFT;
        out->line_h = 19;
        out->text_transform = TT_NONE;
        out->hidden = 0;
    }
    out->underline = 0;
    out->strike = 0;
    out->is_link = 0;
    out->display = DISP_INLINE;
    out->has_bg = 0;
    out->bg = 0;
    out->margin_top = 0;
    out->margin_bottom = 0;
    out->indent = 0;
    out->pad_top = 0;
    out->pad_bottom = 0;
    out->border_w = 0;
    out->border_color = fb_rgb(190, 196, 206);
    out->width_px = 0;

    if (!node || node->type != DOM_NODE_ELEMENT) { out->display = DISP_INLINE; return; }

    apply_ua_defaults(node, out);
    if (out->display == DISP_NONE) return;

    if (g_sorted_n < 0) build_sorted();
    for (int k = 0; k < g_sorted_n; k++) {
        CssRule *r = &g_rules[g_sorted[k]];
        if (match_selector(node, g_css + r->sel_off, r->sel_len))
            apply_decl_block(out, g_css + r->decl_off, r->decl_len);
    }

    /* inline style="" always wins over stylesheet rules */
    const char *inline_style = dom_get_attr(node, "style");
    if (inline_style[0]) {
        int l = 0; while (inline_style[l]) l++;
        apply_decl_block(out, inline_style, l);
    }

    /* presentational attributes, still all over the real web */
    const char *bgcolor = dom_get_attr(node, "bgcolor");
    if (bgcolor[0]) { color_t c; if (css_parse_color(bgcolor, &c)) { out->bg = c; out->has_bg = 1; } }
    const char *fgcolor = dom_get_attr(node, "color");
    if (fgcolor[0]) { color_t c; if (css_parse_color(fgcolor, &c)) out->color = c; }
    const char *alignattr = dom_get_attr(node, "align");
    if (alignattr[0]) {
        if (str_ieq(alignattr, "center")) out->align = ALIGN_CENTER;
        else if (str_ieq(alignattr, "right")) out->align = ALIGN_RIGHT;
        else if (str_ieq(alignattr, "left")) out->align = ALIGN_LEFT;
    }
    if (dom_has_attr(node, "hidden")) out->display = DISP_NONE;

    if (out->line_h < 16) out->line_h = out->scale == 2 ? 30 : 19;
}
