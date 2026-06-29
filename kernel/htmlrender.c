/* kernel/htmlrender.c
 * -----------------------------------------------------------------------------
 * The browser's layout + paint engine. It now works off a real DOM tree:
 *
 *   HTML bytes --domparse--> DOM tree
 *                  |--> collect <style>/<link> --css--> rule list
 *                  |--> run <script> --js--> mutate DOM
 *                  '--> tree walk + css_cascade --> flowed spans --> framebuffer
 *
 * Layout is still a single flowed run of "spans" (text/image/rule) so painting
 * and hit-testing stay cheap, but every span's colour, weight, size (scaled
 * glyphs), alignment and block backgrounds come from the cascaded computed
 * style of the element it belongs to. Decoded images are cached per-URL so a
 * re-layout doesn't re-hit the network.
 * -----------------------------------------------------------------------------
 */
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

/* ========================================================================== */
/* capacities                                                                 */
/* ========================================================================== */

#define PAGE_CAP   262144
#define MAX_SPANS  18000
#define ARENA_CAP  400000
#define MAX_LINKS  2400
#define HREF_CAP   256
#define MAX_BG     1024

typedef struct {
    int      x, y, w;
    color_t  fg;
    uint8_t  bold;
    uint8_t  underline;
    uint8_t  rule;
    uint8_t  scale;         /* glyph pixel scale (1 or 2)                  */
    int      link;          /* index into g_links or -1                    */
    int      toff;          /* offset into g_arena, or -1                  */
    int      img;           /* index into g_images, or -1                  */
    int      imgh;          /* image draw height (px)                      */
} Span;

typedef struct { int x, y, w, h; color_t color; } BgRect;

/* Decoded, downscaled images referenced by image spans (per layout). */
#define MAX_IMAGES   48
#define IMG_MEM_CAP  (48 * 1024 * 1024)
typedef struct { uint32_t *pixels; int w, h; } RImage;
static RImage g_images[MAX_IMAGES];
static int    g_nimages = 0;
static long   g_img_mem = 0;

/* Per-URL cache of full decoded bitmaps, persists across re-layouts of the
 * same document and is cleared when the page changes. */
#define IMG_CACHE_N    40
#define IMG_CACHE_MEM  (32 * 1024 * 1024)
typedef struct { char url[256]; uint32_t *px; int w, h; int valid; } ImgCacheEnt;
static ImgCacheEnt g_icache[IMG_CACHE_N];
static long        g_icache_mem = 0;

static void free_images(void)
{
    for (int i = 0; i < g_nimages; i++) if (g_images[i].pixels) kfree(g_images[i].pixels);
    g_nimages = 0;
    g_img_mem = 0;
}
static void img_cache_clear(void)
{
    for (int i = 0; i < IMG_CACHE_N; i++) {
        if (g_icache[i].px) kfree(g_icache[i].px);
        g_icache[i].px = 0; g_icache[i].valid = 0; g_icache[i].url[0] = 0;
    }
    g_icache_mem = 0;
}

static char     g_page[PAGE_CAP];
static char     g_base_url[256];
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

/* ========================================================================== */
/* colours                                                                    */
/* ========================================================================== */

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

/* ========================================================================== */
/* tiny string helpers                                                        */
/* ========================================================================== */

static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static int  ieq(const char *a, const char *b) { int i=0; while(a[i]&&b[i]){ if(lc(a[i])!=lc(b[i])) return 0; i++; } return lc(a[i])==lc(b[i]); }
static int  starts_ci(const char *s, const char *p){ int i=0; while(p[i]){ if(lc(s[i])!=lc(p[i])) return 0; i++; } return 1; }
static int  slen(const char *s){ int n=0; while(s&&s[n]) n++; return n; }
static void scopy(char *d, const char *s, int cap){ int i=0; if(cap<=0)return; while(s&&s[i]&&i<cap-1){ d[i]=s[i]; i++; } d[i]=0; }

