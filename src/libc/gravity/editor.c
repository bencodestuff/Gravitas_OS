#include <gravity/editor.h>
#include <grv/syscall.h>
#include <grv/input.h>
#include <grv/clipboard.h>

#include <errno.h>
#include <stdio.h>

// gravity_editor — see editor.h. This is what edit.elf used to be, with the
// window, the status line and the save/quit policy left to the host, so the IDE
// can put it in a pane and give it tabs.

// Catppuccin Mocha, the desktop theme. A shared gravity_theme is on the
// roadmap; until then each widget carries its own palette.
static const unsigned int BG     = 0xFF1E1E2E; // base
static const unsigned int FG     = 0xFFCDD6F4; // text
static const unsigned int CURSOR = 0xFF89B4FA; // blue
static const unsigned int SEL_BG = 0xFF585B70; // surface2
static const unsigned int SEL_FG = 0xFFCDD6F4; // text
static const unsigned int SB_BG  = 0xFF181825; // mantle
static const unsigned int SB_FG  = 0xFF585B70; // surface2

#define CTRL(c) ((c) - 'a' + 1)
#define CW   GRAVITY_EDITOR_CW
#define SB_W GRAVITY_EDITOR_SB

static void set_msg(struct gravity_editor* e, const char* s) {
    int i = 0;
    while (s[i] && i < (int)sizeof(e->msg) - 1) { e->msg[i] = s[i]; i++; }
    e->msg[i] = '\0';
}

/* Number of full lines from byte offset 'a' up to 'b'. */
static int lines_between(const struct gravity_editor* e, int a, int b) {
    int n = 0, off = a;
    while (off < b) {
        int end = gravity_buffer_line_end(&e->text, off);
        if (end >= e->text.len) break;
        off = end + 1;
        n++;
    }
    return n;
}

static void ensure_visible(struct gravity_editor* e) {
    int ls = gravity_buffer_line_start(&e->text, e->cursor);
    while (e->top > ls) e->top = gravity_buffer_prev_line(&e->text, e->top);
    while (e->top < ls && lines_between(e, e->top, ls) >= e->rows)
        e->top = gravity_buffer_next_line(&e->text, e->top);
    int col = gravity_buffer_col(&e->text, e->cursor);
    if (col < e->left) e->left = col;
    if (col >= e->left + e->cols) e->left = col - e->cols + 1;
    if (e->left < 0) e->left = 0;
}

/* --- selection --- */

/* [sel_lo, sel_hi): the anchor and the caret, either order. has_sel() is false
   for a bare anchor (a click that never dragged). */
static int sel_lo(const struct gravity_editor* e) {
    return (e->sel < e->cursor) ? e->sel : e->cursor;
}
static int sel_hi(const struct gravity_editor* e) {
    return (e->sel < e->cursor) ? e->cursor : e->sel;
}
static int has_sel(const struct gravity_editor* e) {
    return e->sel >= 0 && e->sel != e->cursor;
}

/* --- editing --- */

static void edited(struct gravity_editor* e) {
    e->modified = 1;
    e->rev++;
    e->goal_col = -1;
    e->msg[0] = '\0'; // drop a stale "saved"/error line once typing resumes
    ensure_visible(e);
}

static void insert_text(struct gravity_editor* e, const char* s, int n) {
    if (gravity_buffer_insert(&e->text, e->cursor, s, n) != 0) {
        set_msg(e, "out of memory");
        return;
    }
    e->cursor += n;
    edited(e);
}

/* Insert 's' at the caret, replacing the selection if there is one. */
static void replace_sel(struct gravity_editor* e, const char* s, int n) {
    if (has_sel(e)) {
        int lo = sel_lo(e);
        gravity_buffer_erase(&e->text, lo, sel_hi(e) - lo);
        e->cursor = lo;
        e->sel = -1;
    }
    if (n > 0) { insert_text(e, s, n); return; }
    edited(e);
}

