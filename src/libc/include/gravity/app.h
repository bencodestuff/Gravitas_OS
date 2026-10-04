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
   block for an event, dispatch it, redraw, present. */
struct gravity_app {
    struct gravity_canvas canvas;
    void (*on_event)(struct gravity_app* app, const struct gravity_event* ev);
    void (*on_draw)(struct gravity_app* app);
    void* user;
};

void gravity_run(struct gravity_app* app, int w, int h);

__END_DECLS

#endif