static void resolve_url(const char *href, char *out, int cap)
{
    if (!href || !href[0]) { out[0]=0; return; }
    if (starts_ci(href, "http://") || starts_ci(href, "https://")) { scopy(out, href, cap); return; }
    if (starts_ci(href, "//")) {                       /* protocol-relative */
        int sc = starts_ci(g_base_url, "http://") ? 1 : 0;
        scopy(out, sc ? "http:" : "https:", cap);
        int o = slen(out);
        for (int i = 0; href[i] && o < cap-1; i++) out[o++] = href[i];
        out[o] = 0; return;
    }
    if (!g_base_url[0]) { scopy(out, href, cap); return; }

    if (href[0] == '/') {
        int o = 0;
        int sc = starts_ci(g_base_url, "http://") ? 7 : starts_ci(g_base_url, "https://") ? 8 : 0;
        for (int i = 0; i < sc && o < cap-1; i++) out[o++] = g_base_url[i];
        int hs = sc;
        while (g_base_url[hs] && g_base_url[hs] != '/' && o < cap-1) out[o++] = g_base_url[hs++];
        for (int i = 0; href[i] && o < cap-1; i++) out[o++] = href[i];
        out[o] = 0;
        if (!sc) scopy(out, href, cap);
        return;
    }

    char base[256]; scopy(base, g_base_url, sizeof(base));
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
    if (g_arena_len + len + 1 > ARENA_CAP) return -1;
    int off = g_arena_len;
    for (int i = 0; i < len; i++) g_arena[off + i] = s[i];
    g_arena[off + len] = 0;
    g_arena_len += len + 1;
    return off;
}

/* ========================================================================== */
/* layout state                                                               */
/* ========================================================================== */

static int     g_x0, g_contentW;
static int     g_cx, g_cy;
static int     g_line_h;
static color_t g_fg;
static int     g_bold, g_scale, g_align, g_underline, g_link, g_indent, g_pre;
static int     g_line_start;     /* first span index of the current line    */

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
        int freep = (right - line_left) - used;
        if (freep > 0) {
            int sh = (g_align == ALIGN_CENTER) ? freep / 2 : freep;
            for (int i = g_line_start; i < g_nspans; i++)
                if (!g_spans[i].rule) g_spans[i].x += sh;
        }
    }
    g_line_start = g_nspans;
}

static void newline(void) { align_line(); g_cx = g_x0 + g_indent; g_cy += g_line_h; }
static void block_break(int top) { if (g_cx > g_x0 + g_indent) newline(); else align_line(); g_cy += top; }

static void add_span(int w, int toff)
{
    if (g_nspans >= MAX_SPANS) return;
    Span *s = &g_spans[g_nspans++];
    s->x = g_cx; s->y = g_cy; s->w = w;
    s->fg = g_fg; s->bold = (uint8_t)g_bold; s->underline = (uint8_t)(g_underline || g_link >= 0);
    s->rule = 0; s->scale = (uint8_t)g_scale; s->link = g_link; s->toff = toff; s->img = -1; s->imgh = 0;
}

/* word/space metrics depend on the active glyph scale */
static int cw(void)  { return 8 * g_scale; }

static void place_word(const char *word, int wl)
{
    if (wl <= 0) return;
    int ww = wl * cw();
    if (g_cx + ww > g_x0 + g_contentW && g_cx > g_x0 + g_indent) newline();
    int off = arena_put(word, wl);
    if (off < 0) return;
    add_span(ww, off);
    g_cx += ww + cw();
}

static void add_rule(void)
{
    block_break(6);
    if (g_nspans < MAX_SPANS) {
        Span *s = &g_spans[g_nspans++];
        s->x = g_x0; s->y = g_cy; s->w = g_contentW;
        s->fg = C_RULE; s->bold = 0; s->underline = 0; s->rule = 1; s->scale = 1;
        s->link = -1; s->toff = -1; s->img = -1; s->imgh = 0;
    }
    g_line_start = g_nspans;
    g_cy += 10;
}

