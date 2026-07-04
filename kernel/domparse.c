/* kernel/domparse.c - a small, forgiving HTML-to-DOM builder.
 *
 * This is not a spec-compliant tree constructor (no adoption agency
 * algorithm, no foster parenting, none of that). It handles what real pages
 * actually throw at it: quoted/unquoted/boolean attributes, void elements,
 * <script>/<style> bodies treated as raw text, comments and doctypes, and a
 * pragmatic subset of implicit tag closing so a page full of unclosed <p>
 * and <li> tags still ends up as a sane tree instead of one long chain of
 * nesting.
 */
#include "domparse.h"

static char to_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static int tag_ieq(const char *a, const char *b)
{
    int i = 0;
    while (a[i] && b[i]) { if (to_lower(a[i]) != to_lower(b[i])) return 0; i++; }
    return to_lower(a[i]) == to_lower(b[i]);
}

static int is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

static int is_void_element(const char *t)
{
    static const char *voids[] = { "br","hr","img","input","meta","link","area","base",
                                   "col","embed","source","track","wbr","param",0 };
    for (int i = 0; voids[i]; i++) if (tag_ieq(t, voids[i])) return 1;
    return 0;
}

static int is_raw_text_element(const char *t) { return tag_ieq(t, "script") || tag_ieq(t, "style"); }

/* Decodes the handful of entities that actually show up inside attribute
 * values (mostly URLs with stray &amp;s in query strings). */
static int decode_attr_entities(const char *s, int len, char *out, int cap)
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

/* Should opening `opening` implicitly pop the currently-open tag `open_top`?
 * This is the "browsers are forgiving" table - real HTML relies on all of it
 * (unclosed <p>, <li>, table sections, etc). */
static int should_auto_close(const char *open_top, const char *opening)
{
    if (tag_ieq(open_top, "p")) {
        static const char *block_starts[] = {
            "p","div","ul","ol","table","h1","h2","h3","h4","h5","h6",
            "blockquote","pre","section","article","header","footer",
            "nav","main","aside","form","figure","hr",0
        };
        for (int i = 0; block_starts[i]; i++) if (tag_ieq(opening, block_starts[i])) return 1;
    }
    if (tag_ieq(open_top, "li") && tag_ieq(opening, "li")) return 1;
    if ((tag_ieq(open_top, "dt") || tag_ieq(open_top, "dd")) &&
        (tag_ieq(opening, "dt") || tag_ieq(opening, "dd"))) return 1;
    if (tag_ieq(open_top, "option") && (tag_ieq(opening, "option") || tag_ieq(opening, "optgroup"))) return 1;
    if (tag_ieq(open_top, "tr") && tag_ieq(opening, "tr")) return 1;
    if ((tag_ieq(open_top, "td") || tag_ieq(open_top, "th")) &&
        (tag_ieq(opening, "td") || tag_ieq(opening, "th") || tag_ieq(opening, "tr"))) return 1;
    if (tag_ieq(open_top, "thead") || tag_ieq(open_top, "tbody") || tag_ieq(open_top, "tfoot")) {
        if (tag_ieq(opening, "tbody") || tag_ieq(opening, "thead") || tag_ieq(opening, "tfoot")) return 1;
    }
    return 0;
}

/* --- the actual builder: a stack of currently-open elements --- */
#define OPEN_STACK_CAP 192

typedef struct {
    DomNode *open[OPEN_STACK_CAP];
    int      depth;
} BuildStack;

static DomNode *current_parent(BuildStack *st) { return st->depth > 0 ? st->open[st->depth - 1] : 0; }

static void enter(BuildStack *st, DomNode *n)
{
    if (st->depth < OPEN_STACK_CAP) st->open[st->depth++] = n;
}

/* Pops up to and including the nearest open tag matching `tag`. A close tag
 * with no matching opener anywhere on the stack is simply ignored, which is
 * what browsers do with stray </whatever>. */
static void close_to(BuildStack *st, const char *tag)
{
    int match = -1;
    for (int i = st->depth - 1; i >= 0; i--)
        if (tag_ieq(st->open[i]->tag, tag)) { match = i; break; }
    if (match < 0) return;
    st->depth = match;
}

/* Parses the attribute section of a start tag (the bytes between the tag
 * name and the closing '>') onto node n. */
