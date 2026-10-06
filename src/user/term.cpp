#include <stdio.h>
#include <gravity/gravity.h>
#include <grv/syscall.h>
#include <grv/input.h>

// term — the Gravitas terminal: a Gravity text grid that hosts the userland
// shell. The kernel console endpoint carries the shell's stdio here (see the
// CONSOLE_* syscalls), so keystrokes go in and output comes back without the
// kernel knowing what a terminal is.

#define COLS   80
#define ROWS   25
#define CELL_W 8
#define CELL_H 16
#define SB_W   6   /* scrollbar strip to the right of the grid */

static const unsigned int FG = 0xFFD0E0F0;
static const unsigned int BG = 0xFF101418;

static struct gravity_app app;
static struct gravity_term term;
static int shell_pid = -1;

// Output arrives in our console ring, woken by an EV_CONSOLE_OUT mailbox
// message; poll it each loop and feed the grid.
static void on_poll(struct gravity_app* a) {
    char buf[512];
    long n;
    int got = 0;
    while ((n = grv_console_read(buf, sizeof(buf))) > 0) {
        gravity_term_write(&term, buf, (int)n);
        got = 1;
    }
    if (got) gravity_refresh(a);
}

static int sb_drag = 0;   // dragging the scrollbar thumb

// Map a point in the scrollbar strip to a scrollback position, centring the
// thumb on it (so a click jumps and a held drag follows).
static void scrollbar_drag_to(int my) {
    int max = gravity_term_maxview(&term);
    if (max <= 0) return;
    int h = ROWS * CELL_H;
    int total = max + ROWS;
    int thumb = h * ROWS / total;
    if (thumb < 16) thumb = 16;
    int span = h - thumb;
    if (span <= 0) return;

    int top = my - thumb / 2;
    if (top < 0) top = 0;
    if (top > span) top = span;

    int view = max - top * max / span;
    gravity_term_scrollback(&term, view - gravity_term_view(&term));
}

static void on_event(struct gravity_app* a, const struct gravity_event* ev) {
    if (ev->type == GRV_EV_SCROLL) {
        // Wheel: three lines per notch (positive = up/back in time).
        gravity_term_scrollback(&term, grv_scroll_delta(ev->b) * 3);
        gravity_refresh(a);
        return;
    }
    if (ev->type == GRV_EV_MOUSE) {
        // The strip right of the grid is the scrollbar: click or drag it. The
        // WM captures the pointer while the button is held, so a drag keeps
        // arriving even if it leaves the strip.
        int mx = grv_mouse_x(ev->b);
        int my = grv_mouse_y(ev->b);
        int held = (ev->a & GRV_BTN_LEFT) != 0;
        if (held && (sb_drag || mx >= COLS * CELL_W)) {
            sb_drag = 1;
            scrollbar_drag_to(my);
            gravity_refresh(a);
        } else if (!held) {
            sb_drag = 0;
        }
        return;
    }
    if (ev->type != GRV_EV_KEY) return;
    long v = ev->a;

    if (v == GRV_KEY_PAGEUP) {
        gravity_term_scrollback(&term, ROWS - 1);
        gravity_refresh(a);
        return;
    }
    if (v == GRV_KEY_PAGEDOWN) {
        gravity_term_scrollback(&term, -(ROWS - 1));
        gravity_refresh(a);
        return;
    }
    if (shell_pid < 0) return;

    // Everything else is a byte for the hosted shell's stdin. Typing snaps
    // the scrollback back to the live screen.
    char c;
    if (v >= 32 && v < 127) c = (char)v;
    else if (v == '\n' || v == '\b' || v == '\t') c = (char)v;
    else if (v == 3) c = 3;   // Ctrl+C: the kernel interrupts the shell's job
    else return;

    gravity_term_snap_bottom(&term);
    grv_console_write(shell_pid, &c, 1);
    gravity_refresh(a);
}

static void draw_scrollbar(struct gravity_canvas* c) {
    int x = COLS * CELL_W;
    int h = ROWS * CELL_H;
    gravity_fill(c, x, 0, SB_W, h, 0xFF202428); // track
    int max = gravity_term_maxview(&term);
    if (max <= 0) return;
    int total = max + ROWS;
    int thumb = h * ROWS / total;
    if (thumb < 16) thumb = 16;
    int view = gravity_term_view(&term);        // 0 = bottom, max = top
    int y = (h - thumb) * (max - view) / max;
    gravity_fill(c, x, y, SB_W, thumb, 0xFF6A7A8A);
}

static void on_draw(struct gravity_app* a) {
    // No clear: the grid owns every cell and repaints only the rows that
    // changed, so the surface must persist between frames. The first render
    // (all rows dirty) paints the whole thing.
    gravity_term_render(&term, &a->canvas, 0, 0);
    draw_scrollbar(&a->canvas); // lives in the strip right of the grid
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    if (gravity_term_init(&term, COLS, ROWS, CELL_W, CELL_H, FG, BG) != 0) {
        printf("term: out of memory\n");
        return 1;
    }

    const char* cargv[1] = { "shell.elf" };
    shell_pid = (int)grv_spawn_bg("shell.elf", cargv, 1);
    if (shell_pid < 0) {
        const char* msg = "term: cannot start shell\n";
        int n = 0; while (msg[n]) n++;
        gravity_term_write(&term, msg, n);
    } else if (grv_console_attach(shell_pid) != 0) {
        const char* msg = "term: cannot attach console\n";
        int n = 0; while (msg[n]) n++;
        gravity_term_write(&term, msg, n);
    } else if (argc > 1) {
        // An initial command (e.g. `term cat /hello.txt`): type it for the
        // shell once attached, so a launcher can open a file straight away.
        static char cmd[256];
        int n = 0;
        for (int i = 1; i < argc && n < 250; i++) {
            if (i > 1) cmd[n++] = ' ';
            for (int j = 0; argv[i][j] && n < 250; j++) cmd[n++] = argv[i][j];
        }
        cmd[n++] = '\n';
        grv_console_write(shell_pid, cmd, (unsigned long)n);
    }
    printf("term: hosting shell as task %d\n", shell_pid);

    app.on_event = on_event;
    app.on_draw = on_draw;
    app.on_poll = on_poll;
    app.title = "Terminal";
    app.user = 0;
    gravity_run(&app, COLS * CELL_W + SB_W, ROWS * CELL_H);
    return 0;
}