/* ========================================================================== */
/* entity decoding + text emission                                            */
/* ========================================================================== */

static int decode_entity(const char *s, int len, int *adv)
{
    if (len <= 0) { *adv = 0; return '&'; }
    if (s[0] == '#') {
        int i = 1, val = 0, hex = 0;
        if (i < len && (s[i] == 'x' || s[i] == 'X')) { hex = 1; i++; }
        while (i < len && s[i] != ';') {
            char c = s[i];
            if (hex) {
                if (c>='0'&&c<='9') val = val*16 + (c-'0');
                else if (c>='a'&&c<='f') val = val*16 + (10+c-'a');
                else if (c>='A'&&c<='F') val = val*16 + (10+c-'A');
                else break;
            } else { if (c>='0'&&c<='9') val = val*10 + (c-'0'); else break; }
            i++;
        }
        if (i < len && s[i] == ';') i++;
        *adv = i;
        if (val == 0 || val > 255) return (val==8217||val==8216) ? '\'' :
                                          (val==8220||val==8221) ? '"' :
                                          (val==8211||val==8212) ? '-' : ' ';
        return val;
    }
    static const struct { const char *n; char c; } ents[] = {
        {"amp;",'&'},{"lt;",'<'},{"gt;",'>'},{"quot;",'"'},{"apos;",'\''},
        {"nbsp;",' '},{"copy;",'c'},{"reg;",'r'},{"mdash;",'-'},{"ndash;",'-'},
        {"hellip;",'.'},{"rsquo;",'\''},{"lsquo;",'\''},{"rdquo;",'"'},{"ldquo;",'"'},
        {"middot;",'.'},{"trade;",'t'},{"#39;",'\''}
    };
    for (int e = 0; e < (int)(sizeof(ents)/sizeof(ents[0])); e++) {
        const char *n = ents[e].n; int nl = 0; while (n[nl]) nl++;
        int ok = 1;
        for (int j = 0; j < nl; j++) if (j >= len || lc(s[j]) != lc(n[j])) { ok = 0; break; }
        if (ok) { *adv = nl; return ents[e].c; }
    }
    *adv = 0;
    return '&';
}

static char g_word[2048];
static void emit_run(const char *s, int len)
{
    int wl = 0;
    for (int i = 0; i < len; i++) {
        char c = s[i];
        if (c == '&') {
            int adv = 0;
            int ch = decode_entity(s + i + 1, len - i - 1, &adv);
            i += adv;
            if (ch == ' ' || ch == '\n' || ch == '\t') { if (wl) { place_word(g_word, wl); wl = 0; } }
            else if (wl < (int)sizeof(g_word)-1) g_word[wl++] = (char)ch;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r' || (c == '\n' && !g_pre)) {
            if (wl) { place_word(g_word, wl); wl = 0; }
            continue;
        }
        if (c == '\n' && g_pre) { if (wl){place_word(g_word,wl);wl=0;} newline(); continue; }
        if (wl < (int)sizeof(g_word)-1) g_word[wl++] = c;
    }
    if (wl) place_word(g_word, wl);
}

/* ========================================================================== */
/* images                                                                     */
/* ========================================================================== */

static void emit_img_alt(const char *alt)
{
    color_t save = g_fg; g_fg = fb_rgb(120, 128, 140);
    if (alt && alt[0]) emit_run(alt, slen(alt));
    else emit_run("[img]", 5);
    g_fg = save;
}

static void place_image(int idx, int w, int h)
{
    if (g_nspans >= MAX_SPANS) return;
    if (g_cx > g_x0 + g_indent) newline(); else align_line();
    Span *s = &g_spans[g_nspans++];
    s->x = g_x0 + g_indent; s->y = g_cy; s->w = w;
    s->fg = C_TEXT; s->bold = 0; s->underline = 0; s->rule = 0; s->scale = 1;
    s->link = g_link; s->toff = -1; s->img = idx; s->imgh = h;
    align_line();                          /* centre/right standalone images */
    g_cy += h + 6;
    g_cx = g_x0 + g_indent;
}

