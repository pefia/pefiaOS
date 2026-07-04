/* Folder listing rendered into a window, with clicks handled directly here
 * rather than through some generic widget system - ".." goes up a level, a
 * folder navigates into it, a file pops open an info window with its
 * contents. Each window tracks its own current directory (w->cwd) and
 * selected row (w->sel), so multiple explorer windows browse independently. */
#include "explorer.h"
#include "wm.h"
#include "vfs.h"
#include "framebuffer.h"
#include "console.h"

#define HDR_H 24
#define ROW_H 18

/* Builds the row order shown on screen: an optional ".." row, then dirs,
 * then files. Each slot in `entries` holds a vfs node index, or -1 for
 * the ".." row - keeps the paint and click code walking identical lists. */
static int list_rows(Window *w, int *entries, int max)
{
    int n = 0;
    if (w->cwd != vfs_root() && n < max) entries[n++] = -1;

    int kids[64];
    int kid_count = vfs_children(w->cwd, kids, 64);
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < kid_count && n < max; i++) {
            const VNode *c = vfs_node(kids[i]);
            if ((pass == 0) != (c->is_dir != 0)) continue;  /* dirs on pass 0, files on pass 1 */
            entries[n++] = kids[i];
        }
    return n;
}

void explorer_paint(Window *w, int cx, int cy, int cw, int ch)
{
    color_t bg       = fb_rgb(245, 245, 250);
    color_t hdr_bg   = fb_rgb(222, 226, 235);
    color_t dir_ink  = fb_rgb(40, 80, 200);
    color_t file_ink = fb_rgb(40, 40, 48);
    color_t sel_bg   = fb_rgb(180, 205, 255);

    fb_fill_rect(cx, cy, cw, ch, bg);
    fb_fill_rect(cx, cy, cw, HDR_H, hdr_bg);
    const VNode *dir = vfs_node(w->cwd);
    gfx_text(cx + 8, cy + 4, (dir && dir->name[0]) ? dir->name : "/", fb_rgb(20, 20, 20), hdr_bg);

    int entries[80];
    int count = list_rows(w, entries, 80);
    for (int r = 0; r < count; r++) {
        int y = cy + HDR_H + r * ROW_H;
        if (y + ROW_H > cy + ch) break;   /* rest would spill past the window */

        int selected = (w->sel == r);
        color_t row_bg = selected ? sel_bg : bg;
        if (selected) fb_fill_rect(cx, y, cw, ROW_H, sel_bg);

        if (entries[r] == -1) {
            gfx_text(cx + 8, y + 1, "[..]", dir_ink, row_bg);
        } else {
            const VNode *node = vfs_node(entries[r]);
            char line[40];
            int p = 0;
            const char *prefix = node->is_dir ? "[D] " : "    ";
            for (int i = 0; prefix[i] && p < 39; i++)     line[p++] = prefix[i];
            for (int i = 0; node->name[i] && p < 39; i++) line[p++] = node->name[i];
            line[p] = '\0';
            gfx_text(cx + 8, y + 1, line, node->is_dir ? dir_ink : file_ink, row_bg);
        }
    }
}

void explorer_click(Window *w, int lx, int ly)
{
    (void)lx;
    if (ly < HDR_H) return;

    int entries[80];
    int count = list_rows(w, entries, 80);
    int row = (ly - HDR_H) / ROW_H;
    if (row < 0 || row >= count) return;

    w->sel = row;
    int target = entries[row];

    if (target == -1) {   /* ".." - step up to the parent dir */
        const VNode *dir = vfs_node(w->cwd);
        if (dir && dir->parent >= 0) { w->cwd = dir->parent; w->sel = -1; }
        return;
    }

    const VNode *node = vfs_node(target);
    if (!node) return;
    if (node->is_dir) {
        w->cwd = target; w->sel = -1;
    } else {
        wm_create_info(w->x + 34, w->y + 40, 320, 170,
                       node->name, node->content ? node->content : "(empty)");
    }
}

void app_launch_explorer(void)
{
    static int spawn_count = 0;
    int x = 140 + (spawn_count % 5) * 26;
    int y = 70  + (spawn_count % 5) * 26;
    spawn_count++;
    wm_create_explorer(x, y, 360, 300, "File Explorer", vfs_root());
}
