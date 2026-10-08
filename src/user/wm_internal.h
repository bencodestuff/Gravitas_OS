#pragma once
// Internal interface shared by the window manager's translation units.
//
// The WM is one program (wm.elf) built from several units: wm.cpp is the
// core — the overlay, the window table, the clipped drawing primitives and
// the main loop — and wm_windows.cpp, wm_taskbar.cpp, wm_menu.cpp and
// wm_input.cpp hold the window geometry/lifecycle, the taskbar, the launcher
// menu and the input routing. This header is the seam between them; it is
// NOT a public interface (clients talk to the WM over the kernel's window
// protocol in grv/wm.h, they never link against it).
//
// Only the state and entry points that genuinely cross a unit boundary are
// declared here; anything used by a single unit stays static in that unit.

#include <gravity/font.h>

// --- geometry constants -------------------------------------------------

constexpr int TITLE_H = 18;
constexpr int BORDER  = 2;
constexpr int CLOSE_W = 12;
constexpr int CLOSE_H = 12;
constexpr int MAXBOX_W = 12;          // the maximize box, left of the close box
constexpr int RESIZE_BAND = 5;        // the grab margin just outside the body
constexpr int MIN_W = 120, MIN_H = 80;
constexpr int MAXWIN  = 32;
constexpr int TASKBAR_H = 24;
constexpr int CLOCK_W   = 40;   // "HH:MM" at 8 px per glyph
constexpr unsigned long CLOSE_GRACE = 75; // ticks a client has to exit (1.5s)
constexpr int MAXAPPS = 32;
constexpr int MENU_W      = 150;
constexpr int MENU_ITEM_H = 22;

// --- palette ------------------------------------------------------------

constexpr unsigned int FOCUS_BAR   = 0xFF3B5A8C;
constexpr unsigned int UNFOCUS_BAR = 0xFF4A4A4A;
constexpr unsigned int EDGE        = 0xFF202020;
constexpr unsigned int CLOSE_BG    = 0xFFC04040;
constexpr unsigned int MAXBOX_BG   = 0xFF4A5568;
constexpr unsigned int MAXBOX_ON   = 0xFF6A8AB0;
constexpr unsigned int TITLE_FG    = 0xFFFFFFFF;
constexpr unsigned int BAR_BG      = 0xFF17191C;
constexpr unsigned int LAUNCH_BG   = 0xFF356B46;
constexpr unsigned int MENU_BG     = 0xFF24282E;
constexpr unsigned int MENU_EDGE   = 0xFF4A5568;
constexpr unsigned int MENU_SEP    = 0xFF343A44;
constexpr unsigned int MENU_HOT    = 0xFF35507A;

// --- records ------------------------------------------------------------

struct Win {
    int  sid;          // surface id
    int  owner;        // owning task id
    int  x, y;         // surface top-left on screen
    int  w, h;         // surface size
    int  role;         // GRV_WM_ROLE_NORMAL or _POPUP
    unsigned long close_deadline; // tick to force-kill a close request, 0 = none
    int  maximized;    // and where it was before, to restore
    int  rest_x, rest_y, rest_w, rest_h;
    char title[33];
};

struct AppItem { char label[24]; char prog[32]; };

struct TBtn { int x, w; int kind; int idx; }; // kind 0 = apps, 1 = window

// --- core state (defined in wm.cpp) -------------------------------------

extern int   overlay_id;
extern unsigned int* overlay_px;
extern int   overlay_w, overlay_h;

extern struct gravity_font font;
extern int   font_ok;

extern Win   wins[MAXWIN];
extern int   nwin;

extern int   focused;       // focused task id, or -1
extern int   console_task;  // the shell: focus returns here on desktop clicks

// Clipped drawing into the overlay. clip_* bound fill()/text(); a unit that
// paints (e.g. draw_window) narrows them around its own content.
extern int clip_x0, clip_y0, clip_x1, clip_y1;

// --- taskbar state (defined in wm_taskbar.cpp) --------------------------

extern TBtn tbtns[MAXWIN + 4];
extern int  ntbtn;
extern char clock_str[8];

// --- launcher state (defined in wm_menu.cpp) ----------------------------

extern AppItem apps[MAXAPPS];
extern int napps;
extern int menu_open;
extern int menu_hot;   // hovered item, -1 = none

// --- input/drag/resize state (defined in wm_input.cpp) ------------------

extern int   click_sid;     // window that received the button-down (for release)
extern int   grab_owner;    // task the pointer is captured to (drag inside a window)
extern int   dragging;
extern int   drag_sid;
extern int   drag_off_x, drag_off_y;

extern int   resizing;
extern int   resize_sid;
extern int   resize_edge;                 // 1 = right, 2 = bottom, 3 = corner
extern int   resize_w0, resize_h0;        // size when the drag began
extern int   resize_px, resize_py;        // pointer when it began
extern int   resize_sent_w, resize_sent_h; // the last size asked for

// --- helpers ------------------------------------------------------------

static inline int mini(int a, int b) { return a < b ? a : b; }
static inline int maxi(int a, int b) { return a > b ? a : b; }

static inline int in_rect(int px, int py, int x, int y, int w, int h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

// --- core (wm.cpp) ------------------------------------------------------

void focus_console();
void raise_overlay();
void fill(int x, int y, int w, int h, unsigned int c);
void text(int x, int y, const char* s, unsigned int fg);
void redraw(int x0, int y0, int x1, int y1);
void full_redraw();

// --- windows (wm_windows.cpp) -------------------------------------------

void decor_rect(const Win* w, int* rx, int* ry, int* rw, int* rh);
void close_rect(const Win* w, int* rx, int* ry, int* rw, int* rh);
void max_rect(const Win* w, int* rx, int* ry, int* rw, int* rh);
int  in_title(const Win* w, int px, int py);
int  in_body(const Win* w, int px, int py);
int  in_close(const Win* w, int px, int py);
int  in_maxbox(const Win* w, int px, int py);
int  in_resize_band(const Win* w, int px, int py);
void draw_window(const Win* w);
Win* find_sid(int sid);
Win* find_owner(int owner);
void focus_window(Win* w);
void cycle_focus(int dir);
void add_window(int sid);
void remove_window(int sid);
void scan_existing();
void place_window(Win* w, int x, int y);
void request_resize(Win* w, int nw, int nh);
void resize_wanted(int cx, int cy, int* nw, int* nh);
void maximize_toggle(Win* w);
int  load_wallpaper(const char* path, int sw, int sh);

// --- taskbar (wm_taskbar.cpp) -------------------------------------------

void taskbar_layout();
void draw_taskbar();
int  clock_refresh();
void clock_redraw();

// --- launcher menu (wm_menu.cpp) ----------------------------------------

int  menu_x();
int  menu_y();
void load_apps();
void draw_menu();
int  menu_item_at(int cx, int cy);
void redraw_menu();
void launch(const char* prog);

// --- input (wm_input.cpp) -----------------------------------------------

void forward_mouse(Win* w, int buttons, int cx, int cy);
void forward_scroll(Win* w, int delta);
Win* window_body_at(int cx, int cy);
void on_press(int cx, int cy);
void on_release(int cx, int cy);
void drag_to(int cx, int cy);
void resize_to(int cx, int cy);
