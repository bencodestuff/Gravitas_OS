#include <gravity/app.h>
#include <grv/syscall.h>
#include <grv/input.h>

void gravity_run(struct gravity_app* app, int w, int h) {
    if (gravity_canvas_init(&app->canvas, w, h) < 0) return;
    grv_input_mode(1); // keys and mouse arrive as messages

    if (app->on_draw) app->on_draw(app);
    gravity_present(&app->canvas);

    for (;;) {
        struct grv_msg m;
        if (grv_recv(&m, 1) != 0) continue;

        if (m.type == GRV_EV_KEY || m.type == GRV_EV_MOUSE) {
            struct gravity_event ev;
            ev.type = (long)m.type;
            ev.a = (long)m.a;
            ev.b = (long)m.b;
            if (app->on_event) app->on_event(app, &ev);
        }
        if (app->on_draw) app->on_draw(app);
        gravity_present(&app->canvas);
    }
}