static void backspace(struct gravity_editor* e) {
    if (has_sel(e)) { replace_sel(e, 0, 0); return; }
    if (e->cursor <= 0) return;
    gravity_buffer_erase(&e->text, e->cursor - 1, 1);
    e->cursor--;
    edited(e);
}

static void delete_forward(struct gravity_editor* e) {
    if (has_sel(e)) { replace_sel(e, 0, 0); return; }
    if (e->cursor >= e->text.len) return;
    gravity_buffer_erase(&e->text, e->cursor, 1);
    edited(e);
}

static void insert_tab(struct gravity_editor* e) {
    int col = gravity_buffer_col(&e->text, e->cursor);
    int n = GRAVITY_BUFFER_TAB - (col % GRAVITY_BUFFER_TAB);
    char sp[GRAVITY_BUFFER_TAB];
    for (int i = 0; i < n; i++) sp[i] = ' ';
    replace_sel(e, sp, n);
}

/* --- clipboard (Ctrl+Shift+C/X/V; a file, see <grv/clipboard.h>) --- */

static void copy_sel(struct gravity_editor* e) {
    if (!has_sel(e)) { set_msg(e, "nothing selected"); return; }
    int lo = sel_lo(e);
    int n = sel_hi(e) - lo;
    if (grv_clip_copy(e->text.buf + lo, n) != 0) {
        set_msg(e, "copy failed");
        return;
    }
    snprintf(e->msg, sizeof(e->msg), "copied %d bytes", n);
}

static void cut_sel(struct gravity_editor* e) {
    if (!has_sel(e)) { set_msg(e, "nothing selected"); return; }
    int lo = sel_lo(e);
    int n = sel_hi(e) - lo;
    if (grv_clip_copy(e->text.buf + lo, n) != 0) {
        set_msg(e, "cut failed");
        return;
    }
    gravity_buffer_erase(&e->text, lo, n);
    e->cursor = lo;
    e->sel = -1;
    edited(e);
    snprintf(e->msg, sizeof(e->msg), "cut %d bytes", n);
}

static void paste(struct gravity_editor* e) {
    static char buf[GRV_CLIPBOARD_MAX];
    int n = grv_clip_paste(buf, (int)sizeof(buf));
    if (n <= 0) { set_msg(e, "clipboard empty"); return; }
    replace_sel(e, buf, n);
    snprintf(e->msg, sizeof(e->msg), "pasted %d bytes", n);
}

/* A shifted movement extends the selection from the caret; an unshifted one
   drops it. */
static void anchor_for(struct gravity_editor* e, int shift) {
    if (shift) { if (e->sel < 0) e->sel = e->cursor; }
    else e->sel = -1;
}

static void move_h(struct gravity_editor* e, int d, int shift) {
    anchor_for(e, shift);
    if (d < 0 && e->cursor > 0) e->cursor--;
    else if (d > 0 && e->cursor < e->text.len) e->cursor++;
    e->goal_col = -1;
    ensure_visible(e);
}

/* Move the caret 'n' lines (negative = up), holding the target column. */
static void move_lines(struct gravity_editor* e, int n, int shift) {
    anchor_for(e, shift);
    if (e->goal_col < 0) e->goal_col = gravity_buffer_col(&e->text, e->cursor);
    while (n > 0) {
        int ls = gravity_buffer_line_start(&e->text, e->cursor);
        int t = gravity_buffer_next_line(&e->text, ls);
        if (t == ls) break;
        e->cursor = gravity_buffer_offset_at_col(&e->text, t, e->goal_col);
        n--;
    }
    while (n < 0) {
        int ls = gravity_buffer_line_start(&e->text, e->cursor);
        if (ls == 0) break;
        e->cursor = gravity_buffer_offset_at_col(
            &e->text, gravity_buffer_prev_line(&e->text, ls), e->goal_col);
        n++;
    }
    ensure_visible(e);
}

/* --- file --- */

