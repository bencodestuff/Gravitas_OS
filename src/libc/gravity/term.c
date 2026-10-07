#include <gravity/term.h>
#include <grv/syscall.h>
#include <stdlib.h>
#include <string.h>

/* The history is one linear buffer of 'capacity' lines; the visible screen
   is a window into it. When the cursor runs off the bottom the whole buffer
   shifts up a line (memmove) — cheap at these sizes and far easier to reason
   about than a wrapping ring. */

int gravity_term_init(struct gravity_term* t, int cols, int rows,
                      int cell_w, int cell_h, unsigned int fg,
                      unsigned int bg) {
    if (cols <= 0 || rows <= 0 || cell_w <= 0 || cell_h <= 0) return -1;

    t->cols = cols;
    t->rows = rows;
    t->cell_w = cell_w;
    t->cell_h = cell_h;
    t->def_fg = fg;
    t->def_bg = bg;
    t->capacity = rows + GRAVITY_TERM_SCROLL;
    t->chars = 0;
    t->fg = 0;
    t->dirty = 0;

    unsigned long cells = (unsigned long)t->capacity * (unsigned long)cols;
    t->chars = (unsigned char*)malloc(cells);
    t->fg = (unsigned int*)malloc(cells * sizeof(unsigned int));
    t->dirty = (unsigned char*)malloc((unsigned long)t->capacity);
    if (!t->chars || !t->fg || !t->dirty) {
        gravity_term_free(t);
        return -1;
    }
    gravity_term_clear(t);
    return 0;
}

void gravity_term_free(struct gravity_term* t) {
    if (t->chars) free(t->chars);
    if (t->fg) free(t->fg);
    if (t->dirty) free(t->dirty);
    t->chars = 0;
    t->fg = 0;
    t->dirty = 0;
}

void gravity_term_clear(struct gravity_term* t) {
    unsigned long cells = (unsigned long)t->capacity * (unsigned long)t->cols;
    memset(t->chars, ' ', cells);
    for (unsigned long i = 0; i < cells; i++) t->fg[i] = t->def_fg;
    memset(t->dirty, 1, (unsigned long)t->capacity);
    t->used = 1;
    t->cur = 0;
    t->cx = 0;
    t->view = 0;
    t->all_dirty = 1;
}

/* Advance to a fresh line, scrolling the history when at the bottom. */
static void term_newline(struct gravity_term* t) {
    t->dirty[t->cur] = 1; // cursor leaves this line
    t->cx = 0;

    if (t->cur + 1 < t->capacity) {
        t->cur++;
        if (t->cur + 1 > t->used) t->used = t->cur + 1;
    } else {
        unsigned long cols = (unsigned long)t->cols;
        memmove(t->chars, t->chars + cols,
                (unsigned long)(t->capacity - 1) * cols);
        memmove(t->fg, t->fg + cols,
                (unsigned long)(t->capacity - 1) * cols * sizeof(unsigned int));
        unsigned long base = (unsigned long)(t->capacity - 1) * cols;
        memset(t->chars + base, ' ', cols);
        for (unsigned long i = 0; i < cols; i++) t->fg[base + i] = t->def_fg;
        t->cur = t->capacity - 1;
        t->all_dirty = 1; // every visible line shifted
    }
    t->dirty[t->cur] = 1;
}

static void put_cell(struct gravity_term* t, unsigned char ch) {
    unsigned long idx = (unsigned long)t->cur * (unsigned long)t->cols + t->cx;
    t->chars[idx] = ch;
    t->fg[idx] = t->def_fg;
    t->dirty[t->cur] = 1;
    t->cx++;
    if (t->cx >= t->cols) term_newline(t); // auto-wrap
}

void gravity_term_putc(struct gravity_term* t, int c) {
    if (c == '\n') { term_newline(t); return; }
    if (c == '\r') { t->cx = 0; t->dirty[t->cur] = 1; return; }
    if (c == '\b') {
        // Destructive backspace, matching the kernel console: the shell
        // erases with a bare '\b' and expects the cell to blank.
        if (t->cx > 0) {
            t->cx--;
            unsigned long idx =
                (unsigned long)t->cur * (unsigned long)t->cols + t->cx;
            t->chars[idx] = ' ';
            t->fg[idx] = t->def_fg;
        }
        t->dirty[t->cur] = 1;
        return;
    }
    if (c == '\t') {
        int n = ((t->cx + 8) & ~7) - t->cx;
        for (int i = 0; i < n; i++) {
            if (t->cx == 0 && i > 0) break; // wrapped mid-tab
            put_cell(t, ' ');
        }
        return;
    }
    if (c < 32 || c > 126) return; // other control bytes: swallow
    put_cell(t, (unsigned char)c);
}

