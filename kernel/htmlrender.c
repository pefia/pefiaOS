#include "htmlrender.h"
#include "framebuffer.h"
#include "font8x16.h"
#include "net.h"
#include "image.h"
#include "heap.h"
#include "domrt.h"
#include "domparse.h"
#include "css.h"
#include "js.h"
#include "clock.h"

#define PAGE_CAP   786432
#define MAX_SPANS  30000
#define ARENA_CAP  700000
#define MAX_LINKS  4000
#define HREF_CAP   384
#define MAX_BG     3000

typedef struct {
    int      x, y, w;
    color_t  fg;
    uint8_t  bold;
    uint8_t  underline;
    uint8_t  strike;
    uint8_t  rule;
    uint8_t  scale;         /* glyph pixel scale (1 or 2)                  */
    uint8_t  hidden;
    int      link;
    int      toff;
    int      img;
    int      imgh;
    int      field;         /* index into g_fields (form control), or -1   */
    DomNode *node;          /* originating element, for script click hits  */
} Span;

/* `inline_box` marks the chrome of a form control or media placeholder: it
 * sits on a text line and must travel with that line when it gets centered
 * or right-aligned, unlike a block background that already spans the full
 * content width. */
typedef struct { int x, y, w, h; color_t color; uint8_t border, inline_box; color_t bcolor; } BgRect;

/* Decoded, downscaled images referenced by image spans - rebuilt each layout.
 * An animated GIF keeps every frame plus its delay, and the painter picks the
 * frame from the wall clock. The window manager repaints the whole visible
 * page every frame anyway, so animation costs an index computation. */
#define MAX_IMAGES   64
#define IMG_MEM_CAP  (64 * 1024 * 1024)
typedef struct {
    uint32_t *frames[ANIM_MAX_FRAMES];
    int       delays[ANIM_MAX_FRAMES];
    int       nframes, total_ms, w, h;
} RImage;
static RImage g_images[MAX_IMAGES];
static int    g_nimages = 0;
static long   g_img_mem = 0;
static int    g_has_anim = 0;

/* Per-URL cache of the full-resolution decoded frames, kept across
 * re-layouts of the same document (window resize, scroll-triggered relayout)
 * and thrown away when the document itself changes. */
#define IMG_CACHE_N    48
#define IMG_CACHE_MEM  (48 * 1024 * 1024)
typedef struct { char url[256]; AnimBitmap anim; int valid; } ImgCacheEnt;
static ImgCacheEnt g_icache[IMG_CACHE_N];
static long        g_icache_mem = 0;

static void free_scaled_images(void)
{
    for (int i = 0; i < g_nimages; i++)
        for (int f = 0; f < g_images[i].nframes; f++)
            if (g_images[i].frames[f]) { kfree(g_images[i].frames[f]); g_images[i].frames[f] = 0; }
    g_nimages = 0;
    g_img_mem = 0;
    g_has_anim = 0;
}

static void img_cache_clear(void)
{
    for (int i = 0; i < IMG_CACHE_N; i++) {
        if (g_icache[i].valid) anim_free(&g_icache[i].anim);
        g_icache[i].valid = 0; g_icache[i].url[0] = 0;
    }
    g_icache_mem = 0;
}

static char     g_page[PAGE_CAP];
static char     g_base_url[512];
static int      g_page_len = 0;
static const void *g_active_id = 0;
static unsigned g_active_gen = 0xFFFFFFFFu;

static Span     g_spans[MAX_SPANS];
static int      g_nspans = 0;
static char     g_arena[ARENA_CAP];
static int      g_arena_len = 0;
static char     g_links[MAX_LINKS][HREF_CAP];
static int      g_nlinks = 0;
static BgRect   g_bgs[MAX_BG];
static int      g_nbgs = 0;
static int      g_doc_h = 0;
static int      g_laid_w = -1;
static char     g_title[96];
static int      g_title_len;

static DomNode *g_root = 0;
static int      g_doc_dirty = 1;     /* page bytes changed: needs a re-parse */
static int      g_pending_load = 0;  /* fire load handlers on the next pump  */

/* --- interactive form fields ---
 * emit_field records one entry per control (in document order) so clicks can
 * hit-test, keystrokes can edit, and submit can serialize the whole form.
 * Geometry/flags are rebuilt every layout; the *state* (typed text, checkbox
 * state, selected option) lives in parallel arrays keyed by document-order
 * ordinal so it survives relayouts and is only reset on navigation. */
#define MAX_FIELDS 64
#define FVAL_CAP   160
#define OPT_MAX    12
#define OPT_LEN    40

enum { FLD_INERT = 0, FLD_TEXT, FLD_PASSWORD, FLD_CHECKBOX, FLD_RADIO,
       FLD_SELECT, FLD_SUBMIT, FLD_BUTTON, FLD_HIDDEN };

typedef struct {
    int         x, y, w, h;
    uint8_t     kind;
    uint8_t     method_post;
    char        name[48];
    char        action[256];
    char        ph[48];
    char        defval[64];
    const void *form;
    int         nopts;
    char        opts[OPT_MAX][OPT_LEN];
} Field;
static Field    g_fields[MAX_FIELDS];
static int      g_nfields = 0;
static char     g_fval[MAX_FIELDS][FVAL_CAP];
static int      g_fvlen[MAX_FIELDS];
static uint8_t  g_fchk[MAX_FIELDS];
static int      g_fsel[MAX_FIELDS];
static int      g_field_focus = -1;
static int      g_state_seeded = 0;

static color_t C_TEXT, C_RULE, C_BG;
static int g_colors_ready = 0;
static void init_colors(void)
{
    if (g_colors_ready) return;
    C_TEXT  = fb_rgb(28, 30, 36);
    C_RULE  = fb_rgb(208, 214, 222);
    C_BG    = fb_rgb(255, 255, 255);
    g_colors_ready = 1;
}

static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static char uc(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }
static int  ieq(const char *a, const char *b) { int i=0; while(a[i]&&b[i]){ if(lc(a[i])!=lc(b[i])) return 0; i++; } return lc(a[i])==lc(b[i]); }
static int  starts_ci(const char *s, const char *p){ int i=0; while(p[i]){ if(lc(s[i])!=lc(p[i])) return 0; i++; } return 1; }
static int  slen(const char *s){ int n=0; while(s&&s[n]) n++; return n; }
static void scopy(char *d, const char *s, int cap){ int i=0; if(cap<=0)return; while(s&&s[i]&&i<cap-1){ d[i]=s[i]; i++; } d[i]=0; }

static void resolve_url(const char *href, char *out, int cap)
{
    if (!href || !href[0]) { out[0]=0; return; }
    if (starts_ci(href, "http://") || starts_ci(href, "https://")) { scopy(out, href, cap); return; }
    if (starts_ci(href, "//")) {
        int base_is_http = starts_ci(g_base_url, "http://") ? 1 : 0;
        scopy(out, base_is_http ? "http:" : "https:", cap);
        int o = slen(out);
        for (int i = 0; href[i] && o < cap-1; i++) out[o++] = href[i];
        out[o] = 0; return;
    }
    if (starts_ci(href, "data:") || starts_ci(href, "javascript:") ||
        starts_ci(href, "mailto:") || starts_ci(href, "about:")) { scopy(out, href, cap); return; }
    if (!g_base_url[0]) { scopy(out, href, cap); return; }

    if (href[0] == '/') {
        int o = 0;
        int scheme_len = starts_ci(g_base_url, "http://") ? 7 : starts_ci(g_base_url, "https://") ? 8 : 0;
        for (int i = 0; i < scheme_len && o < cap-1; i++) out[o++] = g_base_url[i];
        int hs = scheme_len;
        while (g_base_url[hs] && g_base_url[hs] != '/' && o < cap-1) out[o++] = g_base_url[hs++];
        for (int i = 0; href[i] && o < cap-1; i++) out[o++] = href[i];
        out[o] = 0;
        if (!scheme_len) scopy(out, href, cap);   /* base had no scheme we recognize; give up gracefully */
        return;
    }

    char base[512]; scopy(base, g_base_url, sizeof(base));
    /* a query or fragment on the base isn't part of the directory */
    for (int i = 0; base[i]; i++) if (base[i] == '?' || base[i] == '#') { base[i] = 0; break; }
    int cut = slen(base);
    while (cut > 0 && base[cut-1] != '/') cut--;
    if (cut <= 0) { scopy(out, href, cap); return; }

    int o = 0;
    for (int i = 0; i < cut && o < cap-1; i++) out[o++] = base[i];
    for (int i = 0; href[i] && o < cap-1; i++) out[o++] = href[i];
    out[o] = 0;
}

