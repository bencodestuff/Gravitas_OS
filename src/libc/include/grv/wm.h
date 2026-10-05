#ifndef _GRV_WM_H
#define _GRV_WM_H

#include <grv/syscall.h>
#include <sys/cdefs.h>

__BEGIN_DECLS

/* Userland window-manager protocol, carried over the ordinary IPC mailbox —
   the kernel is not involved, the WM is just another task.

   Discovery: the WM sends GRV_WM_HELLO to a new window's owner (its id is
   the message's 'from'), and the client answers with GRV_WM_SET_TITLE. A
   client that never answers keeps its task name as the title. The title can
   be updated at any time (grv_wm_title_send), which also handles titles
   longer than 15 characters by following up with GRV_WM_SET_TITLE2.

   Closing: the WM asks (GRV_WM_CLOSE) instead of killing. A client that
   answers immediately just exits; one that needs time (a save prompt) sends
   GRV_WM_CLOSE_ACK first and the WM then waits indefinitely. A client that
   neither exits nor acks is force-killed after a grace period. */

#define GRV_WM_HELLO      0x10 /* WM -> client: a = surface id              */
#define GRV_WM_SET_TITLE  0x11 /* client -> WM: a, b = 16 title bytes       */
#define GRV_WM_SET_ROLE   0x12 /* client -> WM: a = surface id, b = role    */
#define GRV_WM_DISMISS    0x13 /* WM -> client: a = surface id (popup away) */
#define GRV_WM_CLOSE      0x14 /* WM -> client: a = surface id (please exit) */
#define GRV_WM_CLOSE_ACK  0x15 /* client -> WM: a = sid (handling the close) */
#define GRV_WM_SET_TITLE2 0x16 /* client -> WM: the title's next 16 bytes    */

#define GRV_WM_ROLE_NORMAL 0
#define GRV_WM_ROLE_POPUP  1 /* undecorated, on top, client-positioned      */

/* A window title is at most 31 characters. Pack the first 16 bytes of a
   title into two payload words (a shorter title is NUL-padded). */
static inline void grv_wm_title_pack(const char* s, unsigned long* a,
                                     unsigned long* b) {
    unsigned long x = 0, y = 0;
    for (int i = 0; i < 8; i++)
        x |= (unsigned long)(unsigned char)(s[i] ? s[i] : 0) << (i * 8);
    for (int i = 0; i < 8; i++)
        y |= (unsigned long)(unsigned char)(s[8 + i] ? s[8 + i] : 0) << (i * 8);
    *a = x;
    *b = y;
}

/* Unpack exactly 16 title bytes (no terminator). */
static inline void grv_wm_title_raw(unsigned long a, unsigned long b, char* out) {
    for (int i = 0; i < 8; i++) out[i] = (char)((a >> (i * 8)) & 0xFF);
    for (int i = 0; i < 8; i++) out[8 + i] = (char)((b >> (i * 8)) & 0xFF);
}

/* Unpack one 16-byte title message into 'out' (NUL-terminated, 'max' bytes). */
static inline void grv_wm_title_unpack(unsigned long a, unsigned long b,
                                       char* out, int max) {
    int i = 0;
    for (; i < 8 && i < max - 1; i++) out[i] = (char)((a >> (i * 8)) & 0xFF);
    for (; i < 16 && i < max - 1; i++) out[i] = (char)((b >> ((i - 8) * 8)) & 0xFF);
    out[i] = '\0';
}

/* Set the window title of 'wm_task'. Sends the second title message only
   when the title is long enough to need it. */
static inline void grv_wm_title_send(long wm_task, const char* s) {
    struct grv_msg m;
    m.from = 0;
    m.type = GRV_WM_SET_TITLE;
    grv_wm_title_pack(s, &m.a, &m.b);
    grv_send(wm_task, &m);

    if (s[16]) { // more title than one message carries
        m.type = GRV_WM_SET_TITLE2;
        grv_wm_title_pack(s + 16, &m.a, &m.b);
        grv_send(wm_task, &m);
    }
}

__END_DECLS

#endif