#include <stdio.h>
#include <grv/syscall.h>
#include "wm_internal.h"

// wm_menu — the "Apps" launcher popup, drawn above the taskbar. The list of
// programs comes from /apps ("prog.elf  Label" per line), so a new app needs
// no WM change — just an entry and its ELF in the tar. It falls back to a
// couple of defaults if /apps is missing.

AppItem apps[MAXAPPS];
int napps = 0;

static void add_app(const char* prog, const char* label) {
    if (napps >= MAXAPPS) return;
    int i = 0;
    while (label[i] && i < 23) { apps[napps].label[i] = label[i]; i++; }
    apps[napps].label[i] = '\0';
    i = 0;
    while (prog[i] && i < 31) { apps[napps].prog[i] = prog[i]; i++; }
    apps[napps].prog[i] = '\0';
    napps++;
}

void load_apps(void) {
    napps = 0;
    static char buf[1024];
    int fd = grv_open("/apps", GRV_O_RDONLY);
    long n = -1;
    if (fd >= 0) { n = grv_read_fd(fd, buf, sizeof(buf) - 1); grv_close(fd); }

    if (n > 0) {
        buf[n] = '\0';
        char* p = buf;
        while (*p && napps < MAXAPPS) {
            char* nl = p;
            while (*nl && *nl != '\n') nl++;
            char* q = p;
            while (*q == ' ' || *q == '\t') q++;
            if (*q && *q != '#') {
                char prog[32], label[24];
                int pi = 0;
                while (*q && *q != ' ' && *q != '\t' && pi < 31) prog[pi++] = *q++;
                prog[pi] = '\0';
                while (*q == ' ' || *q == '\t') q++;
                int li = 0;
                while (*q && *q != '\n' && li < 23) label[li++] = *q++;
                while (li > 0 && (label[li - 1] == ' ' || label[li - 1] == '\t')) li--;
                label[li] = '\0';
                if (pi && li) add_app(prog, label);
            }
            p = *nl ? nl + 1 : nl;
        }
    }

    if (napps == 0) { // no manifest: a minimal fallback
        add_app("term.elf", "Terminal");
        add_app("files.elf", "Files");
    }
}

int menu_open = 0;
int menu_hot = -1;   // hovered item, -1 = none

int menu_x() { return 4; }
int menu_y() { return (overlay_h - TASKBAR_H) - napps * MENU_ITEM_H; }

void draw_menu() {
    if (!menu_open) return;
    int fh = font_ok ? (int)font.height : 8;
    int x = menu_x(), y = menu_y();
    fill(x, y, MENU_W, napps * MENU_ITEM_H, MENU_BG);
    fill(x, y, MENU_W, 1, MENU_EDGE);
    for (int i = 0; i < napps; i++) {
        int iy = y + i * MENU_ITEM_H;
        if (i == menu_hot) fill(x + 1, iy + 1, MENU_W - 2, MENU_ITEM_H - 1, MENU_HOT);
        if (i > 0) fill(x + 6, iy, MENU_W - 12, 1, MENU_SEP);
        text(x + 9, iy + (MENU_ITEM_H - fh) / 2, apps[i].label, TITLE_FG);
    }
}

// The menu item under a point, or -1.
int menu_item_at(int cx, int cy) {
    if (!menu_open) return -1;
    int x = menu_x(), y = menu_y();
    if (cx < x || cx >= x + MENU_W || cy < y) return -1;
    int i = (cy - y) / MENU_ITEM_H;
    return (i >= 0 && i < napps) ? i : -1;
}

// Repaint just the menu's rectangle (open/close), restoring what it covered.
void redraw_menu() {
    int x = menu_x(), y = menu_y();
    redraw(x, y, x + MENU_W, y + napps * MENU_ITEM_H);
}

void launch(const char* prog) {
    const char* argv[1] = { prog };
    long pid = grv_spawn_bg(prog, argv, 1);
    printf("wm: launch '%s' -> %d\n", prog, (int)pid);
}