/* Look up (or fetch+decode) a full-resolution bitmap for url. */
static ImgCacheEnt *img_cache_get(const char *url)
{
    for (int i = 0; i < IMG_CACHE_N; i++)
        if (g_icache[i].valid && ieq(g_icache[i].url, url)) return &g_icache[i];

    NetResponse r;
    if (net_fetch_limited(url, 1400000, &r) != 0 || !r.body || r.body_len <= 0) return 0;
    Bitmap bmp;
    if (img_decode((const uint8_t *)r.body, r.body_len, &bmp) != 0 || !bmp.pixels) return 0;

    /* capture dimensions BEFORE bmp_free (which zeroes the struct) */
    int bw = bmp.width, bh = bmp.height, bstride = bmp.stride;
    long bytes = (long)bw * bh * 4;
    if (bytes <= 0 || g_icache_mem + bytes > IMG_CACHE_MEM) { bmp_free(&bmp); return 0; }

    int slot = -1;
    for (int i = 0; i < IMG_CACHE_N; i++) if (!g_icache[i].valid) { slot = i; break; }
    if (slot < 0) { bmp_free(&bmp); return 0; }

    uint32_t *px = (uint32_t *)kmalloc((int)bytes);
    if (!px) { bmp_free(&bmp); return 0; }
    for (int y = 0; y < bh; y++)
        for (int x = 0; x < bw; x++)
            px[y * bw + x] = bmp.pixels[y * bstride + x];
    bmp_free(&bmp);

    ImgCacheEnt *e = &g_icache[slot];
    scopy(e->url, url, sizeof(e->url));
    e->px = px; e->w = bw; e->h = bh; e->valid = 1;
    g_icache_mem += bytes;
    return e;
}

static void emit_image(const char *src, const char *alt, int attr_w, int attr_h)
{
    char full[256];
    resolve_url(src, full, sizeof(full));
    if (!full[0] || g_nimages >= MAX_IMAGES || g_img_mem >= IMG_MEM_CAP) { emit_img_alt(alt); return; }

    ImgCacheEnt *ce = img_cache_get(full);
    if (!ce) { emit_img_alt(alt); return; }

    int maxw = g_contentW; if (maxw < 16) maxw = 16;
    int maxh = 700;
    int dw = ce->w, dh = ce->h;
    if (attr_w > 0 && attr_h > 0) { dw = attr_w; dh = attr_h; }   /* respect width/height attrs */
    if (dw > maxw) { dh = (int)((long)dh * maxw / dw); dw = maxw; }
    if (dh > maxh) { dw = (int)((long)dw * maxh / dh); dh = maxh; }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;

    long bytes = (long)dw * dh * 4;
    if (g_img_mem + bytes > IMG_MEM_CAP) { emit_img_alt(alt); return; }
    uint32_t *scaled = (uint32_t *)kmalloc((int)bytes);
    if (!scaled) { emit_img_alt(alt); return; }

    for (int y = 0; y < dh; y++) {
        int sy = (int)((long)y * ce->h / dh);
        for (int x = 0; x < dw; x++) {
            int sx = (int)((long)x * ce->w / dw);
            scaled[y * dw + x] = ce->px[sy * ce->w + sx];
        }
    }

    int idx = g_nimages++;
    g_images[idx].pixels = scaled; g_images[idx].w = dw; g_images[idx].h = dh;
    g_img_mem += bytes;
    place_image(idx, dw, dh);
}

/* ========================================================================== */
/* CSS + JS pre-passes                                                        */
/* ========================================================================== */

static DomNode *preorder_next(DomNode *n, DomNode *root)
{
    if (n->first_child) return n->first_child;
    while (n && n != root) {
        if (n->next_sibling) return n->next_sibling;
        n = n->parent;
    }
    return 0;
}

