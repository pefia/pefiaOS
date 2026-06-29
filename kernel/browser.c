#include "browser.h"
#include "framebuffer.h"
#include "console.h"
#include "heap.h"
#include "net.h"
#include "htmlrender.h"

#define URL_CAP   256
#define PAGE_CAP  196608
#define HIST_CAP  16
#define TOOLBAR_H 40
#define STATUS_H  20
#define SCROLLBAR 12

typedef struct BrowserState {
    char     url[URL_CAP];
    int      url_len;
    char     title[96];
    char     status[120];

    int      address_focused;
    int      caret_tick;

    int      scroll;
    int      doc_h;
    unsigned gen;

    char     hist[HIST_CAP][URL_CAP];
    int      hist_count, hist_pos;

    /* last painted content rectangle (absolute screen coords) for overlays */
    int      scr_x, scr_y, scr_w, scr_h;

    int      page_len;
    char     page[PAGE_CAP];
} BrowserState;

/* ------------------------------------------------------------------------- */
static int  slen(const char *s){int n=0;while(s[n])n++;return n;}
static void scopy(char *d,const char *s,int cap){int i=0;if(cap<=0)return;while(s&&s[i]&&i<cap-1){d[i]=s[i];i++;}d[i]=0;}
static int  seq(const char *a,const char *b){int i=0;while(a[i]&&b[i]){if(a[i]!=b[i])return 0;i++;}return a[i]==b[i];}
static int  starts(const char *s,const char *p){int i=0;while(p[i]){if(s[i]!=p[i])return 0;i++;}return 1;}
static char lc(char c){return (c>='A'&&c<='Z')?(char)(c+32):c;}
static int  starts_ci(const char *s,const char *p){int i=0;while(p[i]){if(lc(s[i])!=lc(p[i]))return 0;i++;}return 1;}

static const char *HOME_HTML =
    "<html><head><title>pefiaOS Start</title></head><body>"
    "<h1>pefiaOS Web</h1>"
    "<p>A real TCP/IP + TLS 1.3 browser running on bare metal. Type a URL above "
    "and press Enter. https:// is assumed if you omit the scheme.</p>"
    "<hr>"
    "<h2>Milestone B test pages</h2>"
    "<ul>"
    "<li><a href='about:test-layout'>about:test-layout</a> &mdash; layout and links</li>"
    "<li><a href='about:test-js'>about:test-js</a> &mdash; inline/external script degrade</li>"
    "<li><a href='about:test-img'>about:test-img</a> &mdash; image pipeline + fallback</li>"
    "<li><a href='about:test-engine'>about:test-engine</a> &mdash; DOM + CSS cascade + JavaScript</li>"
    "</ul>"
    "<h2>External</h2>"
    "<ul>"
    "<li><a href='https://example.com/'>example.com</a> &mdash; a tiny test page</li>"
    "<li><a href='https://www.google.com/'>google.com</a> &mdash; compatibility stress page</li>"
    "<li><a href='http://info.cern.ch/'>info.cern.ch</a> &mdash; the first website (plain HTTP)</li>"
    "</ul>"
    "<h2>Notes</h2>"
    "<p>Google and other JS-heavy apps will not fully render yet. Use about:test-* pages to verify engine progress.</p>"
    "</body></html>";

static const char *TEST_LAYOUT_HTML =
    "<html><head><title>about:test-layout</title></head><body>"
    "<h1>Layout Test</h1><p>This page verifies text flow, headings, links, and lists.</p>"
    "<h2>Links</h2><p><a href='about:test-js'>Go JS test</a> | <a href='about:test-img'>Go IMG test</a></p>"
    "<h2>List</h2><ul><li>one</li><li>two</li><li>three</li></ul>"
    "<hr><p>Done.</p></body></html>";

static const char *TEST_JS_HTML =
    "<html><head><title>about:test-js</title></head><body>"
    "<h1>JS Degrade Test</h1>"
    "<p>Inline script below should inject words via document.write degrade:</p>"
    "<script>document.write('INLINE_OK');</script>"
    "<p>External script test:</p>"
    "<script src='https://example.com/'></script>"
    "<p>If external script has no document.write literals, no extra output appears (expected).</p>"
    "</body></html>";

