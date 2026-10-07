#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <gravity/gravity.h>
#include <grv/syscall.h>
#include <grv/input.h>
#include <grv/clipboard.h>

// edit — a text editor built on Gravity.
//
// A viewport over a gravity_text buffer. Arrow keys, Home/End, PageUp/
// PageDown and the wheel move the caret; printable keys, Enter, Tab (spaces
// to the next stop), Backspace and Delete edit; clicking places the caret.
// Shift with a movement key, or a mouse drag, makes a selection. Ctrl+S saves,
// Ctrl+Q quits — and the WM's close box goes through the same unsaved-changes
// check (the app vetoes once, then discards, which is what Gravity's
// cooperative-close protocol is for).
//
// Ctrl+Shift+C/X/V copy, cut and paste through the file clipboard
// (<grv/clipboard.h>); the Shift is what distinguishes them from Ctrl+C.
//
// The title tracks the file name and a trailing '*' while modified.

#define DEF_W    760
#define DEF_H    520
#define CW       8     /* glyph cell width — the PSF2 console font is 8px */
#define SB_W     6     /* scrollbar strip on the right */
#define MARGIN   6

#define CTRL(c)  ((c) - 'a' + 1)

static struct gravity_app app;
static struct gravity_buffer text;

static int  win_w = DEF_W;  /* the window size (--geometry may change it) */
static int  win_h = DEF_H;

static char path[160] = "/tmp/untitled.txt";
static char status[96];
static int  cursor = 0;
static int  sel = -1;       /* selection anchor, -1 = no selection */
static int  top = 0;        /* byte offset of the first visible line */
static int  left = 0;       /* first visible display column */
static int  goal_col = -1;  /* target column while moving vertically */
static int  modified = 0;
static int  quit_asked = 0;
static int  rows = 1, cols = 1;
static int  g_ch = 16;      /* glyph height */
static int  stat_h = 22;

// Catppuccin Mocha (the user's theme across the desktop).
static const unsigned int BG      = 0xFF1E1E2E; // base
static const unsigned int FG      = 0xFFCDD6F4; // text
static const unsigned int CURSOR  = 0xFF89B4FA; // blue
static const unsigned int SEL_BG  = 0xFF585B70; // surface2
static const unsigned int SEL_FG  = 0xFFCDD6F4; // text
static const unsigned int STAT_BG = 0xFF313244; // surface0
static const unsigned int STAT_FG = 0xFFA6ADC8; // subtext0
static const unsigned int SB_BG   = 0xFF181825; // mantle
static const unsigned int SB_FG   = 0xFF585B70; // surface2
static const unsigned int DIM     = 0xFF7F849C; // overlay1

static void refresh(void) { gravity_refresh(&app); }

static void set_status(const char* s) {
    int i = 0;
    while (s[i] && i < (int)sizeof(status) - 1) { status[i] = s[i]; i++; }
    status[i] = '\0';
}

/* "edit: <basename> *" — sent only when it actually changes, so typing does
   not flood the WM with title messages. */
static char title_text[40];
static char title_sent[40];

static void update_title(void) {
    const char* name = path;
    for (const char* p = path; *p; p++) if (*p == '/') name = p + 1;

    int n = 0;
    const char* pre = "edit: ";
    for (int i = 0; pre[i] && n < 31; i++) title_text[n++] = pre[i];
    for (int i = 0; name[i] && n < 30; i++) title_text[n++] = name[i];
    if (modified) { title_text[n++] = ' '; title_text[n++] = '*'; }
    title_text[n] = '\0';

    int same = 1;
    for (int i = 0; i <= n; i++)
        if (title_text[i] != title_sent[i]) { same = 0; break; }
    if (same) return;

    for (int i = 0; i <= n; i++) title_sent[i] = title_text[i];
    gravity_set_title(&app, title_text);
}

/* Number of full lines from byte offset 'a' up to 'b'. */
static int lines_between(int a, int b) {
    int n = 0, off = a;
    while (off < b) {
        int e = gravity_buffer_line_end(&text, off);
        if (e >= text.len) break;
        off = e + 1;
        n++;
    }
    return n;
}

