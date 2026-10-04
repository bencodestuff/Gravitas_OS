#include <gravity/widget.h>

void gravity_button_draw(struct gravity_canvas* c,
                         const struct gravity_button* b, int hot) {
    gravity_fill(c, b->x, b->y, b->w, b->h, hot ? b->bg_hot : b->bg);
    gravity_border(c, b->x, b->y, b->w, b->h, 0xFF000000);
    if (b->label) {
        int tw = gravity_text_width_of(b->label);
        int th = gravity_font_height();
        gravity_text(c, b->x + (b->w - tw) / 2, b->y + (b->h - th) / 2,
                     b->label, b->fg);
    }
}
