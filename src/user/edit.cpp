#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <gravity/gravity.h>
#include <grv/syscall.h>
#include <grv/input.h>

// edit — a text editor built on Gravity.
//
// A viewport over a gravity_text buffer. Arrow keys, Home/End, PageUp/
// PageDown and the wheel move the caret; printable keys, Enter, Tab (spaces
// to the next stop), Backspace and Delete edit; clicking places the caret.
// Ctrl+S saves, Ctrl+Q quits — and the WM's close box goes through the same
// unsaved-changes check (the app vetoes once, then discards, which is what
// Gravity's cooperative-close protocol is for).
//
// The title tracks the file name and a trailing '*' while modified.

#define W        760
#define H        520
#define CW       8     /* glyph cell width — the PSF2 console font is 8px */
#define SB_W     6     /* scrollbar strip on the right */
#define MARGIN   6

#define CTRL(c)  ((c) - 'a' + 1)

static struct gravity_app app;
static struct gravity_text text;

static char path[160] = "/tmp/untitled.txt";
static char status[96];
static int  cursor = 0;
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
        int e = gravity_text_line_end(&text, off);
        if (e >= text.len) break;
        off = e + 1;
        n++;
    }
    return n;
}

static void ensure_visible(void) {
    int ls = gravity_text_line_start(&text, cursor);
    while (top > ls) top = gravity_text_prev_line(&text, top);
    while (top < ls && lines_between(top, ls) >= rows)
        top = gravity_text_next_line(&text, top);
    int col = gravity_text_col(&text, cursor);
    if (col < left) left = col;
    if (col >= left + cols) left = col - cols + 1;
    if (left < 0) left = 0;
}

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
    if (gravity_text_insert(&text, cursor, s, n) != 0) {
        set_status("out of memory");
        return;
    }
    cursor += n;
    edited();
}

static void backspace(void) {
    if (cursor <= 0) return;
    gravity_text_erase(&text, cursor - 1, 1);
    cursor--;
    edited();
}

static void delete_forward(void) {
    if (cursor >= text.len) return;
    gravity_text_erase(&text, cursor, 1);
    edited();
}

static void insert_tab(void) {
    int col = gravity_text_col(&text, cursor);
    int n = GRAVITY_TEXT_TAB - (col % GRAVITY_TEXT_TAB);
    char sp[GRAVITY_TEXT_TAB];
    for (int i = 0; i < n; i++) sp[i] = ' ';
    insert_text(sp, n);
}

static void move_h(int d) {
    if (d < 0 && cursor > 0) cursor--;
    else if (d > 0 && cursor < text.len) cursor++;
    goal_col = -1;
    ensure_visible();
    refresh();
}

/* Move the caret 'n' lines (negative = up), holding the target column. */
static void move_lines(int n) {
    if (goal_col < 0) goal_col = gravity_text_col(&text, cursor);
    while (n > 0) {
        int ls = gravity_text_line_start(&text, cursor);
        int t = gravity_text_next_line(&text, ls);
        if (t == ls) break;
        cursor = gravity_text_offset_at_col(&text, t, goal_col);
        n--;
    }
    while (n < 0) {
        int ls = gravity_text_line_start(&text, cursor);
        if (ls == 0) break;
        cursor = gravity_text_offset_at_col(&text, gravity_text_prev_line(&text, ls), goal_col);
        n++;
    }
    ensure_visible();
    refresh();
}

/* --- file --- */

