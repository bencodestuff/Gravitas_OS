#ifndef _FCNTL_H
#define _FCNTL_H 1

#include <sys/cdefs.h>
#include <sys/types.h>
#include <grv/syscall.h>   /* the open flags are the kernel's VFS flags */

#define O_RDONLY GRV_O_RDONLY
#define O_WRONLY GRV_O_WRONLY
#define O_RDWR   GRV_O_RDWR
#define O_CREAT  GRV_O_CREATE
#define O_TRUNC  GRV_O_TRUNC

/* Accepted for source compatibility; not implemented. fopen("a") emulates
   append by seeking to the end, and there is no exclusive-create. */
#define O_APPEND 0x100
#define O_EXCL   0x200

__BEGIN_DECLS

/* 'mode' (for O_CREAT) is accepted and ignored: there are no permissions. */
int open(const char* path, int flags, ...);
int creat(const char* path, mode_t mode);

__END_DECLS

#endif
