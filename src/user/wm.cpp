#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <grv/syscall.h>
#include <grv/input.h>
#include <grv/wm.h>
#include "wm_internal.h"

// wm — the Gravitas window manager (userland policy).
//
// The kernel composites; the WM decides geometry, z-order and focus. It:
//   * claims the display authority (may move/restack any window, owns the
//     mouse, sets keyboard focus);
//   * draws decorations into one full-screen alpha-transparent overlay
//     surface, raised above every window (title bars + borders + close box);
//   * discovers windows from EVT_SURFACE_ADDED/REMOVED, places new ones,
//     focuses on click, drags by the title bar, and closes via the close box
//     by asking the client to exit (force-killing one that ignores the ask).
//
// It draws no window content: each client owns its pixels and commits them.
//
// This file is the core: the shared state, the clipped drawing primitives and
// the main loop. The window geometry/lifecycle, taskbar, launcher and input
// routing live in wm_windows.cpp, wm_taskbar.cpp, wm_menu.cpp and
// wm_input.cpp; wm_internal.h is the seam between them.

// --- shared state ---------------------------------------------------

int   overlay_id = -1;
unsigned int* overlay_px = 0;
int   overlay_w = 0, overlay_h = 0;

struct gravity_font font;
int   font_ok = 0;

Win   wins[MAXWIN];
int   nwin = 0;

int   focused = -1;       // focused task id, or -1
int   console_task = -1;  // the shell: focus returns here on desktop clicks

static int   last_fx = -1, last_fy = -1; // last forwarded pointer position
static unsigned char btn_prev = 0;

// --- clipped drawing into the overlay -------------------------------

int clip_x0, clip_y0, clip_x1, clip_y1;

// Decoration occlusion. While drawing a window's decoration into the overlay,
// pixels inside these rectangles are skipped: they are the CLIENT areas of
// windows stacked above it, whose real surfaces the compositor paints *below*
// the overlay. Without this, a lower window's title bar/border would be drawn
// over a higher window (the overlay is above every surface).
struct ClipRect { int x, y, w, h; };
static ClipRect occl[MAXWIN];
static int noccl = 0;

static int occluded(int x, int y) {
    for (int i = 0; i < noccl; i++) {
        const ClipRect* r = &occl[i];
        if (x >= r->x && x < r->x + r->w &&
            y >= r->y && y < r->y + r->h) return 1;
    }
    return 0;
}

void fill(int x, int y, int w, int h, unsigned int c) {
    for (int yy = y; yy < y + h; yy++) {
        if (yy < clip_y0 || yy >= clip_y1) continue;
        for (int xx = x; xx < x + w; xx++) {
            if (xx < clip_x0 || xx >= clip_x1) continue;
            if (noccl && occluded(xx, yy)) continue;
            overlay_px[(unsigned long)yy * overlay_w + xx] = c;
        }
    }
}

void text(int x, int y, const char* s, unsigned int fg) {
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
                    if (noccl && occluded(xx, yy)) continue;
                    overlay_px[(unsigned long)yy * overlay_w + xx] = fg;
                }
            }
        }
        x += 8;
    }
}

// Find a task by name (the console is the shell). The WM focuses it when no
// window is focused, so keyboard input has a deterministic owner. More than
// one task can share a name — the terminal hosts a second "shell.elf" — so
// return the LOWEST id: the one spawned first, which at boot is the console.
static int find_task_by_name(const char* name) {
    static struct grv_taskinfo tl[32];
    int n = grv_task_list(tl, 32);
    int best = -1;
    for (int i = 0; i < n; i++) {
        int j = 0;
        while (name[j] && tl[i].name[j] == name[j]) j++;
        if (name[j] == 0 && tl[i].name[j] == 0 &&
            (best < 0 || tl[i].id < best))
            best = tl[i].id;
    }
    return best;
}

// The console shell. The boot manifest spawns it by program name, and the
// kernel names a task after the program it was asked to run — so it is
// "shell.elf". Accept the bare "shell" too, both because an older kernel
// named it that way and because it costs one comparison.
static int find_console_task() {
    int a = find_task_by_name("shell.elf");
    int b = find_task_by_name("shell");
    if (a < 0) return b;
    if (b < 0) return a;
    return a < b ? a : b;
}