static int arena_put(const char *s, int len)
{
    if (len < 0 || g_arena_len + len + 1 > ARENA_CAP) return -1;
    int off = g_arena_len;
    for (int i = 0; i < len; i++) g_arena[off + i] = s[i];
    g_arena[off + len] = 0;
    g_arena_len += len + 1;
    return off;
}

/* --- layout cursor state (mutated by the tree walk below) --- */

static int      g_x0, g_contentW;
static int      g_cx, g_cy;
static int      g_line_h;
static color_t  g_fg;
static int      g_bold, g_scale, g_align, g_underline, g_strike, g_link;
static int      g_indent, g_pre, g_transform, g_hidden;
static int      g_line_start;     /* first span index of the current line    */
static int      g_line_bg;        /* first inline bg rect of the current line */
static int      g_line_field;     /* first field of the current line          */
static DomNode *g_cur_node;       /* element whose content is being emitted  */

/* Centering or right-aligning a line moves everything that sits on it: the
 * text spans, and also the boxes of any form control on that line. Shifting
 * only the spans is how a centered search box ended up drawn to the left of
 * its own placeholder text. */
static void align_line(void)
{
    if (g_align != ALIGN_LEFT && g_nspans > g_line_start) {
        int line_left = g_x0 + g_indent, right = g_x0 + g_contentW;
        int maxx = line_left;
        for (int i = g_line_start; i < g_nspans; i++) {
            if (g_spans[i].rule) continue;
            int r = g_spans[i].x + g_spans[i].w;
            if (r > maxx) maxx = r;
        }
        int used = maxx - line_left;
        int free_space = (right - line_left) - used;
        if (free_space > 0) {
            int shift = (g_align == ALIGN_CENTER) ? free_space / 2 : free_space;
            for (int i = g_line_start; i < g_nspans; i++)
                if (!g_spans[i].rule) g_spans[i].x += shift;
            for (int i = g_line_bg; i < g_nbgs; i++)
                if (g_bgs[i].inline_box) g_bgs[i].x += shift;
            for (int i = g_line_field; i < g_nfields; i++)
                g_fields[i].x += shift;
        }
    }
    g_line_start = g_nspans;
    g_line_bg = g_nbgs;
    g_line_field = g_nfields;
}

static void newline(void) { align_line(); g_cx = g_x0 + g_indent; g_cy += g_line_h; }
static void block_break(int top_margin) { if (g_cx > g_x0 + g_indent) newline(); else align_line(); g_cy += top_margin; }

static void add_span(int w, int toff)
{
    if (g_nspans >= MAX_SPANS) return;
    Span *s = &g_spans[g_nspans++];
    s->x = g_cx; s->y = g_cy; s->w = w;
    s->fg = g_fg; s->bold = (uint8_t)g_bold; s->underline = (uint8_t)(g_underline || g_link >= 0);
    s->strike = (uint8_t)g_strike; s->rule = 0; s->scale = (uint8_t)g_scale;
    s->hidden = (uint8_t)g_hidden;
    s->link = g_link; s->toff = toff; s->img = -1; s->imgh = 0;
    s->field = -1; s->node = g_cur_node;
}

/* Glyph advance width depends on the active scale (1x or 2x). */
static int glyph_w(void) { return 8 * g_scale; }

static void place_word(const char *word, int wl)
{
    if (wl <= 0) return;
    int ww = wl * glyph_w();
    if (g_cx + ww > g_x0 + g_contentW && g_cx > g_x0 + g_indent) newline();
    int off = arena_put(word, wl);
    if (off < 0) return;
    add_span(ww, off);
    g_cx += ww + glyph_w();
}

static void add_rule(void)
{
    block_break(6);
    if (g_nspans < MAX_SPANS) {
        Span *s = &g_spans[g_nspans++];
        s->x = g_x0; s->y = g_cy; s->w = g_contentW;
        s->fg = C_RULE; s->bold = 0; s->underline = 0; s->strike = 0; s->rule = 1;
        s->scale = 1; s->hidden = (uint8_t)g_hidden;
        s->link = -1; s->toff = -1; s->img = -1; s->imgh = 0; s->field = -1; s->node = g_cur_node;
    }
    g_line_start = g_nspans;
    g_line_bg = g_nbgs;
    g_line_field = g_nfields;
    g_cy += 10;
}

static int decode_entity(const char *s, int len, int *advance)
{
    if (len <= 0) { *advance = 0; return '&'; }
    if (s[0] == '#') {
        int i = 1, code = 0, is_hex = 0;
        if (i < len && (s[i] == 'x' || s[i] == 'X')) { is_hex = 1; i++; }
        while (i < len && s[i] != ';') {
            char c = s[i];
            if (is_hex) {
                if (c>='0'&&c<='9') code = code*16 + (c-'0');
                else if (c>='a'&&c<='f') code = code*16 + (10+c-'a');
                else if (c>='A'&&c<='F') code = code*16 + (10+c-'A');
                else break;
            } else {
                if (c>='0'&&c<='9') code = code*10 + (c-'0'); else break;
            }
            i++;
        }
        if (i < len && s[i] == ';') i++;
        *advance = i;
        if (code == 0 || code > 255)
            return (code==8217||code==8216) ? '\'' :
                   (code==8220||code==8221) ? '"' :
                   (code==8211||code==8212) ? '-' :
                   (code==8226) ? '*' : (code==8594) ? '>' : (code==8592) ? '<' : ' ';
        return code;
    }
    static const struct { const char *n; char c; } named[] = {
        {"amp;",'&'},{"lt;",'<'},{"gt;",'>'},{"quot;",'"'},{"apos;",'\''},
        {"nbsp;",' '},{"copy;",'c'},{"reg;",'r'},{"mdash;",'-'},{"ndash;",'-'},
        {"hellip;",'.'},{"rsquo;",'\''},{"lsquo;",'\''},{"rdquo;",'"'},{"ldquo;",'"'},
        {"middot;",'.'},{"trade;",'t'},{"bull;",'*'},{"laquo;",'<'},{"raquo;",'>'},
        {"times;",'x'},{"deg;",'o'},{"euro;",'E'},{"pound;",'L'},{"#39;",'\''}
    };
    for (int e = 0; e < (int)(sizeof(named)/sizeof(named[0])); e++) {
        const char *n = named[e].n; int nl = 0; while (n[nl]) nl++;
        int ok = 1;
        for (int j = 0; j < nl; j++) if (j >= len || lc(s[j]) != lc(n[j])) { ok = 0; break; }
        if (ok) { *advance = nl; return named[e].c; }
    }
    *advance = 0;
    return '&';
}

