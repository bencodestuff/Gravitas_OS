#include <fcntl.h>
#include <grv/syscall.h>

// The POSIX open() spelling of the VFS open. The grv_ wrapper already sets
// errno and returns -1, which is what POSIX callers expect.
int open(const char* path, int flags, ...) {
    return grv_open(path, flags);   // the optional mode is ignored
}

int creat(const char* path, mode_t mode) {
    (void)mode;
    return open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
}