static void ensure_visible(void) {
    int ls = gravity_buffer_line_start(&text, cursor);
    while (top > ls) top = gravity_buffer_prev_line(&text, top);
    while (top < ls && lines_between(top, ls) >= rows)
        top = gravity_buffer_next_line(&text, top);
    int col = gravity_buffer_col(&text, cursor);
    if (col < left) left = col;
    if (col >= left + cols) left = col - cols + 1;
    if (left < 0) left = 0;
}

/* --- selection --- */

/* The selection is [sel_lo, sel_hi): the anchor 'sel' and the caret, either
   order. has_sel() is false for a bare anchor (a click that never dragged). */
static int sel_lo(void) { return (sel < cursor) ? sel : cursor; }
static int sel_hi(void) { return (sel < cursor) ? cursor : sel; }
static int has_sel(void) { return sel >= 0 && sel != cursor; }

/* --- editing --- */

static void edited(void) {
    modified = 1;
    quit_asked = 0;
    goal_col = -1;
    status[0] = '\0'; // drop a stale "saved"/error line once typing resumes
    update_title();
    ensure_visible();
    refresh();
}

static void insert_text(const char* s, int n) {
    if (gravity_buffer_insert(&text, cursor, s, n) != 0) {
        set_status("out of memory");
        return;
    }
    cursor += n;
    edited();
}

/* Insert 's' at the caret, replacing the selection if there is one. */
static void replace_sel(const char* s, int n) {
    if (has_sel()) {
        int lo = sel_lo();
        gravity_buffer_erase(&text, lo, sel_hi() - lo);
        cursor = lo;
        sel = -1;
    }
    if (n > 0) { insert_text(s, n); return; }
    edited();
}

static void backspace(void) {
    if (has_sel()) { replace_sel(0, 0); return; }
    if (cursor <= 0) return;
    gravity_buffer_erase(&text, cursor - 1, 1);
    cursor--;
    edited();
}

static void delete_forward(void) {
    if (has_sel()) { replace_sel(0, 0); return; }
    if (cursor >= text.len) return;
    gravity_buffer_erase(&text, cursor, 1);
    edited();
}

static void insert_tab(void) {
    int col = gravity_buffer_col(&text, cursor);
    int n = GRAVITY_BUFFER_TAB - (col % GRAVITY_BUFFER_TAB);
    char sp[GRAVITY_BUFFER_TAB];
    for (int i = 0; i < n; i++) sp[i] = ' ';
    replace_sel(sp, n);
}

/* --- clipboard (Ctrl+Shift+C/X/V; a file, see <grv/clipboard.h>) --- */

static void copy_sel(void) {
    if (!has_sel()) { set_status("nothing selected"); refresh(); return; }
    int lo = sel_lo();
    int n = sel_hi() - lo;
    if (grv_clip_copy(text.buf + lo, n) != 0) {
        set_status("copy failed");
        refresh();
        return;
    }
    snprintf(status, sizeof(status), "copied %d bytes", n);
    refresh();
}

static void cut_sel(void) {
    if (!has_sel()) { set_status("nothing selected"); refresh(); return; }
    int lo = sel_lo();
    int n = sel_hi() - lo;
    if (grv_clip_copy(text.buf + lo, n) != 0) {
        set_status("cut failed");
        refresh();
        return;
    }
    gravity_buffer_erase(&text, lo, n);
    cursor = lo;
    sel = -1;
    edited();
    snprintf(status, sizeof(status), "cut %d bytes", n);
    refresh();
}

static void paste(void) {
    static char buf[GRV_CLIPBOARD_MAX];
    int n = grv_clip_paste(buf, (int)sizeof(buf));
    if (n <= 0) { set_status("clipboard empty"); refresh(); return; }
    replace_sel(buf, n);
    snprintf(status, sizeof(status), "pasted %d bytes", n);
    refresh();
}

/* A shifted movement extends the selection from the current caret; an
   unshifted one drops it. */
static void anchor_for(int shift) {
    if (shift) { if (sel < 0) sel = cursor; }
    else sel = -1;
}

static void move_h(int d, int shift) {
    anchor_for(shift);
    if (d < 0 && cursor > 0) cursor--;
    else if (d > 0 && cursor < text.len) cursor++;
    goal_col = -1;
    ensure_visible();
    refresh();
}

