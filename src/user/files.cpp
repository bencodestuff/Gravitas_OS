#include <stdio.h>
#include <gravity/gravity.h>
#include <grv/syscall.h>
#include <grv/input.h>
#include <gravity/image.h>

// files — a standalone file browser: the filer widget (gravity_filer) owns the
// directory and the list widget shows it; this owns the window, the path bar
// and what "open" means — a directory is entered, a file opens in the editor,
// an image in the viewer.
//
// Up/Down move the selection, Enter opens it, Backspace goes up, and clicking
// selects (a second click, or a directory, opens).

#define DEF_W    520
#define DEF_H    380
#define PATH_H   26
#define ROW_H    20
#define MARGIN   6

static struct gravity_app app;
static struct gravity_filer filer;

static int win_w = DEF_W;   /* the window size (--geometry may change it) */
static int win_h = DEF_H;

static void open_file(const char* name) {
    char path[160];
    gravity_filer_path(&filer, path, sizeof(path), name);
    if (gravity_image_supported(path)) {
        const char* argv[2] = { "imageview.elf", path };
        grv_spawn_bg("imageview.elf", argv, 2);
    } else {
        // Text (or unknown): hand it to the editor.
        const char* argv[2] = { "edit.elf", path };
        grv_spawn_bg("edit.elf", argv, 2);
    }
    printf("files: open %s\n", path);
}

static void open_selected(void) {
    const char* name = gravity_filer_selected(&filer);
    if (name == 0) return;
    if (gravity_filer_enter(&filer)) return; // a directory (or "..")
    open_file(name);
}

static void on_event(struct gravity_app* a, const struct gravity_event* ev) {
    if (ev->type == GRV_EV_KEY) {
        long v = ev->a;
        if (v == GRV_KEY_UP) { gravity_list_move(&filer.list, -1); gravity_refresh(a); }
        else if (v == GRV_KEY_DOWN) { gravity_list_move(&filer.list, 1); gravity_refresh(a); }
        else if (v == GRV_KEY_PAGEUP) { gravity_list_scroll(&filer.list, -gravity_list_rows(&filer.list)); gravity_refresh(a); }
        else if (v == GRV_KEY_PAGEDOWN) { gravity_list_scroll(&filer.list, gravity_list_rows(&filer.list)); gravity_refresh(a); }
        else if (v == '\n') { open_selected(); gravity_refresh(a); }
        else if (v == '\b') { gravity_filer_up(&filer); gravity_refresh(a); }
    } else if (ev->type == GRV_EV_SCROLL) {
        gravity_list_scroll(&filer.list, -grv_scroll_delta(ev->b) * 3);
        gravity_refresh(a);
    } else if (ev->type == GRV_EV_MOUSE) {
        if (ev->a & GRV_BTN_LEFT) {
            int idx = gravity_list_hit(&filer.list, grv_mouse_x(ev->b),
                                       grv_mouse_y(ev->b));
            if (idx >= 0) {
                int was_selected = (idx == filer.list.sel);
                gravity_list_select(&filer.list, idx);
                // A directory (or "..") opens on the first click; a file on
                // the second (click to select, click again to open).
                if (was_selected || (filer.has_dotdot && idx == 0) ||
                    gravity_filer_is_dir(filer.items[idx]))
                    open_selected();
                gravity_refresh(a);
            }
        }
    }
}

static void on_draw(struct gravity_app* a) {
    struct gravity_canvas* c = &a->canvas;
    gravity_clear(c, 0xFF1E2430);
    gravity_fill(c, 0, 0, win_w, PATH_H, 0xFF2C3546);
    gravity_text(c, MARGIN, (PATH_H - gravity_font_height()) / 2, filer.cwd,
                 0xFFE6EEF6);
    gravity_list_draw(&filer.list, c);
}

int main(int argc, char** argv) {
    int argi = 1;
    gravity_args(argc, argv, &win_w, &win_h, &argi);
    (void)argi;

    gravity_filer_init(&filer, "/", 0, PATH_H, win_w, win_h - PATH_H, ROW_H);

    app.on_event = on_event;
    app.on_draw = on_draw;
    app.title = "Files";
    app.user = 0;
    gravity_run(&app, win_w, win_h);
    return 0;
}