static const char *TEST_IMG_HTML =
    "<html><head><title>about:test-img</title></head><body>"
    "<h1>Image Test</h1>"
    "<p>Image pipeline placeholder/metadata check:</p>"
    "<img src='https://example.com/favicon.ico' alt='[img fallback alt]'>"
    "<p>Relative image resolution test:</p>"
    "<img src='/favicon.ico' alt='[relative img alt]'>"
    "<p>If decoded BMP is available you should see [img WxH] metadata now.</p>"
    "</body></html>";

static const char *TEST_ENGINE_HTML =
    "<html><head><title>Engine Test</title>"
    "<style>"
    " body { color:#222; }"
    " h1 { color:#1a3c8c; text-align:center; }"
    " .card { background:#eaf1fb; padding:8px; margin-top:8px; }"
    " .ok { color:green; font-weight:bold; }"
    " .warn { color:#c04000; }"
    " #out { color:#0a8a0a; font-weight:bold; }"
    "</style></head><body>"
    "<h1>PefiaOS Engine Test</h1>"
    "<img src='https://www.google.com/images/branding/googlelogo/1x/googlelogo_white_background_color_272x92dp.png' width='272' height='92' alt='Google logo'>"
    "<div class='card'>"
    "<p class='ok'>CSS cascade works if this line is green and bold.</p>"
    "<p class='warn'>This line should be orange.</p>"
    "<p style='color:purple'>Inline style sets this purple.</p>"
    "</div>"
    "<h2>JavaScript + DOM</h2>"
    "<p>Loop result: <span id='out'>(script did not run)</span></p>"
    "<p>List built by innerHTML:</p><ul id='list'></ul>"
    "<script>"
    " var sum = 0;"
    " for (var i = 1; i <= 5; i++) { sum = sum + i; }"
    " document.getElementById('out').innerHTML = 'sum of 1..5 = ' + sum;"
    " var html = '';"
    " for (var k = 0; k < 3; k++) { html = html + '<li>generated item ' + (k + 1) + '</li>'; }"
    " document.getElementById('list').innerHTML = html;"
    "</script>"
    "<p style='text-align:center'>This paragraph is centered by CSS.</p>"
    "</body></html>";

/* ------------------------------------------------------------------------- */
static void load_html(BrowserState *s, const char *html, int len)
{
    if (len > PAGE_CAP - 1) len = PAGE_CAP - 1;
    for (int i = 0; i < len; i++) s->page[i] = html[i];
    s->page[len] = 0;
    s->page_len = len;
    s->gen++;
    s->scroll = 0;
    html_set_base_url(s->url);
    html_set_page(s, s->gen, s->page, s->page_len);
    html_layout(s->scr_w > 40 ? s->scr_w - SCROLLBAR : 600, s->title, sizeof(s->title));
    s->doc_h = html_doc_height();
    if (!s->title[0]) scopy(s->title, s->url, sizeof(s->title));
}

static void itoa_s(int v, char *out)
{
    char t[12]; int i=0; if(v<0){*out++='-';v=-v;} if(v==0){out[0]='0';out[1]=0;return;}
    while(v){t[i++]=(char)('0'+v%10);v/=10;} int j=0; while(i)out[j++]=t[--i]; out[j]=0;
}

static void show_error(BrowserState *s, const char *url, int status)
{
    char buf[1024]; int o=0;
    const char *parts1 = "<html><title>Problem loading page</title><body><h1>Could not load page</h1><p>";
    for(int i=0;parts1[i];i++) buf[o++]=parts1[i];
    for(int i=0;url[i]&&o<700;i++) buf[o++]=url[i];
    const char *parts2 = "</p><hr><p>";
    for(int i=0;parts2[i];i++) buf[o++]=parts2[i];
    const char *st = net_status_text();
    for(int i=0;st[i]&&o<900;i++) buf[o++]=st[i];
    if (status < 0) {
        const char *p3 = "</p><p>The network request failed before a response arrived.</p></body></html>";
        for(int i=0;p3[i];i++) buf[o++]=p3[i];
    } else {
        const char *p3 = "</p><p>Server returned status ";
        for(int i=0;p3[i];i++) buf[o++]=p3[i];
        char sc[12]; itoa_s(status, sc);
        for(int i=0;sc[i];i++) buf[o++]=sc[i];
        const char *p4 = ".</p></body></html>";
        for(int i=0;p4[i];i++) buf[o++]=p4[i];
    }
    buf[o]=0;
    load_html(s, buf, o);
}

