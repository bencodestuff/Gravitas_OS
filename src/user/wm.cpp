#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <grv/syscall.h>
#include <gravity/font.h>
#include <grv/input.h>
#include <grv/wm.h>
#include <gravity/image.h>

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

static const int TITLE_H = 18;
static const int BORDER  = 2;
static const int CLOSE_W = 12;
static const int CLOSE_H = 12;
static const int MAXWIN  = 32;
static const int TASKBAR_H = 24;
static const int CLOCK_W   = 40;   // "HH:MM" at 8 px per glyph
static const unsigned long CLOSE_GRACE = 75; // ticks a client has to exit (1.5s)

static const unsigned int FOCUS_BAR   = 0xFF3B5A8C;
static const unsigned int UNFOCUS_BAR = 0xFF4A4A4A;
static const unsigned int EDGE        = 0xFF202020;
static const unsigned int CLOSE_BG    = 0xFFC04040;
static const unsigned int TITLE_FG    = 0xFFFFFFFF;
static const unsigned int BAR_BG      = 0xFF17191C;
static const unsigned int LAUNCH_BG   = 0xFF356B46;
static const unsigned int MENU_BG     = 0xFF24282E;
static const unsigned int MENU_EDGE   = 0xFF4A5568;
static const unsigned int MENU_SEP    = 0xFF343A44;
static const unsigned int MENU_HOT    = 0xFF35507A;

struct Win {
    int  sid;          // surface id
    int  owner;        // owning task id
    int  x, y;         // surface top-left on screen
    int  w, h;         // surface size
    int  role;         // GRV_WM_ROLE_NORMAL or _POPUP
    unsigned long close_deadline; // tick to force-kill a close request, 0 = none
    char title[33];
};

static int   overlay_id = -1;
static unsigned int* overlay_px = 0;
static int   overlay_w = 0, overlay_h = 0;

static struct gravity_font font;
static int   font_ok = 0;

static Win   wins[MAXWIN];
static int   nwin = 0;

static int   focused = -1;       // focused task id, or -1
static int   console_task = -1;  // the shell: focus returns here on desktop clicks
static int   click_sid = -1;     // window that received the button-down (for release)
static int   grab_owner = -1;    // task the pointer is captured to (drag inside a window)
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
    static struct grv_taskinfo tl[32];
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

