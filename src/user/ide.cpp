#include <stdio.h>
#include <string.h>
#include <gravity/gravity.h>
#include <grv/syscall.h>
#include <grv/input.h>
#include <grv/clipboard.h>
#include <gravity/image.h>

// ide — the Gravitas IDE: ONE window, with the editor, the file tree and the
// terminal as panes inside it.
//
// It is a composition, not a monolith: every pane is a widget the standalone
// apps use too (gravity_editor, gravity_filer + gravity_list, gravity_term), and
// this program owns only the window, the layout, the tab bar, which pane has
// focus — and the two things that make it an IDE rather than three windows:
// a file opened from the tree lands in a tab of the centre pane, and F5 runs
// the active file in the terminal below.
//
//   +-----------+---------------------------+
//   | explorer  | [tab] [tab*]              |
//   |           |          editor           |
//   |           |---------------------------|
//   |           |          terminal         |
//   +-----------+---------------------------+
//   | status: path            line:col      |
//   +---------------------------------------+
//
// Keys: Ctrl+1/2/3 focus a pane, F5 runs the active file, Ctrl+Tab cycles tabs,
// Ctrl+W closes one, Ctrl+S saves, Ctrl+R re-reads the tree, Ctrl+Q quits.

#define DEF_EX_W   260   /* the explorer pane's width */
#define DEF_TERM_H 200   /* the terminal pane's height */
#define SEP        2     /* the gap between panes */
#define RING       2     /* the focused pane's accent strip */
#define MAX_TABS   8
#define CELL_W     8     /* the terminal grid's cell, matching the console font */
#define CELL_H     16

#define CTRL(c) ((c) - 'a' + 1)

enum { P_EXPLORER = 0, P_EDITOR, P_TERMINAL };

// Catppuccin Mocha (the user's theme across the desktop).
static const unsigned int BG      = 0xFF1E1E2E; // base
static const unsigned int CHROME  = 0xFF11111B; // crust (tab bar, status)
static const unsigned int SEP_C   = 0xFF313244; // surface0 (separators)
static const unsigned int FG      = 0xFFCDD6F4; // text
static const unsigned int DIM     = 0xFF7F849C; // overlay1
static const unsigned int ACCENT  = 0xFF89B4FA; // blue (the focused pane)
static const unsigned int TAB_ON  = 0xFF313244; // surface0: the active tab
static const unsigned int TAB_OFF = 0xFF181825; // mantle: the others
static const unsigned int TAB_FG  = 0xFFCDD6F4;
static const unsigned int TAB_DIM = 0xFF7F849C;

static struct gravity_app app;
static struct gravity_editor tabs[MAX_TABS];
static int  ntab = 0, active = 0;
static struct gravity_filer filer;
static struct gravity_term term;
static int  shell_pid = -1;

static int  win_w = 1272, win_h = 754;
static int  focus = P_EDITOR;
static int  quit_asked = 0;
static char close_asked[MAX_TABS];
static char note[96];           /* the IDE's own message line */

/* The layout, computed once from the window size. */
static int g_ch = 16, status_h = 22, tab_h = 22;
static int ex_x, ex_y, ex_w, ex_h;
static int rx, rw;              /* the right-hand column */
static int ed_x, ed_y, ed_w, ed_h;
static int tm_x, tm_y, tm_w, tm_h;
static int grid_cols, grid_rows, grid_w, grid_h;

/* Which panes need repainting (the widgets own their own regions, so a terminal
   line of output must not drag the editor along with it). */
static int dirty_chrome = 1, dirty_ex = 1, dirty_ed = 1, dirty_term = 1;

static void refresh(void) { gravity_refresh(&app); }

static void set_note(const char* s) {
    int i = 0;
    while (s[i] && i < (int)sizeof(note) - 1) { note[i] = s[i]; i++; }
    note[i] = '\0';
}

static int streq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static int has_suffix(const char* s, const char* suf) {
    int n = 0, m = 0;
    while (s[n]) n++;
    while (suf[m]) m++;
    if (m > n) return 0;
    for (int i = 0; i < m; i++) if (s[n - m + i] != suf[i]) return 0;
    return 1;
}

