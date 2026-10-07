#include <stdio.h>
#include <gravity/gravity.h>
#include <grv/syscall.h>
#include <grv/input.h>
#include <grv/clipboard.h>

// term — a standalone window over the terminal widget (gravity_term). The grid
// hosts the userland shell; this owns the window, the scrollbar and the keys.
// The kernel console endpoint carries the shell's stdio here (see the
// CONSOLE_* syscalls), so keystrokes go in and output comes back without the
// kernel knowing what a terminal is.

#define DEF_COLS 80
#define DEF_ROWS 25
#define CELL_W   8
#define CELL_H   16

static const unsigned int FG = 0xFFD0E0F0;
static const unsigned int BG = 0xFF101418;

static struct gravity_app app;
static struct gravity_term term;
static int shell_pid = -1;

static int g_cols = DEF_COLS;   /* the grid size (--geometry may change it) */
static int g_rows = DEF_ROWS;

static int strip_x(void) { return g_cols * CELL_W; }
static int strip_h(void) { return g_rows * CELL_H; }

// Output arrives in our console ring, woken by an EV_CONSOLE_OUT mailbox
// message; drain it each loop and feed the grid.
static void on_poll(struct gravity_app* a) {
    if (gravity_term_pump(&term) > 0) gravity_refresh(a);
}

static int sb_drag = 0;   // dragging the scrollbar thumb

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
        if (held && (sb_drag || mx >= strip_x())) {
            sb_drag = 1;
            gravity_term_scrollbar_drag(&term, my, strip_h());
            gravity_refresh(a);
        } else if (!held) {
            sb_drag = 0;
        }
        return;
    }
    if (ev->type != GRV_EV_KEY) return;
    long v = ev->a;

    if (v == GRV_KEY_PAGEUP) {
        gravity_term_scrollback(&term, g_rows - 1);
        gravity_refresh(a);
        return;
    }
    if (v == GRV_KEY_PAGEDOWN) {
        gravity_term_scrollback(&term, -(g_rows - 1));
        gravity_refresh(a);
        return;
    }
    if (shell_pid < 0) return;

    // Ctrl+Shift+V pastes the file clipboard into the hosted shell's stdin —
    // the same path as typing, so a pasted command runs.
    if ((ev->b & GRV_MOD_CTRL) && (ev->b & GRV_MOD_SHIFT) &&
        (v == 'V' || v == 'v')) {
        static char buf[GRV_CLIPBOARD_MAX];
        int n = grv_clip_paste(buf, (int)sizeof(buf));
        if (n > 0) {
            gravity_term_snap_bottom(&term);
            grv_console_write(shell_pid, buf, (unsigned long)n);
            gravity_refresh(a);
        }
        return;
    }

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

static void on_draw(struct gravity_app* a) {
    // No clear: the grid owns every cell and repaints only the rows that
    // changed, so the surface must persist between frames. The first render
    // (all rows dirty) paints the whole thing.
    gravity_term_render(&term, &a->canvas, 0, 0);
    gravity_term_scrollbar_draw(&term, &a->canvas, strip_x(), 0,
                                GRAVITY_TERM_SB, strip_h());
}

int main(int argc, char** argv) {
    int w = 0, h = 0, argi = 1;
    gravity_args(argc, argv, &w, &h, &argi);

    // The grid derives its cell count from the requested window size.
    if (w > 0) g_cols = (w - GRAVITY_TERM_SB) / CELL_W;
    if (h > 0) g_rows = h / CELL_H;
    if (g_cols < 20) g_cols = 20;
    if (g_rows < 4) g_rows = 4;

    if (gravity_term_init(&term, g_cols, g_rows, CELL_W, CELL_H, FG, BG) != 0) {
        printf("term: out of memory\n");
        return 1;
    }

    shell_pid = gravity_term_host(&term, "shell.elf");
    if (shell_pid >= 0 && argi < argc) {
        // An initial command (e.g. `term cat /hello.txt`): type it for the
        // shell once attached, so a launcher can open a file straight away.
        static char cmd[256];
        int n = 0;
        for (int i = argi; i < argc && n < 250; i++) {
            if (i > argi) cmd[n++] = ' ';
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
    gravity_run(&app, g_cols * CELL_W + GRAVITY_TERM_SB, g_rows * CELL_H);
    return 0;
}