static const char *first_text(DomNode *el)
{
    for (DomNode *c = el->first_child; c; c = c->next_sibling)
        if (c->type == DOM_NODE_TEXT) return dom_text(c);
    return "";
}

static void collect_css(DomNode *root)
{
    css_reset();
    for (DomNode *n = root->first_child; n; n = preorder_next(n, root)) {
        if (n->type != DOM_NODE_ELEMENT) continue;
        if (ieq(n->tag, "style")) {
            const char *txt = first_text(n);
            css_add_stylesheet(txt, slen(txt));
        } else if (ieq(n->tag, "link")) {
            const char *rel = dom_get_attr(n, "rel");
            const char *href = dom_get_attr(n, "href");
            if (href[0] && (ieq(rel, "stylesheet") || starts_ci(rel, "stylesheet"))) {
                char full[256]; resolve_url(href, full, sizeof(full));
                NetResponse r;
                if (full[0] && net_fetch_limited(full, 196608, &r) == 0 && r.body && r.body_len > 0)
                    css_add_stylesheet(r.body, r.body_len);
            }
        }
    }
}

static void run_scripts(DomNode *root)
{
    js_engine_reset();
    for (DomNode *n = root->first_child; n; n = preorder_next(n, root)) {
        if (n->type != DOM_NODE_ELEMENT || !ieq(n->tag, "script")) continue;
        const char *type = dom_get_attr(n, "type");
        if (type[0] && !ieq(type, "text/javascript") && !ieq(type, "application/javascript")
            && !ieq(type, "module") && !ieq(type, "text/ecmascript")) continue;
        const char *src = dom_get_attr(n, "src");
        if (src[0]) {
            char full[256]; resolve_url(src, full, sizeof(full));
            NetResponse r;
            if (full[0] && net_fetch_limited(full, 131072, &r) == 0 && r.body && r.body_len > 0)
                js_run(r.body, r.body_len);
        } else {
            const char *txt = first_text(n);
            int l = slen(txt);
            if (l > 0) js_run(txt, l);
        }
    }
}

/* ========================================================================== */
/* DOM -> layout                                                              */
/* ========================================================================== */

typedef struct {
    color_t fg; int bold, scale, align, underline, link, indent, line_h, pre;
    int bg_idx, mbottom, isblock;
} StyleFrame;

#define SF_DEPTH 256
static StyleFrame g_sf[SF_DEPTH];
static int        g_sfp;

static int register_link(DomNode *n)
{
    const char *href = dom_get_attr(n, "href");
    if (!href[0] || g_nlinks >= MAX_LINKS) return -1;
    char full[HREF_CAP]; resolve_url(href, full, sizeof(full));
    if (!full[0]) scopy(full, href, sizeof(full));
    int li = g_nlinks++;
    scopy(g_links[li], full, HREF_CAP);
    return li;
}

