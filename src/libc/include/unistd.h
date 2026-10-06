#ifndef _UNISTD_H
#define _UNISTD_H 1

#include <sys/cdefs.h>
#include <sys/types.h>
#include <stddef.h>

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

/* access() modes. Only existence is modelled: there are no permissions. */
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

__BEGIN_DECLS

/* Descriptors 0/1/2 are the console — there are no kernel descriptors 0/1/2
   (stdio models the console directly). read() on stdin is line-oriented and
   write() on stdout/stderr is unbuffered, like a terminal. Every other fd is a
   VFS file. */
ssize_t read(int fd, void* buf, size_t count);
ssize_t write(int fd, const void* buf, size_t count);
int     close(int fd);
off_t   lseek(int fd, off_t offset, int whence);

int unlink(const char* path);
int rmdir(const char* path);      /* removes an empty directory */
int chdir(const char* path);
char* getcwd(char* buf, size_t size);
int access(const char* path, int mode);

unsigned int sleep(unsigned int seconds);
int usleep(unsigned int usec);

__attribute__((__noreturn__)) void _exit(int status);

__END_DECLS

#endif