static char g_word_buf[2048];

static char transformed(char c)
{
    if (g_transform == TT_UPPER) return uc(c);
    if (g_transform == TT_LOWER) return lc(c);
    return c;
}

static void emit_run(const char *s, int len)
{
    int wl = 0;
    for (int i = 0; i < len; i++) {
        char c = s[i];
        if (c == '&') {
            int advance = 0;
            int ch = decode_entity(s + i + 1, len - i - 1, &advance);
            i += advance;
            if (ch == ' ' || ch == '\n' || ch == '\t') { if (wl) { place_word(g_word_buf, wl); wl = 0; } }
            else if (wl < (int)sizeof(g_word_buf)-1) g_word_buf[wl++] = transformed((char)ch);
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r' || (c == '\n' && !g_pre)) {
            if (wl) { place_word(g_word_buf, wl); wl = 0; }
            continue;
        }
        if (c == '\n' && g_pre) { if (wl) { place_word(g_word_buf, wl); wl = 0; } newline(); continue; }
        if (wl < (int)sizeof(g_word_buf)-1) g_word_buf[wl++] = transformed(c);
    }
    if (wl) place_word(g_word_buf, wl);
}

static void emit_img_alt_text(const char *alt)
{
    color_t saved = g_fg;
    g_fg = fb_rgb(120, 128, 140);
    if (alt && alt[0]) emit_run(alt, slen(alt));
    else emit_run("[img]", 5);
    g_fg = saved;
}

static void place_image(int idx, int w, int h)
{
    if (g_nspans >= MAX_SPANS) return;
    if (g_cx > g_x0 + g_indent) newline(); else align_line();
    Span *s = &g_spans[g_nspans++];
    s->x = g_x0 + g_indent; s->y = g_cy; s->w = w;
    s->fg = C_TEXT; s->bold = 0; s->underline = 0; s->strike = 0; s->rule = 0; s->scale = 1;
    s->hidden = (uint8_t)g_hidden;
    s->link = g_link; s->toff = -1; s->img = idx; s->imgh = h; s->field = -1; s->node = g_cur_node;
    align_line();                          /* images sit alone on their line, so center/right applies */
    g_cy += h + 6;
    g_cx = g_x0 + g_indent;
}

/* Fetches (if not already cached) and decodes every frame for url. */
static ImgCacheEnt *img_cache_get(const char *url)
{
    for (int i = 0; i < IMG_CACHE_N; i++)
        if (g_icache[i].valid && ieq(g_icache[i].url, url)) return &g_icache[i];

    NetResponse r;
    if (net_fetch_limited(url, 1400000, &r) != 0 || !r.body || r.body_len <= 0) return 0;

    int slot = -1;
    for (int i = 0; i < IMG_CACHE_N; i++) if (!g_icache[i].valid) { slot = i; break; }
    if (slot < 0) return 0;

    AnimBitmap anim;
    if (img_decode_anim((const uint8_t *)r.body, r.body_len, &anim) != 0 || anim.count <= 0) return 0;

    long bytes = 0;
    for (int f = 0; f < anim.count; f++)
        bytes += (long)anim.frames[f].width * anim.frames[f].height * 4;
    if (bytes <= 0 || g_icache_mem + bytes > IMG_CACHE_MEM) { anim_free(&anim); return 0; }

    ImgCacheEnt *e = &g_icache[slot];
    scopy(e->url, url, sizeof(e->url));
    e->anim = anim;
    e->valid = 1;
    g_icache_mem += bytes;
    return e;
}

static void emit_image(const char *src, const char *alt, int attr_w, int attr_h)
{
    char full[512];
    resolve_url(src, full, sizeof(full));
    if (!full[0] || g_nimages >= MAX_IMAGES || g_img_mem >= IMG_MEM_CAP) { emit_img_alt_text(alt); return; }

    ImgCacheEnt *ce = img_cache_get(full);
    if (!ce) { emit_img_alt_text(alt); return; }

    const Bitmap *first = &ce->anim.frames[0];
    int maxw = g_contentW; if (maxw < 16) maxw = 16;
    int maxh = 700;
    int dw = first->width, dh = first->height;
    if (attr_w > 0 && attr_h > 0) { dw = attr_w; dh = attr_h; }   /* honor width/height attrs over intrinsic size */
    if (dw > maxw) { dh = (int)((long)dh * maxw / dw); dw = maxw; }
    if (dh > maxh) { dw = (int)((long)dw * maxh / dh); dh = maxh; }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;

    long per_frame = (long)dw * dh * 4;
    int want = ce->anim.count;
    /* Keep the animation only if the whole scaled strip fits the budget;
     * otherwise show it as a still. */
    if (g_img_mem + per_frame * want > IMG_MEM_CAP) want = 1;
    if (g_img_mem + per_frame > IMG_MEM_CAP) { emit_img_alt_text(alt); return; }

    int idx = g_nimages++;
    RImage *ri = &g_images[idx];
    ri->nframes = 0; ri->total_ms = 0; ri->w = dw; ri->h = dh;

    for (int f = 0; f < want && f < ANIM_MAX_FRAMES; f++) {
        uint32_t *scaled = (uint32_t *)kmalloc((int)per_frame);
        if (!scaled) break;
        const Bitmap *src_bm = &ce->anim.frames[f];
        /* nearest-neighbor resample - fine at these sizes, and cheap */
        for (int y = 0; y < dh; y++) {
            int sy = (int)((long)y * src_bm->height / dh);
            if (sy >= src_bm->height) sy = src_bm->height - 1;
            for (int x = 0; x < dw; x++) {
                int sx = (int)((long)x * src_bm->width / dw);
                if (sx >= src_bm->width) sx = src_bm->width - 1;
                scaled[y * dw + x] = src_bm->pixels[sy * src_bm->stride + sx];
            }
        }
        ri->frames[ri->nframes] = scaled;
        int d = ce->anim.delays_ms[f];
        ri->delays[ri->nframes] = d > 0 ? d : 100;
        ri->total_ms += ri->delays[ri->nframes];
        ri->nframes++;
        g_img_mem += per_frame;
    }

    if (ri->nframes == 0) { g_nimages--; emit_img_alt_text(alt); return; }
    if (ri->nframes > 1) g_has_anim = 1;
    place_image(idx, dw, dh);
}

/* Which frame of an animation is showing right now. */
static int anim_frame(const RImage *im)
{
    if (im->nframes <= 1 || im->total_ms <= 0) return 0;
    int t = (int)(clock_ms() % (unsigned)im->total_ms);
    int acc = 0;
    for (int i = 0; i < im->nframes; i++) {
        acc += im->delays[i];
        if (t < acc) return i;
    }
    return im->nframes - 1;
}

/* --- CSS + JS pre-passes over the DOM before layout --- */

static DomNode *preorder_next(DomNode *n, DomNode *root)
{
    if (n->first_child) return n->first_child;
    while (n && n != root) {
        if (n->next_sibling) return n->next_sibling;
        n = n->parent;
    }
    return 0;
}

static const char *first_text_child(DomNode *el)
{
    for (DomNode *c = el->first_child; c; c = c->next_sibling)
        if (c->type == DOM_NODE_TEXT) return dom_text(c);
    return "";
}

/* Nearest enclosing <form> ancestor, or 0. */
static DomNode *enclosing_form(DomNode *n)
{
    for (DomNode *p = n->parent; p; p = p->parent)
        if (p->type == DOM_NODE_ELEMENT && ieq(p->tag, "form")) return p;
    return 0;
}

