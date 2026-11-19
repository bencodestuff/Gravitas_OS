// poll.cpp — waiting for a descriptor to be ready, without consuming it.
//
// A shell has to run a child and still notice a keypress on its own console:
// it needs to ask "is there input?" rather than block in a read. poll_fds is
// that question for the descriptors the kernel can answer without a blocking
// probe. It is a mechanism, not a policy: it reports the state of shared
// kernel objects and returns, and nothing is taken out of a ready descriptor,
// so a caller that sees POLLIN still finds its byte in the following read.
//
// Readiness sources:
//   pipe     the ring and the end counters (see pipe_readable/pipe_room)
//   file     always ready: a read or write on a file never waits
//   console  writable, never readable: testing the keyboard would take a key
//
// Waiting. Two independent events can end the wait — another task changing a
// pipe, and the deadline — and the scheduler can wake a task for one of them
// only: a task parked with block_until_woken is woken by pipe_wake and never by
// the timer (wake_timestamp stays -1), while a task sleeping on a timestamp is
// not woken by pipe_wake (Sched::wake_task refuses it). Parking would lose the
// deadline; arming the timer would lose the wake. So the loop naps for one
// tick and re-tests. The cost is one wake per tick while a poll is outstanding
// and at most one tick of extra latency, against a round-robin quantum that is
// the same 20 ms. The deadline is absolute and computed once, so a wake that
// finds nothing ready cannot extend it.

#include "fs/vfs.h"
#include "kernel/core/sched.h"
#include "kernel/core/timer.h"
#include "kernel/core/errno.h"

namespace VFS {

namespace {

// One entry's answer. ERR, HUP and NVAL are reported whatever 'events' asked
// for, as POSIX does: a caller that did not ask for the condition still needs
// to know the descriptor cannot be used.
short poll_one(FileHandle* h) {
    if (h == nullptr) return POLLNVAL;
    if (h->kind == FdKind::File) return (short)(POLLIN | POLLOUT); // never waits
    if (h->kind == FdKind::Console) return POLLOUT;                // writes never wait
    return pipe_poll(h);
}

// Wait one tick. Called with interrupts off; yield returns with them on, and
// the caller closes the region again.
//
// Interrupts must be on across the switch: the PIT is what advances the tick
// counter, and yield's wake loop spins until some task is READY, so a nap
// taken with IF=0 and every other task blocked would wait for a tick that can
// no longer arrive. sleep_current() only records the deadline, so closing the
// region for it is enough; a timer firing between it and yield simply switches
// away from a task that has already decided to sleep, which loses nothing.
void nap_one_tick() {
    Sched::sleep_current(1);
    asm volatile("sti");
    Sched::yield();
}

} // namespace

long poll_fds(PollFd* fds, uint64_t nfds, int64_t timeout_ms) {
    if (nfds > (uint64_t)POLL_MAX_FDS) return -Errno::EINVAL;
    if (timeout_ms < -1) return -Errno::EINVAL;
    if (nfds > 0 && fds == nullptr) return -Errno::EFAULT;

    FileHandle** table = Sched::current_task_fds();

    // Snapshot the set. The answer has to describe what the caller asked about
    // even if it writes to its array while it waits, and a descriptor number
    // outside the table can never be opened later in this call.
    int   fd[POLL_MAX_FDS];
    short events[POLL_MAX_FDS];
    for (uint64_t i = 0; i < nfds; i++) {
        fd[i] = fds[i].fd;
        events[i] = fds[i].events;
        if (fd[i] < 0 || fd[i] >= MAX_FDS) return -Errno::EINVAL;
    }

    // One absolute deadline for the whole call, in ticks. timeout_ms == 0
    // gives a deadline of "now", so the first pass tests and returns.
    const bool forever = timeout_ms < 0;
    uint64_t deadline = 0;
    if (!forever) {
        uint64_t ms = (uint64_t)timeout_ms;
        uint64_t ticks = (ms + Timer::TICK_MS - 1) / Timer::TICK_MS;
        uint64_t now = Timer::get_ticks();
        deadline = (ticks > UINT64_MAX - now) ? UINT64_MAX : now + ticks;
    }

    while (true) {
        // The test and the nap are one interrupt-off region. A ring change
        // cannot land between "nothing ready" and the sleep, which is the
        // missed-wake case the pipe waits themselves guard with cli.
        asm volatile("cli");
        long ready = 0;
        for (uint64_t i = 0; i < nfds; i++) {
            short re = poll_one(table[fd[i]]);
            fds[i].revents = re;
            if (re & (events[i] | POLLERR | POLLHUP | POLLNVAL)) ready++;
        }
        if (ready > 0) return ready;
        if (timeout_ms == 0) return 0;
        if (!forever && Timer::get_ticks() >= deadline) return 0;
        nap_one_tick();
    }
}

} // namespace VFS