/* --- layout --- */

/* Each pane keeps RING pixels clear at its top for the focus strip: it is drawn
   with the chrome, so a widget repainting its region cannot wipe it, and it is
   a thin fill, so showing focus costs almost no damage. */
static void layout(void) {
    status_h = g_ch + 6;
    tab_h = g_ch + 6;

    int body_h = win_h - status_h;
    ex_x = 0; ex_y = RING; ex_w = DEF_EX_W; ex_h = body_h - RING;

    rx = ex_w + SEP;
    rw = win_w - rx;
    if (rw < 80) rw = 80;

    ed_x = rx; ed_y = tab_h + RING;
    ed_w = rw;
    ed_h = body_h - ed_y - DEF_TERM_H - SEP - RING;
    if (ed_h < g_ch) ed_h = g_ch;

    tm_x = rx;
    tm_y = ed_y + ed_h + SEP + RING;
    tm_w = rw;
    tm_h = body_h - tm_y;
    if (tm_h < CELL_H) tm_h = CELL_H;

    grid_cols = (tm_w - GRAVITY_TERM_SB) / CELL_W;
    grid_rows = tm_h / CELL_H;
    if (grid_cols < 20) grid_cols = 20;
    if (grid_rows < 4) grid_rows = 4;
    grid_w = grid_cols * CELL_W;
    grid_h = grid_rows * CELL_H;
}

/* --- tabs --- */

static void tab_label(int i, char* out, int max) {
    int n = 0;
    const char* name = gravity_editor_basename(&tabs[i]);
    for (int k = 0; name[k] && n < max - 3; k++) out[n++] = name[k];
    if (gravity_editor_dirty(&tabs[i])) { out[n++] = ' '; out[n++] = '*'; }
    out[n] = '\0';
}

/* Open 'path' in a tab (or activate the tab that already has it). */
static int open_in_tab(const char* path) {
    for (int i = 0; i < ntab; i++) {
        if (streq(gravity_editor_path(&tabs[i]), path)) {
            active = i;
            close_asked[i] = 0;
            return 0;
        }
    }
    if (ntab >= MAX_TABS) { set_note("too many open files"); return -1; }
    if (gravity_editor_init(&tabs[ntab], path, ed_x, ed_y, ed_w, ed_h) != 0) {
        set_note("out of memory");
        return -1;
    }
    close_asked[ntab] = 0;
    active = ntab++;
    printf("ide: open %s (%d bytes)\n", path, gravity_editor_len(&tabs[active]));
    return 0;
}

/* Close tab 'i', asking once about unsaved changes. */
static void close_tab(int i) {
    if (i < 0 || i >= ntab) return;
    if (gravity_editor_dirty(&tabs[i]) && !close_asked[i]) {
        close_asked[i] = 1;
        gravity_editor_set_msg(&tabs[i],
            "unsaved changes: Ctrl+W again to discard, Ctrl+S to save");
        return;
    }
    printf("ide: close %s\n", gravity_editor_path(&tabs[i]));
    gravity_editor_free(&tabs[i]);
    for (int k = i; k < ntab - 1; k++) tabs[k] = tabs[k + 1];
    ntab--;
    if (active >= ntab) active = ntab > 0 ? ntab - 1 : 0;
    if (ntab == 0) open_in_tab("/tmp/untitled.txt"); // never leave it empty
}

static void update_title(void) {
    static char title[48], sent[48];
    int n = 0;
    const char* pre = "Gravitas IDE — ";
    for (int i = 0; pre[i] && n < 30; i++) title[n++] = pre[i];
    if (ntab > 0) {
        const char* name = gravity_editor_basename(&tabs[active]);
        for (int i = 0; name[i] && n < 30; i++) title[n++] = name[i];
        if (gravity_editor_dirty(&tabs[active])) { title[n++] = ' '; title[n++] = '*'; }
    }
    title[n] = '\0';

    int same = 1;
    for (int i = 0; i <= n; i++) if (title[i] != sent[i]) { same = 0; break; }
    if (same) return;
    for (int i = 0; i <= n; i++) sent[i] = title[i];
    gravity_set_title(&app, title);
}

