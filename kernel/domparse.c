/* kernel/domparse.c - a small, forgiving HTML tree builder.
 *
 * Not a spec parser, but it handles the things real pages rely on: attributes
 * (quoted/unquoted/boolean), void elements, raw <script>/<style> bodies,
 * comments/doctype, and a useful subset of implicit tag closing so that
 * sloppy markup still yields a sane tree instead of runaway nesting.
 */
#include "domparse.h"

/* --- helpers ------------------------------------------------------------- */
static char p_lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static int  p_ieq(const char *a, const char *b)
{
    int i = 0;
    while (a[i] && b[i]) { if (p_lc(a[i]) != p_lc(b[i])) return 0; i++; }
    return p_lc(a[i]) == p_lc(b[i]);
}
static int  p_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

static int is_void(const char *t)
{
    static const char *v[] = { "br","hr","img","input","meta","link","area","base",
                               "col","embed","source","track","wbr","param",0 };
    for (int i = 0; v[i]; i++) if (p_ieq(t, v[i])) return 1;
    return 0;
}
static int is_raw(const char *t) { return p_ieq(t, "script") || p_ieq(t, "style"); }

/* Decode the handful of entities that show up in attribute values / URLs. */
static int attr_decode(const char *s, int len, char *out, int cap)
{
    int o = 0;
    for (int i = 0; i < len && o < cap - 1; i++) {
        if (s[i] == '&') {
            if (i + 4 < len && s[i+1]=='a'&&s[i+2]=='m'&&s[i+3]=='p'&&s[i+4]==';') { out[o++]='&'; i+=4; continue; }
            if (i + 3 < len && s[i+1]=='l'&&s[i+2]=='t'&&s[i+3]==';')              { out[o++]='<'; i+=3; continue; }
            if (i + 3 < len && s[i+1]=='g'&&s[i+2]=='t'&&s[i+3]==';')              { out[o++]='>'; i+=3; continue; }
            if (i + 5 < len && s[i+1]=='q'&&s[i+2]=='u'&&s[i+3]=='o'&&s[i+4]=='t'&&s[i+5]==';') { out[o++]='"'; i+=5; continue; }
            if (i + 4 < len && s[i+1]=='#'&&s[i+2]=='3'&&s[i+3]=='9'&&s[i+4]==';') { out[o++]='\''; i+=4; continue; }
        }
        out[o++] = s[i];
    }
    out[o] = 0;
    return o;
}

/* Implicit-close: opening `opening` should pop these currently-open tags. */
static int implies_close(const char *open_top, const char *opening)
{
    if (p_ieq(open_top, "p")) {
        static const char *blk[] = { "p","div","ul","ol","table","h1","h2","h3","h4","h5","h6",
                                     "blockquote","pre","section","article","header","footer",
                                     "nav","main","aside","form","figure","hr",0 };
        for (int i = 0; blk[i]; i++) if (p_ieq(opening, blk[i])) return 1;
    }
    if (p_ieq(open_top, "li") && p_ieq(opening, "li")) return 1;
    if ((p_ieq(open_top, "dt") || p_ieq(open_top, "dd")) && (p_ieq(opening, "dt") || p_ieq(opening, "dd"))) return 1;
    if (p_ieq(open_top, "option") && (p_ieq(opening, "option") || p_ieq(opening, "optgroup"))) return 1;
    if (p_ieq(open_top, "tr") && p_ieq(opening, "tr")) return 1;
    if ((p_ieq(open_top, "td") || p_ieq(open_top, "th")) &&
        (p_ieq(opening, "td") || p_ieq(opening, "th") || p_ieq(opening, "tr"))) return 1;
    if (p_ieq(open_top, "thead") || p_ieq(open_top, "tbody") || p_ieq(open_top, "tfoot")) {
        if (p_ieq(opening, "tbody") || p_ieq(opening, "thead") || p_ieq(opening, "tfoot")) return 1;
    }
    return 0;
}

/* --- the builder --------------------------------------------------------- */
#define PSTACK 192

typedef struct {
    DomNode *stack[PSTACK];
    int      sp;
} PState;

static DomNode *top(PState *st) { return st->sp > 0 ? st->stack[st->sp - 1] : 0; }

static void push(PState *st, DomNode *n)
{
    if (st->sp < PSTACK) st->stack[st->sp++] = n;
}

/* Pop until (and including) the nearest matching open tag; no-op if none. */
static void pop_to(PState *st, const char *tag)
{
    int found = -1;
    for (int i = st->sp - 1; i >= 0; i--)
        if (p_ieq(st->stack[i]->tag, tag)) { found = i; break; }
    if (found < 0) return;            /* stray close tag: ignore */
    st->sp = found;
}