static int register_href(const char *resolved)
{
    if (!resolved[0] || g_nlinks >= MAX_LINKS) return -1;
    int li = g_nlinks++;
    scopy(g_links[li], resolved, HREF_CAP);
    return li;
}

/* --- form controls ---------------------------------------------------------
 * Text-like inputs and textareas are live editable boxes; checkboxes, radios
 * and selects hold real state that submit serializes; hidden inputs are
 * recorded (never drawn) so they ride along on submit, which is what most
 * server-rendered forms depend on. */

static int field_kind_of(DomNode *n)
{
    const char *tag  = n->tag;
    const char *type = dom_get_attr(n, "type");

    if (ieq(tag, "textarea")) return FLD_TEXT;
    if (ieq(tag, "select"))   return FLD_SELECT;
    if (ieq(tag, "button")) {
        const char *bt = dom_get_attr(n, "type");
        return (!bt[0] || ieq(bt, "submit")) ? FLD_SUBMIT : FLD_BUTTON;
    }
    if (!ieq(tag, "input")) return FLD_INERT;

    if (ieq(type, "hidden")) return FLD_HIDDEN;
    if (ieq(type, "checkbox")) return FLD_CHECKBOX;
    if (ieq(type, "radio")) return FLD_RADIO;
    if (ieq(type, "password")) return FLD_PASSWORD;
    if (ieq(type, "submit") || ieq(type, "image")) return FLD_SUBMIT;
    if (ieq(type, "button") || ieq(type, "reset")) return FLD_BUTTON;
    if (!type[0] || ieq(type, "text") || ieq(type, "search") || ieq(type, "email") ||
        ieq(type, "url") || ieq(type, "tel") || ieq(type, "number") || ieq(type, "date"))
        return FLD_TEXT;
    return FLD_TEXT;   /* unknown input types behave as text, like browsers do */
}

static void collect_options(DomNode *sel, Field *f)
{
    f->nopts = 0;
    for (DomNode *c = sel->first_child; c && f->nopts < OPT_MAX; c = c->next_sibling) {
        if (c->type != DOM_NODE_ELEMENT || !ieq(c->tag, "option")) continue;
        const char *txt = first_text_child(c);
        const char *val = dom_get_attr(c, "value");
        scopy(f->opts[f->nopts], txt[0] ? txt : val, OPT_LEN);
        f->nopts++;
    }
}

static void emit_field(DomNode *n)
{
    int kind = field_kind_of(n);
    if (kind == FLD_INERT) return;

    int id = g_nfields;
    if (id >= MAX_FIELDS) return;
    g_nfields++;
    Field *f = &g_fields[id];
    f->kind = (uint8_t)kind;
    f->nopts = 0;
    scopy(f->name, dom_get_attr(n, "name"), sizeof(f->name));
    scopy(f->defval, dom_get_attr(n, "value"), sizeof(f->defval));
    scopy(f->ph, dom_get_attr(n, "placeholder"), sizeof(f->ph));

    DomNode *form = enclosing_form(n);
    f->form = form;
    const char *action = form ? dom_get_attr(form, "action") : "";
    if (action[0]) resolve_url(action, f->action, sizeof(f->action));
    else f->action[0] = 0;
    f->method_post = (uint8_t)(form && ieq(dom_get_attr(form, "method"), "post"));

    if (kind == FLD_SELECT) collect_options(n, f);

    /* First layout of this document seeds the state from the markup; later
     * layouts (resize, script mutation) must not stomp what the user typed. */
    if (!g_state_seeded) {
        scopy(g_fval[id], f->defval, FVAL_CAP);
        g_fvlen[id] = slen(g_fval[id]);
        g_fchk[id] = (uint8_t)dom_has_attr(n, "checked");
        g_fsel[id] = 0;
        if (kind == FLD_TEXT || kind == FLD_PASSWORD) {
            /* <textarea>text</textarea> puts its value in the child text */
            if (ieq(n->tag, "textarea")) { scopy(g_fval[id], first_text_child(n), FVAL_CAP); g_fvlen[id] = slen(g_fval[id]); }
        }
    }

    if (kind == FLD_HIDDEN) { f->x = f->y = f->w = f->h = 0; return; }

    /* label text for the things that draw one */
    const char *label = "";
    if (kind == FLD_SUBMIT || kind == FLD_BUTTON) {
        label = f->defval[0] ? f->defval : first_text_child(n);
        if (!label[0]) label = (kind == FLD_SUBMIT) ? "Submit" : "Button";
    } else if (kind == FLD_SELECT) {
        label = (f->nopts > 0 && g_fsel[id] < f->nopts) ? f->opts[g_fsel[id]] : "";
    }

    int bw = 200, bh = 22;
    const char *sz = dom_get_attr(n, "size");
    if (sz[0]) { int c = 0; for (int i = 0; sz[i] >= '0' && sz[i] <= '9'; i++) c = c*10 + (sz[i]-'0'); if (c > 0) bw = c*8 + 16; }
    if (kind == FLD_SELECT) { bw = slen(label)*8 + 40; if (bw < 100) bw = 100; }
    if (kind == FLD_SUBMIT || kind == FLD_BUTTON) { bw = slen(label)*8 + 24; if (bw < 48) bw = 48; }
    if (kind == FLD_CHECKBOX || kind == FLD_RADIO) { bw = 16; bh = 16; }
    if (ieq(n->tag, "textarea")) bh = 44;
    if (bw > g_contentW) bw = g_contentW;

    if (g_cx + bw > g_x0 + g_contentW && g_cx > g_x0 + g_indent) newline();

    f->x = g_cx; f->y = g_cy - 1; f->w = bw; f->h = bh;

    /* control chrome (a background rect the paint pass draws under the text) */
    if (g_nbgs < MAX_BG) {
        BgRect *b = &g_bgs[g_nbgs++];
        b->x = g_cx; b->y = g_cy - 1; b->w = bw; b->h = bh;
        b->color = (kind == FLD_SUBMIT || kind == FLD_BUTTON) ? fb_rgb(224, 228, 238) : fb_rgb(249, 250, 252);
        b->border = 1; b->inline_box = 1;
        b->bcolor = fb_rgb(178, 186, 200);
    }

    if (g_nspans < MAX_SPANS) {
        Span *s = &g_spans[g_nspans++];
        s->x = g_cx; s->y = g_cy - 1; s->w = bw;
        s->fg = C_TEXT; s->bold = 0; s->underline = 0; s->strike = 0; s->rule = 0; s->scale = 1;
        s->hidden = (uint8_t)g_hidden;
        s->link = -1; s->toff = -1; s->img = -1; s->imgh = bh; s->field = id; s->node = n;
        /* buttons/selects draw a static label; the value store drives the rest */
        if (kind == FLD_SUBMIT || kind == FLD_BUTTON) {
            int fit = (bw - 12) / 8; if (fit < 0) fit = 0;
            int ll = slen(label); if (ll > fit) ll = fit;
            s->toff = arena_put(label, ll);
        }
    }
    g_cx += bw + 8;
}

/* --- <video> / <audio> ---
 * We have no codec, so media renders as a labelled box that navigates to the
 * media URL when clicked (the poster image, when present, is shown for real).
 * That beats the old behavior, where the element laid out as nothing. */
