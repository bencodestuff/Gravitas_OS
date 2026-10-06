#include <stdio.h>
#include <gravity/gravity.h>
#include <grv/syscall.h>
#include <grv/input.h>
#include <grv/wm.h>

// popuptest — exercises a real popup: click the window (or any key) to open
// an undecorated, on-top surface beside it; a key (or a click in the popup)
// closes it. Prints markers the test suite asserts on.

static struct gravity_app app;

static void popup_open(void) {
    if (app.popup_open) return;

    // Place the popup just inside the main window's top-left.
    int px = 100, py = 100;
    struct grv_surface_info li[8];
    int n = grv_surface_list(li, 8);
    for (int i = 0; i < n; i++)
        if ((int)li[i].id == app.canvas.surface_id) {
            px = (int)li[i].x + 20;
            py = (int)li[i].y + 20;
            break;
        }

    if (gravity_popup_open(&app, px, py, 160, 100) != 0) return;
    printf("popuptest: popup open\n");
    gravity_refresh(&app);
}

static void popup_close(void) {
    if (!app.popup_open) return;
    gravity_popup_close(&app);
    printf("popuptest: popup closed\n");
    gravity_refresh(&app);
}

static void on_event(struct gravity_app* a, const struct gravity_event* ev) {
    if (ev->type == GRV_WM_DISMISS) { // WM: a click landed away from the popup
        popup_close();
        return;
    }
    if (ev->type == GRV_EV_KEY) {
        if (a->popup_open) popup_close(); else popup_open();
        return;
    }
    if (ev->type == GRV_EV_MOUSE && (ev->a & GRV_BTN_LEFT)) {
        int sid = grv_mouse_surface(ev->a);
        if (a->popup_open && sid == a->popup.surface_id) popup_close();
        else popup_open();
    }
}

static void on_draw(struct gravity_app* a) {
    gravity_clear(&a->canvas, 0xFF203040);
    gravity_text(&a->canvas, 12, 12,
                 a->popup_open ? "popup open" : "click for popup", 0xFFFFFFFF);
}

// Open the popup on the first loop so the test suite can drive it with no input.
static void on_poll(struct gravity_app* a) {
    (void)a;
    static int started = 0;
    if (!started) { started = 1; popup_open(); }
}

static void on_popup_draw(struct gravity_app* a) {
    gravity_clear(&a->popup, 0xFF405060);
    gravity_border(&a->popup, 0, 0, a->popup.w, a->popup.h, 0xFFA0C0E0);
    gravity_text(&a->popup, 10, 10, "popup!", 0xFFFFE080);
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    app.on_event = on_event;
    app.on_draw = on_draw;
    app.on_popup_draw = on_popup_draw;
    app.on_poll = on_poll;
    app.title = "Popup test";
    app.user = 0;

    printf("popuptest: main ready\n");
    gravity_run(&app, 320, 200);
    return 0;
}
