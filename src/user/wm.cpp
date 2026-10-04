#include <stdio.h>
#include <grv/syscall.h>
#include <grv/text.h>
#include <grv/input.h>

// wm — the Gravitas window manager (userland policy).
//
// The kernel composites; the WM decides geometry, z-order and focus. It:
//   * claims the display authority (may move/restack any window, owns the
//     mouse, sets keyboard focus);
//   * draws decorations into one full-screen alpha-transparent overlay
//     surface, raised above every window (title bars + borders + close box);
//   * discovers windows from EVT_SURFACE_ADDED/REMOVED, places new ones,
//     focuses on click, drags by the title bar, closes via the close box
//     (KILL the owner — teardown removes the surface).
//
// It draws no window content: each client owns its pixels and commits them.

static const int TITLE_H = 18;
static const int BORDER  = 2;
static const int CLOSE_W = 12;
static const int CLOSE_H = 12;
static const int MAXWIN  = 32;

static const unsigned int FOCUS_BAR   = 0xFF3B5A8C;
static const unsigned int UNFOCUS_BAR = 0xFF4A4A4A;
static const unsigned int EDGE        = 0xFF202020;
static const unsigned int CLOSE_BG    = 0xFFC04040;
static const unsigned int TITLE_FG    = 0xFFFFFFFF;

struct Win {
    int  sid;          // surface id
    int  owner;        // owning task id
    int  x, y;         // surface top-left on screen
    int  w, h;         // surface size
    char title[16];
};

static int   overlay_id = -1;
static unsigned int* overlay_px = 0;
static int   overlay_w = 0, overlay_h = 0;

static struct grv_font font;
static int   font_ok = 0;

static Win   wins[MAXWIN];
static int   nwin = 0;

static int   focused = -1;       // focused task id, or -1
static int   console_task = -1;  // the shell: focus returns here on desktop clicks
static int   click_sid = -1;     // window that received the button-down (for release)
static int   last_fx = -1, last_fy = -1; // last forwarded pointer position
static int   dragging = 0;
static int   drag_sid = -1;
static int   drag_off_x = 0, drag_off_y = 0;
static unsigned char btn_prev = 0;

static int mini(int a, int b) { return a < b ? a : b; }
static int maxi(int a, int b) { return a > b ? a : b; }

// Find a task by name (the console is the shell). The WM focuses it when no
// window is focused, so keyboard input has a deterministic owner.
static int find_task_by_name(const char* name) {
    struct grv_taskinfo tl[32];
    int n = grv_task_list(tl, 32);
    for (int i = 0; i < n; i++) {
        int j = 0;
        while (name[j] && tl[i].name[j] == name[j]) j++;
        if (name[j] == 0 && tl[i].name[j] == 0) return tl[i].id;
    }
    return -1;
}

static void focus_console() {
    focused = -1;
    grv_input_focus(console_task); // -1 if the shell is gone (legacy behavior)
}

// --- clipped drawing into the overlay -------------------------------

static int clip_x0, clip_y0, clip_x1, clip_y1;

static void fill(int x, int y, int w, int h, unsigned int c) {
    for (int yy = y; yy < y + h; yy++) {
        if (yy < clip_y0 || yy >= clip_y1) continue;
        for (int xx = x; xx < x + w; xx++) {
            if (xx < clip_x0 || xx >= clip_x1) continue;
            overlay_px[(unsigned long)yy * overlay_w + xx] = c;
        }
    }
}

static void text(int x, int y, const char* s, unsigned int fg) {
    if (!font_ok) return;
    unsigned gh = font.height;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c >= 32 && c <= 126) {
            const unsigned char* g = font.glyphs + (unsigned long)c * gh;
            for (unsigned row = 0; row < gh; row++) {
                int yy = y + (int)row;
                if (yy < clip_y0 || yy >= clip_y1) continue;
                unsigned char bits = g[row];
                for (int col = 0; col < 8; col++) {
                    if (!(bits & (0x80 >> col))) continue;
                    int xx = x + col;
                    if (xx < clip_x0 || xx >= clip_x1) continue;
                    overlay_px[(unsigned long)yy * overlay_w + xx] = fg;
                }
            }
        }
        x += 8;
    }
}

// --- window geometry + decoration -----------------------------------

static void decor_rect(const Win* w, int* rx, int* ry, int* rw, int* rh) {
    *rx = w->x - BORDER;
    *ry = w->y - TITLE_H;
    *rw = w->w + 2 * BORDER;
    *rh = TITLE_H + w->h + BORDER;
}

static void close_rect(const Win* w, int* rx, int* ry, int* rw, int* rh) {
    *rw = CLOSE_W;
    *rh = CLOSE_H;
    *rx = w->x + w->w - CLOSE_W - 2;
    *ry = w->y - TITLE_H + (TITLE_H - CLOSE_H) / 2;
}

