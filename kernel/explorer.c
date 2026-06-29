/* kernel/explorer.c
 * Draws a folder listing into a window and handles clicks: ".." goes up, a
 * folder opens, a file pops up an info window showing its contents. The window
 * remembers its current directory (w->cwd) and selected row (w->sel).
 */
#include "explorer.h"
#include "wm.h"
#include "vfs.h"
#include "framebuffer.h"
#include "console.h"

#define HDR_H 24
#define ROW_H 18

/* Build the visible row order: optional ".." then dirs, then files.
 * Each entry is a node index, or -1 meaning the ".." up-row. */
static int explorer_rows(Window *w, int *entries, int max)
{
    int n = 0;
    if (w->cwd != vfs_root() && n < max) entries[n++] = -1;

    int kids[64];
    int k = vfs_children(w->cwd, kids, 64);
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < k && n < max; i++) {
            const VNode *c = vfs_node(kids[i]);
            if ((pass == 0) != (c->is_dir != 0)) continue;  /* dirs, then files */
            entries[n++] = kids[i];
        }
    return n;
}

void explorer_paint(Window *w, int cx, int cy, int cw, int ch)
{
    color_t bg     = fb_rgb(245, 245, 250);
    color_t hdr    = fb_rgb(222, 226, 235);
    color_t fgdir  = fb_rgb(40, 80, 200);
    color_t fgfile = fb_rgb(40, 40, 48);
    color_t selbg  = fb_rgb(180, 205, 255);

    fb_fill_rect(cx, cy, cw, ch, bg);
    fb_fill_rect(cx, cy, cw, HDR_H, hdr);
    const VNode *d = vfs_node(w->cwd);
    gfx_text(cx + 8, cy + 4, (d && d->name[0]) ? d->name : "/", fb_rgb(20, 20, 20), hdr);

    int entries[80];
    int cnt = explorer_rows(w, entries, 80);
    for (int r = 0; r < cnt; r++) {
        int y = cy + HDR_H + r * ROW_H;
        if (y + ROW_H > cy + ch) break;             /* clip to the window */

        int selected = (w->sel == r);
        color_t rowbg = selected ? selbg : bg;
        if (selected) fb_fill_rect(cx, y, cw, ROW_H, selbg);

        if (entries[r] == -1) {
            gfx_text(cx + 8, y + 1, "[..]", fgdir, rowbg);
        } else {
            const VNode *c = vfs_node(entries[r]);
            char line[40];
            int p = 0;
            const char *pfx = c->is_dir ? "[D] " : "    ";
            for (int i = 0; pfx[i] && p < 39; i++)     line[p++] = pfx[i];
            for (int i = 0; c->name[i] && p < 39; i++) line[p++] = c->name[i];
            line[p] = '\0';
            gfx_text(cx + 8, y + 1, line, c->is_dir ? fgdir : fgfile, rowbg);
        }
    }
}

void explorer_click(Window *w, int lx, int ly)
{
    (void)lx;
    if (ly < HDR_H) return;

    int entries[80];
    int cnt = explorer_rows(w, entries, 80);
    int row = (ly - HDR_H) / ROW_H;
    if (row < 0 || row >= cnt) return;

    w->sel = row;
    int target = entries[row];

    if (target == -1) {                              /* ".." up one level */
        const VNode *d = vfs_node(w->cwd);
        if (d && d->parent >= 0) { w->cwd = d->parent; w->sel = -1; }
        return;
    }

    const VNode *c = vfs_node(target);
    if (!c) return;
    if (c->is_dir) {                                 /* enter folder */
        w->cwd = target; w->sel = -1;
    } else {                                         /* open file -> info window */
        wm_create_info(w->x + 34, w->y + 40, 320, 170,
                       c->name, c->content ? c->content : "(empty)");
    }
}

void app_launch_explorer(void)
{
    static int n = 0;
    int x = 140 + (n % 5) * 26;
    int y = 70  + (n % 5) * 26;
    n++;
    wm_create_explorer(x, y, 360, 300, "File Explorer", vfs_root());
}
