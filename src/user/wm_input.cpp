#include <stdio.h>
#include <grv/syscall.h>
#include <grv/input.h>
#include <grv/wm.h>
#include "wm_internal.h"

// wm_input — pointer routing and the WM-driven drag/resize gestures. Clicks
// go to the launcher, then the taskbar, then the window under the pointer;
// motion during a title-bar drag or an edge resize is handled here, and
// everything else is forwarded to the owning client.

int   click_sid = -1;     // window that received the button-down (for release)
int   grab_owner = -1;    // task the pointer is captured to (drag inside a window)
int   dragging = 0;
int   drag_sid = -1;
int   drag_off_x = 0, drag_off_y = 0;

// An edge-drag resize in progress.
int   resizing = 0;
int   resize_sid = -1;
int   resize_edge = 0;                     // 1 = right, 2 = bottom, 3 = corner
int   resize_w0 = 0, resize_h0 = 0;        // size when the drag began
int   resize_px = 0, resize_py = 0;        // pointer when it began
int   resize_sent_w = 0, resize_sent_h = 0; // the last size asked for

// Send a mouse event to the task owning window 'w', in window-local
// coordinates. 'buttons' is the current button mask.
void forward_mouse(Win* w, int buttons, int cx, int cy) {
    struct grv_msg m;
    m.from = 0;
    m.type = GRV_EV_MOUSE;
    // High 16 bits: target surface id; low bits: button mask.
    m.a = ((unsigned long)w->sid << 16) | (unsigned long)(buttons & 0xFFFF);
    m.b = grv_mouse_pack(cx - w->x, cy - w->y);
    grv_send(w->owner, &m);
}

// Send a wheel-scroll event to the task owning window 'w'. 'delta' is the
// number of wheel notches (positive = up/away).
void forward_scroll(Win* w, int delta) {
    struct grv_msg m;
    m.from = 0;
    m.type = GRV_EV_SCROLL;
    m.a = ((unsigned long)w->sid << 16);
    m.b = (long)delta;
    grv_send(w->owner, &m);
}

// Top-most window whose client body contains the point, or 0.
Win* window_body_at(int cx, int cy) {
    for (int i = nwin - 1; i >= 0; i--) {
        if (in_body(&wins[i], cx, cy)) return &wins[i];
    }
    return 0;
}

// Popups: a click anywhere outside every popup dismisses them all.
static int popup_at(int cx, int cy) {
    for (int i = 0; i < nwin; i++)
        if (wins[i].role == GRV_WM_ROLE_POPUP && in_body(&wins[i], cx, cy))
            return i;
    return -1;
}

static void dismiss_popups() {
    for (int i = 0; i < nwin; i++) {
        if (wins[i].role != GRV_WM_ROLE_POPUP) continue;
        struct grv_msg m;
        m.from = 0;
        m.type = GRV_WM_DISMISS;
        m.a = (unsigned long)wins[i].sid;
        m.b = 0;
        grv_send(wins[i].owner, &m);
    }
}