static void load(void) {
    text.len = 0;
    cursor = 0; top = 0; left = 0; goal_col = -1;
    modified = 0; quit_asked = 0;

    int fd = grv_open(path, GRV_O_RDONLY);
    if (fd < 0) {
        set_status(errno == ENOENT ? "new file" : "open failed");
        return;
    }
    char chunk[1024];
    long n;
    while ((n = grv_read_fd(fd, chunk, sizeof(chunk))) > 0) {
        if (gravity_text_insert(&text, text.len, chunk, (int)n) != 0) {
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
        if (errno == EROFS) set_status("read-only filesystem — save under /tmp");
        else snprintf(status, sizeof(status), "save failed (errno %d)", errno);
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
    int total = gravity_text_line_count(&text);
    if (total <= rows) return;
    int x = W - SB_W;
    gravity_fill(&app.canvas, x, 0, SB_W, track, SB_BG);
    int thumb_h = track * rows / total;
    if (thumb_h < 16) thumb_h = 16;
    int thumb_y = (int)((long)track * lines_between(0, top) / total);
    if (thumb_y + thumb_h > track) thumb_y = track - thumb_h;
    gravity_fill(&app.canvas, x, thumb_y, SB_W, thumb_h, SB_FG);
}

static void draw_view(void) {
    struct gravity_canvas* c = &app.canvas;
    gravity_clear(c, BG);

    int off = top;
    for (int row = 0; row < rows; row++) {
        int y = row * g_ch;
        if (off > text.len) break;
        int e = gravity_text_line_end(&text, off);

        int col = 0;
        for (int i = off; i < e; i++) {
            int w = (text.buf[i] == '\t')
                        ? GRAVITY_TEXT_TAB - (col % GRAVITY_TEXT_TAB) : 1;
            int vcol = col - left;
            if (vcol >= 0 && vcol < cols) {
                int x = vcol * CW;
                unsigned char gc = (unsigned char)text.buf[i];
                if (gc == '\t') {
                    for (int k = 0; k < w && vcol + k < cols; k++)
                        gravity_fill(c, x + k * CW, y, CW, g_ch, BG);
                } else if (gc >= 32 && gc < 127) {
                    gravity_cell(c, x, y, gc, FG, BG);
                }
            }
            col += w;
            if (col - left >= cols) break;
        }

        // The caret draws on its own line, including the empty one.
        if (cursor >= off && cursor <= e) {
            int ccol = gravity_text_col(&text, cursor) - left;
            if (ccol >= 0 && ccol < cols) {
                unsigned char under = (cursor < e) ? (unsigned char)text.buf[cursor] : ' ';
                gravity_cell(c, ccol * CW, y, under, BG, CURSOR);
            }
        }

        if (e >= text.len) break;
        off = e + 1;
    }

    draw_scrollbar(H - stat_h);
}

static void draw_status(void) {
    struct gravity_canvas* c = &app.canvas;
    int y = H - stat_h;
    gravity_fill(c, 0, y, W, stat_h, STAT_BG);
    int ty = y + (stat_h - g_ch) / 2;
    gravity_text(c, MARGIN, ty, path, STAT_FG);

    if (status[0]) {
        int sw = gravity_text_width_of(status);
        int sx = (W - sw) / 2;
        if (sx > MARGIN) gravity_text(c, sx, ty, status, DIM);
    }

    char buf[32];
    int line = lines_between(0, gravity_text_line_start(&text, cursor)) + 1;
    int col = gravity_text_col(&text, cursor) + 1;
    snprintf(buf, sizeof(buf), "%d:%d%s", line, col, modified ? " *" : "");
    gravity_text(c, W - MARGIN - gravity_text_width_of(buf), ty, buf, STAT_FG);
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
        if (v == CTRL('s')) { save(); refresh(); return; }
        if (v == CTRL('q')) { try_quit(); return; }
        if (v == CTRL('a') || v == GRV_KEY_HOME) {
            cursor = gravity_text_line_start(&text, cursor);
            goal_col = -1; ensure_visible(); refresh(); return;
        }
        if (v == CTRL('e') || v == GRV_KEY_END) {
            cursor = gravity_text_line_end(&text, cursor);
            goal_col = -1; ensure_visible(); refresh(); return;
        }
        if (v == GRV_KEY_LEFT)  { move_h(-1); return; }
        if (v == GRV_KEY_RIGHT) { move_h(1); return; }
        if (v == GRV_KEY_UP)    { move_lines(-1); return; }
        if (v == GRV_KEY_DOWN)  { move_lines(1); return; }
        if (v == GRV_KEY_PAGEUP)   { move_lines(-(rows - 1)); return; }
        if (v == GRV_KEY_PAGEDOWN) { move_lines(rows - 1); return; }
        if (v == GRV_KEY_DELETE)   { delete_forward(); return; }
        if (v == '\n') { insert_text("\n", 1); return; }
        if (v == '\b') { backspace(); return; }
        if (v == '\t') { insert_tab(); return; }
        if (v >= 32 && v < 127) { char ch2 = (char)v; insert_text(&ch2, 1); return; }
        return;
    }

    if (ev->type == GRV_EV_SCROLL) {
        int d = grv_scroll_delta(ev->b);
        while (d > 0 && top > 0) { top = gravity_text_prev_line(&text, top); d--; }
        while (d < 0) {
            int t = gravity_text_next_line(&text, top);
            if (t == top) break;
            top = t; d++;
        }
        refresh();
        return;
    }

    if (ev->type == GRV_EV_MOUSE && (ev->a & GRV_BTN_LEFT)) {
        int row = grv_mouse_y(ev->b) / g_ch;
        int col = grv_mouse_x(ev->b) / CW + left;
        if (row >= 0 && row < rows) {
            int off = top;
            for (int r = 0; r < row; r++) {
                int e = gravity_text_line_end(&text, off);
                if (e >= text.len) { off = text.len; break; }
                off = e + 1;
            }
            cursor = gravity_text_offset_at_col(&text, off, col);
            goal_col = -1;
            ensure_visible();
            refresh();
        }
        return;
    }
}

int main(int argc, char** argv) {
    if (argc > 1 && argv[1] && argv[1][0]) {
        int i = 0;
        while (argv[1][i] && i < (int)sizeof(path) - 1) { path[i] = argv[1][i]; i++; }
        path[i] = '\0';
    }

    gravity_init(); // loads the font; needed to size the text grid
    g_ch = gravity_font_height();
    stat_h = g_ch + 6;
    rows = (H - stat_h) / g_ch;
    cols = (W - SB_W - 2 * MARGIN) / CW;
    if (rows < 1) rows = 1;
    if (cols < 1) cols = 1;

    if (gravity_text_init(&text, 4096) != 0) {
        printf("edit: out of memory\n");
        return 1;
    }
    load();
    update_title();

    app.on_event = on_event;
    app.on_draw = on_draw;
    app.on_close = on_close;
    app.user = 0;

    printf("edit: %s (%d bytes)\n", path, text.len);
    gravity_run(&app, W, H);
    return 0;
}