void gravity_editor_reload(struct gravity_editor* e) {
    e->text.len = 0;
    e->cursor = 0; e->top = 0; e->left = 0; e->goal_col = -1;
    e->sel = -1;
    e->modified = 0;

    int fd = grv_open(e->path, GRV_O_RDONLY);
    if (fd < 0) {
        set_msg(e, errno == ENOENT ? "new file" : "open failed");
        return;
    }
    char chunk[1024];
    long n;
    while ((n = grv_read_fd(fd, chunk, sizeof(chunk))) > 0) {
        if (gravity_buffer_insert(&e->text, e->text.len, chunk, (int)n) != 0) {
            set_msg(e, "out of memory");
            break;
        }
    }
    grv_close(fd);
    snprintf(e->msg, sizeof(e->msg), "%d bytes", e->text.len);
}

int gravity_editor_save(struct gravity_editor* e) {
    int fd = grv_open(e->path, GRV_O_WRONLY | GRV_O_CREATE | GRV_O_TRUNC);
    if (fd < 0) {
        snprintf(e->msg, sizeof(e->msg), "save failed (errno %d)", errno);
        return -1;
    }
    long n = grv_write_fd(fd, e->text.buf, (unsigned long)e->text.len);
    grv_close(fd);
    if (n != (long)e->text.len) {
        set_msg(e, "short write");
        return -1;
    }
    e->modified = 0;
    snprintf(e->msg, sizeof(e->msg), "saved %d bytes", e->text.len);
    return (int)e->text.len;
}

/* --- drawing --- */

static void draw_scrollbar(struct gravity_editor* e, struct gravity_canvas* c) {
    int total = gravity_buffer_line_count(&e->text);
    if (total <= e->rows) return;
    int x = e->x + e->w - SB_W;
    int track = e->h;
    gravity_fill(c, x, e->y, SB_W, track, SB_BG);
    int thumb_h = track * e->rows / total;
    if (thumb_h < 16) thumb_h = 16;
    int thumb_y = (int)((long)track * lines_between(e, 0, e->top) / total);
    if (thumb_y + thumb_h > track) thumb_y = track - thumb_h;
    gravity_fill(c, x, e->y + thumb_y, SB_W, thumb_h, SB_FG);
}

/* The buffer offset under a point, or -1 if it is outside the text area. */
static int offset_at(const struct gravity_editor* e, int lx, int ly) {
    int row = ly / e->g_ch;
    if (row < 0 || row >= e->rows) return -1;
    int col = lx / CW + e->left;
    int off = e->top;
    for (int r = 0; r < row; r++) {
        int end = gravity_buffer_line_end(&e->text, off);
        if (end >= e->text.len) { off = e->text.len; break; }
        off = end + 1;
    }
    return gravity_buffer_offset_at_col(&e->text, off, col);
}

/* Map a point in the scrollbar strip to the first visible line, centring the
   thumb on it (so a click jumps and a held drag follows). */
static void scrollbar_drag_to(struct gravity_editor* e, int ly) {
    int total = gravity_buffer_line_count(&e->text);
    if (total <= e->rows) return;
    int track = e->h;
    int thumb_h = track * e->rows / total;
    if (thumb_h < 16) thumb_h = 16;
    int span = track - thumb_h;
    if (span <= 0) return;

    int ty = ly - thumb_h / 2;
    if (ty < 0) ty = 0;
    if (ty > span) ty = span;

    int want = (int)((long)ty * total / span); // line index from the top
    int off = 0;
    for (int i = 0; i < want; i++) {
        int nx = gravity_buffer_next_line(&e->text, off);
        if (nx == off) break;
        off = nx;
    }
    e->top = off;
}

