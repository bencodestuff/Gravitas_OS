#ifndef _ERRNO_H
#define _ERRNO_H 1

#include <sys/cdefs.h>

__BEGIN_DECLS

/* Syscalls report failure as a negated error code; the libc wrappers set
   `errno` from it and return the usual -1/NULL. A successful call never
   clears errno (check the return value, as always). */
extern int errno;

#define EPERM    1  /* operation not permitted              */
#define ENOENT   2  /* no such file or directory / task     */
#define EIO      5  /* I/O error                            */
#define EBADF    9  /* bad file descriptor                  */
#define EAGAIN  11  /* would block (mailbox full/empty)     */
#define ENOMEM  12  /* out of memory                        */
#define EACCES  13  /* permission denied                    */
#define EFAULT  14  /* bad address                          */
#define EEXIST  17  /* already exists                       */
#define ENOTDIR 20  /* not a directory                      */
#define EISDIR  21  /* is a directory                       */
#define EINVAL  22  /* invalid argument                     */
#define EMFILE  24  /* per-task fd table full               */
#define ENOSPC  28  /* no space left                        */
#define EROFS   30  /* read-only filesystem                 */
#define ENAMETOOLONG 36 /* a name or path too long to represent */
#define ENOSYS  38  /* no such syscall (retired/unknown id) */

__END_DECLS

#endif