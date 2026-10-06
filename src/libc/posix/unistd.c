#include <unistd.h>
#include <errno.h>
#include <grv/syscall.h>
#include "../stdio/stdio_io.h"   // the console read seam (it echoes)

// Descriptors 0/1/2 are the console (see unistd.h); every other fd is a VFS
// file, where the grv_ wrappers already report failure as -1 + errno.

ssize_t read(int fd, void* buf, size_t count) {
    if (fd == STDIN_FILENO) {
        // The console has no EOF, so this reads until the newline or the count
        // — a terminal's line discipline. __stdio_getc echoes the input.
        char* p = (char*)buf;
        for (size_t i = 0; i < count; i++) {
            p[i] = (char)__stdio_getc();
            if (p[i] == '\n') return (ssize_t)(i + 1);
        }
        return (ssize_t)count;
    }
    return grv_read_fd(fd, buf, count);
}

ssize_t write(int fd, const void* buf, size_t count) {
    if (fd == STDOUT_FILENO || fd == STDERR_FILENO) {
        const char* p = (const char*)buf;
        for (size_t i = 0; i < count; i++) grv_putc(p[i]);
        return (ssize_t)count;
    }
    return grv_write_fd(fd, buf, count);
}

int close(int fd) {
    if (fd == STDIN_FILENO || fd == STDOUT_FILENO || fd == STDERR_FILENO)
        return 0;                    // the console is not closable
    grv_close(fd);
    return 0;
}

off_t lseek(int fd, off_t offset, int whence) {
    if (fd >= 0 && fd <= 2) { errno = EINVAL; return -1; }  // not seekable
    return grv_lseek(fd, offset, whence);
}

int unlink(const char* path) { return grv_unlink(path); }
int rmdir(const char* path)  { return grv_unlink(path); }  // empty dirs only

int chdir(const char* path) { return grv_chdir(path); }

char* getcwd(char* buf, size_t size) {
    if (buf == 0 || size == 0) { errno = EINVAL; return 0; }
    grv_getcwd(buf, size);
    return buf;
}

int access(const char* path, int mode) {
    (void)mode;                      // existence only: no permissions
    struct grv_stat st;
    return grv_stat(path, &st);
}

unsigned int sleep(unsigned int seconds) {
    grv_sleep((unsigned long)seconds * 1000UL);
    return 0;
}

int usleep(unsigned int usec) {
    unsigned long ms = ((unsigned long)usec + 999UL) / 1000UL;
    if (ms == 0) ms = 1;             // the timer's resolution is 20 ms
    grv_sleep(ms);
    return 0;
}

void _exit(int status) {
    grv_exit(status);
}
