#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <grv/syscall.h>

// pipeprobe — writes one short message to standard output after a pause, then
// reports on standard error what happened. It exists to pin down a descriptor
// inheritance rule: a pipeline stage must not receive the shell's copy of the
// read end of its own output pipe. If it does, the stage itself counts as a
// reader for ever, so a write with the real reader already gone is accepted
// (and a longer write would block on a full pipe) instead of failing with
// EPIPE. Run as `pipeprobe | <reader that exits>` and the pause lets the reader
// go first.
//
// Deliberately short: fewer bytes than the pipe buffer, so the correct outcome
// is an immediate EPIPE rather than a block.

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    grv_sleep(1000);

    long n = grv_write_fd(1, "ping", 4);

    char msg[64];
    if (n < 0)
        snprintf(msg, sizeof(msg), "pipeprobe: refused, errno %d\n", errno);
    else
        snprintf(msg, sizeof(msg), "pipeprobe: accepted %ld bytes\n", n);
    grv_write_fd(2, msg, (long)strlen(msg));
    return 0;
}