void gravity_editor_draw(struct gravity_editor* e, struct gravity_canvas* c) {
    // Only the widget's own region is cleared: a host may have panes beside it.
    gravity_fill(c, e->x, e->y, e->w, e->h, BG);

    int slo = has_sel(e) ? sel_lo(e) : -1;
    int shi = has_sel(e) ? sel_hi(e) : -1;

    int off = e->top;
    for (int row = 0; row < e->rows; row++) {
        int y = e->y + row * e->g_ch;
        if (off > e->text.len) break;
        int end = gravity_buffer_line_end(&e->text, off);

        int col = 0;
        for (int i = off; i < end; i++) {
            int w = (e->text.buf[i] == '\t')
                        ? GRAVITY_BUFFER_TAB - (col % GRAVITY_BUFFER_TAB) : 1;
            int vcol = col - e->left;
            if (vcol >= 0 && vcol < e->cols) {
                int x = e->x + vcol * CW;
                unsigned char gc = (unsigned char)e->text.buf[i];
                int isel = (slo >= 0 && i >= slo && i < shi);
                unsigned int bg = isel ? SEL_BG : BG;
                unsigned int fg = isel ? SEL_FG : FG;
                if (gc == '\t') {
                    for (int k = 0; k < w && vcol + k < e->cols; k++)
                        gravity_fill(c, x + k * CW, y, CW, e->g_ch, bg);
                } else if (gc >= 32 && gc < 127) {
                    gravity_cell(c, x, y, gc, fg, bg);
                }
            }
            col += w;
            if (col - e->left >= e->cols) break;
        }

        // The caret draws on its own line, including the empty one.
        if (e->cursor >= off && e->cursor <= end) {
            int ccol = gravity_buffer_col(&e->text, e->cursor) - e->left;
            if (ccol >= 0 && ccol < e->cols) {
                unsigned char under = (e->cursor < end)
                                          ? (unsigned char)e->text.buf[e->cursor] : ' ';
                gravity_cell(c, e->x + ccol * CW, y, under, BG, CURSOR);
            }
        }

        if (end >= e->text.len) break;
        off = end + 1;
    }

    draw_scrollbar(e, c);
}

/* --- events --- */

int gravity_editor_key(struct gravity_editor* e, long v, int mods) {
    int shift = (mods & GRV_MOD_SHIFT) != 0;

    // Ctrl+Shift+<letter>: the clipboard chords. The kernel leaves the letter
    // intact (and uppercase) when Shift is held, so this cannot be confused
    // with Ctrl+C, which stays the interrupt key.
    if ((mods & GRV_MOD_CTRL) && shift) {
        if (v == 'C') { copy_sel(e); return 1; }
        if (v == 'X') { cut_sel(e); return 1; }
        if (v == 'V') { paste(e); return 1; }
    }

    if (v == CTRL('a') || v == GRV_KEY_HOME) {
        anchor_for(e, shift);
        e->cursor = gravity_buffer_line_start(&e->text, e->cursor);
        e->goal_col = -1; ensure_visible(e); return 1;
    }
    if (v == CTRL('e') || v == GRV_KEY_END) {
        anchor_for(e, shift);
        e->cursor = gravity_buffer_line_end(&e->text, e->cursor);
        e->goal_col = -1; ensure_visible(e); return 1;
    }
    if (v == GRV_KEY_LEFT)  { move_h(e, -1, shift); return 1; }
    if (v == GRV_KEY_RIGHT) { move_h(e, 1, shift); return 1; }
    if (v == GRV_KEY_UP)    { move_lines(e, -1, shift); return 1; }
    if (v == GRV_KEY_DOWN)  { move_lines(e, 1, shift); return 1; }
    if (v == GRV_KEY_PAGEUP)   { move_lines(e, -(e->rows - 1), shift); return 1; }
    if (v == GRV_KEY_PAGEDOWN) { move_lines(e, e->rows - 1, shift); return 1; }
    if (v == GRV_KEY_DELETE)   { delete_forward(e); return 1; }
    if (v == '\n') { replace_sel(e, "\n", 1); return 1; }
    if (v == '\b') { backspace(e); return 1; }
    if (v == '\t') { insert_tab(e); return 1; }
    if (v >= 32 && v < 127) { char ch = (char)v; replace_sel(e, &ch, 1); return 1; }
    return 0;
}