static void fill(int x, int y, int w, int h, unsigned int c) {
    for (int yy = y; yy < y + h; yy++) {
        if (yy < clip_y0 || yy >= clip_y1) continue;
        for (int xx = x; xx < x + w; xx++) {
            if (xx < clip_x0 || xx >= clip_x1) continue;
            if (noccl && occluded(xx, yy)) continue;
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
                    if (noccl && occluded(xx, yy)) continue;
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

static Win* find_owner(int owner) {
    for (int i = 0; i < nwin; i++) if (wins[i].owner == owner) return &wins[i];
    return 0;
}

// Repaint a screen rectangle of the overlay: clear it, then redraw every
// window (bottom-to-top) that overlaps it, clipped to the rectangle.
static void draw_taskbar(); // defined below; always drawn on top
static void draw_menu();    // launcher popup, above the taskbar

static void redraw(int x0, int y0, int x1, int y1) {
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

static void full_redraw() { redraw(0, 0, overlay_w, overlay_h); }

// --- taskbar, app launcher, menu ------------------------------------
// Drawn into the overlay, above every window. The taskbar holds an "Apps"
// button (which opens a small program menu) and a button per open window.
// Layout is computed once into tbtns so drawing and hit-testing cannot
// drift apart.

// The launcher lists programs from /apps ("prog.elf  Label" per line), so a
// new app needs no WM change — just an entry and its ELF in the tar. Falls
// back to a couple of defaults if /apps is missing.
#define MAXAPPS 32
struct AppItem { char label[24]; char prog[32]; };
static AppItem apps[MAXAPPS];
static int napps = 0;

static void add_app(const char* prog, const char* label) {
    if (napps >= MAXAPPS) return;
    int i = 0;
    while (label[i] && i < 23) { apps[napps].label[i] = label[i]; i++; }
    apps[napps].label[i] = '\0';
    i = 0;
    while (prog[i] && i < 31) { apps[napps].prog[i] = prog[i]; i++; }
    apps[napps].prog[i] = '\0';
    napps++;
}

static void load_apps(void) {
    napps = 0;
    static char buf[1024];
    int fd = grv_open("/apps", GRV_O_RDONLY);
    long n = -1;
    if (fd >= 0) { n = grv_read_fd(fd, buf, sizeof(buf) - 1); grv_close(fd); }

    if (n > 0) {
        buf[n] = '\0';
        char* p = buf;
        while (*p && napps < MAXAPPS) {
            char* nl = p;
            while (*nl && *nl != '\n') nl++;
            char* q = p;
            while (*q == ' ' || *q == '\t') q++;
            if (*q && *q != '#') {
                char prog[32], label[24];
                int pi = 0;
                while (*q && *q != ' ' && *q != '\t' && pi < 31) prog[pi++] = *q++;
                prog[pi] = '\0';
                while (*q == ' ' || *q == '\t') q++;
                int li = 0;
                while (*q && *q != '\n' && li < 23) label[li++] = *q++;
                while (li > 0 && (label[li - 1] == ' ' || label[li - 1] == '\t')) li--;
                label[li] = '\0';
                if (pi && li) add_app(prog, label);
            }
            p = *nl ? nl + 1 : nl;
        }
    }

    if (napps == 0) { // no manifest: a minimal fallback
        add_app("term.elf", "Terminal");
        add_app("files.elf", "Files");
    }
}

static const int MENU_W      = 150;
static const int MENU_ITEM_H = 22;

static int menu_open = 0;
static int menu_hot = -1;   // hovered item, -1 = none

static int menu_x() { return 4; }
static int menu_y() { return (overlay_h - TASKBAR_H) - napps * MENU_ITEM_H; }

struct TBtn { int x, w; int kind; int idx; }; // kind 0 = apps, 1 = window
static TBtn tbtns[MAXWIN + 4];
static int  ntbtn = 0;

static int slen(const char* s) { int n = 0; while (s[n]) n++; return n; }

static void taskbar_layout() {
    ntbtn = 0;
    int x = 4;
    tbtns[ntbtn].x = x; tbtns[ntbtn].w = 8 * 4 + 16; // "Apps"
    tbtns[ntbtn].kind = 0; tbtns[ntbtn].idx = 0;
    ntbtn++;
    x += tbtns[0].w + 10;
    for (int i = 0; i < nwin && ntbtn < MAXWIN + 4; i++) {
        if (wins[i].role == GRV_WM_ROLE_POPUP) continue; // no button for popups
        int w = 8 * slen(wins[i].title) + 14;
        if (w < 84) w = 84;
        tbtns[ntbtn].x = x; tbtns[ntbtn].w = w;
        tbtns[ntbtn].kind = 1; tbtns[ntbtn].idx = i;
        ntbtn++;
        x += w + 6;
    }
}

// --- the taskbar clock -------------------------------------------------
// The main loop already wakes every 8 ms, so the clock is recomputed each pass
// and the bar repainted only when the displayed text changes.
static char clock_str[8] = "";

static int clock_refresh() {
    time_t now = time(0);
    struct tm* t = localtime(&now);
    char buf[8];
    if (strftime(buf, sizeof(buf), "%H:%M", t) == 0) return 0;
    if (strcmp(buf, clock_str) == 0) return 0;

    int i = 0;
    while (buf[i] && i < (int)sizeof(clock_str) - 1) { clock_str[i] = buf[i]; i++; }
    clock_str[i] = '\0';
    return 1;
}

static void clock_redraw() {
    int y = overlay_h - TASKBAR_H;
    redraw(overlay_w - CLOCK_W - 10, y, overlay_w, y + TASKBAR_H);
}

static void draw_taskbar() {
    int fh = font_ok ? (int)font.height : 8;
    int y = overlay_h - TASKBAR_H;
    fill(0, y, overlay_w, TASKBAR_H, BAR_BG);

    taskbar_layout();
    for (int i = 0; i < ntbtn; i++) {
        const TBtn* b = &tbtns[i];
        unsigned int bg;
        const char* label;
        if (b->kind == 0) {
            bg = LAUNCH_BG;
            label = "Apps";
        } else {
            bg = (wins[b->idx].owner == focused) ? FOCUS_BAR : UNFOCUS_BAR;
            label = wins[b->idx].title;
        }
        fill(b->x, y + 3, b->w, TASKBAR_H - 6, bg);
        text(b->x + 7, y + (TASKBAR_H - fh) / 2, label, TITLE_FG);
    }

    // The clock sits at the right end of the bar.
    if (clock_str[0])
        text(overlay_w - gravity_font_width(clock_str) - 8,
             y + (TASKBAR_H - fh) / 2, clock_str, TITLE_FG);
}

static void draw_menu() {
    if (!menu_open) return;
    int fh = font_ok ? (int)font.height : 8;
    int x = menu_x(), y = menu_y();
    fill(x, y, MENU_W, napps * MENU_ITEM_H, MENU_BG);
    fill(x, y, MENU_W, 1, MENU_EDGE);
    for (int i = 0; i < napps; i++) {
        int iy = y + i * MENU_ITEM_H;
        if (i == menu_hot) fill(x + 1, iy + 1, MENU_W - 2, MENU_ITEM_H - 1, MENU_HOT);
        if (i > 0) fill(x + 6, iy, MENU_W - 12, 1, MENU_SEP);
        text(x + 9, iy + (MENU_ITEM_H - fh) / 2, apps[i].label, TITLE_FG);
    }
}

// The menu item under a point, or -1.
static int menu_item_at(int cx, int cy) {
    if (!menu_open) return -1;
    int x = menu_x(), y = menu_y();
    if (cx < x || cx >= x + MENU_W || cy < y) return -1;
    int i = (cy - y) / MENU_ITEM_H;
    return (i >= 0 && i < napps) ? i : -1;
}

// Repaint just the menu's rectangle (open/close), restoring what it covered.
static void redraw_menu() {
    int x = menu_x(), y = menu_y();
    redraw(x, y, x + MENU_W, y + napps * MENU_ITEM_H);
}

static void launch(const char* prog) {
    const char* argv[1] = { prog };
    long pid = grv_spawn_bg(prog, argv, 1);
    printf("wm: launch '%s' -> %d\n", prog, (int)pid);
}

// --- wallpaper ------------------------------------------------------
// Decode a picture (gravity_image) and scale it to COVER the screen (crop the
// overflow) into an opaque full-screen surface at the bottom of the stack.

static int load_wallpaper(const char* path, int sw, int sh) {
    struct gravity_image img;
    if (gravity_image_load(&img, path) != 0) return -1;

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

// Alt+Tab / Alt+Shift+Tab: cycle keyboard focus through the windows.
static void cycle_focus(int dir) {
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

static void add_window(int sid) {
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
    static struct grv_surface_info list[48];
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
    // High 16 bits: target surface id; low bits: button mask.
    m.a = ((unsigned long)w->sid << 16) | (unsigned long)(buttons & 0xFFFF);
    m.b = grv_mouse_pack(cx - w->x, cy - w->y);
    grv_send(w->owner, &m);
}

// Send a wheel-scroll event to the task owning window 'w'. 'delta' is the
// number of wheel notches (positive = up/away).
static void forward_scroll(Win* w, int delta) {
    struct grv_msg m;
    m.from = 0;
    m.type = GRV_EV_SCROLL;
    m.a = ((unsigned long)w->sid << 16);
    m.b = (long)delta;
    grv_send(w->owner, &m);
}

// Top-most window whose client body contains the point, or 0.
static Win* window_body_at(int cx, int cy) {
    for (int i = nwin - 1; i >= 0; i--) {
        if (in_body(&wins[i], cx, cy)) return &wins[i];
    }
    return 0;
}

// Popups: a click anywhere outside every popup dismisses them all.
static int popup_at(int cx, int cy) {
    for (int i = 0; i < nwin; i++)
        if (wins[i].role == GRV_WM_ROLE_POPUP && in_body(&wins[i], cx, cy))
            return i;
    return -1;
}

static void dismiss_popups() {
    for (int i = 0; i < nwin; i++) {
        if (wins[i].role != GRV_WM_ROLE_POPUP) continue;
        struct grv_msg m;
        m.from = 0;
        m.type = GRV_WM_DISMISS;
        m.a = (unsigned long)wins[i].sid;
        m.b = 0;
        grv_send(wins[i].owner, &m);
    }
}

static void on_press(int cx, int cy) {
    // The launcher menu floats above everything and takes the click first.
    if (menu_open) {
        int mx = menu_x(), my = menu_y();
        if (cx >= mx && cx < mx + MENU_W &&
            cy >= my && cy < my + napps * MENU_ITEM_H) {
            int i = (cy - my) / MENU_ITEM_H;
            if (i >= 0 && i < napps) launch(apps[i].prog);
        }
        menu_open = 0;
        menu_hot = -1;
        redraw_menu();
        return;
    }

    // A click outside every popup dismisses the open popups (and is taken).
    {
        int has_popup = 0;
        for (int i = 0; i < nwin; i++)
            if (wins[i].role == GRV_WM_ROLE_POPUP) { has_popup = 1; break; }
        if (has_popup && popup_at(cx, cy) < 0) {
            dismiss_popups();
            return;
        }
    }

    // The taskbar is above every window: it takes the click next.
    if (cy >= overlay_h - TASKBAR_H) {
        taskbar_layout();
        for (int i = 0; i < ntbtn; i++) {
            const TBtn* b = &tbtns[i];
            if (cx >= b->x && cx < b->x + b->w) {
                if (b->kind == 0) { menu_open = 1; menu_hot = -1; redraw_menu(); }
                else focus_window(&wins[b->idx]);
                return;
            }
        }
        return; // the bar's background swallows the click
    }

    for (int i = nwin - 1; i >= 0; i--) {
        Win* w = &wins[i];
        if (in_close(w, cx, cy)) {
            // Ask the client to exit rather than killing it: it may need to
            // save. A client that ignores the ask is force-killed once the
            // grace period runs out.
            printf("wm: close #%d\n", w->sid);
            w->close_deadline = grv_ticks() + CLOSE_GRACE;
            struct grv_msg cm;
            cm.from = 0;
            cm.type = GRV_WM_CLOSE;
            cm.a = (unsigned long)w->sid;
            cm.b = 0;
            grv_send(w->owner, &cm);
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
            grab_owner = w->owner; // capture the pointer for the drag
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
    grab_owner = -1; // pointer capture ends with the button
    dragging = 0;
    drag_sid = -1;
}

// --- window placement -------------------------------------------------
// The WM owns where a window sits (clients never move themselves), so both a
// title-bar drag and a workspace layout go through place_window.

static void place_window(Win* w, int x, int y) {
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

static void drag_to(int cx, int cy) {
    Win* w = find_sid(drag_sid);
    if (w == 0) { on_release(cx, cy); return; }
    place_window(w, cx - drag_off_x, cy - drag_off_y);
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
