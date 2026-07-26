#include "browser.h"
#include "framebuffer.h"
#include "console.h"
#include "heap.h"
#include "net.h"
#include "htmlrender.h"
#include "clock.h"

#define URL_CAP   512
#define PAGE_CAP  786432   /* match htmlrender g_page - the render ceiling */
#define BODY_CAP  1024
#define HIST_CAP  32
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

    /* last painted content rectangle (absolute screen coords), so click
     * handling can hit-test against what was actually drawn */
    int      scr_x, scr_y, scr_w, scr_h;

    unsigned last_anim_ms;

    int      page_len;
    char     page[PAGE_CAP];
} BrowserState;

static int str_len(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void str_copy(char *dst, const char *src, int cap)
{
    int i = 0;
    if (cap <= 0) return;
    while (src && src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static int str_eq(const char *a, const char *b)
{
    int i = 0;
    while (a[i] && b[i]) { if (a[i] != b[i]) return 0; i++; }
    return a[i] == b[i];
}

static char to_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static int str_starts_ci(const char *s, const char *prefix)
{
    int i = 0;
    while (prefix[i]) { if (to_lower(s[i]) != to_lower(prefix[i])) return 0; i++; }
    return 1;
}

/* Small bounded string builder - used instead of hand-rolled append loops
 * everywhere a page of HTML or a status line gets assembled below. */
typedef struct { char *buf; int len; int cap; } StrBuf;

static void sb_init(StrBuf *sb, char *buf, int cap) { sb->buf = buf; sb->len = 0; sb->cap = cap; buf[0] = '\0'; }

static void sb_str(StrBuf *sb, const char *s)
{
    while (*s && sb->len < sb->cap - 1) sb->buf[sb->len++] = *s++;
    sb->buf[sb->len] = '\0';
}

static void sb_int(StrBuf *sb, int v)
{
    char digits[12];
    int i = 0;
    if (v < 0) { sb_str(sb, "-"); v = -v; }
    if (v == 0) { sb_str(sb, "0"); return; }
    while (v) { digits[i++] = (char)('0' + v % 10); v /= 10; }
    char out[12];
    int j = 0;
    while (i) out[j++] = digits[--i];
    out[j] = '\0';
    sb_str(sb, out);
}

static const char *HOME_HTML =
    "<html><head><title>pefiaOS Start</title>"
    "<style>"
    " body { text-align:center; }"
    " .search { margin-top:22px; margin-bottom:10px; }"
    " .hint { color:#6a7383; }"
    " .links { margin-top:14px; }"
    " .links a { margin-top:4px; }"
    " h1 { margin-top:26px; margin-bottom:2px; }"
    " h2 { margin-top:20px; }"
    "</style></head><body>"
    "<h1>pefiaOS Web</h1>"
    "<p class='hint'>A TCP/IP + TLS 1.3 browser on bare metal.</p>"
    "<form class='search' action='https://lite.duckduckgo.com/lite/'>"
    "<input type='text' name='q' size='40' placeholder='Search DuckDuckGo...'> "
    "<input type='submit' value='Search'>"
    "</form>"
    "<p class='hint'>Click the box and type, then press Enter. Or type any URL "
    "in the address bar above &mdash; https:// is assumed.</p>"
    "<hr>"
    "<h2>Quick searches</h2>"
    "<p class='links'>"
    "<a href='https://lite.duckduckgo.com/lite/?q=bare+metal+os'>bare metal os</a> &middot; "
    "<a href='https://lite.duckduckgo.com/lite/?q=osdev+wiki'>osdev wiki</a> &middot; "
    "<a href='https://lite.duckduckgo.com/lite/?q=x86+protected+mode'>x86 protected mode</a> &middot; "
    "<a href='https://lite.duckduckgo.com/lite/?q=news'>news</a>"
    "</p>"
    "<h2>Sites that render well here</h2>"
    "<p class='links'>"
    "<a href='https://lite.duckduckgo.com/lite/'>DuckDuckGo Lite</a> &middot; "
    "<a href='https://example.com/'>example.com</a> &middot; "
    "<a href='http://info.cern.ch/'>info.cern.ch</a> &middot; "
    "<a href='https://en.wikipedia.org/wiki/Operating_system'>Wikipedia</a>"
    "</p>"
    "<h2>Engine test pages</h2>"
    "<p class='links'>"
    "<a href='about:test-layout'>layout</a> &middot; "
    "<a href='about:test-engine'>DOM + CSS + JS</a> &middot; "
    "<a href='about:test-forms'>form controls</a> &middot; "
    "<a href='about:test-js'>events + timers</a> &middot; "
    "<a href='about:test-img'>images + video</a>"
    "</p>"
    "</body></html>";

static const char *TEST_LAYOUT_HTML =
    "<html><head><title>about:test-layout</title></head><body>"
    "<h1>Layout Test</h1><p>This page verifies text flow, headings, links, and lists.</p>"
    "<h2>Links</h2><p><a href='about:test-js'>Go JS test</a> | <a href='about:test-img'>Go IMG test</a></p>"
    "<h2>List</h2><ul><li>one</li><li>two</li><li>three</li></ul>"
    "<h2>Table</h2><table><tr><th>Name</th><th>Value</th></tr>"
    "<tr><td>alpha</td><td>1</td></tr><tr><td>beta</td><td>2</td></tr></table>"
    "<hr><p>Done.</p></body></html>";

static const char *TEST_JS_HTML =
    "<html><head><title>about:test-js</title>"
    "<style> .box { background:#eef3fb; padding:8px; margin-top:6px; border:1px solid #b9c6de; }"
    " #tick { color:#0a7a0a; font-weight:bold; } button { font-weight:bold; }</style>"
    "</head><body>"
    "<h1>JavaScript Engine Test</h1>"
    "<div class='box'><p>Arrays and objects: <span id='arr'>?</span></p>"
    "<p>Closure counter: <span id='clo'>?</span></p>"
    "<p>String methods: <span id='str'>?</span></p></div>"
    "<div class='box'><p>Click the button; a handler updates this line:</p>"
    "<p id='clicked'>(not clicked yet)</p>"
    "<button id='btn'>Click me</button></div>"
    "<div class='box'><p>setInterval tick (should count up): <span id='tick'>0</span></p></div>"
    "<script>"
    " var a = [3, 1, 2]; a.push(9);"
    " document.getElementById('arr').textContent = 'len=' + a.length + ' join=' + a.join('-') + ' map=' + a.map(function(x){return x*2;}).join(',');"
    " function counter() { var n = 0; return function () { n = n + 1; return n; }; }"
    " var c = counter(); c(); c();"
    " document.getElementById('clo').textContent = 'called 3x -> ' + c();"
    " var s = 'Hello,World';"
    " document.getElementById('str').textContent = s.split(',').join(' + ') + ' | upper=' + s.toUpperCase() + ' | has World=' + s.includes('World');"
    " document.getElementById('btn').addEventListener('click', function () {"
    "   document.getElementById('clicked').innerHTML = 'handler ran - DOM updated live';"
    " });"
    " var n = 0;"
    " setInterval(function () { n = n + 1; document.getElementById('tick').textContent = n; }, 1000);"
    "</script>"
    "</body></html>";

static const char *TEST_FORMS_HTML =
    "<html><head><title>about:test-forms</title>"
    "<style> fieldset { margin-top:8px; } label { color:#334; }</style></head><body>"
    "<h1>Form Controls</h1>"
    "<form action='https://duckduckgo.com/html/'>"
    "<p>Text (GET to DuckDuckGo): <input type='text' name='q' size='28' placeholder='type a search...'>"
    " <input type='submit' value='Search'></p>"
    "</form>"
    "<form action='https://httpbin.org/post' method='post'>"
    "<p>POST form &mdash; every control below is serialized into the request body.</p>"
    "<p>Name: <input type='text' name='name' value='pefia' size='16'></p>"
    "<p>Password: <input type='password' name='pw' size='16' value='secret'></p>"
    "<p>Subscribe: <input type='checkbox' name='sub' value='yes' checked> yes</p>"
    "<p>Plan: <input type='radio' name='plan' value='free' checked> free "
    "<input type='radio' name='plan' value='pro'> pro</p>"
    "<p>Color: <select name='color'><option>red</option><option>green</option><option>blue</option></select> "
    "(click to cycle)</p>"
    "<input type='hidden' name='src' value='pefiaOS'>"
    "<p><input type='submit' value='POST it'></p>"
    "</form>"
    "<p>Password text is masked, unchecked boxes are omitted, and the hidden "
    "field rides along - the same rules a real browser follows.</p>"
    "</body></html>";

static const char *TEST_IMG_HTML =
    "<html><head><title>about:test-img</title></head><body>"
    "<h1>Image + Media Test</h1>"
    "<p>PNG over HTTPS:</p>"
    "<img src='https://upload.wikimedia.org/wikipedia/commons/thumb/8/8b/Tux-G2.png/240px-Tux-G2.png' width='240' height='283' alt='[png over https]'>"
    "<p>Animated GIF (every frame is decoded and played):</p>"
    "<img src='https://upload.wikimedia.org/wikipedia/commons/2/2c/Rotating_earth_%28large%29.gif' alt='[animated gif]'>"
    "<p>Video element (no codec - renders a clickable placeholder):</p>"
    "<video src='https://example.com/movie.mp4' poster=''></video>"
    "<p>Relative image resolution test:</p>"
    "<img src='/favicon.ico' alt='[relative img alt]'>"
    "</body></html>";

static const char *TEST_ENGINE_HTML =
    "<html><head><title>Engine Test</title>"
    "<style>"
    " body { color:#222; }"
    " h1 { color:#1a3c8c; text-align:center; }"
    " .card { background:#eaf1fb; padding:8px; margin-top:8px; border:1px solid #aac; }"
    " .ok { color:green; font-weight:bold; }"
    " .warn { color:#c04000; }"
    " #out { color:#0a8a0a; font-weight:bold; }"
    " ul.tests > li { color:#333; }"
    " ul.tests li:first-child { font-weight:bold; }"
    " li[data-kind='attr'] { color:#8a0a8a; }"
    " h2 + p { color:#555; text-transform:uppercase; }"
    " @media print { body { color:#ff0000; } }"
    "</style></head><body>"
    "<h1>PefiaOS Engine Test</h1>"
    "<div class='card'>"
    "<p class='ok'>CSS cascade works if this line is green and bold.</p>"
    "<p class='warn'>This line should be orange.</p>"
    "<p style='color:purple'>Inline style sets this purple.</p>"
    "</div>"
    "<h2>Selectors</h2>"
    "<p>This line is uppercased by an adjacent-sibling rule (h2 + p).</p>"
    "<ul class='tests'>"
    "<li>child combinator + :first-child (bold)</li>"
    "<li data-kind='attr'>attribute selector (purple)</li>"
    "<li>plain item</li>"
    "</ul>"
    "<p>Print-only rules must NOT apply: this text stays dark, not red.</p>"
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

static void load_html(BrowserState *s, const char *html, int len)
{
    if (len > PAGE_CAP - 1) len = PAGE_CAP - 1;
    for (int i = 0; i < len; i++) s->page[i] = html[i];
    s->page[len] = '\0';
    s->page_len = len;
    s->gen++;
    s->scroll = 0;
    html_set_base_url(s->url);
    html_set_page(s, s->gen, s->page, s->page_len);
    html_layout(s->scr_w > 40 ? s->scr_w - SCROLLBAR : 600, s->title, sizeof(s->title));
    s->doc_h = html_doc_height();
    if (!s->title[0]) str_copy(s->title, s->url, sizeof(s->title));
}

static void show_error(BrowserState *s, const char *url, int status)
{
    char buf[1024];
    StrBuf sb;
    sb_init(&sb, buf, sizeof(buf));

    sb_str(&sb, "<html><title>Problem loading page</title><body><h1>Could not load page</h1><p>");
    sb_str(&sb, url);
    sb_str(&sb, "</p><hr><p>");
    sb_str(&sb, net_status_text());

    if (status < 0) {
        sb_str(&sb, "</p><p>The network request failed before a response arrived.</p></body></html>");
    } else {
        sb_str(&sb, "</p><p>Server returned status ");
        sb_int(&sb, status);
        sb_str(&sb, ".</p></body></html>");
    }

    load_html(s, buf, sb.len);
}

static void draw_loading_overlay(BrowserState *s)
{
    int cx = s->scr_x, cy = s->scr_y + TOOLBAR_H;
    int cw = s->scr_w, ch = s->scr_h - TOOLBAR_H - STATUS_H;
    if (cw <= 0 || ch <= 0) return;

    fb_fill_rect(cx, cy, cw, ch, fb_rgb(248, 250, 253));

    char line[300];
    StrBuf sb;
    sb_init(&sb, line, sizeof(line));
    sb_str(&sb, "Loading ");
    sb_str(&sb, s->url);
    sb_str(&sb, "...");

    gfx_text(cx + 16, cy + 24, line, fb_rgb(40, 60, 100), fb_rgb(248, 250, 253));
}

static void push_hist(BrowserState *s, const char *url)
{
    if (s->hist_count > 0 && str_eq(s->hist[s->hist_pos], url)) return;

    if (s->hist_pos < s->hist_count - 1) s->hist_count = s->hist_pos + 1;

    if (s->hist_count < HIST_CAP) {
        str_copy(s->hist[s->hist_count], url, URL_CAP);
        s->hist_pos = s->hist_count;
        s->hist_count++;
    } else {
        for (int i = 1; i < HIST_CAP; i++) str_copy(s->hist[i - 1], s->hist[i], URL_CAP);
        str_copy(s->hist[HIST_CAP - 1], url, URL_CAP);
        s->hist_pos = HIST_CAP - 1;
    }
}

/* about: pages are served locally without touching the network - handy both
 * as a start page and as fixtures for exercising the layout/JS/image paths. */
static int try_local_page(BrowserState *s, const char *url, int add_hist)
{
    struct { const char *name; const char *html; const char *status; } pages[] = {
        { "about:home",        HOME_HTML,        "Start page" },
        { "about:test-layout", TEST_LAYOUT_HTML, "Local test: layout" },
        { "about:test-js",     TEST_JS_HTML,     "Local test: JS events, timers, arrays" },
        { "about:test-img",    TEST_IMG_HTML,    "Local test: images, animation, media" },
        { "about:test-forms",  TEST_FORMS_HTML,  "Local test: form controls, GET and POST" },
        { "about:test-engine", TEST_ENGINE_HTML, "Local test: DOM + CSS + JS engine" },
    };
    int is_home = str_eq(url, "") || str_eq(url, "home");

    for (unsigned i = 0; i < sizeof(pages) / sizeof(pages[0]); i++) {
        if ((i == 0 && is_home) || str_eq(url, pages[i].name)) {
            str_copy(s->url, pages[i].name, sizeof(s->url));
            s->url_len = str_len(s->url);
            load_html(s, pages[i].html, str_len(pages[i].html));
            str_copy(s->status, pages[i].status, sizeof(s->status));
            if (add_hist) push_hist(s, s->url);
            return 1;
        }
    }
    return 0;
}

/* One fetch + render. `post_body` non-NULL means POST it (form submission);
 * everything else about the two paths is identical. */
static void navigate_body(BrowserState *s, int add_hist, const char *post_body)
{
    char url[URL_CAP];
    str_copy(url, s->url, sizeof(url));

    if (!post_body && try_local_page(s, url, add_hist)) return;

    if (!str_starts_ci(url, "http://") && !str_starts_ci(url, "https://") && !str_starts_ci(url, "about:")) {
        char fixed[URL_CAP];
        StrBuf sb;
        sb_init(&sb, fixed, sizeof(fixed));
        sb_str(&sb, "https://");
        sb_str(&sb, url);
        str_copy(s->url, fixed, sizeof(s->url));
        str_copy(url, fixed, sizeof(url));
    }

    draw_loading_overlay(s);

    NetResponse resp;
    int r = post_body ? net_fetch_post(url, post_body, &resp) : net_fetch(url, &resp);

    if (r == 0 && resp.body_len > 0) {
        const char *content_type = resp.content_type;
        int is_html = !content_type[0] ||
                      str_starts_ci(content_type, "text/html") ||
                      str_starts_ci(content_type, "text/plain") ||
                      str_starts_ci(content_type, "application/xhtml+xml");

        /* the URL has to be current before load_html, since the renderer
         * resolves every relative resource against it */
        str_copy(s->url, resp.final_url[0] ? resp.final_url : url, sizeof(s->url));
        s->url_len = str_len(s->url);

        if (is_html) {
            load_html(s, resp.body, resp.body_len);
        } else {
            char buf[1400];
            StrBuf sb;
            sb_init(&sb, buf, sizeof(buf));
            sb_str(&sb, "<html><head><title>Unsupported content</title></head><body>"
                        "<h1>Cannot render this resource directly</h1><p>URL: ");
            sb_str(&sb, resp.final_url[0] ? resp.final_url : url);
            sb_str(&sb, "</p><p>Content-Type: ");
            sb_str(&sb, content_type);
            sb_str(&sb, "</p><p>This browser renders HTML and plain text. Images load "
                        "inside pages; other binary types are not decoded here.</p></body></html>");
            load_html(s, buf, sb.len);
        }

        char status_line[120];
        StrBuf sb;
        sb_init(&sb, status_line, sizeof(status_line));
        sb_str(&sb, resp.is_tls ? "https - " : "http - ");
        sb_int(&sb, resp.status);
        sb_str(&sb, " ");
        sb_str(&sb, content_type);
        str_copy(s->status, status_line, sizeof(s->status));
    } else {
        show_error(s, url, resp.status);
        str_copy(s->status, net_status_text(), sizeof(s->status));
    }
    if (add_hist) push_hist(s, s->url);
}

static void navigate(BrowserState *s, int add_hist) { navigate_body(s, add_hist, 0); }

/* Submits the form owning `field` (field < 0 = the focused one). */
static void submit_form(BrowserState *s, int field)
{
    char nav[URL_CAP], body[BODY_CAP];
    int how = html_field_submit(field, nav, sizeof(nav), body, sizeof(body));
    if (!how) return;
    html_field_focus(-1);
    str_copy(s->url, nav, sizeof(s->url));
    s->url_len = str_len(s->url);
    navigate_body(s, 1, how == 2 ? body : 0);
}

void *browser_new(void)
{
    BrowserState *s = (BrowserState *)kmalloc(sizeof(BrowserState));
    if (!s) return 0;

    str_copy(s->url, "about:home", sizeof(s->url));
    s->url_len = str_len(s->url);
    s->title[0] = '\0';
    str_copy(s->status, "Ready", sizeof(s->status));
    s->address_focused = 0;
    s->caret_tick = 0;
    s->scroll = 0;
    s->doc_h = 0;
    s->gen = 0;
    s->hist_count = 0;
    s->hist_pos = -1;
    s->scr_x = s->scr_y = 0;
    s->scr_w = 600;
    s->scr_h = 400;
    s->last_anim_ms = 0;
    s->page_len = 0;

    load_html(s, HOME_HTML, str_len(HOME_HTML));
    push_hist(s, s->url);
    return s;
}

void browser_goto(Window *w, const char *url)
{
    BrowserState *s = (BrowserState *)w->state;
    if (!s) return;
    str_copy(s->url, url, sizeof(s->url));
    s->url_len = str_len(s->url);
    s->address_focused = 0;
    navigate(s, 1);
}

static void draw_button(int x, int y, int w, int h, const char *label, color_t bg, color_t fg)
{
    fb_fill_rect(x, y, w, h, bg);
    fb_fill_rect(x, y + h - 1, w, 1, fb_rgb(20, 40, 90));
    int label_w = str_len(label);
    gfx_text(x + (w - label_w * 8) / 2, y + (h - 16) / 2, label, fg, bg);
}

void browser_paint(Window *w, int x, int y, int wdt, int hgt)
{
    BrowserState *s = (BrowserState *)w->state;
    if (!s) return;

    s->scr_x = x; s->scr_y = y; s->scr_w = wdt; s->scr_h = hgt;

    color_t chrome = fb_rgb(236, 240, 247);
    color_t btn    = fb_rgb(214, 222, 236);
    color_t accent = fb_rgb(46, 110, 245);

    fb_fill_rect(x, y, wdt, TOOLBAR_H, chrome);
    fb_fill_rect(x, y + TOOLBAR_H - 1, wdt, 1, fb_rgb(196, 206, 222));

    int by = y + 9, bh = 22;
    int can_back = s->hist_pos > 0;
    int can_fwd  = s->hist_pos + 1 < s->hist_count;
    draw_button(x + 8,  by, 26, bh, "<", btn, can_back ? fb_rgb(20, 40, 90) : fb_rgb(150, 158, 170));
    draw_button(x + 38, by, 26, bh, ">", btn, can_fwd  ? fb_rgb(20, 40, 90) : fb_rgb(150, 158, 170));
    draw_button(x + 68, by, 26, bh, "R", btn, fb_rgb(20, 40, 90));

    int go_w = 44;
    int addr_x = x + 100, addr_w = wdt - 100 - go_w - 16;
    color_t addr_bg = s->address_focused ? fb_rgb(255, 255, 255) : fb_rgb(247, 249, 253);
    fb_fill_rect(addr_x, by, addr_w, bh, addr_bg);
    fb_fill_rect(addr_x, by, addr_w, 1, fb_rgb(170, 182, 206));
    fb_fill_rect(addr_x, by + bh - 1, addr_w, 1, fb_rgb(170, 182, 206));
    fb_fill_rect(addr_x, by, 1, bh, fb_rgb(170, 182, 206));
    fb_fill_rect(addr_x + addr_w - 1, by, 1, bh, fb_rgb(170, 182, 206));

    int max_cols = (addr_w - 12) / 8;
    char shown[160];
    int url_len = s->url_len, from = 0;
    if (url_len > max_cols) from = url_len - max_cols;
    int shown_len = 0;
    while (s->url[from + shown_len] && shown_len < 159) { shown[shown_len] = s->url[from + shown_len]; shown_len++; }
    shown[shown_len] = '\0';
    gfx_text(addr_x + 6, by + 3, shown, fb_rgb(28, 34, 48), addr_bg);
    if (s->address_focused) {
        s->caret_tick++;
        if ((s->caret_tick / 8) % 2 == 0) {
            int caret_x = addr_x + 6 + shown_len * 8;
            fb_fill_rect(caret_x, by + 3, 1, 16, fb_rgb(20, 20, 20));
        }
    }

    draw_button(x + wdt - go_w - 8, by, go_w, bh, "Go", accent, fb_rgb(255, 255, 255));

    int content_y = y + TOOLBAR_H;
    int content_h = hgt - TOOLBAR_H - STATUS_H;
    int view_w = wdt - SCROLLBAR;
    if (view_w < 40) view_w = 40;

    html_set_base_url(s->url);
    html_set_page(s, s->gen, s->page, s->page_len);
    html_layout(view_w, s->title, sizeof(s->title));
    s->doc_h = html_doc_height();

    int max_scroll = s->doc_h - content_h;
    if (max_scroll < 0) max_scroll = 0;
    if (s->scroll > max_scroll) s->scroll = max_scroll;
    if (s->scroll < 0) s->scroll = 0;

    html_paint(x, content_y, view_w, content_h, s->scroll);

    int scrollbar_x = x + view_w;
    fb_fill_rect(scrollbar_x, content_y, SCROLLBAR, content_h, fb_rgb(226, 231, 240));
    if (s->doc_h > content_h && s->doc_h > 0) {
        int thumb_h = content_h * content_h / s->doc_h;
        if (thumb_h < 24) thumb_h = 24;
        int track = content_h - thumb_h;
        int thumb_y = max_scroll > 0 ? (s->scroll * track / max_scroll) : 0;
        fb_fill_rect(scrollbar_x + 2, content_y + thumb_y, SCROLLBAR - 4, thumb_h, fb_rgb(150, 164, 188));
    }

    int status_y = y + hgt - STATUS_H;
    fb_fill_rect(x, status_y, wdt, STATUS_H, fb_rgb(228, 233, 242));
    fb_fill_rect(x, status_y, wdt, 1, fb_rgb(196, 206, 222));
    int status_cols = (wdt - 16) / 8;
    char status_line[200];
    int q = 0;
    while (s->status[q] && q < status_cols && q < 199) { status_line[q] = s->status[q]; q++; }
    status_line[q] = '\0';
    gfx_text(x + 8, status_y + 2, status_line, fb_rgb(60, 72, 96), fb_rgb(228, 233, 242));
}

void browser_key(Window *w, char c)
{
    BrowserState *s = (BrowserState *)w->state;
    if (!s) return;

    /* A focused in-page text field eats keys before scroll/address handling.
     * Enter submits the enclosing form. */
    if (!s->address_focused && html_field_focused() >= 0) {
        if (c == '\n' || c == '\r') { submit_form(s, -1); return; }
        html_field_key(c);
        return;
    }

    if (!s->address_focused) {
        int content_h = s->scr_h - TOOLBAR_H - STATUS_H;
        if (c == ' ')      s->scroll += content_h * 4 / 5;
        else if (c == 'b') s->scroll -= content_h * 4 / 5;
        else if (c == 'j') s->scroll += 40;
        else if (c == 'k') s->scroll -= 40;
        else if (c == 'g') s->scroll = 0;
        else if (c == 'G') s->scroll = s->doc_h;
        else if (c == '\b') {
            if (s->hist_pos > 0) {
                s->hist_pos--;
                str_copy(s->url, s->hist[s->hist_pos], URL_CAP);
                s->url_len = str_len(s->url);
                navigate(s, 0);
            }
        }
        if (s->scroll < 0) s->scroll = 0;
        return;
    }

    if (c == '\n' || c == '\r') { s->address_focused = 0; navigate(s, 1); return; }
    if (c == '\b') {
        if (s->url_len > 0) { s->url_len--; s->url[s->url_len] = '\0'; }
        return;
    }
    if (c >= 32 && c < 127 && s->url_len < URL_CAP - 1) {
        s->url[s->url_len++] = c;
        s->url[s->url_len] = '\0';
    }
}

/* Mouse wheel: positive notches scroll down, three text lines per notch.
 * Clamped like browser_key - low end here, high end in browser_paint. */
void browser_scroll(Window *w, int notches)
{
    BrowserState *s = (BrowserState *)w->state;
    if (!s) return;
    s->scroll += notches * 3 * 16;
    if (s->scroll < 0) s->scroll = 0;
}

/* Runs once per frame per visible browser window: keeps the renderer pointed
 * at this window's page, then lets the script engine run its timers and
 * pending load handlers and acts on whatever they asked for. Returns 1 when
 * something changed on screen, because the frame loop otherwise only
 * repaints in response to input - a setInterval counter or an animated GIF
 * would update the DOM and never be seen. */
int browser_tick(Window *w)
{
    BrowserState *s = (BrowserState *)w->state;
    if (!s) return 0;
    html_set_base_url(s->url);
    html_set_page(s, s->gen, s->page, s->page_len);

    int repaint = html_pump();

    /* An animation has no DOM change to report, so pace it here instead of
     * repainting the whole desktop as fast as the loop spins. */
    if (html_animating()) {
        unsigned now = clock_ms();
        if ((unsigned)(now - s->last_anim_ms) >= 40) { s->last_anim_ms = now; repaint = 1; }
    }

    char msg[200];
    if (html_take_alert(msg, sizeof(msg))) {
        char line[200];
        StrBuf sb;
        sb_init(&sb, line, sizeof(line));
        sb_str(&sb, "alert: ");
        sb_str(&sb, msg);
        str_copy(s->status, line, sizeof(s->status));
        repaint = 1;
    }

    char nav[URL_CAP];
    if (html_take_nav(nav, sizeof(nav)) && nav[0]) {
        str_copy(s->url, nav, sizeof(s->url));
        s->url_len = str_len(s->url);
        navigate(s, 1);
        repaint = 1;
    }

    s->doc_h = html_doc_height();
    return repaint;
}

void browser_click(Window *w, int relx, int rely)
{
    BrowserState *s = (BrowserState *)w->state;
    if (!s) return;

    int wdt = s->scr_w, hgt = s->scr_h;
    int by = 9, bh = 22;

    if (rely >= by && rely < by + bh) {
        if (relx >= 8 && relx < 34) {
            if (s->hist_pos > 0) {
                s->hist_pos--;
                str_copy(s->url, s->hist[s->hist_pos], URL_CAP);
                s->url_len = str_len(s->url);
                navigate(s, 0);
            }
            return;
        }
        if (relx >= 38 && relx < 64) {
            if (s->hist_pos + 1 < s->hist_count) {
                s->hist_pos++;
                str_copy(s->url, s->hist[s->hist_pos], URL_CAP);
                s->url_len = str_len(s->url);
                navigate(s, 0);
            }
            return;
        }
        if (relx >= 68 && relx < 94) { navigate(s, 0); return; }

        int go_w = 44;
        if (relx >= wdt - go_w - 8 && relx < wdt - 8) { s->address_focused = 0; navigate(s, 1); return; }

        int addr_x = 100, addr_w = wdt - 100 - go_w - 16;
        if (relx >= addr_x && relx < addr_x + addr_w) { s->address_focused = 1; s->caret_tick = 0; html_field_focus(-1); return; }
    }

    int content_y = TOOLBAR_H;
    int content_h = hgt - TOOLBAR_H - STATUS_H;
    int view_w = wdt - SCROLLBAR;

    if (relx >= view_w && rely >= content_y && rely < content_y + content_h) {
        int max_scroll = s->doc_h - content_h;
        if (max_scroll < 0) max_scroll = 0;
        int rel = rely - content_y;
        s->scroll = (content_h > 0) ? (rel * max_scroll / content_h) : 0;
        if (s->scroll < 0) s->scroll = 0;
        if (s->scroll > max_scroll) s->scroll = max_scroll;
        s->address_focused = 0;
        return;
    }

    if (rely >= content_y && rely < content_y + content_h && relx < view_w) {
        s->address_focused = 0;
        int doc_x = relx;
        int doc_y = (rely - content_y) + s->scroll;

        /* form controls take precedence over links at the same spot */
        int fid = html_field_at(doc_x, doc_y);
        if (fid >= 0) {
            switch (html_field_kind(fid)) {
                case FK_SUBMIT: submit_form(s, fid); return;
                case FK_TEXT:   html_field_focus(fid); return;
                case FK_CHECK:
                case FK_SELECT: html_field_focus(-1); html_field_toggle(fid); return;
                default:        html_field_focus(-1); return;
            }
        }
        html_field_focus(-1);

        /* a script click handler gets the click before link navigation, the
         * same order a real browser uses (a handler may replace the page) */
        if (html_click_script(doc_x, doc_y)) {
            char nav_js[URL_CAP];
            if (html_take_nav(nav_js, sizeof(nav_js)) && nav_js[0]) {
                str_copy(s->url, nav_js, sizeof(s->url));
                s->url_len = str_len(s->url);
                navigate(s, 1);
            }
            return;
        }

        /* Hrefs arrive already resolved against the base URL - the renderer
         * does that at layout time, and resolving a second time here is how
         * "about:test-engine" used to turn into "about:home/about:test-engine". */
        const char *href = html_link_at(doc_x, doc_y);
        if (href && href[0]) {
            if (href[0] == '#') return;   /* in-page anchor: no target to jump to */
            if (str_starts_ci(href, "javascript:")) return;

            str_copy(s->url, href, sizeof(s->url));
            s->url_len = str_len(s->url);
            navigate(s, 1);
        }
        return;
    }

    s->address_focused = 0;
    html_field_focus(-1);
}
