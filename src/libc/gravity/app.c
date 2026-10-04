#include <gravity/app.h>
#include <grv/syscall.h>
#include <grv/input.h>
#include <grv/wm.h>

static void maybe_draw(struct gravity_app* app) {
    if (app->redraw && app->on_draw) {
        app->on_draw(app);
        gravity_present(&app->canvas);
        app->redraw = 0;
    }
}

// Answer the WM's title query (pure userland IPC).
static void answer_hello(struct gravity_app* app, const struct grv_msg* m) {
    unsigned long a, b;
    grv_wm_title_pack(app->title ? app->title : "", &a, &b);
    struct grv_msg r;
    r.from = 0;
    r.type = GRV_WM_SET_TITLE;
    r.a = a;
    r.b = b;
    grv_send((long)m->from, &r);
}

void gravity_run(struct gravity_app* app, int w, int h) {
    if (gravity_canvas_init(&app->canvas, w, h) < 0) return;
    grv_input_mode(1); // keys and mouse arrive as messages

    if (app->on_draw) app->on_draw(app);
    gravity_present(&app->canvas);
    app->redraw = 0;

    for (;;) {
        // Poll for work that is not an input event (the mailbox message that
        // woke us may just have been a "there is output" nudge).
        if (app->on_poll) app->on_poll(app);
        maybe_draw(app);

        struct grv_msg m;
        if (grv_recv(&m, 1) != 0) continue;

        if (m.type == GRV_EV_KEY || m.type == GRV_EV_MOUSE) {
            struct gravity_event ev;
            ev.type = (long)m.type;
            ev.a = (long)m.a;
            ev.b = (long)m.b;
            if (app->on_event) app->on_event(app, &ev);
        } else if (m.type == GRV_WM_HELLO) {
            answer_hello(app, &m);
        }
        // Redraw only when the app asked — an ignored event (a hover
        // motion) does no drawing and, crucially, no commit.
        maybe_draw(app);
    }
}