/* Move the caret 'n' lines (negative = up), holding the target column. */
static void move_lines(int n, int shift) {
    anchor_for(shift);
    if (goal_col < 0) goal_col = gravity_buffer_col(&text, cursor);
    while (n > 0) {
        int ls = gravity_buffer_line_start(&text, cursor);
        int t = gravity_buffer_next_line(&text, ls);
        if (t == ls) break;
        cursor = gravity_buffer_offset_at_col(&text, t, goal_col);
        n--;
    }
    while (n < 0) {
        int ls = gravity_buffer_line_start(&text, cursor);
        if (ls == 0) break;
        cursor = gravity_buffer_offset_at_col(&text, gravity_buffer_prev_line(&text, ls), goal_col);
        n++;
    }
    ensure_visible();
    refresh();
}

/* --- file --- */

static void load(void) {
    text.len = 0;
    cursor = 0; top = 0; left = 0; goal_col = -1;
    sel = -1;
    modified = 0; quit_asked = 0;

    int fd = grv_open(path, GRV_O_RDONLY);
    if (fd < 0) {
        set_status(errno == ENOENT ? "new file" : "open failed");
        return;
    }
    char chunk[1024];
    long n;
    while ((n = grv_read_fd(fd, chunk, sizeof(chunk))) > 0) {
        if (gravity_buffer_insert(&text, text.len, chunk, (int)n) != 0) {
            set_status("out of memory");
            break;
        }
    }
    grv_close(fd);
    snprintf(status, sizeof(status), "%d bytes", text.len);
}

static int save(void) {
    int fd = grv_open(path, GRV_O_WRONLY | GRV_O_CREATE | GRV_O_TRUNC);
    if (fd < 0) {
        snprintf(status, sizeof(status), "save failed (errno %d)", errno);
        return -1;
    }
    long n = grv_write_fd(fd, text.buf, (unsigned long)text.len);
    grv_close(fd);
    if (n != (long)text.len) { set_status("short write"); return -1; }

    modified = 0;
    quit_asked = 0;
    snprintf(status, sizeof(status), "saved %d bytes", text.len);
    update_title();
    printf("edit: saved %s (%d bytes)\n", path, text.len);
    return 0;
}

static void try_quit(void) {
    if (modified && !quit_asked) {
        quit_asked = 1;
        set_status("unsaved changes: Ctrl+Q again to discard, Ctrl+S to save");
        refresh();
        return;
    }
    grv_exit(0);
}

static int on_close(struct gravity_app* a) {
    (void)a;
    if (!modified || quit_asked) return 1; // nothing to lose (or already asked)
    quit_asked = 1;
    set_status("unsaved changes: close again to discard, or Ctrl+S to save");
    refresh();
    return 0; // veto for now
}

/* --- drawing --- */

static void draw_scrollbar(int track) {
    int total = gravity_buffer_line_count(&text);
    if (total <= rows) return;
    int x = win_w - SB_W;
    gravity_fill(&app.canvas, x, 0, SB_W, track, SB_BG);
    int thumb_h = track * rows / total;
    if (thumb_h < 16) thumb_h = 16;
    int thumb_y = (int)((long)track * lines_between(0, top) / total);
    if (thumb_y + thumb_h > track) thumb_y = track - thumb_h;
    gravity_fill(&app.canvas, x, thumb_y, SB_W, thumb_h, SB_FG);
}

static int sb_drag = 0;   // dragging the scrollbar thumb
static int mouse_held = 0; // the left button was down on the previous event

/* The buffer offset under a canvas point, or -1 if outside the text area. */
static int offset_at(int mx, int my) {
    int row = my / g_ch;
    if (row < 0 || row >= rows) return -1;
    int col = mx / CW + left;
    int off = top;
    for (int r = 0; r < row; r++) {
        int e = gravity_buffer_line_end(&text, off);
        if (e >= text.len) { off = text.len; break; }
        off = e + 1;
    }
    return gravity_buffer_offset_at_col(&text, off, col);
}

