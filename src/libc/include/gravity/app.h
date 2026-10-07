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
    struct gravity_canvas popup;   /* second surface, valid while popup_open */
    int popup_open;
    int popup_x, popup_y;          /* where the popup should sit on screen */
    void (*on_event)(struct gravity_app* app, const struct gravity_event* ev);
    void (*on_draw)(struct gravity_app* app);
    /* Optional: draw the popup's contents (into app->popup); Gravity presents
       it. Called on redraw while a popup is open. */
    void (*on_popup_draw)(struct gravity_app* app);
    /* Optional: run once per loop, before blocking for the next event. For
       work that arrives outside the input mailbox — e.g. a terminal
       draining the output of the programs it hosts. */
    void (*on_poll)(struct gravity_app* app);
    /* Optional: the WM asked us to close. Return nonzero to exit now, 0 to
       stay open (e.g. after prompting to save). With no handler Gravity
       exits straight away. */
    int (*on_close)(struct gravity_app* app);
    const char* title;  /* optional window title (answers the WM's hello) */
    void* user;
    int redraw;   /* set by gravity_refresh; internal */
    int wm_task;  /* the WM's task id (from its hello); 0 until known */
};

/* Ask for a redraw. Call from on_event when the app's state changed. */
static inline void gravity_refresh(struct gravity_app* app) { app->redraw = 1; }

/* Arm the app's one-shot timer: after 'ms' the app receives a
   GRV_EV_TIMER event in on_event (call again to make it repeat). 0 cancels. */
static inline void gravity_timer(struct gravity_app* app, unsigned long ms) {
    (void)app;
    grv_timer_set(ms);
}

void gravity_run(struct gravity_app* app, int w, int h);

/* Leading option flags for a Gravity app: "--geometry WxH" sizes the window (0,
   or no 'x', keeps the default for that axis). Returns 1 if the flag was seen;
   *argi is the index of the first positional argument (1 when there are none).
   Any pointer may be null. */
int  gravity_args(int argc, char** argv, int* w, int* h, int* argi);

/* Popups: a second, undecorated surface the WM places at (x,y) and raises
   above the main window. Open one and draw into app->popup via
   on_popup_draw; the WM asks the app to close it (GRV_WM_DISMISS) when the
   user clicks away. */
int  gravity_popup_open(struct gravity_app* app, int x, int y, int w, int h);
void gravity_popup_present(struct gravity_app* app);
void gravity_popup_close(struct gravity_app* app);

/* Change the window title after startup (the WM updates the taskbar/title
   bar). Safe to call any time after gravity_run has started. */
void gravity_set_title(struct gravity_app* app, const char* title);

__END_DECLS

#endif