static void draw_loading_overlay(BrowserState *s)
{
    int cx = s->scr_x, cy = s->scr_y + TOOLBAR_H;
    int cw = s->scr_w, ch = s->scr_h - TOOLBAR_H - STATUS_H;
    if (cw <= 0 || ch <= 0) return;
    fb_fill_rect(cx, cy, cw, ch, fb_rgb(248, 250, 253));
    char line[300]; int o=0;
    const char *pfx = "Loading ";
    for(int i=0;pfx[i];i++) line[o++]=pfx[i];
    for(int i=0;s->url[i]&&o<280;i++) line[o++]=s->url[i];
    line[o++]='.'; line[o++]='.'; line[o++]='.'; line[o]=0;
    gfx_text(cx + 16, cy + 24, line, fb_rgb(40,60,100), fb_rgb(248,250,253));
}

static void push_hist(BrowserState *s, const char *url)
{
    if (s->hist_count > 0 && seq(s->hist[s->hist_pos], url)) return;
    if (s->hist_pos < s->hist_count - 1) s->hist_count = s->hist_pos + 1;
    if (s->hist_count < HIST_CAP) {
        scopy(s->hist[s->hist_count], url, URL_CAP);
        s->hist_pos = s->hist_count; s->hist_count++;
    } else {
        for (int i = 1; i < HIST_CAP; i++) scopy(s->hist[i-1], s->hist[i], URL_CAP);
        scopy(s->hist[HIST_CAP-1], url, URL_CAP);
        s->hist_pos = HIST_CAP-1;
    }
}

static void navigate(BrowserState *s, int add_hist)
{
    /* trim leading spaces */
    char u[URL_CAP]; scopy(u, s->url, sizeof(u));

    if (!u[0] || seq(u, "about:home") || seq(u, "home")) {
        scopy(s->url, "about:home", sizeof(s->url));
        s->url_len = slen(s->url);
        load_html(s, HOME_HTML, slen(HOME_HTML));
        scopy(s->status, "Start page", sizeof(s->status));
        if (add_hist) push_hist(s, s->url);
        return;
    }
    if (seq(u, "about:test-layout")) {
        scopy(s->url, "about:test-layout", sizeof(s->url));
        s->url_len = slen(s->url);
        load_html(s, TEST_LAYOUT_HTML, slen(TEST_LAYOUT_HTML));
        scopy(s->status, "Local test: layout", sizeof(s->status));
        if (add_hist) push_hist(s, s->url);
        return;
    }
    if (seq(u, "about:test-js")) {
        scopy(s->url, "about:test-js", sizeof(s->url));
        s->url_len = slen(s->url);
        load_html(s, TEST_JS_HTML, slen(TEST_JS_HTML));
        scopy(s->status, "Local test: js degrade", sizeof(s->status));
        if (add_hist) push_hist(s, s->url);
        return;
    }
    if (seq(u, "about:test-img")) {
        scopy(s->url, "about:test-img", sizeof(s->url));
        s->url_len = slen(s->url);
        load_html(s, TEST_IMG_HTML, slen(TEST_IMG_HTML));
        scopy(s->status, "Local test: image pipeline", sizeof(s->status));
        if (add_hist) push_hist(s, s->url);
        return;
    }

    if (seq(u, "about:test-engine")) {
        scopy(s->url, "about:test-engine", sizeof(s->url));
        s->url_len = slen(s->url);
        load_html(s, TEST_ENGINE_HTML, slen(TEST_ENGINE_HTML));
        scopy(s->status, "Local test: DOM + CSS + JS engine", sizeof(s->status));
        if (add_hist) push_hist(s, s->url);
        return;
    }

    if (!starts_ci(u, "http://") && !starts_ci(u, "https://") && !starts_ci(u, "about:")) {
        char fixed[URL_CAP];
        int o=0; const char *p="https://";
        for (int i=0; p[i] && o<URL_CAP-1; i++) fixed[o++]=p[i];
        for (int i=0; u[i] && o<URL_CAP-1; i++) fixed[o++]=u[i];
        fixed[o]=0;
        scopy(s->url, fixed, sizeof(s->url));
        scopy(u, fixed, sizeof(u));
    }

    draw_loading_overlay(s);

    NetResponse resp;
    int r = net_fetch(u, &resp);

    if (r == 0 && resp.body_len > 0) {
        const char *ct = resp.content_type;
        int htmlish = 0;
        if (!ct[0]) htmlish = 1;
        else if (starts_ci(ct, "text/html") || starts_ci(ct, "application/xhtml+xml")) htmlish = 1;

        if (htmlish) {
            load_html(s, resp.body, resp.body_len);
        } else {
            char buf[1400]; int o=0;
            const char *h1 = "<html><head><title>Unsupported content</title></head><body><h1>Cannot render this resource directly</h1><p>URL: ";
            for (int i=0; h1[i] && o<1300; i++) buf[o++]=h1[i];
            const char *fu = resp.final_url[0] ? resp.final_url : u;
            for (int i=0; fu[i] && o<1300; i++) buf[o++]=fu[i];
            const char *h2 = "</p><p>Content-Type: ";
            for (int i=0; h2[i] && o<1300; i++) buf[o++]=h2[i];
            for (int i=0; ct[i] && o<1300; i++) buf[o++]=ct[i];
            const char *h3 = "</p><p>This browser currently renders HTML text only. Images and scripts are fetched in compatibility/degrade mode.</p></body></html>";
            for (int i=0; h3[i] && o<1390; i++) buf[o++]=h3[i];
            buf[o]=0;
            load_html(s, buf, o);
        }

        scopy(s->url, resp.final_url[0] ? resp.final_url : u, sizeof(s->url));
        s->url_len = slen(s->url);
        char st[120]; int o=0;
        const char *pfx = resp.is_tls ? "https - " : "http - ";
        for(int i=0;pfx[i];i++) st[o++]=pfx[i];
        char sc[12]; itoa_s(resp.status, sc);
        for(int i=0;sc[i];i++) st[o++]=sc[i];
        st[o++]=' ';
        for(int i=0;ct[i]&&o<110;i++) st[o++]=ct[i];
        st[o]=0;
        scopy(s->status, st, sizeof(s->status));
    } else {
        show_error(s, u, resp.status);
        scopy(s->status, net_status_text(), sizeof(s->status));
    }
    if (add_hist) push_hist(s, s->url);
}

