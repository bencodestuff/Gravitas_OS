#include <stdio.h>
#include <gravity/gravity.h>
#include <grv/syscall.h>
#include <grv/input.h>

// edit — a standalone window over the editor widget (gravity_editor). The
// widget does the editing; this owns the window, the status line and the
// save/quit policy: Ctrl+S saves, Ctrl+Q quits, and the WM's close box goes
// through the same unsaved-changes check (the app vetoes once, then discards,
// which is what Gravity's cooperative-close protocol is for).
//
// The title tracks the file name and a trailing '*' while modified.

#define DEF_W   760
#define DEF_H   520
#define MARGIN  6

#define CTRL(c) ((c) - 'a' + 1)

static struct gravity_app app;
static struct gravity_editor ed;

static int win_w = DEF_W;  /* the window size (--geometry may change it) */
static int win_h = DEF_H;
static int stat_h = 22;
static int quit_asked = 0;

// Catppuccin Mocha (the user's theme across the desktop).
static const unsigned int STAT_BG = 0xFF313244; // surface0
static const unsigned int STAT_FG = 0xFFA6ADC8; // subtext0
static const unsigned int DIM     = 0xFF7F849C; // overlay1

static void refresh(void) { gravity_refresh(&app); }

/* "edit: <basename> *" — sent only when it actually changes, so typing does
   not flood the WM with title messages. */
static char title_text[48];
static char title_sent[48];

static void update_title(void) {
    int n = 0;
    const char* pre = "edit: ";
    for (int i = 0; pre[i] && n < 31; i++) title_text[n++] = pre[i];
    const char* name = gravity_editor_basename(&ed);
    for (int i = 0; name[i] && n < 30; i++) title_text[n++] = name[i];
    if (gravity_editor_dirty(&ed)) { title_text[n++] = ' '; title_text[n++] = '*'; }
    title_text[n] = '\0';

    int same = 1;
    for (int i = 0; i <= n; i++)
        if (title_text[i] != title_sent[i]) { same = 0; break; }
    if (same) return;

    for (int i = 0; i <= n; i++) title_sent[i] = title_text[i];
    gravity_set_title(&app, title_text);
}

static void try_quit(void) {
    if (gravity_editor_dirty(&ed) && !quit_asked) {
        quit_asked = 1;
        gravity_editor_set_msg(&ed,
            "unsaved changes: Ctrl+Q again to discard, Ctrl+S to save");
        refresh();
        return;
    }
    grv_exit(0);
}

static int on_close(struct gravity_app* a) {
    (void)a;
    if (!gravity_editor_dirty(&ed) || quit_asked) return 1; // nothing to lose
    quit_asked = 1;
    gravity_editor_set_msg(&ed,
        "unsaved changes: close again to discard, or Ctrl+S to save");
    refresh();
    return 0; // veto for now
}

static void on_draw(struct gravity_app* a) {
    struct gravity_canvas* c = &a->canvas;
    gravity_editor_draw(&ed, c);

    int y = win_h - stat_h;
    gravity_fill(c, 0, y, win_w, stat_h, STAT_BG);
    int ty = y + (stat_h - gravity_font_height()) / 2;
    gravity_text(c, MARGIN, ty, gravity_editor_path(&ed), STAT_FG);

    const char* msg = gravity_editor_msg(&ed);
    if (msg[0]) {
        int sw = gravity_text_width_of(msg);
        int sx = (win_w - sw) / 2;
        if (sx > MARGIN) gravity_text(c, sx, ty, msg, DIM);
    }

    char buf[32];
    gravity_editor_status(&ed, buf, sizeof(buf));
    gravity_text(c, win_w - MARGIN - gravity_text_width_of(buf), ty, buf, STAT_FG);
}

static void on_resize(struct gravity_app* a, int w, int h) {
    (void)a;
    win_w = w;
    win_h = h;
    stat_h = gravity_font_height() + 6;
    int ed_h = win_h - stat_h;
    if (ed_h < 1) ed_h = 1;
    gravity_editor_resize(&ed, MARGIN, 0, win_w - MARGIN, ed_h);
}

static void on_event(struct gravity_app* a, const struct gravity_event* ev) {
    (void)a;

    if (ev->type == GRV_EV_KEY) {
        long v = ev->a;
        if (v == CTRL('s')) {
            int n = gravity_editor_save(&ed);
            if (n >= 0) printf("edit: saved %s (%d bytes)\n",
                               gravity_editor_path(&ed), n);
            update_title();
            refresh();
            return;
        }
        if (v == CTRL('q')) { try_quit(); return; }

        int rev = gravity_editor_rev(&ed);
        gravity_editor_key(&ed, v, (int)ev->b);
        if (gravity_editor_rev(&ed) != rev) quit_asked = 0; // typing resumes
        update_title();
        refresh();
        return;
    }

    if (ev->type == GRV_EV_SCROLL) {
        gravity_editor_scroll(&ed, grv_scroll_delta(ev->b));
        refresh();
        return;
    }

    if (ev->type == GRV_EV_MOUSE) {
        gravity_editor_mouse(&ed, grv_mouse_x(ev->b), grv_mouse_y(ev->b),
                             (ev->a & GRV_BTN_LEFT) != 0);
        refresh();
        return;
    }
}

int main(int argc, char** argv) {
    const char* path = "/tmp/untitled.txt";
    int argi = 1;
    gravity_args(argc, argv, &win_w, &win_h, &argi);
    if (argi < argc && argv[argi] && argv[argi][0]) path = argv[argi];

    gravity_init(); // the font sets the status line's height
    stat_h = gravity_font_height() + 6;
    int ed_w = win_w - MARGIN;
    int ed_h = win_h - stat_h;
    if (ed_h < 1) ed_h = 1;

    if (gravity_editor_init(&ed, path, MARGIN, 0, ed_w, ed_h) != 0) {
        printf("edit: out of memory\n");
        return 1;
    }
    update_title();

    app.on_event = on_event;
    app.on_draw = on_draw;
    app.on_close = on_close;
    app.on_resize = on_resize;
    app.user = 0;

    printf("edit: %s (%d bytes)\n", gravity_editor_path(&ed),
           gravity_editor_len(&ed));
    gravity_run(&app, win_w, win_h);
    return 0;
}