static void emit_media(DomNode *n, int is_video)
{
    const char *src = dom_get_attr(n, "src");
    if (!src[0]) {
        for (DomNode *c = n->first_child; c; c = c->next_sibling)
            if (c->type == DOM_NODE_ELEMENT && ieq(c->tag, "source")) {
                src = dom_get_attr(c, "src");
                if (src[0]) break;
            }
    }

    char full[512];
    resolve_url(src, full, sizeof(full));
    int saved_link = g_link;
    if (full[0]) { int li = register_href(full); if (li >= 0) g_link = li; }

    const char *poster = dom_get_attr(n, "poster");
    if (is_video && poster[0]) {
        emit_image(poster, "[video]", 0, 0);
    } else {
        block_break(6);
        int bw = g_contentW > 480 ? 480 : g_contentW;
        int bh = is_video ? 180 : 44;
        if (g_nbgs < MAX_BG) {
            BgRect *b = &g_bgs[g_nbgs++];
            b->x = g_cx; b->y = g_cy; b->w = bw; b->h = bh;
            b->color = is_video ? fb_rgb(28, 32, 40) : fb_rgb(238, 240, 246);
            b->border = 1; b->inline_box = 1; b->bcolor = fb_rgb(90, 98, 112);
        }
        color_t saved_fg = g_fg;
        g_fg = is_video ? fb_rgb(226, 232, 240) : fb_rgb(60, 70, 90);
        int saved_cx = g_cx, saved_cy = g_cy;
        g_cx += 16; g_cy += bh / 2 - 8;
        emit_run(is_video ? "[>] Play video (opens the media URL)"
                          : "[>] Play audio (opens the media URL)", 36);
        g_fg = saved_fg;
        g_cx = saved_cx; g_cy = saved_cy + bh + 8;
        g_line_start = g_nspans;
        g_line_bg = g_nbgs;
        g_line_field = g_nfields;
    }
    g_link = saved_link;
}

static void collect_stylesheets(DomNode *root)
{
    css_reset();
    for (DomNode *n = root->first_child; n; n = preorder_next(n, root)) {
        if (n->type != DOM_NODE_ELEMENT) continue;
        if (ieq(n->tag, "style")) {
            const char *txt = first_text_child(n);
            css_add_stylesheet(txt, slen(txt));
        } else if (ieq(n->tag, "link")) {
            const char *rel = dom_get_attr(n, "rel");
            const char *href = dom_get_attr(n, "href");
            if (href[0] && (ieq(rel, "stylesheet") || starts_ci(rel, "stylesheet"))) {
                char full[512]; resolve_url(href, full, sizeof(full));
                NetResponse r;
                if (full[0] && net_fetch_limited(full, 262144, &r) == 0 && r.body && r.body_len > 0)
                    css_add_stylesheet(r.body, r.body_len);
            }
        }
    }
}

static void run_scripts(DomNode *root)
{
    js_engine_reset();
    js_set_location(g_base_url);
    for (DomNode *n = root->first_child; n; n = preorder_next(n, root)) {
        if (n->type != DOM_NODE_ELEMENT || !ieq(n->tag, "script")) continue;
        const char *type = dom_get_attr(n, "type");
        if (type[0] && !ieq(type, "text/javascript") && !ieq(type, "application/javascript")
            && !ieq(type, "module") && !ieq(type, "text/ecmascript")) continue;
        const char *src = dom_get_attr(n, "src");
        if (src[0]) {
            char full[512]; resolve_url(src, full, sizeof(full));
            NetResponse r;
            if (full[0] && net_fetch_limited(full, 262144, &r) == 0 && r.body && r.body_len > 0)
                js_run(r.body, r.body_len);
        } else {
            const char *txt = first_text_child(n);
            int l = slen(txt);
            if (l > 0) js_run(txt, l);
        }
    }
}

typedef struct {
    color_t fg; int bold, scale, align, underline, strike, link, indent, line_h, pre;
    int transform, hidden, bg_idx, mbottom, pad_bottom, isblock;
} StyleFrame;

#define SF_DEPTH 256
static StyleFrame g_style_stack[SF_DEPTH];
static int        g_sf_top;

static int register_link(DomNode *n)
{
    const char *href = dom_get_attr(n, "href");
    if (!href[0] || g_nlinks >= MAX_LINKS) return -1;
    char full[HREF_CAP]; resolve_url(href, full, sizeof(full));
    if (!full[0]) scopy(full, href, sizeof(full));
    return register_href(full);
}

/* Called on entering a node during the tree walk. Returns 1 if its children
 * should be visited next, 0 if the subtree is fully handled here. */
static int layout_open(DomNode *n)
{
    if (n->type == DOM_NODE_TEXT) { emit_run(dom_text(n), slen(dom_text(n))); return 0; }

    ComputedStyle parent_style, cs;
    parent_style.color = g_fg; parent_style.bold = g_bold; parent_style.scale = g_scale;
    parent_style.align = g_align; parent_style.line_h = g_line_h; parent_style.italic = 0;
    parent_style.text_transform = g_transform; parent_style.hidden = g_hidden;
    css_cascade(n, &parent_style, &cs);

    if (g_sf_top < SF_DEPTH) {
        StyleFrame *f = &g_style_stack[g_sf_top];
        f->fg = g_fg; f->bold = g_bold; f->scale = g_scale; f->align = g_align;
        f->underline = g_underline; f->strike = g_strike; f->link = g_link; f->indent = g_indent;
        f->line_h = g_line_h; f->pre = g_pre; f->transform = g_transform; f->hidden = g_hidden;
        f->bg_idx = -1;
        f->mbottom = cs.margin_bottom; f->pad_bottom = cs.pad_bottom;
        f->isblock = (cs.display == DISP_BLOCK || cs.display == DISP_LIST_ITEM);
        g_sf_top++;
    }

    if (cs.display == DISP_NONE) return 0;

    int is_block = (cs.display == DISP_BLOCK || cs.display == DISP_LIST_ITEM);
    if (is_block) block_break(cs.margin_top + cs.pad_top);

    g_fg = cs.color; g_bold = cs.bold; g_scale = cs.scale; g_line_h = cs.line_h;
    g_align = cs.align; g_underline = cs.underline; g_strike = cs.strike;
    g_transform = cs.text_transform; g_hidden = cs.hidden;
    g_indent += cs.indent;
    if (is_block) { g_cx = g_x0 + g_indent; g_line_start = g_nspans; g_line_bg = g_nbgs; g_line_field = g_nfields; }
    g_cur_node = n;

    if (cs.is_link) { int li = register_link(n); if (li >= 0) g_link = li; }

    /* Block background/border rect: opened here, height filled in on close. */
    if ((cs.has_bg || cs.border_w) && g_nbgs < MAX_BG) {
        BgRect *b = &g_bgs[g_nbgs];
        b->x = g_x0 + (is_block ? 0 : g_indent);
        b->y = g_cy - 2;
        b->w = cs.width_px > 0 && cs.width_px < g_contentW ? cs.width_px : g_contentW;
        b->h = 0;
        b->color = cs.has_bg ? cs.bg : C_BG;
        b->border = (uint8_t)(cs.border_w ? 1 : 0);
        b->inline_box = 0;
        b->bcolor = cs.border_color;
        if (g_sf_top > 0) g_style_stack[g_sf_top - 1].bg_idx = g_nbgs;
        g_nbgs++;
    }

    const char *t = n->tag;
    if (ieq(t, "br")) { newline(); return 0; }
    if (ieq(t, "hr")) { add_rule(); return 0; }
    if (ieq(t, "pre")) g_pre = 1;
    if (ieq(t, "li")) place_word("\x2d", 1);
    if (ieq(t, "img")) {
        const char *src = dom_get_attr(n, "src");
        const char *alt = dom_get_attr(n, "alt");
        int aw = 0, ah = 0;
        const char *ws = dom_get_attr(n, "width");
        const char *hs = dom_get_attr(n, "height");
        for (int i = 0; ws[i] >= '0' && ws[i] <= '9'; i++) aw = aw*10 + (ws[i]-'0');
        for (int i = 0; hs[i] >= '0' && hs[i] <= '9'; i++) ah = ah*10 + (hs[i]-'0');
        emit_image(src, alt, aw, ah);
        return 0;
    }
    if (ieq(t, "video") || ieq(t, "audio")) { emit_media(n, ieq(t, "video")); return 0; }
    if (ieq(t, "input") || ieq(t, "textarea") || ieq(t, "button") || ieq(t, "select")) {
        emit_field(n);
        return 0;
    }
    /* table cells are laid out inline-ish with a separating gap, which reads
     * better than one word-wrapped blob and costs no box tree */
    if (ieq(t, "td") || ieq(t, "th")) { if (g_cx > g_x0 + g_indent) g_cx += 8; }
    return 1;
}

