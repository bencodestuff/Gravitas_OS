#include <gravity/list.h>

void gravity_list_init(struct gravity_list* l, int x, int y, int w, int h,
                       int row_h, unsigned int fg, unsigned int bg,
                       unsigned int sel_fg, unsigned int sel_bg) {
    l->x = x; l->y = y; l->w = w; l->h = h;
    if (row_h < 1) row_h = 1;
    l->row_h = row_h;
    l->n = 0;
    l->sel = -1;
    l->top = 0;
    l->fg = fg; l->bg = bg; l->sel_fg = sel_fg; l->sel_bg = sel_bg;
    l->items = 0;
}

int gravity_list_rows(const struct gravity_list* l) {
    return l->h / l->row_h;
}

// Keep the selection inside the visible window.
static void reveal(struct gravity_list* l) {
    int rows = gravity_list_rows(l);
    if (rows <= 0) { l->top = 0; return; }
    if (l->sel < 0) { l->top = 0; return; }
    if (l->sel < l->top) l->top = l->sel;
    if (l->sel >= l->top + rows) l->top = l->sel - rows + 1;
    int max = l->n - rows;
    if (max < 0) max = 0;
    if (l->top > max) l->top = max;
    if (l->top < 0) l->top = 0;
}

void gravity_list_set(struct gravity_list* l, const char* const* items, int n) {
    l->items = items;
    l->n = n;
    if (l->sel >= n) l->sel = n - 1;
    if (l->sel < 0 && n > 0) l->sel = 0;
    reveal(l);
}

int gravity_list_hit(const struct gravity_list* l, int px, int py) {
    if (px < l->x || px >= l->x + l->w || py < l->y || py >= l->y + l->h)
        return -1;
    int idx = l->top + (py - l->y) / l->row_h;
    return (idx >= 0 && idx < l->n) ? idx : -1;
}

void gravity_list_select(struct gravity_list* l, int index) {
    if (l->n == 0) { l->sel = -1; return; }
    if (index < 0) index = 0;
    if (index >= l->n) index = l->n - 1;
    l->sel = index;
    reveal(l);
}

void gravity_list_move(struct gravity_list* l, int delta) {
    if (l->n == 0) return;
    gravity_list_select(l, l->sel + delta);
}

void gravity_list_scroll(struct gravity_list* l, int delta) {
    int rows = gravity_list_rows(l);
    int max = l->n - rows;
    if (max < 0) max = 0;
    l->top += delta;
    if (l->top < 0) l->top = 0;
    if (l->top > max) l->top = max;
}

static void draw_row(struct gravity_list* l, struct gravity_canvas* c, int idx,
                     int ry) {
    int selected = (idx == l->sel);
    gravity_fill(c, l->x, ry, l->w, l->row_h,
                 selected ? l->sel_bg : l->bg);
    const char* s = (l->items && l->items[idx]) ? l->items[idx] : "";
    char buf[160];
    int maxc = l->w / 8;
    if (maxc > (int)sizeof(buf) - 1) maxc = (int)sizeof(buf) - 1;
    int i = 0;
    while (s[i] && i < maxc) { buf[i] = s[i]; i++; }
    buf[i] = '\0';
    gravity_text(c, l->x + 4,
                 ry + (l->row_h - gravity_font_height()) / 2, buf,
                 selected ? l->sel_fg : l->fg);
}

void gravity_list_draw(struct gravity_list* l, struct gravity_canvas* c) {
    gravity_fill(c, l->x, l->y, l->w, l->h, l->bg);
    int rows = gravity_list_rows(l);
    for (int r = 0; r < rows; r++) {
        int idx = l->top + r;
        if (idx >= l->n) break;
        draw_row(l, c, idx, l->y + r * l->row_h);
    }
}