/* ------------------------------------------------------------------------- */
void *browser_new(void)
{
    BrowserState *s = (BrowserState *)kmalloc(sizeof(BrowserState));
    if (!s) return 0;
    scopy(s->url, "about:home", sizeof(s->url));
    s->url_len = slen(s->url);
    s->title[0] = 0;
    scopy(s->status, "Ready", sizeof(s->status));
    s->address_focused = 0;
    s->caret_tick = 0;
    s->scroll = 0;
    s->doc_h = 0;
    s->gen = 0;
    s->hist_count = 0; s->hist_pos = -1;
    s->scr_x = s->scr_y = 0; s->scr_w = 600; s->scr_h = 400;
    s->page_len = 0;

    load_html(s, HOME_HTML, slen(HOME_HTML));
    push_hist(s, s->url);
    return s;
}

void browser_goto(Window *w, const char *url)
{
    BrowserState *s = (BrowserState *)w->state;
    if (!s) return;
    scopy(s->url, url, sizeof(s->url));
    s->url_len = slen(s->url);
    s->address_focused = 0;
    navigate(s, 1);
}

/* ------------------------------------------------------------------------- */
static void draw_button(int x, int y, int w, int h, const char *label, color_t bg, color_t fg)
{
    fb_fill_rect(x, y, w, h, bg);
    fb_fill_rect(x, y + h - 1, w, 1, fb_rgb(20, 40, 90));
    int tl = slen(label);
    gfx_text(x + (w - tl*8)/2, y + (h-16)/2, label, fg, bg);
}

