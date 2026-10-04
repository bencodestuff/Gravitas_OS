#include <gravity/canvas.h>
#include <grv/syscall.h>
#include <grv/text.h>

static struct grv_font g_font;
static int g_font_ok = 0;
static int g_font_tried = 0;

int gravity_init(void) {
    if (!g_font_tried) {
        g_font_tried = 1;
        g_font_ok = grv_font_load(&g_font, "/fonts/bizcat.psf");
    }
    return g_font_ok;
}

int gravity_font_height(void) { return g_font_ok ? (int)g_font.height : 8; }
int gravity_text_width_of(const char* s) { return grv_text_width(s); }

int gravity_canvas_init(struct gravity_canvas* c, int w, int h) {
    struct grv_surface_info si;
    int id = grv_surface_create((unsigned long)w, (unsigned long)h, &si);
    if (id < 0) return -1;

    gravity_init(); // best effort; drawing works without text too
    c->px = (unsigned int*)si.address;
    c->w = w;
    c->h = h;
    c->surface_id = id;
    c->dx0 = c->dy0 = c->dx1 = c->dy1 = 0; // nothing dirty
    return id;
}

void gravity_canvas_done(struct gravity_canvas* c) {
    if (c->surface_id >= 0) grv_surface_destroy(c->surface_id);
    c->surface_id = -1;
    c->px = 0;
}

void gravity_invalidate(struct gravity_canvas* c, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > c->w ? c->w : x + w;
    int y1 = y + h > c->h ? c->h : y + h;
    if (x1 <= x0 || y1 <= y0) return;

    if (c->dx1 <= c->dx0) { // currently empty
        c->dx0 = x0; c->dy0 = y0; c->dx1 = x1; c->dy1 = y1;
        return;
    }
    if (x0 < c->dx0) c->dx0 = x0;
    if (y0 < c->dy0) c->dy0 = y0;
    if (x1 > c->dx1) c->dx1 = x1;
    if (y1 > c->dy1) c->dy1 = y1;
}

void gravity_fill(struct gravity_canvas* c, int x, int y, int w, int h,
                  unsigned int color) {
    for (int yy = y; yy < y + h; yy++) {
        if (yy < 0 || yy >= c->h) continue;
        unsigned int* row = c->px + (unsigned long)yy * c->w;
        for (int xx = x; xx < x + w; xx++) {
            if (xx < 0 || xx >= c->w) continue;
            row[xx] = color;
        }
    }
    gravity_invalidate(c, x, y, w, h);
}

void gravity_clear(struct gravity_canvas* c, unsigned int color) {
    gravity_fill(c, 0, 0, c->w, c->h, color);
}

void gravity_border(struct gravity_canvas* c, int x, int y, int w, int h,
                    unsigned int color) {
    gravity_fill(c, x, y, w, 1, color);             // top
    gravity_fill(c, x, y + h - 1, w, 1, color);     // bottom
    gravity_fill(c, x, y, 1, h, color);             // left
    gravity_fill(c, x + w - 1, y, 1, h, color);     // right
}

void gravity_text(struct gravity_canvas* c, int x, int y, const char* s,
                  unsigned int fg) {
    if (!gravity_init()) return;
    grv_text_draw(&g_font, c->px, c->w, c->w, c->h, x, y, s, fg);
    gravity_invalidate(c, x, y, grv_text_width(s), (int)g_font.height);
}

void gravity_cell(struct gravity_canvas* c, int x, int y, unsigned char ch,
                  unsigned int fg, unsigned int bg) {
    int gh = gravity_font_height();
    gravity_fill(c, x, y, 8, gh, bg); // background first (this invalidates)
    if (!gravity_init() || ch < 32 || ch > 126) return;

    const unsigned char* g = g_font.glyphs + (unsigned long)ch * g_font.height;
    for (unsigned row = 0; row < g_font.height; row++) {
        int py = y + (int)row;
        if (py < 0 || py >= c->h) continue;
        unsigned char bits = g[row];
        for (int col = 0; col < 8; col++) {
            if (!(bits & (0x80 >> col))) continue;
            int px = x + col;
            if (px < 0 || px >= c->w) continue;
            c->px[(unsigned long)py * c->w + px] = fg;
        }
    }
    gravity_invalidate(c, x, y, 8, gh);
}

void gravity_present(struct gravity_canvas* c) {
    if (c->dx1 <= c->dx0 || c->dy1 <= c->dy0) return;
    // The commit is per calling task: it repaints the task's surface(s)
    // clipped to this rectangle. Gravity assumes one canvas per app.
    grv_surface_commit_rect(c->dx0, c->dy0, c->dx1 - c->dx0, c->dy1 - c->dy0);
    c->dx0 = c->dy0 = c->dx1 = c->dy1 = 0; // clean
}