static void layout_close(DomNode *n)
{
    if (n->type != DOM_NODE_ELEMENT) return;
    if (g_sf_top <= 0) return;
    StyleFrame *f = &g_style_stack[--g_sf_top];

    if (f->bg_idx >= 0 && f->bg_idx < g_nbgs) {
        int bottom = g_cy + g_line_h + f->pad_bottom;
        BgRect *b = &g_bgs[f->bg_idx];
        b->h = bottom - b->y;
        if (b->h < 0) b->h = 0;
    }

    if (f->isblock) {
        g_indent = f->indent;   /* restore before the bottom-margin break so spacing lands at parent level */
        block_break(f->mbottom + f->pad_bottom);
    }

    g_fg = f->fg; g_bold = f->bold; g_scale = f->scale; g_align = f->align;
    g_underline = f->underline; g_strike = f->strike; g_link = f->link; g_indent = f->indent;
    g_line_h = f->line_h; g_pre = f->pre; g_transform = f->transform; g_hidden = f->hidden;
    g_cur_node = n->parent;
}

static void layout_tree(DomNode *root)
{
    DomNode *n = root->first_child;
    while (n) {
        int descend = layout_open(n);
        if (descend && n->first_child) { n = n->first_child; continue; }
        layout_close(n);
        while (n != root && !n->next_sibling) {
            n = n->parent;
            if (n == root) break;
            layout_close(n);
        }
        if (n == root || !n) break;
        n = n->next_sibling;
    }
}

static void compute_title(DomNode *root)
{
    g_title[0] = 0; g_title_len = 0;
    for (DomNode *n = root->first_child; n; n = preorder_next(n, root)) {
        if (n->type != DOM_NODE_ELEMENT || !ieq(n->tag, "title")) continue;
        const char *txt = first_text_child(n);
        int i = 0;
        while (txt[i] && g_title_len < 95) {
            char c = txt[i++];
            if (c == '\n' || c == '\t' || c == '\r') c = ' ';
            g_title[g_title_len++] = c;
        }
        g_title[g_title_len] = 0;
        return;
    }
}

/* <meta http-equiv="refresh" content="0; url=..."> - still how a lot of
 * sites move you somewhere else, consent walls especially. Only
 * near-immediate refreshes are honored: a long delay is usually a page that
 * reloads itself forever, and we have nowhere to hang a pending timer that
 * survives navigation. */
static void check_meta_refresh(DomNode *root)
{
    for (DomNode *n = root->first_child; n; n = preorder_next(n, root)) {
        if (n->type != DOM_NODE_ELEMENT || !ieq(n->tag, "meta")) continue;
        if (!ieq(dom_get_attr(n, "http-equiv"), "refresh")) continue;

        /* A refresh inside <noscript> is the page's fallback for browsers
         * that run no scripts at all, and it usually points at a "turn on
         * JavaScript" page. We do run scripts, so honoring it would bounce
         * us off every page that carries one. */
        int in_noscript = 0;
        for (DomNode *p = n->parent; p; p = p->parent)
            if (p->type == DOM_NODE_ELEMENT && ieq(p->tag, "noscript")) { in_noscript = 1; break; }
        if (in_noscript) continue;

        const char *c = dom_get_attr(n, "content");
        int i = 0, delay = 0, saw = 0;
        while (c[i] == ' ') i++;
        while (c[i] >= '0' && c[i] <= '9') { delay = delay*10 + (c[i]-'0'); i++; saw = 1; }
        if (saw && delay > 2) return;

        while (c[i] && c[i] != ';' && c[i] != ',') i++;
        if (c[i]) i++;
        while (c[i] == ' ') i++;
        if (starts_ci(c + i, "url")) {
            i += 3;
            while (c[i] == ' ' || c[i] == '=') i++;
        }
        if (c[i] == '"' || c[i] == '\'') i++;

        char raw[512]; int o = 0;
        while (c[i] && c[i] != '"' && c[i] != '\'' && o < (int)sizeof(raw) - 1) raw[o++] = c[i++];
        while (o > 0 && raw[o-1] == ' ') o--;
        raw[o] = 0;
        if (!raw[0]) return;

        char full[512];
        resolve_url(raw, full, sizeof(full));
        if (full[0] && !ieq(full, g_base_url)) js_request_nav(full);
        return;
    }
}

/* Parse the page bytes into a DOM, collect its stylesheets, and run its
 * scripts. Only called when the document actually changes: node pointers
 * held by script handlers must outlive a mere resize. */
static void build_document(void)
{
    g_root = dom_parse_document(g_page, g_page_len);
    compute_title(g_root);
    collect_stylesheets(g_root);
    run_scripts(g_root);
    compute_title(g_root);       /* scripts may have replaced <body>/its children */
    check_meta_refresh(g_root);
    g_doc_dirty = 0;
    g_pending_load = 1;
    g_state_seeded = 0;          /* controls get their markup defaults again */
}

static void flow(int width)
{
    init_colors();
    free_scaled_images();
    g_nspans = 0; g_arena_len = 0; g_nlinks = 0; g_nbgs = 0; g_nfields = 0;

    g_x0 = 12; g_contentW = width - 24; if (g_contentW < 80) g_contentW = 80;
    g_cx = g_x0; g_cy = 12; g_line_h = 19;
    g_fg = C_TEXT; g_bold = 0; g_scale = 1; g_align = ALIGN_LEFT; g_underline = 0;
    g_strike = 0; g_link = -1; g_indent = 0; g_pre = 0; g_transform = TT_NONE;
    g_hidden = 0; g_sf_top = 0; g_line_start = 0; g_cur_node = 0;
    g_line_bg = 0; g_line_field = 0;

    if (!g_root) return;
    layout_tree(g_root);
    align_line();

    g_doc_h = g_cy + g_line_h + 12;
    g_laid_w = width;
    g_state_seeded = 1;          /* from here on, user state wins over markup */
}

static void relayout(int width)
{
    if (g_doc_dirty) build_document();
    flow(width);
}

void html_set_page(const void *id, unsigned gen, const char *html, int len)
{
    if (id == g_active_id && gen == g_active_gen) return;
    if (len < 0) len = 0;
    if (len > PAGE_CAP - 1) len = PAGE_CAP - 1;
    for (int i = 0; i < len; i++) g_page[i] = html[i];
    g_page_len = len;
    g_page[len] = 0;
    g_active_id = id; g_active_gen = gen;
    g_laid_w = -1;
    g_doc_dirty = 1;
    img_cache_clear();        /* new document: cached images no longer apply */

    /* new document: drop any typed-in field state and clear focus */
    for (int i = 0; i < MAX_FIELDS; i++) { g_fval[i][0] = 0; g_fvlen[i] = 0; g_fchk[i] = 0; g_fsel[i] = 0; }
    g_field_focus = -1;
    g_nfields = 0;
    g_state_seeded = 0;
}

