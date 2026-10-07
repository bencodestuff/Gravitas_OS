#include <stdio.h>
#include <grv/syscall.h>
#include <grv/wm.h>

// ide — the desktop composition, not a new monolith.
//
// It sizes the three panes from the screen, spawns the apps we already have
// (files, edit, term) with --geometry and --role, and asks the WM to place
// them: explorer left, editor centre, terminal bottom. The panes own their
// pixels and the WM owns their positions, so this program only decides the
// split — it draws nothing and exits as soon as the request is sent.

#define EXPLORER_W 300   /* the file tree's width */
#define TERMINAL_H 256   /* the terminal's height (16 rows of 16 px) */
#define GAP        8     /* between the editor and the terminal */

// The WM's frame, so the panes tile it exactly (see wm.cpp).
#define TITLE_H    18
#define TASKBAR_H  24
#define BORDER     2

// The terminal's grid snaps to whole 8x16 cells, so ask for exactly the pixels
// it will take: then the editor and the terminal share one flush column.
#define CELL_W     8
#define CELL_H     16
#define TERM_SB_W  6

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    struct grv_sysinfo si;
    grv_sysinfo(&si);
    int sw = (int)si.screen_w, sh = (int)si.screen_h;
    if (sw <= 0 || sh <= 0) { printf("ide: no framebuffer\n"); return 1; }

    int top = TITLE_H;
    int bottom = sh - TASKBAR_H;
    int ex_h = bottom - top;                      // the tree reaches the bottom
    int col_x = BORDER + EXPLORER_W + 2 * BORDER; // the editor/terminal column
    int col_w = sw - col_x - BORDER;

    int term_w = ((col_w - TERM_SB_W) / CELL_W) * CELL_W + TERM_SB_W;
    int term_h = (TERMINAL_H / CELL_H) * CELL_H;
    int ed_h = bottom - top - term_h - GAP;

    char gex[16], ged[16], gtm[16];
    snprintf(gex, sizeof(gex), "%dx%d", EXPLORER_W, ex_h);
    snprintf(ged, sizeof(ged), "%dx%d", term_w, ed_h);
    snprintf(gtm, sizeof(gtm), "%dx%d", term_w, term_h);

    const char* exv[] = { "files.elf", "--geometry", gex, "--role", "explorer" };
    const char* edv[] = { "edit.elf",  "--geometry", ged, "--role", "editor" };
    const char* tmv[] = { "term.elf",  "--geometry", gtm, "--role", "terminal" };

    long ex = grv_spawn_bg("files.elf", exv, 5);
    long ed = grv_spawn_bg("edit.elf", edv, 5);
    long tm = grv_spawn_bg("term.elf", tmv, 5);
    printf("ide: explorer %d, editor %d, terminal %d\n", (int)ex, (int)ed, (int)tm);

    long wm = grv_task_by_name("wm.elf");
    if (wm < 0) { printf("ide: no window manager\n"); return 1; }

    struct grv_msg m;
    m.from = 0;
    m.type = GRV_WM_WORKSPACE;
    m.a = GRV_WM_WS_IDE;
    m.b = 0;
    grv_send(wm, &m);

    // The panes are still starting, so the WM holds the request until each has
    // announced its role; there is nothing left for us to do.
    printf("ide: layout requested %dx%d, panes %dx%d + %dx%d\n",
           sw, sh, EXPLORER_W, ex_h, term_w, ed_h);
    return 0;
}
