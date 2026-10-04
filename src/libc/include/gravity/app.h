#ifndef _GRAVITY_APP_H
#define _GRAVITY_APP_H

#include <gravity/canvas.h>
#include <grv/input.h>   /* event types, button bits, key codes */
#include <sys/cdefs.h>

__BEGIN_DECLS

/* A decoded input event (codes in grv/input.h). */
struct gravity_event {
    long type; /* GRV_EV_KEY or GRV_EV_MOUSE */
    long a;
    long b;
};

/* An app is a canvas plus two callbacks. Fill them in and call
   gravity_run(); it creates the window, enables event input, and loops:
   block for an event, dispatch it, and redraw+present only when the app
   asked to (see gravity_refresh) — so an event it ignores (a mouse move
   it does not care about) costs nothing. */
struct gravity_app {
    struct gravity_canvas canvas;
    void (*on_event)(struct gravity_app* app, const struct gravity_event* ev);
    void (*on_draw)(struct gravity_app* app);
    void* user;
    int redraw;   /* set by gravity_refresh; internal */
};

/* Ask for a redraw. Call from on_event when the app's state changed. */
static inline void gravity_refresh(struct gravity_app* app) { app->redraw = 1; }

void gravity_run(struct gravity_app* app, int w, int h);

__END_DECLS

#endif