void html_layout(int width, char *title_out, int title_cap)
{
    if (width != g_laid_w || g_doc_dirty) relayout(width);
    if (title_out && title_cap > 0) {
        int i = 0; while (g_title[i] && i < title_cap-1) { title_out[i]=g_title[i]; i++; } title_out[i]=0;
    }
}

int html_doc_height(void) { return g_doc_h; }
int html_animating(void) { return g_has_anim; }

int html_take_nav(char *out, int cap) { return js_take_nav(out, cap); }
int html_take_alert(char *out, int cap) { return js_take_alert(out, cap); }

int html_pump(void)
{
    int dirty = 0;
    if (g_pending_load) { g_pending_load = 0; dirty |= js_fire_load(); }
    dirty |= js_pump();
    if (dirty && g_laid_w > 0) { flow(g_laid_w); return 1; }
    return 0;
}

static void draw_glyph(int x, int y, char c, color_t fg, int bold, int scale)
{
    unsigned char ch = (unsigned char)c;
    if (ch > 127) ch = '?';
    const unsigned char *cov = font8x16aa[ch];
    for (int row = 0; row < 16; row++) {
        for (int col = 0; col < 8; col++) {
            unsigned char t = cov[row * 8 + col];
            if (!t) continue;
            if (scale <= 1) {
                /* full coverage: fb_mix(bg,fg,255)==fg, so skip the read+blend */
                fb_put_pixel(x + col, y + row,
                             t == 255 ? fg : fb_mix(fb_get_pixel(x + col, y + row), fg, t));
                if (bold)
                    fb_put_pixel(x + col + 1, y + row,
                                 t == 255 ? fg : fb_mix(fb_get_pixel(x + col + 1, y + row), fg, t));
            } else {
                int px = x + col * scale, py = y + row * scale;
                for (int dy = 0; dy < scale; dy++)
                    for (int dx = 0; dx < scale; dx++)
                        fb_put_pixel(px + dx, py + dy,
                                     fb_mix(fb_get_pixel(px + dx, py + dy), fg, t));
            }
        }
    }
}

/* Draws the interior of one form control: the value text (masked for
 * passwords), the check/radio mark, or the select's current option. */
static void paint_field(int id, int px, int py, int w, int h)
{
    Field *f = &g_fields[id];
    switch (f->kind) {
        case FLD_CHECKBOX:
        case FLD_RADIO:
            if (g_fchk[id]) {
                int inset = f->kind == FLD_RADIO ? 4 : 3;
                fb_fill_rect(px + inset, py + inset, w - 2*inset, h - 2*inset, fb_rgb(40, 90, 200));
            }
            break;
        case FLD_SELECT: {
            const char *txt = (f->nopts > 0 && g_fsel[id] < f->nopts) ? f->opts[g_fsel[id]] : "";
            int cols = (w - 28) / 8; if (cols < 0) cols = 0;
            int gx = px + 6;
            for (int j = 0; txt[j] && j < cols; j++) { draw_glyph(gx, py + 4, txt[j], C_TEXT, 0, 1); gx += 8; }
            /* the arrow marker, drawn as a small triangle of rows */
            for (int r = 0; r < 4; r++)
                fb_fill_rect(px + w - 16 + r, py + h/2 - 2 + r, 8 - 2*r, 1, fb_rgb(80, 88, 104));
            break;
        }
        default: {
            const char *txt; color_t col;
            char masked[FVAL_CAP];
            if (g_fvlen[id] > 0) {
                if (f->kind == FLD_PASSWORD) {
                    int n = g_fvlen[id]; if (n > FVAL_CAP - 1) n = FVAL_CAP - 1;
                    for (int j = 0; j < n; j++) masked[j] = '*';
                    masked[n] = 0;
                    txt = masked;
                } else txt = g_fval[id];
                col = C_TEXT;
            } else { txt = f->ph; col = fb_rgb(140, 146, 158); }
            int cols = (w - 12) / 8; if (cols < 0) cols = 0;
            int len = slen(txt), from = (len > cols) ? len - cols : 0;
            int gx = px + 6;
            for (int j = from; txt[j]; j++) { draw_glyph(gx, py + 4, txt[j], col, 0, 1); gx += 8; }
            if (id == g_field_focus) fb_fill_rect(gx, py + 3, 1, 16, fb_rgb(30, 30, 30));
            break;
        }
    }
}

void html_paint(int x, int y, int w, int h, int scroll)
{
    init_colors();
    fb_fill_rect(x, y, w, h, C_BG);

    /* backgrounds painted outer-to-inner (creation order), then text/images on top */
    for (int i = 0; i < g_nbgs; i++) {
        BgRect *b = &g_bgs[i];
        if (b->h <= 0) continue;
        int by = b->y - scroll;
        int y0 = by, y1 = by + b->h;
        if (y1 < 0 || y0 > h) continue;
        if (y0 < 0) y0 = 0;
        if (y1 > h) y1 = h;
        fb_fill_rect(x + b->x, y + y0, b->w, y1 - y0, b->color);
        if (b->border) {
            if (by >= 0 && by < h) fb_fill_rect(x + b->x, y + by, b->w, 1, b->bcolor);
            int bot = by + b->h - 1;
            if (bot >= 0 && bot < h) fb_fill_rect(x + b->x, y + bot, b->w, 1, b->bcolor);
            fb_fill_rect(x + b->x, y + y0, 1, y1 - y0, b->bcolor);
            fb_fill_rect(x + b->x + b->w - 1, y + y0, 1, y1 - y0, b->bcolor);
        }
    }

    for (int i = 0; i < g_nspans; i++) {
        Span *s = &g_spans[i];
        if (s->hidden) continue;
        int sh = (s->img >= 0 || s->field >= 0) ? s->imgh : 16 * (s->scale ? s->scale : 1);
        int sy = s->y - scroll;
        if (sy + sh < 0 || sy > h) continue;
        int px = x + s->x, py = y + sy;

        if (s->img >= 0 && s->img < g_nimages) {
            RImage *im = &g_images[s->img];
            const uint32_t *pix = im->frames[anim_frame(im)];
            if (!pix) continue;
            for (int iy = 0; iy < im->h; iy++) {
                int ry = py + iy;
                if (ry < y || ry >= y + h) continue;
                const uint32_t *row = pix + iy * im->w;
                for (int ix = 0; ix < im->w; ix++) {
                    uint32_t p = row[ix];
                    if ((p >> 24) < 16) continue;
                    fb_put_pixel(px + ix, ry, fb_rgb((p>>16)&0xFF, (p>>8)&0xFF, p&0xFF));
                }
            }
            continue;
        }
        if (s->rule) { fb_fill_rect(px, py + 8, s->w, 1, s->fg); continue; }

        if (s->field >= 0 && s->field < g_nfields) {
            if (s->toff >= 0) {
                const char *text = g_arena + s->toff;
                int gx = px + 6;
                for (int j = 0; text[j]; j++) { draw_glyph(gx, py + 3, text[j], C_TEXT, 0, 1); gx += 8; }
            } else {
                paint_field(s->field, px, py, s->w, sh);
            }
            continue;
        }

        const char *text = g_arena + s->toff;
        int gx = px, advance = 8 * (s->scale ? s->scale : 1);
        for (int j = 0; text[j]; j++) { draw_glyph(gx, py, text[j], s->fg, s->bold, s->scale ? s->scale : 1); gx += advance; }
        if (s->underline) fb_fill_rect(px, py + sh - 2, s->w, 1, s->fg);
        if (s->strike) fb_fill_rect(px, py + sh / 2, s->w, 1, s->fg);
    }
}