static void parse_attributes(DomNode *n, const char *s, int len)
{
    int i = 0;
    char name[64], raw[1024], decoded[1024];
    while (i < len) {
        while (i < len && (is_space(s[i]) || s[i] == '/')) i++;
        if (i >= len) break;

        int nl = 0;
        while (i < len && !is_space(s[i]) && s[i] != '=' && s[i] != '/' && nl < 63)
            name[nl++] = s[i++];
        name[nl] = 0;
        if (!nl) { i++; continue; }

        while (i < len && is_space(s[i])) i++;
        if (i < len && s[i] == '=') {
            i++;
            while (i < len && is_space(s[i])) i++;
            int vl = 0;
            if (i < len && (s[i] == '"' || s[i] == '\'')) {
                char quote = s[i++];
                while (i < len && s[i] != quote && vl < 1023) raw[vl++] = s[i++];
                if (i < len && s[i] == quote) i++;
            } else {
                while (i < len && !is_space(s[i]) && s[i] != '>' && vl < 1023) raw[vl++] = s[i++];
            }
            raw[vl] = 0;
            decode_attr_entities(raw, vl, decoded, sizeof(decoded));
            dom_set_attr(n, name, decoded);
        } else {
            dom_set_attr(n, name, "");     /* bare/boolean attribute */
        }
    }
}

static void build(BuildStack *st, const char *p, int n)
{
    int i = 0;
    while (i < n) {
        if (p[i] != '<') {
            int start = i;
            while (i < n && p[i] != '<') i++;
            DomNode *t = dom_create_text(p + start, i - start);
            if (t) dom_append_child(current_parent(st), t);
            continue;
        }

        if (i + 3 < n && p[i+1]=='!' && p[i+2]=='-' && p[i+3]=='-') {
            i += 4;
            while (i + 2 < n && !(p[i]=='-'&&p[i+1]=='-'&&p[i+2]=='>')) i++;
            i += 3;
            continue;
        }

        /* doctype / processing instruction / cdata-ish: skip to the next '>' */
        if (i + 1 < n && (p[i+1] == '!' || p[i+1] == '?')) {
            i += 2;
            while (i < n && p[i] != '>') i++;
            if (i < n) i++;
            continue;
        }

        int tag_start = i + 1, tag_end = tag_start;
        while (tag_end < n && p[tag_end] != '>') tag_end++;
        int tag_len = tag_end - tag_start;
        const char *tag_bytes = p + tag_start;
        i = (tag_end < n) ? tag_end + 1 : n;
        if (tag_len <= 0) continue;

        int is_closing = 0, k = 0;
        if (tag_bytes[0] == '/') { is_closing = 1; k = 1; }

        char name[16]; int nl = 0;
        while (k < tag_len && !is_space(tag_bytes[k]) && tag_bytes[k] != '/' && nl < 15)
            name[nl++] = to_lower(tag_bytes[k++]);
        name[nl] = 0;
        if (!nl) continue;

        if (is_closing) {
            close_to(st, name);
            continue;
        }

        while (st->depth > 1 && should_auto_close(current_parent(st)->tag, name))
            st->depth--;

        DomNode *el = dom_create_element(name);
        if (!el) continue;
        parse_attributes(el, tag_bytes + k, tag_len - k);
        dom_append_child(current_parent(st), el);

        if (is_raw_text_element(name)) {
            /* Capture everything up to the matching </tag> verbatim, as one
             * text child - no entity decoding, no nested tag scanning. This
             * is what lets <script> bodies contain "<" and "</" in strings
             * without confusing the parser. */
            char closer[10]; int cl = 0;
            closer[cl++] = '<'; closer[cl++] = '/';
            for (int z = 0; name[z] && cl < 8; z++) closer[cl++] = name[z];
            closer[cl] = 0;

            int j = i;
            while (j < n) {
                if (p[j] == '<') {
                    int matches = 1;
                    for (int z = 0; closer[z]; z++)
                        if (j + z >= n || to_lower(p[j + z]) != to_lower(closer[z])) { matches = 0; break; }
                    if (matches) break;
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

        int self_closed = (tag_len > 0 && tag_bytes[tag_len - 1] == '/');
        if (!is_void_element(name) && !self_closed) enter(st, el);
    }
}

DomNode *dom_parse_document(const char *html, int len)
{
    dom_init();
    BuildStack st; st.depth = 0;
    enter(&st, dom_root());
    build(&st, html, len);
    return dom_root();
}

void dom_parse_fragment(DomNode *parent, const char *html, int len)
{
    if (!parent) return;
    dom_remove_children(parent);
    BuildStack st; st.depth = 0;
    enter(&st, parent);
    build(&st, html, len);
}

void dom_parse_append(DomNode *parent, const char *html, int len)
{
    if (!parent) return;
    BuildStack st; st.depth = 0;
    enter(&st, parent);
    build(&st, html, len);
}