void on_press(int cx, int cy) {
    // The launcher menu floats above everything and takes the click first.
    if (menu_open) {
        int mx = menu_x(), my = menu_y();
        if (cx >= mx && cx < mx + MENU_W &&
            cy >= my && cy < my + napps * MENU_ITEM_H) {
            int i = (cy - my) / MENU_ITEM_H;
            if (i >= 0 && i < napps) launch(apps[i].prog);
        }
        menu_open = 0;
        menu_hot = -1;
        redraw_menu();
        return;
    }

    // A click outside every popup dismisses the open popups (and is taken).
    {
        int has_popup = 0;
        for (int i = 0; i < nwin; i++)
            if (wins[i].role == GRV_WM_ROLE_POPUP) { has_popup = 1; break; }
        if (has_popup && popup_at(cx, cy) < 0) {
            dismiss_popups();
            return;
        }
    }

    // The taskbar is above every window: it takes the click next.
    if (cy >= overlay_h - TASKBAR_H) {
        taskbar_layout();
        for (int i = 0; i < ntbtn; i++) {
            const TBtn* b = &tbtns[i];
            if (cx >= b->x && cx < b->x + b->w) {
                if (b->kind == 0) { menu_open = 1; menu_hot = -1; redraw_menu(); }
                else focus_window(&wins[b->idx]);
                return;
            }
        }
        return; // the bar's background swallows the click
    }

    for (int i = nwin - 1; i >= 0; i--) {
        Win* w = &wins[i];
        if (in_close(w, cx, cy)) {
            // Ask the client to exit rather than killing it: it may need to
            // save. A client that ignores the ask is force-killed once the
            // grace period runs out.
            printf("wm: close #%d\n", w->sid);
            w->close_deadline = grv_ticks() + CLOSE_GRACE;
            struct grv_msg cm;
            cm.from = 0;
            cm.type = GRV_WM_CLOSE;
            cm.a = (unsigned long)w->sid;
            cm.b = 0;
            grv_send(w->owner, &cm);
            return;
        }
        if (w->role != GRV_WM_ROLE_POPUP && in_maxbox(w, cx, cy)) {
            maximize_toggle(w);
            return;
        }
        if (in_title(w, cx, cy)) {
            focus_window(w);
            dragging = 1;
            drag_sid = w->sid;
            drag_off_x = cx - w->x;
            drag_off_y = cy - w->y;
            return;
        }
        // A popup has no decoration; every other window can be resized from its
        // edge band (checked before the body, since the band straddles it).
        if (w->role != GRV_WM_ROLE_POPUP && in_resize_band(w, cx, cy)) {
            focus_window(w);
            resizing = 1;
            resize_sid = w->sid;
            int right = cx >= w->x + w->w - 2;
            int bottom = cy >= w->y + w->h - 2;
            resize_edge = (right && bottom) ? 3 : (right ? 1 : 2);
            resize_w0 = w->w;
            resize_h0 = w->h;
            resize_px = cx;
            resize_py = cy;
            resize_sent_w = w->w;
            resize_sent_h = w->h;
            printf("wm: resize #%d begin %dx%d\n", w->sid, w->w, w->h);
            return;
        }
        if (in_body(w, cx, cy)) {
            focus_window(w);
            forward_mouse(w, GRV_BTN_LEFT, cx, cy); // deliver the click
            click_sid = w->sid;
            grab_owner = w->owner; // capture the pointer for the drag
            return;
        }
    }
    if (focused != -1) { // desktop click: back to the console
        focus_console();
        full_redraw();
    }
}

void on_release(int cx, int cy) {
    if (resizing) {
        Win* w = find_sid(resize_sid);
        if (w != 0) {
            // One last request at the exact size: the drag asks in 8-pixel
            // steps, so without this the window could end up a few pixels off.
            int nw, nh;
            resize_wanted(cx, cy, &nw, &nh);
            request_resize(w, nw, nh);
            printf("wm: resize #%d %dx%d\n", w->sid, w->w, w->h);
        }
        resizing = 0;
        resize_sid = -1;
    }
    if (click_sid >= 0) {
        Win* w = find_sid(click_sid);
        if (w != 0) forward_mouse(w, 0, cx, cy);
        click_sid = -1;
    }
    grab_owner = -1; // pointer capture ends with the button
    dragging = 0;
    drag_sid = -1;
}

void drag_to(int cx, int cy) {
    Win* w = find_sid(drag_sid);
    if (w == 0) { on_release(cx, cy); return; }
    place_window(w, cx - drag_off_x, cy - drag_off_y);
}

void resize_to(int cx, int cy) {
    Win* w = find_sid(resize_sid);
    if (w == 0) { resizing = 0; return; }

    int nw, nh;
    resize_wanted(cx, cy, &nw, &nh);

    // Reallocating a surface per pixel would be silly: ask in steps (the
    // release sends the exact size).
    int dw = nw - resize_sent_w, dh = nh - resize_sent_h;
    if (dw < 0) dw = -dw;
    if (dh < 0) dh = -dh;
    if (dw < 8 && dh < 8) return;
    resize_sent_w = nw;
    resize_sent_h = nh;
    request_resize(w, nw, nh);
}
