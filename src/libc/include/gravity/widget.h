#ifndef _GRAVITY_WIDGET_H
#define _GRAVITY_WIDGET_H

#include <gravity/canvas.h>
#include <sys/cdefs.h>

__BEGIN_DECLS

/* A minimal widget: enough to build a button. Retained widgets (state,
   focus, callbacks) can grow from here as apps need them. */

struct gravity_button {
    int x, y, w, h;
    const char* label;
    unsigned int bg, bg_hot, fg;
};

static inline int gravity_button_hit(const struct gravity_button* b,
                                     int x, int y) {
    return x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h;
}

/* Draw the button; 'hot' selects the highlighted background (hover/press). */
void gravity_button_draw(struct gravity_canvas* c,
                         const struct gravity_button* b, int hot);

__END_DECLS

#endif
