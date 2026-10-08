#include "stdio_io.h"

/* The real seam: a thin pass-through to the grv_ wrappers, which turn a
   negated-errno return into -1 + errno. */

int __stdio_open(const char* path, int flags) {
    return grv_open(path, flags);
}

long __stdio_read(int fd, void* buf, size_t n) {
    return grv_read_fd(fd, buf, n);
}

long __stdio_write(int fd, const void* buf, size_t n) {
    return grv_write_fd(fd, buf, n);
}

long __stdio_lseek(int fd, long off, int whence) {
    return grv_lseek(fd, off, whence);
}

int __stdio_close(int fd) {
    grv_close(fd);
    return 0;
}