// Map a point in the scrollbar strip to the first visible line, centring the
// thumb on it (so a click jumps and a held drag follows).
static void scrollbar_drag_to(int my) {
    int total = gravity_buffer_line_count(&text);
    if (total <= rows) return;
    int track = win_h - stat_h;
    int thumb_h = track * rows / total;
    if (thumb_h < 16) thumb_h = 16;
    int span = track - thumb_h;
    if (span <= 0) return;

    int ty = my - thumb_h / 2;
    if (ty < 0) ty = 0;
    if (ty > span) ty = span;

    int want = (int)((long)ty * total / span);   // line index from the top
    int off = 0;
    for (int i = 0; i < want; i++) {
        int nx = gravity_buffer_next_line(&text, off);
        if (nx == off) break;
        off = nx;
    }
    top = off;
    refresh();
}

static void draw_view(void) {
    struct gravity_canvas* c = &app.canvas;
    gravity_clear(c, BG);

    int slo = has_sel() ? sel_lo() : -1;
    int shi = has_sel() ? sel_hi() : -1;

    int off = top;
    for (int row = 0; row < rows; row++) {
        int y = row * g_ch;
        if (off > text.len) break;
        int e = gravity_buffer_line_end(&text, off);

        int col = 0;
        for (int i = off; i < e; i++) {
            int w = (text.buf[i] == '\t')
                        ? GRAVITY_BUFFER_TAB - (col % GRAVITY_BUFFER_TAB) : 1;
            int vcol = col - left;
            if (vcol >= 0 && vcol < cols) {
                int x = vcol * CW;
                unsigned char gc = (unsigned char)text.buf[i];
                int isel = (slo >= 0 && i >= slo && i < shi);
                unsigned int bg = isel ? SEL_BG : BG;
                unsigned int fg = isel ? SEL_FG : FG;
                if (gc == '\t') {
                    for (int k = 0; k < w && vcol + k < cols; k++)
                        gravity_fill(c, x + k * CW, y, CW, g_ch, bg);
                } else if (gc >= 32 && gc < 127) {
                    gravity_cell(c, x, y, gc, fg, bg);
                }
            }
            col += w;
            if (col - left >= cols) break;
        }

        // The caret draws on its own line, including the empty one.
        if (cursor >= off && cursor <= e) {
            int ccol = gravity_buffer_col(&text, cursor) - left;
            if (ccol >= 0 && ccol < cols) {
                unsigned char under = (cursor < e) ? (unsigned char)text.buf[cursor] : ' ';
                gravity_cell(c, ccol * CW, y, under, BG, CURSOR);
            }
        }

        if (e >= text.len) break;
        off = e + 1;
    }

    draw_scrollbar(win_h - stat_h);
}

static void draw_status(void) {
    struct gravity_canvas* c = &app.canvas;
    int y = win_h - stat_h;
    gravity_fill(c, 0, y, win_w, stat_h, STAT_BG);
    int ty = y + (stat_h - g_ch) / 2;
    gravity_text(c, MARGIN, ty, path, STAT_FG);

    if (status[0]) {
        int sw = gravity_text_width_of(status);
        int sx = (win_w - sw) / 2;
        if (sx > MARGIN) gravity_text(c, sx, ty, status, DIM);
    }

    char buf[32];
    int line = lines_between(0, gravity_buffer_line_start(&text, cursor)) + 1;
    int col = gravity_buffer_col(&text, cursor) + 1;
    snprintf(buf, sizeof(buf), "%d:%d%s", line, col, modified ? " *" : "");
    gravity_text(c, win_w - MARGIN - gravity_text_width_of(buf), ty, buf, STAT_FG);
}

static void on_draw(struct gravity_app* a) {
    (void)a;
    draw_view();
    draw_status();
}

/* --- events --- */

