#ifndef _GRV_INPUT_H
#define _GRV_INPUT_H

#include <grv/syscall.h>
#include <sys/cdefs.h>

__BEGIN_DECLS

/* Input events.
   A task that calls grv_input_mode(1) receives keyboard events (and, via
   the WM, mouse events) as messages in its mailbox; read them with
   grv_recv(). Events use reserved msg 'type' values, so an app can mix
   input with its own IPC traffic. The console never opts in and keeps
   using grv_getc(). */

#define GRV_EV_KEY   1  /* m.a = key value, m.b = modifiers          */
#define GRV_EV_MOUSE 2  /* m.a = buttons,   m.b = (x<<32)|y (local)   */

/* Key values: printable ASCII as-is; special keys above ASCII. */
#define GRV_KEY_ESCAPE 0x100
#define GRV_KEY_UP     0x101
#define GRV_KEY_DOWN   0x102
#define GRV_KEY_LEFT   0x103
#define GRV_KEY_RIGHT  0x104

/* Modifier bits (GRV_EV_KEY's m.b). */
#define GRV_MOD_SHIFT 1
#define GRV_MOD_CTRL  2
#define GRV_MOD_ALT   4
#define GRV_MOD_CAPS  8

/* Mouse button bits (GRV_EV_MOUSE's m.a). */
#define GRV_BTN_LEFT   1
#define GRV_BTN_RIGHT  2
#define GRV_BTN_MIDDLE 4

/* Opt into (or out of) message-delivered input. Returns 0. */
static inline int grv_input_mode(int events) {
    return (int)syscall1(GRV_SYSCALL_INPUT_MODE, events ? 1 : 0);
}

/* Pack/unpack a mouse event's window-local coordinates (m.b). */
static inline long grv_mouse_pack(int x, int y) {
    return ((long)(unsigned int)x << 32) | (unsigned int)y;
}
static inline int grv_mouse_x(long b) { return (int)(b >> 32); }
static inline int grv_mouse_y(long b) { return (int)(unsigned int)b; }

__END_DECLS

#endif
