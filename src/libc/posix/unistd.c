#include <unistd.h>
#include <errno.h>
#include <grv/syscall.h>

// Descriptors 0/1/2 are the standard streams, and they are real kernel
// descriptors: a console by default, or whatever the shell redirected them to
// (a file, a pipe end). Every read and write therefore goes through the fd
// layer, which decides what the descriptor means — there is no console special
// case left here. A console read is line-oriented and echoes; that lives in the
// kernel's console descriptor.

ssize_t read(int fd, void* buf, size_t count) {
    return grv_read_fd(fd, buf, count);
}

ssize_t write(int fd, const void* buf, size_t count) {
    return grv_write_fd(fd, buf, count);
}

int close(int fd) {
    grv_close(fd);
    return 0;
}

int dup(int fd) { return grv_dup(fd); }
int dup2(int oldfd, int newfd) { return grv_dup2(oldfd, newfd); }
int pipe(int fds[2]) { return grv_pipe(fds); }

off_t lseek(int fd, off_t offset, int whence) {
    return grv_lseek(fd, offset, whence);   // a console/pipe is not seekable
}

int unlink(const char* path) { return grv_unlink(path); }
int rmdir(const char* path)  { return grv_unlink(path); }  // empty dirs only

int fsync(int fd) { return grv_fsync(fd); }
int sync(void)    { return grv_sync(); }

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