void browser_paint(Window *w, int x, int y, int wdt, int hgt)
{
    BrowserState *s = (BrowserState *)w->state;
    if (!s) return;

    s->scr_x = x; s->scr_y = y; s->scr_w = wdt; s->scr_h = hgt;

    color_t chrome = fb_rgb(236, 240, 247);
    color_t btn    = fb_rgb(214, 222, 236);
    color_t accent = fb_rgb(46, 110, 245);

    /* toolbar */
    fb_fill_rect(x, y, wdt, TOOLBAR_H, chrome);
    fb_fill_rect(x, y + TOOLBAR_H - 1, wdt, 1, fb_rgb(196, 206, 222));

    int by = y + 9, bh = 22;
    int canback = s->hist_pos > 0;
    int canfwd  = s->hist_pos + 1 < s->hist_count;
    draw_button(x + 8,  by, 26, bh, "<", btn, canback ? fb_rgb(20,40,90) : fb_rgb(150,158,170));
    draw_button(x + 38, by, 26, bh, ">", btn, canfwd  ? fb_rgb(20,40,90) : fb_rgb(150,158,170));
    draw_button(x + 68, by, 26, bh, "R", btn, fb_rgb(20,40,90));

    int go_w = 44;
    int addr_x = x + 100, addr_w = wdt - 100 - go_w - 16;
    color_t addr_bg = s->address_focused ? fb_rgb(255,255,255) : fb_rgb(247,249,253);
    fb_fill_rect(addr_x, by, addr_w, bh, addr_bg);
    fb_fill_rect(addr_x, by, addr_w, 1, fb_rgb(170,182,206));
    fb_fill_rect(addr_x, by + bh - 1, addr_w, 1, fb_rgb(170,182,206));
    fb_fill_rect(addr_x, by, 1, bh, fb_rgb(170,182,206));
    fb_fill_rect(addr_x + addr_w - 1, by, 1, bh, fb_rgb(170,182,206));

    int maxcols = (addr_w - 12) / 8;
    char shown[128];
    int ul = s->url_len, from = 0;
    if (ul > maxcols) from = ul - maxcols;
    int z = 0; while (s->url[from + z] && z < 127) { shown[z] = s->url[from + z]; z++; } shown[z] = 0;
    gfx_text(addr_x + 6, by + 3, shown, fb_rgb(28,34,48), addr_bg);
    if (s->address_focused) {
        s->caret_tick++;
        if ((s->caret_tick / 8) % 2 == 0) {
            int cxp = addr_x + 6 + z*8;
            fb_fill_rect(cxp, by + 3, 1, 16, fb_rgb(20,20,20));
        }
    }

    draw_button(x + wdt - go_w - 8, by, go_w, bh, "Go", accent, fb_rgb(255,255,255));

    /* content */
    int cy = y + TOOLBAR_H;
    int ch = hgt - TOOLBAR_H - STATUS_H;
    int viewW = wdt - SCROLLBAR;
    if (viewW < 40) viewW = 40;

    html_set_base_url(s->url);
    html_set_page(s, s->gen, s->page, s->page_len);
    html_layout(viewW, s->title, sizeof(s->title));
    s->doc_h = html_doc_height();

    int maxscroll = s->doc_h - ch; if (maxscroll < 0) maxscroll = 0;
    if (s->scroll > maxscroll) s->scroll = maxscroll;
    if (s->scroll < 0) s->scroll = 0;

    html_paint(x, cy, viewW, ch, s->scroll);

    /* scrollbar */
    int sbx = x + viewW;
    fb_fill_rect(sbx, cy, SCROLLBAR, ch, fb_rgb(226, 231, 240));
    if (s->doc_h > ch && s->doc_h > 0) {
        int thumb_h = ch * ch / s->doc_h; if (thumb_h < 24) thumb_h = 24;
        int track = ch - thumb_h;
        int thumb_y = maxscroll > 0 ? (s->scroll * track / maxscroll) : 0;
        fb_fill_rect(sbx + 2, cy + thumb_y, SCROLLBAR - 4, thumb_h, fb_rgb(150, 164, 188));
    }

    /* status bar */
    int syb = y + hgt - STATUS_H;
    fb_fill_rect(x, syb, wdt, STATUS_H, fb_rgb(228, 233, 242));
    fb_fill_rect(x, syb, wdt, 1, fb_rgb(196, 206, 222));
    int scols = (wdt - 16) / 8;
    char sline[160];
    int q = 0; while (s->status[q] && q < scols && q < 159) { sline[q]=s->status[q]; q++; } sline[q]=0;
    gfx_text(x + 8, syb + 2, sline, fb_rgb(60, 72, 96), fb_rgb(228, 233, 242));
}

/* ------------------------------------------------------------------------- */
void browser_key(Window *w, char c)
{
    BrowserState *s = (BrowserState *)w->state;
    if (!s) return;

    if (!s->address_focused) {
        int ch = s->scr_h - TOOLBAR_H - STATUS_H;
        if (c == ' ')      { s->scroll += ch * 4 / 5; }
        else if (c == 'b') { s->scroll -= ch * 4 / 5; }
        else if (c == 'j') { s->scroll += 40; }
        else if (c == 'k') { s->scroll -= 40; }
        if (s->scroll < 0) s->scroll = 0;
        return;
    }

    if (c == '\n' || c == '\r') { s->address_focused = 0; navigate(s, 1); return; }
    if (c == '\b') { if (s->url_len > 0) { s->url_len--; s->url[s->url_len] = 0; } return; }
    if (c >= 32 && c < 127 && s->url_len < URL_CAP - 1) {
        s->url[s->url_len++] = c; s->url[s->url_len] = 0;
    }
}

