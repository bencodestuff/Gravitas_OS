#ifndef _GRV_WM_H
#define _GRV_WM_H

#include <sys/cdefs.h>

__BEGIN_DECLS

/* Userland window-manager protocol, carried over the ordinary IPC mailbox —
   the kernel is not involved, the WM is just another task.

   Discovery: the WM sends GRV_WM_HELLO to a new window's owner (its id is
   the message's 'from'), and the client answers with GRV_WM_SET_TITLE. A
   client that never answers keeps its task name as the title. */

#define GRV_WM_HELLO     0x10 /* WM -> client: a = surface id              */
#define GRV_WM_SET_TITLE 0x11 /* client -> WM: a, b = 8 title bytes each   */

/* Pack up to 16 title bytes into the two payload words (NUL-terminated, so
   a shorter title unpacks cleanly). */
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

static inline void grv_wm_title_unpack(unsigned long a, unsigned long b,
                                       char* out, int max) {
    int i = 0;
    for (; i < 8 && i < max - 1; i++) out[i] = (char)((a >> (i * 8)) & 0xFF);
    for (; i < 16 && i < max - 1; i++) out[i] = (char)((b >> ((i - 8) * 8)) & 0xFF);
    out[i] = '\0';
}

__END_DECLS

#endif
