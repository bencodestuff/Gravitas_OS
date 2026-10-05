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
#define GRV_EV_TIMER 4  /* the task's one-shot timer elapsed          */
#define GRV_EV_SCROLL 5 /* m.a = surface id<<16, m.b = signed wheel   */

/* Opt a task's one-shot timer: after 'ms' the kernel posts GRV_EV_TIMER to
   its mailbox (re-arm for a repeating tick). 0 cancels it. */
static inline int grv_timer_set(unsigned long ms) {
    return (int)syscall1(GRV_SYSCALL_TIMER_SET, (long)ms);
}

/* Key values: printable ASCII as-is; special keys above ASCII. */
#define GRV_KEY_ESCAPE 0x100
#define GRV_KEY_UP     0x101
#define GRV_KEY_DOWN   0x102
#define GRV_KEY_LEFT   0x103
#define GRV_KEY_RIGHT  0x104
#define GRV_KEY_PAGEUP 0x105
#define GRV_KEY_PAGEDOWN 0x106
#define GRV_KEY_HOME   0x107
#define GRV_KEY_END    0x108
#define GRV_KEY_DELETE 0x109

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

/* A mouse event carries the target surface id in the high 16 bits of 'a',
   with the button mask (GRV_BTN_*) in the low bits. */
static inline int grv_mouse_surface(long a) { return (int)(a >> 16); }

/* A scroll event's signed wheel delta (m.b): positive = up/away from the
   user, negative = down/toward. */
static inline int grv_scroll_delta(long b) { return (int)b; }

__END_DECLS

#endif
