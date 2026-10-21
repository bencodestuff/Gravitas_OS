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

/* Descriptors 0/1/2 are the standard streams, and they are real descriptors:
   a console by default, or a file / pipe end when the shell redirects them with
   dup2. read() on the console is line-oriented and echoes; write() is
   unbuffered, like a terminal. Every other fd is a VFS file (or a pipe). */
ssize_t read(int fd, void* buf, size_t count);
ssize_t write(int fd, const void* buf, size_t count);
int     close(int fd);
off_t   lseek(int fd, off_t offset, int whence);

/* Descriptor plumbing. dup()/dup2() share an open file description; pipe()
   returns {read end, write end}. A new descriptor is never 0-2. */
int     dup(int fd);
int     dup2(int oldfd, int newfd);
int     pipe(int fds[2]);

int unlink(const char* path);
int rmdir(const char* path);      /* removes an empty directory */
int chdir(const char* path);
char* getcwd(char* buf, size_t size);
int access(const char* path, int mode);

/* Durability. fsync() flushes the filesystem holding fd; sync() flushes every
   mounted filesystem. Both return 0, or -1 with errno. */
int fsync(int fd);
int sync(void);

unsigned int sleep(unsigned int seconds);
int usleep(unsigned int usec);

__attribute__((__noreturn__)) void _exit(int status);

__END_DECLS

#endif
