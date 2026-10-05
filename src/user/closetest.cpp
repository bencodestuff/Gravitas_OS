#include <stdio.h>
#include <gravity/gravity.h>
#include <grv/syscall.h>
#include <grv/input.h>

// closetest — a client that handles the WM's close request instead of being
// killed: it vetoes the first one (as a save prompt would) and accepts the
// next. Also exercises updating the title after startup.

static struct gravity_app app;
static int closes = 0;

static int on_close(struct gravity_app* a) {
    closes++;
    printf("closetest: close requested #%d\n", closes);
    if (closes == 1) {
        printf("closetest: vetoed (staying open)\n");
        gravity_set_title(a, "Close test (vetoed)");
        gravity_refresh(a);
        return 0; // stay open
    }
    printf("closetest: closing\n");
    return 1;     // exit now
}

static void on_draw(struct gravity_app* a) {
    gravity_clear(&a->canvas, 0xFF2A2030);
    gravity_text(&a->canvas, 12, 12,
                 closes ? "close vetoed" : "click the close box", 0xFFFFFFFF);
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    app.on_draw = on_draw;
    app.on_close = on_close;
    app.title = "Close test";
    app.user = 0;

    gravity_run(&app, 360, 200);
    return 0;
}