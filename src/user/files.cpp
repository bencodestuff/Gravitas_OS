#include <stdio.h>
#include <gravity/gravity.h>
#include <grv/syscall.h>
#include <grv/input.h>
#include <grv/image.h>

// files — a file browser built on Gravity.
//
// A path bar over a scrollable list (the gravity_list widget). Up/Down move
// the selection, Enter opens it (a directory is entered, a file opens in the
// editor, an image in the viewer), Backspace goes up, and clicking a
// directory enters it.

#define W        520
#define H        380
#define PATH_H   26
#define ROW_H    20
#define MAXITEMS 160
#define NAME_MAX 64

static struct gravity_app app;
static struct gravity_list list;

static char cwd[128] = "/";
static char listing[2048];
static char names[MAXITEMS][NAME_MAX];
static const char* items[MAXITEMS];
static int  nitems = 0;
static int  has_dotdot = 0;

static int is_dir(const char* s) {
    int n = 0;
    while (s[n]) n++;
    return n > 0 && s[n - 1] == '/';
}

static int name_cmp(const char* a, const char* b) {
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return (int)ca - (int)cb;
        a++; b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static int before(const char* a, const char* b) {
    int da = is_dir(a), db = is_dir(b);
    if (da != db) return da; // directories first
    return name_cmp(a, b) < 0;
}

static void sort_items(void) {
    int start = has_dotdot ? 1 : 0;
    for (int i = start + 1; i < nitems; i++) {
        const char* key = items[i];
        int j = i - 1;
        while (j >= start && before(key, items[j])) { items[j + 1] = items[j]; j--; }
        items[j + 1] = key;
    }
}

// Path of a child of the current directory (drops a trailing '/').
static void child_path(char* out, int max, const char* name) {
    int n = 0;
    while (cwd[n] && n < max - 1) { out[n] = cwd[n]; n++; }
    if (n == 0 || out[n - 1] != '/') out[n++] = '/';
    for (int i = 0; name[i] && name[i] != '/' && n < max - 1; i++) out[n++] = name[i];
    out[n] = '\0';
}

static void load_dir(const char* path) {
    if (grv_chdir(path) != 0) return; // unreadable: stay put
    grv_getcwd(cwd, sizeof(cwd));
    grv_readdir(".", listing, sizeof(listing));

    nitems = 0;
    has_dotdot = !(cwd[0] == '/' && cwd[1] == '\0');
    if (has_dotdot) {
        names[0][0] = '.'; names[0][1] = '.'; names[0][2] = '/'; names[0][3] = '\0';
        items[nitems++] = names[0];
    }
    char* p = listing;
    while (*p && nitems < MAXITEMS) {
        char* nl = p;
        while (*nl && *nl != '\n') nl++;
        int len = (int)(nl - p);
        if (len > 0 && len < NAME_MAX) {
            for (int i = 0; i < len; i++) names[nitems][i] = p[i];
            names[nitems][len] = '\0';
            items[nitems] = names[nitems];
            nitems++;
        }
        p = *nl ? nl + 1 : nl;
    }
    sort_items();
    gravity_list_set(&list, items, nitems);
}

static void up(void) {
    int n = 0;
    while (cwd[n]) n++;
    if (n <= 1) return; // at root
    while (n > 1 && cwd[n - 1] == '/') n--;
    int i = n - 1;
    while (i > 0 && cwd[i] != '/') i--;
    char parent[128];
    if (i == 0) { parent[0] = '/'; parent[1] = '\0'; }
    else { int k; for (k = 0; k < i; k++) parent[k] = cwd[k]; parent[k] = '\0'; }
    load_dir(parent);
}

static void enter(const char* name) {
    char path[160];
    child_path(path, sizeof(path), name);
    load_dir(path);
}

static void open_file(const char* name) {
    char path[160];
    child_path(path, sizeof(path), name);
    if (grv_image_supported(path)) {
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
    if (list.sel < 0 || list.sel >= nitems) return;
    if (has_dotdot && list.sel == 0) { up(); return; }
    const char* name = items[list.sel];
    if (is_dir(name)) enter(name);
    else open_file(name);
}

static void on_event(struct gravity_app* a, const struct gravity_event* ev) {
    if (ev->type == GRV_EV_KEY) {
        long v = ev->a;
        if (v == GRV_KEY_UP) { gravity_list_move(&list, -1); gravity_refresh(a); }
        else if (v == GRV_KEY_DOWN) { gravity_list_move(&list, 1); gravity_refresh(a); }
        else if (v == GRV_KEY_PAGEUP) { gravity_list_scroll(&list, -gravity_list_rows(&list)); gravity_refresh(a); }
        else if (v == GRV_KEY_PAGEDOWN) { gravity_list_scroll(&list, gravity_list_rows(&list)); gravity_refresh(a); }
        else if (v == '\n') { open_selected(); gravity_refresh(a); }
        else if (v == '\b') { up(); gravity_refresh(a); }
    } else if (ev->type == GRV_EV_SCROLL) {
        gravity_list_scroll(&list, -grv_scroll_delta(ev->b) * 3);
        gravity_refresh(a);
    } else if (ev->type == GRV_EV_MOUSE) {
        if (ev->a & GRV_BTN_LEFT) {
            int idx = gravity_list_hit(&list, grv_mouse_x(ev->b), grv_mouse_y(ev->b));
            if (idx >= 0) {
                int was_selected = (idx == list.sel);
                gravity_list_select(&list, idx);
                // A directory (or "..") opens on the first click; a file on
                // the second (click to select, click again to open).
                if (was_selected || (has_dotdot && idx == 0) || is_dir(items[idx]))
                    open_selected();
                gravity_refresh(a);
            }
        }
    }
}

static void on_draw(struct gravity_app* a) {
    struct gravity_canvas* c = &a->canvas;
    gravity_clear(c, 0xFF1E2430);
    gravity_fill(c, 0, 0, W, PATH_H, 0xFF2C3546);
    gravity_text(c, 6, (PATH_H - gravity_font_height()) / 2, cwd, 0xFFE6EEF6);
    gravity_list_draw(&list, c);
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    gravity_list_init(&list, 0, PATH_H, W, H - PATH_H, ROW_H,
                      0xFFD0DCE8, 0xFF1E2430, 0xFFFFFFFF, 0xFF35507A);
    load_dir("/");

    app.on_event = on_event;
    app.on_draw = on_draw;
    app.title = "Files";
    app.user = 0;
    gravity_run(&app, W, H);
    return 0;
}
