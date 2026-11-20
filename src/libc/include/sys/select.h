#ifndef _SYS_SELECT_H
#define _SYS_SELECT_H 1

#include <sys/cdefs.h>
#include <sys/time.h>

__BEGIN_DECLS

/* The kernel holds 16 descriptors (0-2 are the console), so a set never
   needs a bit for anything above 15. 'select' refuses 'nfds' above
   FD_SETSIZE rather than pretending to watch descriptors it cannot name. */
#define FD_SETSIZE 16

typedef struct {
    unsigned long __bits;
} fd_set;

#define FD_ZERO(set)      ((set)->__bits = 0UL)
#define FD_SET(fd, set)   ((set)->__bits |= 1UL << (fd))
#define FD_CLR(fd, set)   ((set)->__bits &= ~(1UL << (fd)))
#define FD_ISSET(fd, set) (((set)->__bits >> (fd)) & 1UL)

/* Waits until any descriptor in 'readfds', 'writefds' or 'exceptfds' is
   ready, or until 'timeout' passes (NULL waits for ever). Sets that are
   ready are left holding just those descriptors. Returns the number of
   ready bits, 0 on timeout, or -1 with EBADF (a descriptor is not open) or
   EINVAL (nfds out of range, or a timeout with a negative or >= 1000000
   microsecond part). Exceptional conditions are POLLERR and POLLHUP: a
   pipe whose last writer has closed is also readable, so a read set sees
   end-of-file without an except set. */
int select(int nfds, fd_set* readfds, fd_set* writefds, fd_set* exceptfds,
           struct timeval* timeout);

__END_DECLS

#endif