void gravity_editor_mouse(struct gravity_editor* e, int mx, int my, int held) {
    int lx = mx - e->x;
    int ly = my - e->y;

    // The strip on the right is the scrollbar: click or drag it. The WM
    // captures the pointer while the button is held, so a drag keeps arriving
    // even if it leaves the strip.
    if (held && (e->sb_drag || lx >= e->w - SB_W)) {
        e->sb_drag = 1;
        e->mouse_held = 1;
        scrollbar_drag_to(e, ly);
        return;
    }
    if (!held) {
        e->sb_drag = 0;
        e->mouse_held = 0;
        if (e->sel >= 0 && e->sel == e->cursor) e->sel = -1; // a click, not a drag
        return;
    }

    // Press or drag inside the text. The press sets the anchor, so a drag from
    // it selects; a drag on its own keeps the anchor and moves the caret.
    int pos = offset_at(e, lx, ly);
    if (pos < 0) return;
    if (!e->mouse_held) { e->mouse_held = 1; e->sel = pos; }
    e->cursor = pos;
    e->goal_col = -1;
    ensure_visible(e);
}

void gravity_editor_scroll(struct gravity_editor* e, int d) {
    while (d > 0 && e->top > 0) {
        e->top = gravity_buffer_prev_line(&e->text, e->top);
        d--;
    }
    while (d < 0) {
        int t = gravity_buffer_next_line(&e->text, e->top);
        if (t == e->top) break;
        e->top = t;
        d++;
    }
}

/* --- accessors --- */

int gravity_editor_init(struct gravity_editor* e, const char* path,
                        int x, int y, int w, int h) {
    e->x = x; e->y = y; e->w = w; e->h = h;

    gravity_init(); // the font decides the cell height; idempotent
    e->g_ch = gravity_font_height();
    if (e->g_ch < 1) e->g_ch = 16;

    e->rows = h / e->g_ch;
    e->cols = (w - SB_W) / CW;
    if (e->rows < 1) e->rows = 1;
    if (e->cols < 1) e->cols = 1;

    e->cursor = 0; e->sel = -1; e->top = 0; e->left = 0; e->goal_col = -1;
    e->modified = 0; e->rev = 0; e->sb_drag = 0; e->mouse_held = 0;
    e->msg[0] = '\0';

    int i = 0;
    while (path && path[i] && i < (int)sizeof(e->path) - 1) { e->path[i] = path[i]; i++; }
    e->path[i] = '\0';

    if (gravity_buffer_init(&e->text, 4096) != 0) return -1;
    gravity_editor_reload(e);
    return 0;
}

void gravity_editor_free(struct gravity_editor* e) {
    gravity_buffer_free(&e->text);
}

int gravity_editor_dirty(const struct gravity_editor* e) { return e->modified; }
int gravity_editor_len(const struct gravity_editor* e) { return e->text.len; }
int gravity_editor_rev(const struct gravity_editor* e) { return e->rev; }

const char* gravity_editor_path(const struct gravity_editor* e) { return e->path; }
const char* gravity_editor_msg(const struct gravity_editor* e) { return e->msg; }

const char* gravity_editor_basename(const struct gravity_editor* e) {
    const char* name = e->path;
    for (const char* p = e->path; *p; p++) if (*p == '/') name = p + 1;
    return name;
}

void gravity_editor_set_msg(struct gravity_editor* e, const char* s) {
    set_msg(e, s);
}

void gravity_editor_status(const struct gravity_editor* e, char* out, int max) {
    int line = lines_between(e, 0,
                             gravity_buffer_line_start(&e->text, e->cursor)) + 1;
    int col = gravity_buffer_col(&e->text, e->cursor) + 1;
    snprintf(out, (unsigned long)max, "%d:%d%s", line, col,
             e->modified ? " *" : "");
}
