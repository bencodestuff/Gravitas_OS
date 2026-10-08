#include <string.h>
#include <time.h>
#include <grv/syscall.h>
#include <grv/wm.h>
#include "wm_internal.h"

// wm_taskbar — the bar at the bottom of the screen: an "Apps" button (which
// opens the launcher in wm_menu.cpp), a button per open window, and the clock.
// It is drawn into the overlay, above every window. Layout is computed once
// into tbtns so drawing and hit-testing cannot drift apart.

TBtn tbtns[MAXWIN + 4];
int  ntbtn = 0;

static int slen(const char* s) { int n = 0; while (s[n]) n++; return n; }

void taskbar_layout() {
    ntbtn = 0;
    int x = 4;
    tbtns[ntbtn].x = x; tbtns[ntbtn].w = 8 * 4 + 16; // "Apps"
    tbtns[ntbtn].kind = 0; tbtns[ntbtn].idx = 0;
    ntbtn++;
    x += tbtns[0].w + 10;
    for (int i = 0; i < nwin && ntbtn < MAXWIN + 4; i++) {
        if (wins[i].role == GRV_WM_ROLE_POPUP) continue; // no button for popups
        int w = 8 * slen(wins[i].title) + 14;
        if (w < 84) w = 84;
        tbtns[ntbtn].x = x; tbtns[ntbtn].w = w;
        tbtns[ntbtn].kind = 1; tbtns[ntbtn].idx = i;
        ntbtn++;
        x += w + 6;
    }
}

// --- the taskbar clock -------------------------------------------------
// The main loop already wakes every 8 ms, so the clock is recomputed each pass
// and the bar repainted only when the displayed text changes.
char clock_str[8] = "";

int clock_refresh() {
    time_t now = time(0);
    struct tm* t = localtime(&now);
    char buf[8];
    if (strftime(buf, sizeof(buf), "%H:%M", t) == 0) return 0;
    if (strcmp(buf, clock_str) == 0) return 0;

    int i = 0;
    while (buf[i] && i < (int)sizeof(clock_str) - 1) { clock_str[i] = buf[i]; i++; }
    clock_str[i] = '\0';
    return 1;
}

void clock_redraw() {
    int y = overlay_h - TASKBAR_H;
    redraw(overlay_w - CLOCK_W - 10, y, overlay_w, y + TASKBAR_H);
}

void draw_taskbar() {
    int fh = font_ok ? (int)font.height : 8;
    int y = overlay_h - TASKBAR_H;
    fill(0, y, overlay_w, TASKBAR_H, BAR_BG);

    taskbar_layout();
    for (int i = 0; i < ntbtn; i++) {
        const TBtn* b = &tbtns[i];
        unsigned int bg;
        const char* label;
        if (b->kind == 0) {
            bg = LAUNCH_BG;
            label = "Apps";
        } else {
            bg = (wins[b->idx].owner == focused) ? FOCUS_BAR : UNFOCUS_BAR;
            label = wins[b->idx].title;
        }
        fill(b->x, y + 3, b->w, TASKBAR_H - 6, bg);
        text(b->x + 7, y + (TASKBAR_H - fh) / 2, label, TITLE_FG);
    }

    // The clock sits at the right end of the bar.
    if (clock_str[0])
        text(overlay_w - gravity_font_width(clock_str) - 8,
             y + (TASKBAR_H - fh) / 2, clock_str, TITLE_FG);
}