/* Parse the attribute section of a start tag (s..s+len) onto node n. */
static void parse_attrs(DomNode *n, const char *s, int len)
{
    int i = 0;
    char name[64], rawval[1024], val[1024];
    while (i < len) {
        while (i < len && (p_ws(s[i]) || s[i] == '/')) i++;
        if (i >= len) break;
        int nl = 0;
        while (i < len && !p_ws(s[i]) && s[i] != '=' && s[i] != '/' && nl < 63)
            name[nl++] = s[i++];
        name[nl] = 0;
        if (!nl) { i++; continue; }
        while (i < len && p_ws(s[i])) i++;
        if (i < len && s[i] == '=') {
            i++;
            while (i < len && p_ws(s[i])) i++;
            int vl = 0;
            if (i < len && (s[i] == '"' || s[i] == '\'')) {
                char q = s[i++];
                while (i < len && s[i] != q && vl < 1023) rawval[vl++] = s[i++];
                if (i < len && s[i] == q) i++;
            } else {
                while (i < len && !p_ws(s[i]) && s[i] != '>' && vl < 1023) rawval[vl++] = s[i++];
            }
            rawval[vl] = 0;
            attr_decode(rawval, vl, val, sizeof(val));
            dom_set_attr(n, name, val);
        } else {
            dom_set_attr(n, name, "");     /* boolean attribute */
        }
    }
}

static void build(PState *st, const char *p, int n)
{
    int i = 0;
    while (i < n) {
        if (p[i] != '<') {
            int start = i;
            while (i < n && p[i] != '<') i++;
            DomNode *t = dom_create_text(p + start, i - start);
            if (t) dom_append_child(top(st), t);
            continue;
        }
        /* comment */
        if (i + 3 < n && p[i+1]=='!' && p[i+2]=='-' && p[i+3]=='-') {
            i += 4;
            while (i + 2 < n && !(p[i]=='-'&&p[i+1]=='-'&&p[i+2]=='>')) i++;
            i += 3;
            continue;
        }
        /* doctype / processing / cdata: skip to '>' */
        if (i + 1 < n && (p[i+1] == '!' || p[i+1] == '?')) {
            i += 2;
            while (i < n && p[i] != '>') i++;
            if (i < n) i++;
            continue;
        }
        /* a tag */
        int ts = i + 1, te = ts;
        while (te < n && p[te] != '>') te++;
        int tlen = te - ts;
        const char *tag = p + ts;
        i = (te < n) ? te + 1 : n;
        if (tlen <= 0) continue;

        int closing = 0, k = 0;
        if (tag[0] == '/') { closing = 1; k = 1; }
        char name[16]; int nl = 0;
        while (k < tlen && !p_ws(tag[k]) && tag[k] != '/' && nl < 15)
            name[nl++] = p_lc(tag[k++]);
        name[nl] = 0;
        if (!nl) continue;

        if (closing) {
            pop_to(st, name);
            continue;
        }

        /* implicit closing of open elements the spec would auto-close */
        while (st->sp > 1 && implies_close(top(st)->tag, name))
            st->sp--;

        DomNode *el = dom_create_element(name);
        if (!el) continue;
        parse_attrs(el, tag + k, tlen - k);
        dom_append_child(top(st), el);

        /* raw text elements: capture body verbatim as one text child */
        if (is_raw(name)) {
            char close[10]; int cl = 0;
            close[cl++] = '<'; close[cl++] = '/';
            for (int z = 0; name[z] && cl < 8; z++) close[cl++] = name[z];
            close[cl] = 0;
            int j = i;
            while (j < n) {
                if (p[j] == '<') {
                    int m = 1;
                    for (int z = 0; close[z]; z++)
                        if (j + z >= n || p_lc(p[j + z]) != p_lc(close[z])) { m = 0; break; }
                    if (m) break;
                }
                j++;
            }
            if (j > i) {
                DomNode *t = dom_create_text(p + i, j - i);
                if (t) dom_append_child(el, t);
            }
            while (j < n && p[j] != '>') j++;
            i = (j < n) ? j + 1 : n;
            continue;
        }

        int selfclose = (tlen > 0 && tag[tlen - 1] == '/');
        if (!is_void(name) && !selfclose) push(st, el);
    }
}

DomNode *dom_parse_document(const char *html, int len)
{
    dom_init();
    PState st; st.sp = 0;
    push(&st, dom_root());
    build(&st, html, len);
    return dom_root();
}

void dom_parse_fragment(DomNode *parent, const char *html, int len)
{
    if (!parent) return;
    dom_remove_children(parent);
    PState st; st.sp = 0;
    push(&st, parent);
    build(&st, html, len);
}

void dom_parse_append(DomNode *parent, const char *html, int len)
{
    if (!parent) return;
    PState st; st.sp = 0;
    push(&st, parent);
    build(&st, html, len);
}