void html_set_base_url(const char *url)
{
    scopy(g_base_url, url ? url : "", sizeof(g_base_url));
}

const char *html_link_at(int doc_x, int doc_y)
{
    for (int i = 0; i < g_nspans; i++) {
        Span *s = &g_spans[i];
        if (s->link < 0) continue;
        int sh = (s->img >= 0) ? s->imgh : 16 * (s->scale ? s->scale : 1);
        if (doc_x >= s->x && doc_x < s->x + s->w && doc_y >= s->y && doc_y < s->y + sh)
            return g_links[s->link];
    }
    return 0;
}

static DomNode *node_at(int doc_x, int doc_y)
{
    /* last match wins: spans are in document order, so the innermost element
     * covering the point is the one emitted latest */
    DomNode *hit = 0;
    for (int i = 0; i < g_nspans; i++) {
        Span *s = &g_spans[i];
        if (!s->node) continue;
        int sh = (s->img >= 0 || s->field >= 0) ? s->imgh : 16 * (s->scale ? s->scale : 1);
        if (doc_x >= s->x && doc_x < s->x + s->w && doc_y >= s->y && doc_y < s->y + sh)
            hit = s->node;
    }
    return hit;
}

int html_click_script(int doc_x, int doc_y)
{
    DomNode *n = node_at(doc_x, doc_y);
    if (!n) return 0;
    if (!js_has_click_handler(n)) return 0;
    int dirty = js_click(n);
    if (dirty && g_laid_w > 0) flow(g_laid_w);
    return dirty;
}

int html_field_at(int doc_x, int doc_y)
{
    for (int i = 0; i < g_nfields; i++) {
        Field *f = &g_fields[i];
        if (f->kind == FLD_HIDDEN || f->w <= 0) continue;
        if (doc_x >= f->x && doc_x < f->x + f->w && doc_y >= f->y && doc_y < f->y + f->h) return i;
    }
    return -1;
}

int html_field_kind(int id)
{
    if (id < 0 || id >= g_nfields) return FK_NONE;
    switch (g_fields[id].kind) {
        case FLD_TEXT: case FLD_PASSWORD: return FK_TEXT;
        case FLD_SUBMIT: case FLD_BUTTON: return FK_SUBMIT;
        case FLD_CHECKBOX: case FLD_RADIO: return FK_CHECK;
        case FLD_SELECT: return FK_SELECT;
        default: return FK_NONE;
    }
}

void html_field_focus(int id)
{
    g_field_focus = (id >= 0 && id < g_nfields &&
                     (g_fields[id].kind == FLD_TEXT || g_fields[id].kind == FLD_PASSWORD)) ? id : -1;
}

int html_field_focused(void) { return g_field_focus; }

void html_field_key(char c)
{
    int id = g_field_focus;
    if (id < 0 || id >= g_nfields) return;
    if (g_fields[id].kind != FLD_TEXT && g_fields[id].kind != FLD_PASSWORD) return;
    if (c == '\b') {
        if (g_fvlen[id] > 0) g_fval[id][--g_fvlen[id]] = 0;
    } else if (c >= 32 && c < 127 && g_fvlen[id] < FVAL_CAP - 1) {
        g_fval[id][g_fvlen[id]++] = c;
        g_fval[id][g_fvlen[id]] = 0;
    }
}

void html_field_toggle(int id)
{
    if (id < 0 || id >= g_nfields) return;
    Field *f = &g_fields[id];
    if (f->kind == FLD_CHECKBOX) { g_fchk[id] = (uint8_t)!g_fchk[id]; return; }
    if (f->kind == FLD_RADIO) {
        /* radios are exclusive within (form, name) */
        for (int j = 0; j < g_nfields; j++)
            if (g_fields[j].kind == FLD_RADIO && g_fields[j].form == f->form &&
                ieq(g_fields[j].name, f->name)) g_fchk[j] = 0;
        g_fchk[id] = 1;
        return;
    }
    if (f->kind == FLD_SELECT && f->nopts > 0) {
        g_fsel[id] = (g_fsel[id] + 1) % f->nopts;
        return;
    }
}

static int append_raw(char *out, int pos, int cap, const char *s)
{
    while (*s && pos < cap - 1) out[pos++] = *s++;
    out[pos] = 0;
    return pos;
}

/* percent-encode a query component; space becomes '+' */
static int append_enc(char *out, int pos, int cap, const char *s)
{
    const char *hex = "0123456789ABCDEF";
    for (; *s && pos < cap - 1; s++) {
        char c = *s;
        int unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                         (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved) { out[pos++] = c; }
        else if (c == ' ') { out[pos++] = '+'; }
        else if (pos < cap - 3) {
            out[pos++] = '%';
            out[pos++] = hex[(c >> 4) & 0xF];
            out[pos++] = hex[c & 0xF];
        }
    }
    out[pos] = 0;
    return pos;
}

/* Serializes every successful control in the same form as field `id`,
 * per the HTML rules that matter here: unchecked boxes and radios are
 * omitted, hidden inputs are included, an unnamed control is skipped. */
static int build_query(int id, char *out, int cap)
{
    Field *f = &g_fields[id];
    int pos = 0, wrote = 0;
    out[0] = 0;
    for (int j = 0; j < g_nfields; j++) {
        Field *g = &g_fields[j];
        if (g->form != f->form || !g->name[0]) continue;

        const char *value = "";
        switch (g->kind) {
            case FLD_TEXT: case FLD_PASSWORD:
                value = g_fvlen[j] ? g_fval[j] : "";
                break;
            case FLD_HIDDEN:
                value = g->defval;
                break;
            case FLD_CHECKBOX: case FLD_RADIO:
                if (!g_fchk[j]) continue;
                value = g->defval[0] ? g->defval : "on";
                break;
            case FLD_SELECT:
                value = (g->nopts > 0 && g_fsel[j] < g->nopts) ? g->opts[g_fsel[j]] : "";
                break;
            case FLD_SUBMIT:
                if (j != id) continue;          /* only the clicked button submits its value */
                if (!g->defval[0]) continue;
                value = g->defval;
                break;
            default:
                continue;
        }
        if (wrote) pos = append_raw(out, pos, cap, "&");
        pos = append_enc(out, pos, cap, g->name);
        pos = append_raw(out, pos, cap, "=");
        pos = append_enc(out, pos, cap, value);
        wrote = 1;
    }
    return pos;
}

int html_field_submit(int id, char *url_out, int url_cap, char *body_out, int body_cap)
{
    if (id < 0) id = g_field_focus;
    if (id < 0 || id >= g_nfields || url_cap <= 1) return 0;

    Field *f = &g_fields[id];
    char base[512];
    scopy(base, f->action[0] ? f->action : g_base_url, sizeof(base));
    if (!base[0]) return 0;

    char query[1024];
    build_query(id, query, sizeof(query));

    if (f->method_post) {
        scopy(url_out, base, url_cap);
        if (body_out && body_cap > 0) scopy(body_out, query, body_cap);
        return 2;
    }

    /* GET: the query replaces anything already on the action URL */
    for (int i = 0; base[i]; i++) if (base[i] == '?' || base[i] == '#') { base[i] = 0; break; }
    int pos = append_raw(url_out, 0, url_cap, base);
    if (query[0]) {
        pos = append_raw(url_out, pos, url_cap, "?");
        append_raw(url_out, pos, url_cap, query);
    }
    if (body_out && body_cap > 0) body_out[0] = 0;
    return 1;
}