/* --- actions --- */

/* F5: run the active file in the terminal pane. It saves first, so what runs is
   what is on screen. */
static void run_active(void) {
    if (ntab == 0) return;
    const char* path = gravity_editor_path(&tabs[active]);
    if (gravity_editor_dirty(&tabs[active])) {
        int n = gravity_editor_save(&tabs[active]);
        if (n < 0) { set_note("run: save failed"); return; }
        printf("ide: saved %s (%d bytes)\n", path, n);
    }

    char cmd[256];
    if (has_suffix(path, ".lua")) snprintf(cmd, sizeof(cmd), "run lua.elf %s", path);
    else snprintf(cmd, sizeof(cmd), "%s", path);
    printf("ide: run %s\n", cmd);

    int n = 0;
    while (cmd[n] && n < (int)sizeof(cmd) - 2) n++;
    cmd[n++] = '\n';
    cmd[n] = '\0';

    if (shell_pid >= 0) {
        gravity_term_snap_bottom(&term);
        grv_console_write(shell_pid, cmd, (unsigned long)n);
        dirty_term = 1;
    }
    set_note("");
    refresh();
}

/* Enter in the tree: descend into a directory, open a file in a new tab. */
static void explorer_open(void) {
    const char* name = gravity_filer_selected(&filer);
    if (name == 0) return;
    if (gravity_filer_enter(&filer)) { dirty_ex = 1; return; }

    char path[160];
    gravity_filer_path(&filer, path, sizeof(path), name);
    if (gravity_image_supported(path)) {
        const char* argv[2] = { "imageview.elf", path };
        grv_spawn_bg("imageview.elf", argv, 2);
        printf("ide: view %s\n", path);
        return;
    }
    if (open_in_tab(path) == 0) {
        focus = P_EDITOR;
        dirty_chrome = 1;
        dirty_ed = 1;
        update_title();
    }
}

static void try_quit(void) {
    for (int i = 0; i < ntab; i++) {
        if (gravity_editor_dirty(&tabs[i])) {
            if (!quit_asked) {
                quit_asked = 1;
                set_note("unsaved changes: Ctrl+Q again to discard, Ctrl+S to save");
                refresh();
                return;
            }
            break;
        }
    }
    if (shell_pid > 0) grv_kill(shell_pid, 0);
    grv_exit(0);
}

static int on_close(struct gravity_app* a) {
    (void)a;
    for (int i = 0; i < ntab; i++) {
        if (gravity_editor_dirty(&tabs[i])) {
            if (quit_asked) break;
            quit_asked = 1;
            set_note("unsaved changes: close again to discard, or Ctrl+S to save");
            refresh();
            return 0; // veto for now
        }
    }
    if (shell_pid > 0) grv_kill(shell_pid, 0);
    return 1;
}

/* --- drawing --- */

static void draw_chrome(struct gravity_canvas* c) {
    gravity_clear(c, BG);

    // The tab bar: one cell per open file, the active one lit.
    gravity_fill(c, rx, 0, rw, tab_h, CHROME);
    int x = rx + 2;
    for (int i = 0; i < ntab; i++) {
        char label[40];
        tab_label(i, label, sizeof(label));
        int w = gravity_text_width_of(label) + 12;
        if (x + w > rx + rw) break;
        int on = (i == active);
        gravity_fill(c, x, 2, w, tab_h - 2, on ? TAB_ON : TAB_OFF);
        gravity_text(c, x + 6, (tab_h - g_ch) / 2, label, on ? TAB_FG : TAB_DIM);
        x += w + 2;
    }

    // Separators between the panes.
    gravity_fill(c, ex_w, 0, SEP, win_h - status_h, SEP_C);
    gravity_fill(c, rx, tm_y - SEP, rw, SEP, SEP_C);

    // The focused pane's accent strip, at its top edge (clear of the widgets).
    int sx, sy, sw;
    if (focus == P_EXPLORER)     { sx = ex_x; sy = 0;          sw = ex_w; }
    else if (focus == P_EDITOR)  { sx = rx;   sy = tab_h;      sw = rw;   }
    else                         { sx = tm_x; sy = tm_y - RING; sw = tm_w; }
    gravity_fill(c, sx, sy, sw, RING, ACCENT);
}

