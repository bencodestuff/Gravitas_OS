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

/* The kernel's VFS flags: O_APPEND makes every write land at the current end
   (fopen("a") passes it), and O_EXCL with O_CREAT refuses to open an existing
   file (fopen's "x"). */
#define O_APPEND GRV_O_APPEND
#define O_EXCL   GRV_O_EXCL

__BEGIN_DECLS

/* 'mode' (for O_CREAT) is accepted and ignored: there are no permissions. */
int open(const char* path, int flags, ...);
int creat(const char* path, mode_t mode);

__END_DECLS

#endif