void gravity_term_write(struct gravity_term* t, const char* buf, int n) {
    for (int i = 0; i < n; i++) gravity_term_putc(t, (unsigned char)buf[i]);
}

void gravity_term_render(struct gravity_term* t, struct gravity_canvas* c,
                         int ox, int oy) {
    int first = t->used - t->rows - t->view;
    if (first < 0) first = 0;

    for (int r = 0; r < t->rows; r++) {
        int line = first + r;
        int repaint = t->all_dirty ||
                      (line >= 0 && line < t->capacity && t->dirty[line]);
        if (!repaint) continue;

        int py = oy + r * t->cell_h;
        for (int col = 0; col < t->cols; col++) {
            int px = ox + col * t->cell_w;
            if (line < 0 || line >= t->used) {
                gravity_cell(c, px, py, ' ', t->def_fg, t->def_bg);
                continue;
            }
            unsigned long idx =
                (unsigned long)line * (unsigned long)t->cols + col;
            if (t->view == 0 && line == t->cur && col == t->cx) // inverse block
                gravity_cell(c, px, py, t->chars[idx], t->def_bg, t->def_fg);
            else
                gravity_cell(c, px, py, t->chars[idx], t->fg[idx], t->def_bg);
        }
    }

    // Only the visible lines were repainted; leave off-screen dirt intact.
    for (int r = 0; r < t->rows; r++) {
        int line = first + r;
        if (line >= 0 && line < t->capacity) t->dirty[line] = 0;
    }
    t->all_dirty = 0;
}

void gravity_term_scrollback(struct gravity_term* t, int lines) {
    int maxview = t->used - t->rows;
    if (maxview < 0) maxview = 0;
    t->view += lines;
    if (t->view < 0) t->view = 0;
    if (t->view > maxview) t->view = maxview;
    t->all_dirty = 1;
}

void gravity_term_snap_bottom(struct gravity_term* t) {
    t->view = 0;
    t->all_dirty = 1;
}

void gravity_term_redraw(struct gravity_term* t) { t->all_dirty = 1; }

int gravity_term_view(const struct gravity_term* t) { return t->view; }

int gravity_term_maxview(const struct gravity_term* t) {
    int m = t->used - t->rows;
    return m > 0 ? m : 0;
}

/* --- the scrollback strip (the host's scrollbar) --- */

// Catppuccin Mocha, the desktop theme (see gravity_editor for the same note).
static const unsigned int SB_TRACK = 0xFF202428;
static const unsigned int SB_THUMB = 0xFF6A7A8A;

void gravity_term_scrollbar_draw(struct gravity_term* t,
                                 struct gravity_canvas* c,
                                 int x, int y, int w, int h) {
    gravity_fill(c, x, y, w, h, SB_TRACK);
    int max = gravity_term_maxview(t);
    if (max <= 0) return; // nothing scrolled back: no thumb
    int total = max + t->rows;
    int thumb = h * t->rows / total;
    if (thumb < 16) thumb = 16;
    int ty = (h - thumb) * (max - t->view) / max;
    gravity_fill(c, x, y + ty, w, thumb, SB_THUMB);
}

void gravity_term_scrollbar_drag(struct gravity_term* t, int y, int h) {
    int max = gravity_term_maxview(t);
    if (max <= 0) return;
    int total = max + t->rows;
    int thumb = h * t->rows / total;
    if (thumb < 16) thumb = 16;
    int span = h - thumb;
    if (span <= 0) return;

    int top = y - thumb / 2;
    if (top < 0) top = 0;
    if (top > span) top = span;

    int view = max - top * max / span;
    gravity_term_scrollback(t, view - t->view);
}

/* --- hosting a program (the console endpoint) --- */

int gravity_term_host(struct gravity_term* t, const char* prog) {
    const char* argv[1] = { prog };
    int pid = (int)grv_spawn_bg(prog, argv, 1);
    if (pid < 0) {
        static const char m[] = "cannot start ";
        gravity_term_write(t, m, (int)sizeof(m) - 1);
        int n = 0;
        while (prog[n]) n++;
        gravity_term_write(t, prog, n);
        gravity_term_write(t, "\n", 1);
        return -1;
    }
    if (grv_console_attach(pid) != 0) {
        static const char m[] = "cannot attach console\n";
        gravity_term_write(t, m, (int)sizeof(m) - 1);
        return -1;
    }
    return pid;
}

int gravity_term_pump(struct gravity_term* t) {
    char buf[512];
    long n;
    int got = 0;
    while ((n = grv_console_read(buf, sizeof(buf))) > 0) {
        gravity_term_write(t, buf, (int)n);
        got += (int)n;
    }
    return got;
}