static void draw_status(struct gravity_canvas* c) {
    int y = win_h - status_h;
    gravity_fill(c, 0, y, win_w, status_h, CHROME);
    int ty = y + (status_h - g_ch) / 2;

    const char* path = ntab > 0 ? gravity_editor_path(&tabs[active]) : "(no file)";
    gravity_text(c, 6, ty, path, FG);

    const char* msg = note[0] ? note
                    : (ntab > 0 ? gravity_editor_msg(&tabs[active]) : "");
    if (msg[0]) {
        int sw = gravity_text_width_of(msg);
        int sx = (win_w - sw) / 2;
        if (sx > 200) gravity_text(c, sx, ty, msg, DIM);
    }

    if (ntab > 0) {
        char buf[32];
        gravity_editor_status(&tabs[active], buf, sizeof(buf));
        gravity_text(c, win_w - 6 - gravity_text_width_of(buf), ty, buf, FG);
    }
}

static void on_draw(struct gravity_app* a) {
    struct gravity_canvas* c = &a->canvas;

    if (dirty_chrome) {
        draw_chrome(c);
        dirty_chrome = 0;
        // Chrome cleared the window: every widget has to paint again.
        dirty_ex = dirty_ed = 1;
        gravity_term_redraw(&term);
        dirty_term = 1;
    }
    if (dirty_ex) { gravity_list_draw(&filer.list, c); dirty_ex = 0; }
    if (dirty_ed && ntab > 0) { gravity_editor_draw(&tabs[active], c); dirty_ed = 0; }
    if (dirty_term) {
        gravity_term_render(&term, c, tm_x, tm_y);
        gravity_term_scrollbar_draw(&term, c, tm_x + grid_w, tm_y,
                                    GRAVITY_TERM_SB, grid_h);
        dirty_term = 0;
    }
    draw_status(c);
}

/* --- events --- */

static void on_poll(struct gravity_app* a) {
    if (gravity_term_pump(&term) > 0) {
        dirty_term = 1;
        gravity_refresh(a);
    }
}

