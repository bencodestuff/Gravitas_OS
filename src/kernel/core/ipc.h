#pragma once
#include <stdint.h>

// IPC — per-task mailboxes (M3).
//
// Mechanism only, like everything in the kernel: a message is four words
// the kernel moves verbatim, and the kernel stamps the true sender id in
// 'from' so userland cannot spoof it. Semantics (what a 'type' means,
// which types exist, who may talk to whom) are userland policy.
//
// DELIVERY CONTRACT:
//   * send is non-blocking: 0 = delivered, or a negated errno:
//     -ENOENT for no such/dead target, -ENOMEM when all 16 mailbox slots are
//     claimed, -EAGAIN when the target's mailbox already holds 16 messages.
//   * messages arrive in send order (FIFO ring per task).
//   * nobody may recv from another task's mailbox (receive-if-owner).
//   * a task's mailbox dies with the task; queued-but-undelivered
//     messages are lost with it (the sender got no error at send time —
//     like a Unix pipe whose reader died between write and read).
//
// EXTENSION SEAMS (deliberately not built yet):
//   - blocking send on full mailbox (waiters-for-space, mirror of recv)
//   - kernel input posts: post_kernel(from=-1) is here so the WM-era
//     keyboard routing can deliver keys as messages to the focused task
//   - large payloads: pointer-words referencing shared memory (surfaces
//     already provide page-granular sharing), not in-kernel buffer copies

namespace Ipc {
    // Four words is the whole wire format. 'from' is kernel-stamped.
    struct Msg {
        uint64_t from;   // sender task id, or -1 for a kernel post
        uint64_t type;   // free-form tag; meaning is userland policy
        uint64_t a;      // payload word 1
        uint64_t b;      // payload word 2
    };

    // Try to deliver 'm' (kernel-side copy) to task 'dst'. Never blocks.
    // 0 delivered, or a negated errno:
    // -ENOENT unknown/dead target, -ENOMEM no free mailbox slot, -EAGAIN full.
    int send(int dst, const Msg& m);

    // Take the next message out of OUR task's mailbox. 0 + *out, or -1.
    int try_recv(Msg* out);

    // Mark the current task as waiting for a message and sleep until
    // something sends one. Caller must hold the check-and-block window
    // with cli (same discipline as block_on_keyboard); the send-side
    // wake races nothing because registration happens before yield.
    void recv_wait();

    // Kernel-internal post: no userland pointer is touched, so this is
    // callable from IRQ context later. 'from' is recorded as -1.
    void post_kernel(int dst, uint64_t type, uint64_t a, uint64_t b);

    // Drop the mailbox when a task dies. Called at the moment of teardown
    // (yield's zombie reap, kill_task) — NOT while the task still runs.
    void task_gone(int id);
}
