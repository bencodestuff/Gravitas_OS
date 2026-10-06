#ifndef _GRV_CLIPBOARD_H
#define _GRV_CLIPBOARD_H

#include <grv/syscall.h>
#include <sys/cdefs.h>

__BEGIN_DECLS

/* The clipboard: one text buffer in a file, so any app can copy and paste
   with no kernel mechanism. The path is a shared convention, and Ctrl+Shift+C
   / Ctrl+Shift+V is the convention for using it — the Shift is what tells the
   chord apart from Ctrl+C, the interrupt key. */

#define GRV_CLIPBOARD_PATH "/tmp/.clipboard"
#define GRV_CLIPBOARD_MAX  16384

/* Replace the clipboard with 'n' bytes of 'buf'. Returns 0, or -1 (errno). */
static inline int grv_clip_copy(const char* buf, int n) {
    int fd = grv_open(GRV_CLIPBOARD_PATH,
                      GRV_O_WRONLY | GRV_O_CREATE | GRV_O_TRUNC);
    if (fd < 0) return -1;
    long w = grv_write_fd(fd, buf, (unsigned long)n);
    grv_close(fd);
    return w == (long)n ? 0 : -1;
}

/* Fill 'buf' with up to 'max' bytes of the clipboard. Returns the length
   (0 when it is empty or missing). */
static inline int grv_clip_paste(char* buf, int max) {
    int fd = grv_open(GRV_CLIPBOARD_PATH, GRV_O_RDONLY);
    if (fd < 0) return 0;
    long n = grv_read_fd(fd, buf, (unsigned long)max);
    grv_close(fd);
    return n > 0 ? (int)n : 0;
}

__END_DECLS

#endif