/* returns 1 if children should be laid out, 0 to skip the subtree */
static int layout_open(DomNode *n)
{
    if (n->type == DOM_NODE_TEXT) { emit_run(dom_text(n), slen(dom_text(n))); return 0; }

    ComputedStyle parent, cs;
    parent.color = g_fg; parent.bold = g_bold; parent.scale = g_scale;
    parent.align = g_align; parent.line_h = g_line_h; parent.italic = 0;
    css_cascade(n, &parent, &cs);

    /* save frame */
    if (g_sfp < SF_DEPTH) {
        StyleFrame *f = &g_sf[g_sfp];
        f->fg = g_fg; f->bold = g_bold; f->scale = g_scale; f->align = g_align;
        f->underline = g_underline; f->link = g_link; f->indent = g_indent;
        f->line_h = g_line_h; f->pre = g_pre; f->bg_idx = -1;
        f->mbottom = cs.margin_bottom; f->isblock = (cs.display == DISP_BLOCK || cs.display == DISP_LIST_ITEM);
        g_sfp++;
    }

    if (cs.display == DISP_NONE) return 0;

    int isblock = (cs.display == DISP_BLOCK || cs.display == DISP_LIST_ITEM);
    if (isblock) block_break(cs.margin_top);

    /* apply computed inline style to the flow */
    g_fg = cs.color; g_bold = cs.bold; g_scale = cs.scale; g_line_h = cs.line_h;
    g_align = cs.align; g_underline = cs.underline;
    g_indent += cs.indent;
    if (isblock) { g_cx = g_x0 + g_indent; g_line_start = g_nspans; }

    if (cs.is_link) { int li = register_link(n); if (li >= 0) g_link = li; }

    /* block background rectangle (filled in at close) */
    if (cs.has_bg && g_nbgs < MAX_BG) {
        BgRect *b = &g_bgs[g_nbgs];
        b->x = g_x0; b->y = g_cy - 2; b->w = g_contentW; b->h = 0; b->color = cs.bg;
        if (g_sfp > 0) g_sf[g_sfp - 1].bg_idx = g_nbgs;
        g_nbgs++;
    }

    /* element-specific imperative bits */
    const char *t = n->tag;
    if (ieq(t, "br")) { newline(); return 0; }
    if (ieq(t, "hr")) { add_rule(); return 0; }
    if (ieq(t, "pre")) g_pre = 1;
    if (ieq(t, "li")) { place_word("\x2d", 1); }
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
    return 1;
}

static void layout_close(DomNode *n)
{
    if (n->type != DOM_NODE_ELEMENT) return;
    if (g_sfp <= 0) return;
    StyleFrame *f = &g_sf[--g_sfp];

    /* finalize block background height */
    if (f->bg_idx >= 0 && f->bg_idx < g_nbgs) {
        if (g_cx > g_x0 + g_indent) { /* there is content on the current line */ }
        int bottom = g_cy + g_line_h;
        BgRect *b = &g_bgs[f->bg_idx];
        b->h = bottom - b->y;
        if (b->h < 0) b->h = 0;
    }

    if (f->isblock) {
        /* restore indent before the bottom break so spacing is at parent level */
        g_indent = f->indent;
        block_break(f->mbottom);
    }

    g_fg = f->fg; g_bold = f->bold; g_scale = f->scale; g_align = f->align;
    g_underline = f->underline; g_link = f->link; g_indent = f->indent;
    g_line_h = f->line_h; g_pre = f->pre;
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
        if (n->type == DOM_NODE_ELEMENT && ieq(n->tag, "title")) {
            const char *txt = first_text(n);
            int i = 0; while (txt[i] && g_title_len < 95) {
                char c = txt[i++];
                if (c == '\n' || c == '\t' || c == '\r') c = ' ';
                g_title[g_title_len++] = c;
            }
            g_title[g_title_len] = 0;
            return;
        }
    }
}

/* ========================================================================== */
/* the relayout driver                                                        */
/* ========================================================================== */

static void relayout(int width)
{
    init_colors();
    free_images();
    g_nspans = 0; g_arena_len = 0; g_nlinks = 0; g_nbgs = 0;

    g_x0 = 12; g_contentW = width - 24; if (g_contentW < 80) g_contentW = 80;
    g_cx = g_x0; g_cy = 12; g_line_h = 19;
    g_fg = C_TEXT; g_bold = 0; g_scale = 1; g_align = ALIGN_LEFT; g_underline = 0;
    g_link = -1; g_indent = 0; g_pre = 0; g_sfp = 0; g_line_start = 0;

    DomNode *root = dom_parse_document(g_page, g_page_len);
    compute_title(root);
    collect_css(root);
    run_scripts(root);
    /* scripts may have replaced the body/children; re-derive title */
    compute_title(root);

    layout_tree(root);
    align_line();

    g_doc_h = g_cy + g_line_h + 12;
    g_laid_w = width;
}