void focus_console() {
    focused = -1;
    grv_input_focus(console_task); // -1 if the shell is gone (legacy behavior)
}

void raise_overlay() { grv_surface_raise(overlay_id); }

// Repaint a screen rectangle of the overlay: clear it, then redraw every
// window (bottom-to-top) that overlaps it, clipped to the rectangle.
void redraw(int x0, int y0, int x1, int y1) {
    x0 = maxi(x0, 0); y0 = maxi(y0, 0);
    x1 = mini(x1, overlay_w); y1 = mini(y1, overlay_h);
    if (x1 <= x0 || y1 <= y0) return;

    clip_x0 = x0; clip_y0 = y0; clip_x1 = x1; clip_y1 = y1;
    fill(x0, y0, x1 - x0, y1 - y0, 0); // transparent

    for (int i = 0; i < nwin; i++) {
        int a, b, c, d;
        decor_rect(&wins[i], &a, &b, &c, &d);
        if (!(a < x1 && a + c > x0 && b < y1 && b + d > y0)) continue;
        // Occlude this window's decoration by the client areas of the windows
        // stacked above it (their surfaces show through there).
        noccl = 0;
        for (int j = i + 1; j < nwin && noccl < MAXWIN; j++) {
            occl[noccl].x = wins[j].x;
            occl[noccl].y = wins[j].y;
            occl[noccl].w = wins[j].w;
            occl[noccl].h = wins[j].h;
            noccl++;
        }
        draw_window(&wins[i]);
    }
    noccl = 0;

    draw_taskbar(); // the bar sits above every window
    draw_menu();    // the launcher popup sits above the bar

    grv_surface_commit_rect(x0, y0, x1 - x0, y1 - y0);
}

void full_redraw() { redraw(0, 0, overlay_w, overlay_h); }