static int in_rect(int px, int py, int x, int y, int w, int h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

/* The pane under a point, or -1. */
static int pane_at(int px, int py) {
    if (py >= win_h - status_h) return -1;
    if (in_rect(px, py, ex_x, 0, ex_w, win_h - status_h)) return P_EXPLORER;
    if (in_rect(px, py, rx, 0, rw, tm_y)) return P_EDITOR; // tab bar + editor
    if (in_rect(px, py, tm_x, tm_y - RING, tm_w, win_h - status_h - tm_y + RING))
        return P_TERMINAL;
    return -1;
}

static void on_key(struct gravity_app* a, const struct gravity_event* ev) {
    (void)a;
    long v = ev->a;
    int mods = (int)ev->b;
    int ctrl = (mods & GRV_MOD_CTRL) != 0;

    // IDE-level chords come first: they mean the same thing in every pane.
    if (ctrl && v == '1') { focus = P_EXPLORER; dirty_chrome = 1; refresh(); return; }
    if (ctrl && v == '2') { focus = P_EDITOR;   dirty_chrome = 1; refresh(); return; }
    if (ctrl && v == '3') { focus = P_TERMINAL; dirty_chrome = 1; refresh(); return; }
    if (v == GRV_KEY_F5) { run_active(); return; }
    if (v == CTRL('r')) { gravity_filer_load(&filer, "."); dirty_ex = 1; refresh(); return; }
    if (v == CTRL('s')) {
        if (ntab > 0) {
            int n = gravity_editor_save(&tabs[active]);
            if (n >= 0) printf("ide: saved %s (%d bytes)\n",
                               gravity_editor_path(&tabs[active]), n);
            update_title();
            refresh();
        }
        return;
    }
    if (v == CTRL('q')) { try_quit(); return; }
    if (v == CTRL('w')) { close_tab(active); dirty_chrome = dirty_ed = 1;
                          update_title(); refresh(); return; }
    if (v == '\t' && ctrl) {
        if (ntab > 1) {
            active = (active + 1) % ntab;
            dirty_chrome = dirty_ed = 1;
            update_title();
            refresh();
        }
        return;
    }

    if (focus == P_EXPLORER) {
        if (v == GRV_KEY_UP)        gravity_list_move(&filer.list, -1);
        else if (v == GRV_KEY_DOWN) gravity_list_move(&filer.list, 1);
        else if (v == GRV_KEY_PAGEUP)
            gravity_list_scroll(&filer.list, -gravity_list_rows(&filer.list));
        else if (v == GRV_KEY_PAGEDOWN)
            gravity_list_scroll(&filer.list, gravity_list_rows(&filer.list));
        else if (v == '\n') { explorer_open(); refresh(); return; }
        else if (v == '\b') { gravity_filer_up(&filer); }
        else return;
        dirty_ex = 1;
        refresh();
        return;
    }

    if (focus == P_EDITOR && ntab > 0) {
        int rev = gravity_editor_rev(&tabs[active]);
        if (gravity_editor_key(&tabs[active], v, mods)) {
            if (gravity_editor_rev(&tabs[active]) != rev) {
                close_asked[active] = 0;
                quit_asked = 0;
            }
            dirty_ed = 1;
            update_title();
            refresh();
        }
        return;
    }

    if (focus == P_TERMINAL) {
        if (shell_pid < 0) return;
        // Ctrl+Shift+V pastes the file clipboard into the shell's stdin.
        if ((mods & GRV_MOD_CTRL) && (mods & GRV_MOD_SHIFT) &&
            (v == 'V' || v == 'v')) {
            static char buf[GRV_CLIPBOARD_MAX];
            int n = grv_clip_paste(buf, (int)sizeof(buf));
            if (n > 0) {
                gravity_term_snap_bottom(&term);
                grv_console_write(shell_pid, buf, (unsigned long)n);
                dirty_term = 1;
                refresh();
            }
            return;
        }
        if (v == GRV_KEY_PAGEUP)   { gravity_term_scrollback(&term, grid_rows - 1); dirty_term = 1; refresh(); return; }
        if (v == GRV_KEY_PAGEDOWN) { gravity_term_scrollback(&term, -(grid_rows - 1)); dirty_term = 1; refresh(); return; }

        char ch;
        if (v >= 32 && v < 127) ch = (char)v;
        else if (v == '\n' || v == '\b' || v == '\t') ch = (char)v;
        else if (v == 3) ch = 3;
        else return;

        gravity_term_snap_bottom(&term);
        grv_console_write(shell_pid, &ch, 1);
        dirty_term = 1;
        refresh();
        return;
    }
}

static int  mouse_pane = -1;   /* the pane the press landed in */
static int  sb_drag = 0;
static int  last_mx = -1, last_my = -1; /* the last pointer position we saw */

static void on_mouse(struct gravity_app* a, const struct gravity_event* ev) {
    (void)a;
    int mx = grv_mouse_x(ev->b);
    int my = grv_mouse_y(ev->b);
    int held = (ev->a & GRV_BTN_LEFT) != 0;
    last_mx = mx;
    last_my = my;

    if (!held) {
        // Release: let the pane finish its drag (the editor's selection).
        if (mouse_pane == P_EDITOR && ntab > 0)
            gravity_editor_mouse(&tabs[active], mx, my, 0);
        else if (mouse_pane == P_TERMINAL)
            sb_drag = 0;
        mouse_pane = -1;
        dirty_ed = 1;
        refresh();
        return;
    }

    if (mouse_pane < 0) {
        // The press: focus the pane under the pointer, then hand it the click.
        int p = pane_at(mx, my);
        if (p < 0) return;
        mouse_pane = p;
        if (p != focus) {
            focus = p;
            dirty_chrome = 1;
        }
        if (p == P_EXPLORER) {
            int idx = gravity_list_hit(&filer.list, mx, my);
            if (idx >= 0) {
                int was = (idx == filer.list.sel);
                gravity_list_select(&filer.list, idx);
                // A directory (or "..") opens on the first click; a file on the
                // second (click to select, click again to open).
                if (was || (filer.has_dotdot && idx == 0) ||
                    gravity_filer_is_dir(filer.items[idx]))
                    explorer_open();
                dirty_ex = 1;
            }
        } else if (p == P_EDITOR && ntab > 0) {
            if (my >= ed_y) gravity_editor_mouse(&tabs[active], mx, my, 1);
            else {
                // A click on the tab bar selects a tab.
                int x = rx + 2;
                for (int i = 0; i < ntab; i++) {
                    char label[40];
                    tab_label(i, label, sizeof(label));
                    int w = gravity_text_width_of(label) + 12;
                    if (mx >= x && mx < x + w) {
                        active = i;
                        dirty_chrome = dirty_ed = 1;
                        update_title();
                        break;
                    }
                    x += w + 2;
                }
            }
        }
        refresh();
        return;
    }

    // A drag: keep feeding the pane that took the press, wherever it wanders.
    if (mouse_pane == P_EDITOR && ntab > 0) {
        gravity_editor_mouse(&tabs[active], mx, my, 1);
        dirty_ed = 1;
    } else if (mouse_pane == P_TERMINAL) {
        if (sb_drag || mx >= tm_x + grid_w) {
            sb_drag = 1;
            gravity_term_scrollbar_drag(&term, my - tm_y, grid_h);
            dirty_term = 1;
        }
    }
    refresh();
}

static void on_event(struct gravity_app* a, const struct gravity_event* ev) {
    if (ev->type == GRV_EV_KEY) { on_key(a, ev); return; }
    if (ev->type == GRV_EV_MOUSE) { on_mouse(a, ev); return; }
    if (ev->type == GRV_EV_SCROLL) {
        // The wheel goes to the pane under the pointer. A scroll event carries
        // no position, so use the last motion the WM forwarded — it forwards
        // hover motion to the window under the pointer.
        int pane = pane_at(last_mx, last_my);
        int d = grv_scroll_delta(ev->b);
        if (pane == P_EXPLORER) { gravity_list_scroll(&filer.list, -d * 3); dirty_ex = 1; }
        else if (pane == P_EDITOR && ntab > 0) { gravity_editor_scroll(&tabs[active], d); dirty_ed = 1; }
        else if (pane == P_TERMINAL) { gravity_term_scrollback(&term, d * 3); dirty_term = 1; }
        refresh();
        return;
    }
}

/* --- main --- */

int main(int argc, char** argv) {
    gravity_init();
    g_ch = gravity_font_height();
    if (g_ch < 1) g_ch = 16;

    // Fill the frame unless told otherwise: the WM's title bar sits above the
    // window and its taskbar below.
    struct grv_sysinfo si;
    grv_sysinfo(&si);
    int sw = (int)si.screen_w, sh = (int)si.screen_h;
    if (sw > 40) win_w = sw - 8;
    if (sh > 80) win_h = sh - 24 - 18 - 4;

    int argi = 1;
    gravity_args(argc, argv, &win_w, &win_h, &argi);
    layout();

    gravity_filer_init(&filer, "/", ex_x, ex_y, ex_w, ex_h, g_ch + 4);

    if (gravity_term_init(&term, grid_cols, grid_rows, CELL_W, CELL_H,
                          0xFFD0E0F0, 0xFF101418) != 0) {
        printf("ide: out of memory\n");
        return 1;
    }
    shell_pid = gravity_term_host(&term, "shell.elf");
    printf("ide: %dx%d, panes explorer %d editor %d terminal %d, shell %d\n",
           win_w, win_h, ex_w, ed_w, tm_h, shell_pid);

    open_in_tab(argi < argc && argv[argi][0] ? argv[argi] : "/tmp/untitled.txt");
    for (int i = argi + 1; i < argc; i++)
        if (argv[i] && argv[i][0]) open_in_tab(argv[i]);
    active = 0;
    update_title();

    app.on_event = on_event;
    app.on_draw = on_draw;
    app.on_poll = on_poll;
    app.on_close = on_close;
    app.title = "Gravitas IDE";
    app.user = 0;
    gravity_run(&app, win_w, win_h);
    return 0;
}
