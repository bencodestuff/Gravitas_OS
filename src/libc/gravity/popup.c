#include <gravity/app.h>
#include <grv/syscall.h>

/* Popups: a second surface the WM leaves undecorated and raises above the
   main window. See app.h. */

int gravity_popup_open(struct gravity_app* app, int x, int y, int w, int h) {
    if (app->popup_open) return -1;
    if (gravity_canvas_init(&app->popup, w, h) < 0) return -1;
    app->popup_open = 1;
    app->popup_x = x;
    app->popup_y = y;
    return 0;
}

void gravity_popup_present(struct gravity_app* app) {
    if (app->popup_open) gravity_present(&app->popup);
}

void gravity_popup_close(struct gravity_app* app) {
    if (!app->popup_open) return;
    gravity_canvas_done(&app->popup);
    app->popup_open = 0;
}
