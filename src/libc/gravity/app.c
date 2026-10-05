#include <gravity/app.h>
#include <grv/syscall.h>
#include <grv/input.h>
#include <grv/wm.h>

static void maybe_draw(struct gravity_app* app) {
    if (!app->redraw) return;
    if (app->on_draw) {
        app->on_draw(app);
        gravity_present(&app->canvas);
    }
    if (app->popup_open && app->on_popup_draw) {
        app->on_popup_draw(app);
        gravity_popup_present(app);
    }
    app->redraw = 0;
}

// Answer the WM's hello: always the title; for the popup surface also its
// role (undecorated/on-top) and where we want it placed.
static void answer_hello(struct gravity_app* app, const struct grv_msg* m) {
    app->wm_task = (int)m->from; // so gravity_set_title can reach it later

    grv_wm_title_send((long)m->from, app->title ? app->title : "");

    if (app->popup_open && (int)m->a == app->popup.surface_id) {
        struct grv_msg r;
        r.from = 0;
        r.type = GRV_WM_SET_ROLE;
        r.a = (unsigned long)app->popup.surface_id;
        r.b = GRV_WM_ROLE_POPUP;
        grv_send((long)m->from, &r);
        // The WM cascaded it on discovery; move it where we want it now that
        // it knows this surface is a popup.
        grv_surface_move(app->popup.surface_id, app->popup_x, app->popup_y);
    }
}

void gravity_run(struct gravity_app* app, int w, int h) {
    if (gravity_canvas_init(&app->canvas, w, h) < 0) return;
    grv_input_mode(1); // keys and mouse arrive as messages

    app->popup_open = 0;
    app->wm_task = 0;

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

        if (m.type == GRV_EV_KEY || m.type == GRV_EV_MOUSE ||
            m.type == GRV_EV_TIMER || m.type == GRV_EV_SCROLL) {
            struct gravity_event ev;
            ev.type = (long)m.type;
            ev.a = (long)m.a;
            ev.b = (long)m.b;
            if (app->on_event) app->on_event(app, &ev);
        } else if (m.type == GRV_WM_HELLO) {
            answer_hello(app, &m);
        } else if (m.type == GRV_WM_DISMISS) {
            // The WM saw a click away from a popup: let the app close it (it
            // owns the policy — what to redraw, whether to keep state).
            struct gravity_event ev;
            ev.type = GRV_WM_DISMISS;
            ev.a = (long)m.a;
            ev.b = 0;
            if (app->on_event) app->on_event(app, &ev);
        } else if (m.type == GRV_WM_CLOSE) {
            // The user hit the close box. An app that must save first can
            // veto (on_close returns 0); we ack so the WM waits instead of
            // force-killing. With no handler we exit straight away.
            if (app->on_close) {
                struct grv_msg ack;
                ack.from = 0;
                ack.type = GRV_WM_CLOSE_ACK;
                ack.a = m.a;
                ack.b = 0;
                grv_send((long)m.from, &ack);
                if (app->on_close(app)) grv_exit(0);
            } else {
                grv_exit(0);
            }
        }
        // Redraw only when the app asked — an ignored event (a hover
        // motion) does no drawing and, crucially, no commit.
        maybe_draw(app);
    }
}

void gravity_set_title(struct gravity_app* app, const char* title) {
    app->title = title;
    if (app->wm_task <= 0) return; // no hello yet: the next answer uses it
    grv_wm_title_send(app->wm_task, title);
}