// --- main -----------------------------------------------------------

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    if (grv_wm_claim() != 0) {
        printf("wm: display authority already held\n");
        return 1;
    }

    // Keep keyboard input owned deterministically: focus the console until
    // the user clicks a window.
    console_task = find_console_task();
    grv_input_focus(console_task);
    grv_wm_hotkey(GRV_MOD_ALT); // Alt chords (Alt+Tab) come to us

    load_apps(); // the launcher menu's contents (from /apps)

    font_ok = gravity_font_load(&font, "/fonts/bizcat.psf");

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
    clock_refresh();   // so the first paint includes the time
    full_redraw();
    printf("wm: clock %s\n", clock_str);

    printf("wm: display authority, screen %dx%d, overlay #%d, font %s\n",
           overlay_w, overlay_h, overlay_id, font_ok ? "yes" : "no");

    scan_existing();

    // Desktop wallpaper: an opaque full-screen surface at the bottom of the
    // stack. Try the shipped pictures; if none load, the console background
    // stays. Created after scan_existing so it is not taken for a window.
    static const char* walls[] = {
        "/Pictures/Rainier.bmp", "/Pictures/Earth.bmp",
        "/Pictures/Sun.bmp",     "/Pictures/lena.bmp",
    };
    for (int i = 0; i < (int)(sizeof(walls) / sizeof(walls[0])); i++) {
        if (load_wallpaper(walls[i], overlay_w, overlay_h) >= 0) {
            grv_surface_commit(); // paint it
            printf("wm: wallpaper %s\n", walls[i]);
            break;
        }
    }

    while (1) {
        struct grv_msg m;
        while (grv_recv(&m, 0) == 0) {
            if (m.type == GRV_EVT_SURFACE_ADDED) add_window((int)m.a);
            else if (m.type == GRV_EVT_SURFACE_REMOVED) remove_window((int)m.a);
            else if (m.type == GRV_EV_KEY) {
                // Global hotkeys we reserved: Alt+Tab cycles focus.
                if ((m.b & GRV_MOD_ALT) && m.a == '\t')
                    cycle_focus((m.b & GRV_MOD_SHIFT) ? -1 : 1);
            }
            else if (m.type == GRV_WM_SET_TITLE) {
                Win* w = find_owner((int)m.from);
                if (w) {
                    grv_wm_title_raw(m.a, m.b, w->title);
                    w->title[16] = '\0'; // ends here unless TITLE2 follows
                    printf("wm: title #%d '%s'\n", w->sid, w->title);
                    full_redraw();
                }
            }
            else if (m.type == GRV_WM_SET_TITLE2) {
                Win* w = find_owner((int)m.from);
                if (w) {
                    grv_wm_title_raw(m.a, m.b, w->title + 16);
                    w->title[32] = '\0';
                    printf("wm: title #%d '%s'\n", w->sid, w->title);
                    full_redraw();
                }
            }
            else if (m.type == GRV_WM_RESIZED) {
                // The client resized its surface to the size it took (it may
                // clamp); our w/h and the decoration follow.
                Win* w = find_sid((int)m.a);
                if (w) {
                    w->w = grv_wm_size_w(m.b);
                    w->h = grv_wm_size_h(m.b);
                    printf("wm: resized #%d %dx%d\n", w->sid, w->w, w->h);
                    full_redraw();
                }
            }
            else if (m.type == GRV_WM_CLOSE_ACK) {
                // The client is handling the close (e.g. prompting to save):
                // stand down the force-kill.
                Win* w = find_sid((int)m.a);
                if (w) w->close_deadline = 0;
            }
            else if (m.type == GRV_WM_SET_ROLE) {
                Win* w = find_sid((int)m.a);
                if (w) {
                    w->role = (int)m.b;
                    if (w->role == GRV_WM_ROLE_POPUP) {
                        grv_surface_raise(w->sid); // above normal windows
                        int idx = (int)(w - wins);
                        Win tmp = *w;
                        for (int k = idx; k < nwin - 1; k++) wins[k] = wins[k + 1];
                        wins[nwin - 1] = tmp;
                        printf("wm: popup #%d\n", tmp.sid);
                    }
                    full_redraw();
                }
            }
        }

        grv_sysinfo(&si);
        int cx = (int)si.cursor_x, cy = (int)si.cursor_y;

        // Collapse button edges across the drained packets, then act once.
        struct grv_mouse_packet p;
        bool pressed = false, released = false;
        int wheel = 0;
        while (grv_mouse_read(&p)) {
            int left = p.buttons & 1;
            if (left && !(btn_prev & 1)) pressed = true;
            if (!left && (btn_prev & 1)) released = true;
            btn_prev = p.buttons;
            wheel += p.dz;
        }

        if (pressed) on_press(cx, cy);
        if (released) on_release(cx, cy);

        if (dragging) {
            drag_to(cx, cy); // WM-driven title-bar drag
        } else if (resizing) {
            resize_to(cx, cy); // WM-driven edge drag
        } else if (!pressed && grab_owner >= 0 && (btn_prev & 1)) {
            // Pointer captured by a window: motion goes to it wherever the
            // cursor is, so the app can track a drag past its own edges.
            Win* w = find_owner(grab_owner);
            if (w) forward_mouse(w, btn_prev, cx, cy);
        } else if (!pressed && !released) {
            // Hover motion goes to the window under the pointer.
            Win* w = window_body_at(cx, cy);
            if (w && (cx != last_fx || cy != last_fy))
                forward_mouse(w, btn_prev, cx, cy);
        }

        // Highlight the launcher item under the pointer (redraw only on a
        // change, so moving inside one item costs nothing).
        if (menu_open) {
            int hot = menu_item_at(cx, cy);
            if (hot != menu_hot) { menu_hot = hot; redraw_menu(); }
        }

        // The wheel scrolls the window under the pointer (menu/taskbar clicks
        // above already returned earlier).
        if (wheel != 0) {
            Win* w = window_body_at(cx, cy);
            if (w) forward_scroll(w, wheel);
        }
        last_fx = cx; last_fy = cy;

        // Force-kill a client that ignored its close request.
        {
            unsigned long now = grv_ticks();
            for (int i = 0; i < nwin; i++) {
                if (wins[i].close_deadline && now >= wins[i].close_deadline) {
                    printf("wm: close timeout #%d\n", wins[i].sid);
                    wins[i].close_deadline = 0;
                    grv_kill(wins[i].owner, 0);
                }
            }
        }

        // Keep the taskbar clock current (repaint only when the minute flips).
        if (clock_refresh()) clock_redraw();

        grv_sleep(8);
    }
    return 0;
}
