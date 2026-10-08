#include <stdio.h>
#include <grv/syscall.h>
#include <grv/wm.h>
#include <gravity/image.h>
#include "wm_internal.h"

// wm_windows — window geometry, decoration, lifecycle and the WM-owned
// placement/resize protocol. The core (wm.cpp) owns the overlay and the
// window table; these functions manipulate the table and ask the compositor
// to move/restack/resize the client surfaces.

// --- window geometry + decoration -----------------------------------

void decor_rect(const Win* w, int* rx, int* ry, int* rw, int* rh) {
    *rx = w->x - BORDER;
    *ry = w->y - TITLE_H;
    *rw = w->w + 2 * BORDER;
    *rh = TITLE_H + w->h + BORDER;
}

void close_rect(const Win* w, int* rx, int* ry, int* rw, int* rh) {
    *rw = CLOSE_W;
    *rh = CLOSE_H;
    *rx = w->x + w->w - CLOSE_W - 2;
    *ry = w->y - TITLE_H + (TITLE_H - CLOSE_H) / 2;
}

void max_rect(const Win* w, int* rx, int* ry, int* rw, int* rh) {
    *rw = MAXBOX_W;
    *rh = CLOSE_H;
    *rx = w->x + w->w - CLOSE_W - 2 - MAXBOX_W - 3;
    *ry = w->y - TITLE_H + (TITLE_H - CLOSE_H) / 2;
}

int in_title(const Win* w, int px, int py) {
    return in_rect(px, py, w->x - BORDER, w->y - TITLE_H, w->w + 2 * BORDER, TITLE_H);
}

int in_body(const Win* w, int px, int py) {
    return in_rect(px, py, w->x, w->y, w->w, w->h);
}

int in_close(const Win* w, int px, int py) {
    int x, y, cw, ch;
    close_rect(w, &x, &y, &cw, &ch);
    return in_rect(px, py, x, y, cw, ch);
}

int in_maxbox(const Win* w, int px, int py) {
    int x, y, cw, ch;
    max_rect(w, &x, &y, &cw, &ch);
    return in_rect(px, py, x, y, cw, ch);
}

// The resize grab margin, straddling the edge: 2 px inside and 3 px outside, so
// a window flush to the screen edge can still be grabbed, and an app's own
// right edge (a scrollbar) keeps most of its clicks.
int in_resize_band(const Win* w, int px, int py) {
    if (in_rect(px, py, w->x + w->w - 2, w->y, RESIZE_BAND, w->h + RESIZE_BAND))
        return 1;   // right edge (and the corner)
    if (in_rect(px, py, w->x, w->y + w->h - 2, w->w + RESIZE_BAND, RESIZE_BAND))
        return 1;   // bottom edge
    return 0;
}

void draw_window(const Win* w) {
    if (w->role == GRV_WM_ROLE_POPUP) return; // popups have no decoration
    unsigned int bar = (w->owner == focused) ? FOCUS_BAR : UNFOCUS_BAR;
    int tx = w->x - BORDER, ty = w->y - TITLE_H;

    fill(tx, ty, w->w + 2 * BORDER, TITLE_H, bar);          // title bar
    fill(tx, w->y, BORDER, w->h, EDGE);                     // left
    fill(w->x + w->w, w->y, BORDER, w->h, EDGE);            // right
    fill(w->x, w->y + w->h, w->w, BORDER, EDGE);            // bottom

    int cx, cy, cw, ch;
    close_rect(w, &cx, &cy, &cw, &ch);
    fill(cx, cy, cw, ch, CLOSE_BG);                         // close box

    // The maximize box, left of it: filled when maximized.
    max_rect(w, &cx, &cy, &cw, &ch);
    if (w->maximized) fill(cx, cy, cw, ch, MAXBOX_ON);
    else              fill(cx, cy, cw, ch, MAXBOX_BG);

    // Title text, clipped to the bar (never over the close box).
    int sx1 = clip_x1;
    clip_x1 = mini(clip_x1, w->x + w->w - CLOSE_W - 4);
    text(tx + 4, ty + (TITLE_H - (font_ok ? (int)font.height : 8)) / 2, w->title, TITLE_FG);
    clip_x1 = sx1;
}

Win* find_sid(int sid) {
    for (int i = 0; i < nwin; i++) if (wins[i].sid == sid) return &wins[i];
    return 0;
}

Win* find_owner(int owner) {
    for (int i = 0; i < nwin; i++) if (wins[i].owner == owner) return &wins[i];
    return 0;
}

// --- window management ----------------------------------------------

void focus_window(Win* w) {
    grv_surface_raise(w->sid);   // above other windows
    raise_overlay();             // decorations stay on top

    // Move to the end of the list (top for decoration overlap).
    int idx = (int)(w - wins);
    Win tmp = *w;
    for (int i = idx; i < nwin - 1; i++) wins[i] = wins[i + 1];
    wins[nwin - 1] = tmp;

    if (focused != tmp.owner) {
        focused = tmp.owner;
        grv_input_focus(focused);
        printf("wm: focus task %d\n", focused);
    }
    full_redraw();
}