/* ========================================================================== */
/* public API                                                                 */
/* ========================================================================== */

void html_set_page(const void *id, unsigned gen, const char *html, int len)
{
    if (id == g_active_id && gen == g_active_gen) return;
    if (len > PAGE_CAP - 1) len = PAGE_CAP - 1;
    for (int i = 0; i < len; i++) g_page[i] = html[i];
    g_page_len = len;
    g_page[len] = 0;
    g_active_id = id; g_active_gen = gen;
    g_laid_w = -1;            /* force re-layout */
    img_cache_clear();        /* new document: drop cached images */
}

void html_layout(int width, char *title_out, int title_cap)
{
    if (width != g_laid_w) relayout(width);
    if (title_out && title_cap > 0) {
        int i = 0; while (g_title[i] && i < title_cap-1) { title_out[i]=g_title[i]; i++; } title_out[i]=0;
    }
}

int html_doc_height(void) { return g_doc_h; }

static void draw_glyph(int x, int y, char c, color_t fg, int bold, int scale)
{
    unsigned char uc = (unsigned char)c;
    if (uc > 127) uc = '?';
    const unsigned char *g = font8x16[uc];
    for (int row = 0; row < 16; row++) {
        unsigned char bits = g[row];
        if (!bits) continue;
        for (int col = 0; col < 8; col++) {
            if (!(bits & (0x80 >> col))) continue;
            if (scale <= 1) {
                fb_put_pixel(x + col, y + row, fg);
                if (bold) fb_put_pixel(x + col + 1, y + row, fg);
            } else {
                int px = x + col * scale, py = y + row * scale;
                for (int dy = 0; dy < scale; dy++)
                    for (int dx = 0; dx < scale; dx++)
                        fb_put_pixel(px + dx, py + dy, fg);
            }
        }
    }
}

void html_paint(int x, int y, int w, int h, int scroll)
{
    init_colors();
    fb_fill_rect(x, y, w, h, C_BG);

    /* block backgrounds first (outer-to-inner = creation order) */
    for (int i = 0; i < g_nbgs; i++) {
        BgRect *b = &g_bgs[i];
        if (b->h <= 0) continue;
        int by = b->y - scroll;
        int y0 = by, y1 = by + b->h;
        if (y1 < 0 || y0 > h) continue;
        if (y0 < 0) y0 = 0;
        if (y1 > h) y1 = h;
        fb_fill_rect(x + b->x, y + y0, b->w, y1 - y0, b->color);
    }

    for (int i = 0; i < g_nspans; i++) {
        Span *s = &g_spans[i];
        int sh = (s->img >= 0) ? s->imgh : 16 * (s->scale ? s->scale : 1);
        int sy = s->y - scroll;
        if (sy + sh < 0 || sy > h) continue;
        int px = x + s->x, py = y + sy;
        if (s->img >= 0 && s->img < g_nimages) {
            RImage *im = &g_images[s->img];
            for (int iy = 0; iy < im->h; iy++) {
                int ry = py + iy;
                if (ry < y || ry >= y + h) continue;
                const uint32_t *row = im->pixels + iy * im->w;
                for (int ix = 0; ix < im->w; ix++) {
                    uint32_t p = row[ix];
                    if ((p >> 24) < 16) continue;          /* transparent */
                    fb_put_pixel(px + ix, ry, fb_rgb((p>>16)&0xFF, (p>>8)&0xFF, p&0xFF));
                }
            }
            continue;
        }
        if (s->rule) { fb_fill_rect(px, py + 8, s->w, 1, s->fg); continue; }
        const char *t = g_arena + s->toff;
        int gx = px, adv = 8 * (s->scale ? s->scale : 1);
        for (int j = 0; t[j]; j++) { draw_glyph(gx, py, t[j], s->fg, s->bold, s->scale ? s->scale : 1); gx += adv; }
        if (s->underline) fb_fill_rect(px, py + sh - 2, s->w, 1, s->fg);
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