static void on_event(struct gravity_app* a, const struct gravity_event* ev) {
    (void)a;

    if (ev->type == GRV_EV_KEY) {
        long v = ev->a;
        int mods = (int)ev->b;
        int shift = (mods & GRV_MOD_SHIFT) != 0;

        // Ctrl+Shift+<letter>: the clipboard chords. The kernel leaves the
        // letter intact (and uppercase) when Shift is held, so this cannot be
        // confused with Ctrl+C, which stays the interrupt key.
        if ((mods & GRV_MOD_CTRL) && shift) {
            if (v == 'C') { copy_sel(); return; }
            if (v == 'X') { cut_sel(); return; }
            if (v == 'V') { paste(); return; }
        }

        if (v == CTRL('s')) { save(); refresh(); return; }
        if (v == CTRL('q')) { try_quit(); return; }
        if (v == CTRL('a') || v == GRV_KEY_HOME) {
            anchor_for(shift);
            cursor = gravity_buffer_line_start(&text, cursor);
            goal_col = -1; ensure_visible(); refresh(); return;
        }
        if (v == CTRL('e') || v == GRV_KEY_END) {
            anchor_for(shift);
            cursor = gravity_buffer_line_end(&text, cursor);
            goal_col = -1; ensure_visible(); refresh(); return;
        }
        if (v == GRV_KEY_LEFT)  { move_h(-1, shift); return; }
        if (v == GRV_KEY_RIGHT) { move_h(1, shift); return; }
        if (v == GRV_KEY_UP)    { move_lines(-1, shift); return; }
        if (v == GRV_KEY_DOWN)  { move_lines(1, shift); return; }
        if (v == GRV_KEY_PAGEUP)   { move_lines(-(rows - 1), shift); return; }
        if (v == GRV_KEY_PAGEDOWN) { move_lines(rows - 1, shift); return; }
        if (v == GRV_KEY_DELETE)   { delete_forward(); return; }
        if (v == '\n') { replace_sel("\n", 1); return; }
        if (v == '\b') { backspace(); return; }
        if (v == '\t') { insert_tab(); return; }
        if (v >= 32 && v < 127) { char ch2 = (char)v; replace_sel(&ch2, 1); return; }
        return;
    }

    if (ev->type == GRV_EV_SCROLL) {
        int d = grv_scroll_delta(ev->b);
        while (d > 0 && top > 0) { top = gravity_buffer_prev_line(&text, top); d--; }
        while (d < 0) {
            int t = gravity_buffer_next_line(&text, top);
            if (t == top) break;
            top = t; d++;
        }
        refresh();
        return;
    }

    if (ev->type == GRV_EV_MOUSE) {
        int mx = grv_mouse_x(ev->b);
        int my = grv_mouse_y(ev->b);
        int held = (ev->a & GRV_BTN_LEFT) != 0;

        // The strip on the right is the scrollbar: click or drag it. The WM
        // captures the pointer while the button is held, so the drag keeps
        // arriving even if it leaves the strip.
        if (held && (sb_drag || mx >= win_w - SB_W)) {
            sb_drag = 1;
            mouse_held = 1;
            scrollbar_drag_to(my);
            return;
        }
        if (!held) {
            sb_drag = 0;
            mouse_held = 0;
            if (sel >= 0 && sel == cursor) sel = -1; // a click, not a drag
            return;
        }

        // Press or drag inside the text. The press sets the anchor, so a drag
        // from it selects; a drag on its own keeps the anchor and moves the
        // caret, which is what extends the selection.
        int pos = offset_at(mx, my);
        if (pos < 0) return;
        if (!mouse_held) { mouse_held = 1; sel = pos; }
        cursor = pos;
        goal_col = -1;
        ensure_visible();
        refresh();
        return;
    }
}

int main(int argc, char** argv) {
    int role = 0, argi = 1;
    gravity_args(argc, argv, &win_w, &win_h, &role, &argi);

    if (argi < argc && argv[argi] && argv[argi][0]) {
        int i = 0;
        while (argv[argi][i] && i < (int)sizeof(path) - 1) { path[i] = argv[argi][i]; i++; }
        path[i] = '\0';
    }

    gravity_init(); // loads the font; needed to size the text grid
    g_ch = gravity_font_height();
    stat_h = g_ch + 6;
    rows = (win_h - stat_h) / g_ch;
    cols = (win_w - SB_W - 2 * MARGIN) / CW;
    if (rows < 1) rows = 1;
    if (cols < 1) cols = 1;

    if (gravity_buffer_init(&text, 4096) != 0) {
        printf("edit: out of memory\n");
        return 1;
    }
    load();
    update_title();

    app.on_event = on_event;
    app.on_draw = on_draw;
    app.on_close = on_close;
    app.user = 0;
    app.wm_role = role;

    printf("edit: %s (%d bytes)\n", path, text.len);
    gravity_run(&app, win_w, win_h);
    return 0;
}