/* ------------------------------------------------------------------------- */
void browser_tick(Window *w)
{
    BrowserState *s = (BrowserState *)w->state;
    if (!s) return;
    html_set_base_url(s->url);
    html_set_page(s, s->gen, s->page, s->page_len);
}

void browser_click(Window *w, int relx, int rely)
{
    BrowserState *s = (BrowserState *)w->state;
    if (!s) return;

    int wdt = s->scr_w, hgt = s->scr_h;
    int by = 9, bh = 22;

    /* toolbar row */
    if (rely >= by && rely < by + bh) {
        if (relx >= 8 && relx < 34) {
            if (s->hist_pos > 0) { s->hist_pos--; scopy(s->url, s->hist[s->hist_pos], URL_CAP); s->url_len = slen(s->url); navigate(s, 0); }
            return;
        }
        if (relx >= 38 && relx < 64) {
            if (s->hist_pos + 1 < s->hist_count) { s->hist_pos++; scopy(s->url, s->hist[s->hist_pos], URL_CAP); s->url_len = slen(s->url); navigate(s, 0); }
            return;
        }
        if (relx >= 68 && relx < 94) { navigate(s, 0); return; }

        int go_w = 44;
        if (relx >= wdt - go_w - 8 && relx < wdt - 8) { s->address_focused = 0; navigate(s, 1); return; }

        int addr_x = 100, addr_w = wdt - 100 - go_w - 16;
        if (relx >= addr_x && relx < addr_x + addr_w) { s->address_focused = 1; s->caret_tick = 0; return; }
    }

    int cy0 = TOOLBAR_H;
    int ch = hgt - TOOLBAR_H - STATUS_H;
    int viewW = wdt - SCROLLBAR;

    /* scrollbar */
    if (relx >= viewW && rely >= cy0 && rely < cy0 + ch) {
        int maxscroll = s->doc_h - ch; if (maxscroll < 0) maxscroll = 0;
        int rel = rely - cy0;
        s->scroll = (ch > 0) ? (rel * maxscroll / ch) : 0;
        if (s->scroll < 0) s->scroll = 0;
        if (s->scroll > maxscroll) s->scroll = maxscroll;
        s->address_focused = 0;
        return;
    }

    /* content: link hit-test */
    if (rely >= cy0 && rely < cy0 + ch && relx < viewW) {
        s->address_focused = 0;
        int doc_x = relx;
        int doc_y = (rely - cy0) + s->scroll;
        const char *href = html_link_at(doc_x, doc_y);
        if (href && href[0]) {
            char nav[URL_CAP];
            if (starts_ci(href, "http://") || starts_ci(href, "https://")) {
                scopy(nav, href, sizeof(nav));
            } else if (href[0] == '/') {
                /* scheme + host from current url */
                char base[URL_CAP]; scopy(base, s->url, sizeof(base));
                int o = 0;
                int sc = starts_ci(base, "http://") ? 7 : starts_ci(base, "https://") ? 8 : 0;
                for (int i = 0; i < sc; i++) nav[o++] = base[i];
                int hs = sc;
                while (base[hs] && base[hs] != '/' && o < URL_CAP-1) nav[o++] = base[hs++];
                for (int i = 0; href[i] && o < URL_CAP-1; i++) nav[o++] = href[i];
                nav[o] = 0;
                if (sc == 0) scopy(nav, href, sizeof(nav));
            } else if (starts(href, "#")) {
                return; /* in-page anchor: ignore */
            } else {
                /* relative to current directory */
                char base[URL_CAP]; scopy(base, s->url, sizeof(base));
                int cut = slen(base);
                while (cut > 0 && base[cut-1] != '/') cut--;
                if (cut < 8) cut = slen(base); /* no path slash; append */
                int o = 0;
                for (int i = 0; i < cut && o < URL_CAP-1; i++) nav[o++] = base[i];
                if (o == 0 || nav[o-1] != '/') if (o<URL_CAP-1) nav[o++]='/';
                for (int i = 0; href[i] && o < URL_CAP-1; i++) nav[o++] = href[i];
                nav[o] = 0;
            }
            scopy(s->url, nav, sizeof(s->url));
            s->url_len = slen(s->url);
            navigate(s, 1);
        }
        return;
    }

    s->address_focused = 0;
}
