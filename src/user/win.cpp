#include <stdio.h>
#include <gravity/gravity.h>

// win — a minimal windowed client, built on Gravity.
//
// Its whole body is a draw callback and an event callback: Gravity owns the
// window surface, the event loop and damage-tracked present. The app just
// reacts to keys and clicks (shown in window-local coordinates).

#define W 360
#define H 220

static struct gravity_app app;
static char last_key = 0;
static int  click_x = -1, click_y = -1;

static void on_event(struct gravity_app* a, const struct gravity_event* ev) {
    if (ev->type == GRV_EV_KEY) {
        long v = ev->a;
        last_key = (v >= 32 && v < 127) ? (char)v : '?';
        printf("win: key '%c'\n", last_key);
        gravity_refresh(a);
    } else if (ev->type == GRV_EV_MOUSE) {
        if (ev->a & GRV_BTN_LEFT) { // a press
            click_x = grv_mouse_x(ev->b);
            click_y = grv_mouse_y(ev->b);
            printf("win: click %d,%d\n", click_x, click_y);
            gravity_refresh(a);
        }
        // hover motion: nothing to draw, so no refresh (and no commit)
    }
}

static void on_draw(struct gravity_app* a) {
    struct gravity_canvas* c = &a->canvas;
    gravity_clear(c, 0xFF203040);
    gravity_text(c, 12, 12, "win.elf", 0xFFFFFFFF);
    gravity_text(c, 12, 40, "click, then type", 0xFFA0C0E0);
    gravity_text(c, 12, 80, "key:", 0xFFA0C0E0);
    if (last_key) {
        char s[2] = { last_key, 0 };
        gravity_text(c, 60, 80, s, 0xFFFFE080);
    }
    if (click_x >= 0) {
        gravity_fill(c, click_x - 3, click_y - 3, 7, 7, 0xFFFF8040);
    }
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    printf("win: window ready\n");
    app.on_event = on_event;
    app.on_draw = on_draw;
    app.title = "Gravity demo";
    app.user = 0;
    gravity_run(&app, W, H);
    return 0;
}