static int in_rect(int px, int py, int x, int y, int w, int h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

static int in_title(const Win* w, int px, int py) {
    return in_rect(px, py, w->x - BORDER, w->y - TITLE_H, w->w + 2 * BORDER, TITLE_H);
}

static int in_body(const Win* w, int px, int py) {
    return in_rect(px, py, w->x, w->y, w->w, w->h);
}

static int in_close(const Win* w, int px, int py) {
    int x, y, cw, ch;
    close_rect(w, &x, &y, &cw, &ch);
    return in_rect(px, py, x, y, cw, ch);
}

static void draw_window(const Win* w) {
    unsigned int bar = (w->owner == focused) ? FOCUS_BAR : UNFOCUS_BAR;
    int tx = w->x - BORDER, ty = w->y - TITLE_H;

    fill(tx, ty, w->w + 2 * BORDER, TITLE_H, bar);          // title bar
    fill(tx, w->y, BORDER, w->h, EDGE);                     // left
    fill(w->x + w->w, w->y, BORDER, w->h, EDGE);            // right
    fill(w->x, w->y + w->h, w->w, BORDER, EDGE);            // bottom

    int cx, cy, cw, ch;
    close_rect(w, &cx, &cy, &cw, &ch);
    fill(cx, cy, cw, ch, CLOSE_BG);                         // close box

    // Title text, clipped to the bar (never over the close box).
    int sx1 = clip_x1;
    clip_x1 = mini(clip_x1, w->x + w->w - CLOSE_W - 4);
    text(tx + 4, ty + (TITLE_H - (font_ok ? (int)font.height : 8)) / 2, w->title, TITLE_FG);
    clip_x1 = sx1;
}

static Win* find_sid(int sid) {
    for (int i = 0; i < nwin; i++) if (wins[i].sid == sid) return &wins[i];
    return 0;
}

// Repaint a screen rectangle of the overlay: clear it, then redraw every
// window (bottom-to-top) that overlaps it, clipped to the rectangle.
static void redraw(int x0, int y0, int x1, int y1) {
    x0 = maxi(x0, 0); y0 = maxi(y0, 0);
    x1 = mini(x1, overlay_w); y1 = mini(y1, overlay_h);
    if (x1 <= x0 || y1 <= y0) return;

    clip_x0 = x0; clip_y0 = y0; clip_x1 = x1; clip_y1 = y1;
    fill(x0, y0, x1 - x0, y1 - y0, 0); // transparent

    for (int i = 0; i < nwin; i++) {
        int a, b, c, d;
        decor_rect(&wins[i], &a, &b, &c, &d);
        if (a < x1 && a + c > x0 && b < y1 && b + d > y0) draw_window(&wins[i]);
    }

    grv_surface_commit_rect(x0, y0, x1 - x0, y1 - y0);
}

static void full_redraw() { redraw(0, 0, overlay_w, overlay_h); }

// --- window management ----------------------------------------------

static void raise_overlay() { grv_surface_raise(overlay_id); }

static void focus_window(Win* w) {
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

static void owner_name(int owner, char* out) {
    struct grv_taskinfo tl[32];
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

static void add_window(int sid) {
    if (find_sid(sid)) return;
    if (nwin >= MAXWIN) return;

    struct grv_surface_info list[48];
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
    owner_name(w->owner, w->title);

    // Cascade new windows so they do not pile up at (0,0).
    int slot = nwin % 6;
    w->x = 80 + slot * 32;
    w->y = 60 + slot * 32;
    if (w->x + w->w > overlay_w) w->x = mini(w->x, overlay_w - w->w);
    grv_surface_move(sid, w->x, w->y);

    nwin++;
    // A new window is raised above the others but does NOT steal focus:
    // the console stays focused until the user clicks a window. (A WM that
    // auto-focuses would break the shell the moment a window appears.)
    grv_surface_raise(sid);
    raise_overlay();
    printf("wm: window #%d '%s' %dx%d at %d,%d\n", sid, w->title, w->w, w->h, w->x, w->y);
    full_redraw();
}

static void remove_window(int sid) {
    int idx = -1;
    for (int i = 0; i < nwin; i++) if (wins[i].sid == sid) { idx = i; break; }
    if (idx < 0) return;

    int owner = wins[idx].owner;
    for (int i = idx; i < nwin - 1; i++) wins[i] = wins[i + 1];
    nwin--;

    if (focused == owner) {
        focus_console();
    }
    printf("wm: window #%d gone\n", sid);
    full_redraw();
}

static void scan_existing() {
    struct grv_surface_info list[48];
    int n = grv_surface_list(list, 48);
    for (int i = 0; i < n; i++) {
        int sid = (int)list[i].id;
        if (sid == overlay_id) continue;
        if ((int)list[i].owner == 0) continue;
        add_window(sid);
    }
}

// --- input ----------------------------------------------------------

// Send a mouse event to the task owning window 'w', in window-local
// coordinates. 'buttons' is the current button mask.
static void forward_mouse(Win* w, int buttons, int cx, int cy) {
    struct grv_msg m;
    m.from = 0;
    m.type = GRV_EV_MOUSE;
    m.a = (unsigned long)buttons;
    m.b = grv_mouse_pack(cx - w->x, cy - w->y);
    grv_send(w->owner, &m);
}

// Top-most window whose client body contains the point, or 0.
static Win* window_body_at(int cx, int cy) {
    for (int i = nwin - 1; i >= 0; i--) {
        if (in_body(&wins[i], cx, cy)) return &wins[i];
    }
    return 0;
}

static void on_press(int cx, int cy) {
    for (int i = nwin - 1; i >= 0; i--) {
        Win* w = &wins[i];
        if (in_close(w, cx, cy)) {
            printf("wm: close #%d\n", w->sid);
            grv_kill(w->owner, 0); // teardown posts EVT_SURFACE_REMOVED
            return;
        }
        if (in_title(w, cx, cy)) {
            focus_window(w);
            dragging = 1;
            drag_sid = w->sid;
            drag_off_x = cx - w->x;
            drag_off_y = cy - w->y;
            return;
        }
        if (in_body(w, cx, cy)) {
            focus_window(w);
            forward_mouse(w, GRV_BTN_LEFT, cx, cy); // deliver the click
            click_sid = w->sid;
            return;
        }
    }
    if (focused != -1) { // desktop click: back to the console
        focus_console();
        full_redraw();
    }
}

static void on_release(int cx, int cy) {
    if (click_sid >= 0) {
        Win* w = find_sid(click_sid);
        if (w != 0) forward_mouse(w, 0, cx, cy);
        click_sid = -1;
    }
    dragging = 0;
    drag_sid = -1;
}

static void drag_to(int cx, int cy) {
    Win* w = find_sid(drag_sid);
    if (w == 0) { on_release(cx, cy); return; }

    int nx = cx - drag_off_x;
    int ny = cy - drag_off_y;
    if (nx == w->x && ny == w->y) return;

    int oa, ob, oc, od;
    decor_rect(w, &oa, &ob, &oc, &od);

    grv_surface_move(w->sid, nx, ny);
    w->x = nx; w->y = ny;

    int na, nb, nc, nd;
    decor_rect(w, &na, &nb, &nc, &nd);

    int x0 = mini(oa, na) - 1, y0 = mini(ob, nb) - 1;
    int x1 = maxi(oa + oc, na + nc) + 1, y1 = maxi(ob + od, nb + nd) + 1;
    redraw(x0, y0, x1, y1);
}

// --- main -----------------------------------------------------------

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    if (grv_wm_claim() != 0) {
        printf("wm: display authority already held\n");
        return 1;
    }

    // Keep keyboard input owned deterministically: focus the console until
    // the user clicks a window.
    console_task = find_task_by_name("shell");
    grv_input_focus(console_task);

    font_ok = grv_font_load(&font, "/fonts/bizcat.psf");

    struct grv_sysinfo si;
    grv_sysinfo(&si);
    overlay_w = (int)si.screen_w;
    overlay_h = (int)si.screen_h;
    if (overlay_w <= 0 || overlay_h <= 0) {
        printf("wm: no framebuffer\n");
        return 1;
    }

    struct grv_surface_info oi;
    overlay_id = grv_surface_create((unsigned long)overlay_w,
                                    (unsigned long)overlay_h, &oi);
    if (overlay_id < 0) {
        printf("wm: overlay create failed\n");
        return 1;
    }
    overlay_px = (unsigned int*)oi.address;
    raise_overlay();
    full_redraw();

    printf("wm: display authority, screen %dx%d, overlay #%d, font %s\n",
           overlay_w, overlay_h, overlay_id, font_ok ? "yes" : "no");

    scan_existing();

    while (1) {
        struct grv_msg m;
        while (grv_recv(&m, 0) == 0) {
            if (m.type == GRV_EVT_SURFACE_ADDED) add_window((int)m.a);
            else if (m.type == GRV_EVT_SURFACE_REMOVED) remove_window((int)m.a);
        }

        grv_sysinfo(&si);
        int cx = (int)si.cursor_x, cy = (int)si.cursor_y;

        // Collapse button edges across the drained packets, then act once.
        struct grv_mouse_packet p;
        bool pressed = false, released = false;
        while (grv_mouse_read(&p)) {
            int left = p.buttons & 1;
            if (left && !(btn_prev & 1)) pressed = true;
            if (!left && (btn_prev & 1)) released = true;
            btn_prev = p.buttons;
        }

        if (pressed) on_press(cx, cy);
        if (released) on_release(cx, cy);
        if (dragging) {
            drag_to(cx, cy);
        } else if (!pressed && !released) {
            // Hover motion goes to the window under the pointer.
            Win* w = window_body_at(cx, cy);
            if (w && (cx != last_fx || cy != last_fy))
                forward_mouse(w, btn_prev, cx, cy);
        }
        last_fx = cx; last_fy = cy;

        grv_sleep(8);
    }
    return 0;
}