// Alt+Tab / Alt+Shift+Tab: cycle keyboard focus through the windows.
void cycle_focus(int dir) {
    if (nwin == 0) return;
    int idx = -1;
    for (int i = 0; i < nwin; i++)
        if (wins[i].owner == focused) { idx = i; break; }
    int next = (idx < 0) ? (dir > 0 ? 0 : nwin - 1)
                         : (idx + dir + nwin) % nwin;
    focus_window(&wins[next]);
}

static void owner_name(int owner, char* out) {
    static struct grv_taskinfo tl[32];
    int n = grv_task_list(tl, 32);
    for (int i = 0; i < 16; i++) out[i] = 0;
    for (int i = 0; i < n; i++) {
        if (tl[i].id == owner) {
            for (int j = 0; j < 15 && tl[i].name[j]; j++) out[j] = tl[i].name[j];
            return;
        }
    }
    // fallback
    out[0] = '?'; out[1] = 0;
}

void add_window(int sid) {
    if (find_sid(sid)) return;
    if (nwin >= MAXWIN) return;

    static struct grv_surface_info list[48];
    int n = grv_surface_list(list, 48);
    struct grv_surface_info* si = 0;
    for (int i = 0; i < n; i++) if ((int)list[i].id == sid) { si = &list[i]; break; }
    if (si == 0) return;
    if ((int)si->owner == 0) return; // kernel-owned: not a window

    Win* w = &wins[nwin];
    w->sid = sid;
    w->owner = (int)si->owner;
    w->w = (int)si->width;
    w->h = (int)si->height;
    w->role = GRV_WM_ROLE_NORMAL;
    w->close_deadline = 0;
    w->maximized = 0;
    w->rest_x = w->rest_y = w->rest_w = w->rest_h = 0;
    owner_name(w->owner, w->title);

    // Cascade new windows so they do not pile up at (0,0).
    int slot = nwin % 6;
    w->x = 80 + slot * 32;
    w->y = 60 + slot * 32;
    if (w->x + w->w > overlay_w) w->x = mini(w->x, overlay_w - w->w);
    if (w->y + w->h > overlay_h - TASKBAR_H)
        w->y = overlay_h - TASKBAR_H - w->h;
    if (w->y < TITLE_H) w->y = TITLE_H;
    grv_surface_move(sid, w->x, w->y);

    nwin++;
    // A new window is raised above the others but does NOT steal focus:
    // the console stays focused until the user clicks a window. (A WM that
    // auto-focuses would break the shell the moment a window appears.)
    grv_surface_raise(sid);
    raise_overlay();

    // Ask the owner for its title (pure userland IPC). A client that does
    // not answer keeps the task name we already set.
    struct grv_msg hello;
    hello.from = 0;
    hello.type = GRV_WM_HELLO;
    hello.a = (unsigned long)sid;
    hello.b = 0;
    grv_send(w->owner, &hello);

    printf("wm: window #%d '%s' %dx%d at %d,%d\n", sid, w->title, w->w, w->h, w->x, w->y);
    full_redraw();
}

void remove_window(int sid) {
    int idx = -1;
    for (int i = 0; i < nwin; i++) if (wins[i].sid == sid) { idx = i; break; }
    if (idx < 0) return;

    int owner = wins[idx].owner;
    if (resize_sid == sid) { resizing = 0; resize_sid = -1; }
    if (drag_sid == sid) { dragging = 0; drag_sid = -1; }
    for (int i = idx; i < nwin - 1; i++) wins[i] = wins[i + 1];
    nwin--;

    if (focused == owner) {
        focus_console();
    }
    printf("wm: window #%d gone\n", sid);
    full_redraw();
}

void scan_existing() {
    static struct grv_surface_info list[48];
    int n = grv_surface_list(list, 48);
    for (int i = 0; i < n; i++) {
        int sid = (int)list[i].id;
        if (sid == overlay_id) continue;
        if ((int)list[i].owner == 0) continue;
        add_window(sid);
    }
}

// --- window placement -------------------------------------------------
// The WM owns where a window sits (clients never move themselves), so both a
// title-bar drag and a workspace layout go through place_window.

void place_window(Win* w, int x, int y) {
    if (x < BORDER) x = BORDER;
    if (y < TITLE_H) y = TITLE_H;
    if (x + w->w > overlay_w - BORDER) x = overlay_w - BORDER - w->w;
    if (y + w->h > overlay_h - TASKBAR_H) y = overlay_h - TASKBAR_H - w->h;
    if (x == w->x && y == w->y) return;

    int oa, ob, oc, od;
    decor_rect(w, &oa, &ob, &oc, &od);

    grv_surface_move(w->sid, x, y);
    w->x = x; w->y = y;

    int na, nb, nc, nd;
    decor_rect(w, &na, &nb, &nc, &nd);
    redraw(mini(oa, na) - 1, mini(ob, nb) - 1,
           maxi(oa + oc, na + nc) + 1, maxi(ob + od, nb + nd) + 1);
}

