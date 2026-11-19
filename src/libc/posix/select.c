/* select(2) on top of poll: the kernel has one readiness mechanism, so the
   second POSIX interface is a translation and not a second implementation.
   The translation is the whole point: fd_set bits in, poll entries out,
   revents back into bits, with the descriptor numbers kept in a side map. */

#include <errno.h>
#include <grv/syscall.h>
#include <sys/select.h>

int select(int nfds, fd_set* readfds, fd_set* writefds, fd_set* exceptfds,
           struct timeval* timeout) {
    if (nfds < 0 || nfds > FD_SETSIZE) {
        errno = EINVAL;
        return -1;
    }

    long wait_ms = -1; /* no timeval: wait for ever */
    if (timeout != 0) {
        if (timeout->tv_sec < 0 || timeout->tv_usec < 0 ||
            timeout->tv_usec >= 1000000L) {
            errno = EINVAL;
            return -1;
        }
        /* The kernel waits in milliseconds; round up so that a caller
           asking for 100 us does not get 0 (a one-shot check). */
        long usec = timeout->tv_usec;
        long ms = timeout->tv_sec * 1000L + (usec + 999L) / 1000L;
        wait_ms = ms > 0x7fffffffL ? 0x7fffffffL : ms;
    }

    struct grv_pollfd fds[FD_SETSIZE];
    int to_fd[FD_SETSIZE];
    int n = 0;
    for (int fd = 0; fd < nfds; fd++) {
        short events = 0;
        if (readfds != 0 && FD_ISSET(fd, readfds)) events |= GRV_POLLIN;
        if (writefds != 0 && FD_ISSET(fd, writefds)) events |= GRV_POLLOUT;
        if (exceptfds != 0 && FD_ISSET(fd, exceptfds)) events |= GRV_POLLERR;
        if (events == 0) continue;
        fds[n].fd = fd;
        fds[n].events = events;
        fds[n].revents = 0;
        to_fd[n] = fd;
        n++;
    }

    int got = grv_poll(fds, (unsigned long)n, wait_ms);
    if (got < 0) return -1; /* grv_poll has set errno */

    /* An entry that names a descriptor outside the table is EBADF, exactly
       as the poll contract reports POLLNVAL. The sets are cleared first so
       a caller that ignores the error cannot act on half a result. */
    for (int i = 0; i < n; i++) {
        if (fds[i].revents & GRV_POLLNVAL) {
            if (readfds) FD_ZERO(readfds);
            if (writefds) FD_ZERO(writefds);
            if (exceptfds) FD_ZERO(exceptfds);
            errno = EBADF;
            return -1;
        }
    }

    if (readfds) FD_ZERO(readfds);
    if (writefds) FD_ZERO(writefds);
    if (exceptfds) FD_ZERO(exceptfds);

    int ready = 0;
    for (int i = 0; i < n; i++) {
        short revents = fds[i].revents;
        int fd = to_fd[i];
        if (readfds != 0 && (revents & GRV_POLLIN)) {
            FD_SET(fd, readfds);
            ready++;
        }
        if (writefds != 0 && (revents & GRV_POLLOUT)) {
            FD_SET(fd, writefds);
            ready++;
        }
        if (exceptfds != 0 && (revents & (GRV_POLLERR | GRV_POLLHUP))) {
            FD_SET(fd, exceptfds);
            ready++;
        }
    }
    return ready;
}