// --- resizing ---------------------------------------------------------
// The client owns its surface's pixels, so the WM only *asks* for a size
// (GRV_WM_RESIZE); the client answers GRV_WM_RESIZED with the size it took.
// The WM's own w/h follow the request at once, so the decoration tracks the
// drag, and the answer corrects them if the client clamped.

void request_resize(Win* w, int nw, int nh) {
    if (nw < MIN_W) nw = MIN_W;
    if (nh < MIN_H) nh = MIN_H;
    if (nw > overlay_w - 2 * BORDER) nw = overlay_w - 2 * BORDER;
    if (nh > overlay_h - TASKBAR_H - TITLE_H - BORDER)
        nh = overlay_h - TASKBAR_H - TITLE_H - BORDER;
    if (nw == (int)w->w && nh == (int)w->h) return;

    int oa, ob, oc, od;
    decor_rect(w, &oa, &ob, &oc, &od);

    w->w = nw;   // optimistic: the decoration follows the pointer now
    w->h = nh;

    int na, nb, nc, nd;
    decor_rect(w, &na, &nb, &nc, &nd);
    redraw(mini(oa, na) - 1, mini(ob, nb) - 1,
           maxi(oa + oc, na + nc) + 1, maxi(ob + od, nb + nd) + 1);

    struct grv_msg m;
    m.from = 0;
    m.type = GRV_WM_RESIZE;
    m.a = (unsigned long)w->sid;
    m.b = grv_wm_size_pack(nw, nh);
    grv_send(w->owner, &m);
}

/* The size a pointer position asks for, given where the drag began. */
void resize_wanted(int cx, int cy, int* nw, int* nh) {
    *nw = resize_w0 + (cx - resize_px);
    *nh = resize_h0 + (cy - resize_py);
    if (resize_edge == 1) *nh = resize_h0; // right edge: height unchanged
    if (resize_edge == 2) *nw = resize_w0; // bottom edge: width unchanged
}

void maximize_toggle(Win* w) {
    if (!w->maximized) {
        w->rest_x = w->x; w->rest_y = w->y;
        w->rest_w = w->w; w->rest_h = w->h;
        w->maximized = 1;
        request_resize(w, overlay_w - 2 * BORDER,
                       overlay_h - TASKBAR_H - TITLE_H - BORDER);
        place_window(w, BORDER, TITLE_H);
    } else {
        w->maximized = 0;
        request_resize(w, w->rest_w, w->rest_h);
        place_window(w, w->rest_x, w->rest_y);
    }
    printf("wm: maximize #%d %s\n", w->sid, w->maximized ? "on" : "off");
    full_redraw();
}

// --- wallpaper ------------------------------------------------------
// Decode a picture (gravity_image) and scale it to COVER the screen (crop the
// overflow) into an opaque full-screen surface at the bottom of the stack.

int load_wallpaper(const char* path, int sw, int sh) {
    struct gravity_image img;
    if (gravity_image_load_mapped(&img, path) != 0) return -1;

    struct grv_surface_info si;
    int sid = grv_surface_create((unsigned long)sw, (unsigned long)sh, &si);
    if (sid < 0) { gravity_image_free(&img); return -1; }

    // Cover: scale by the larger ratio, centre the overflow.
    unsigned long rx = ((unsigned long)sw << 16) / (unsigned long)img.w;
    unsigned long ry = ((unsigned long)sh << 16) / (unsigned long)img.h;
    unsigned long scale = rx > ry ? rx : ry;
    if (scale == 0) scale = 1;
    int offx = (int)((((unsigned long)img.w * scale) >> 16) - (unsigned long)sw) / 2;
    int offy = (int)((((unsigned long)img.h * scale) >> 16) - (unsigned long)sh) / 2;

    static int colmap[4096], rowmap[4096];
    for (int x = 0; x < sw; x++) {
        int s = (int)(((unsigned long)(x + offx) << 16) / scale);
        colmap[x] = s < 0 ? 0 : (s >= img.w ? img.w - 1 : s);
    }
    for (int y = 0; y < sh; y++) {
        int s = (int)(((unsigned long)(y + offy) << 16) / scale);
        rowmap[y] = s < 0 ? 0 : (s >= img.h ? img.h - 1 : s);
    }

    unsigned int* px = (unsigned int*)si.address;
    for (int y = 0; y < sh; y++) {
        const unsigned int* srow = img.px + (unsigned long)rowmap[y] * (unsigned long)img.w;
        unsigned int* drow = px + (unsigned long)y * (unsigned long)sw;
        for (int x = 0; x < sw; x++) drow[x] = srow[colmap[x]];
    }
    gravity_image_free(&img);
    grv_surface_lower(sid); // behind every window
    return sid;
}
