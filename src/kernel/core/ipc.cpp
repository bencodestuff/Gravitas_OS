#include "kernel/core/ipc.h"
#include "kernel/klib/klib.h"
#include "kernel/core/sched.h"


// IPC implementation — see ipc.h for the contract.
//
// Mailboxes live in a fixed table keyed by task id (owner field). Slots
// are claimed lazily (first send-to / recv-on auto-claims) and freed in
// task_gone(), so short-lived tasks never pin slots. Linear search is
// correct here: the table is tiny and every operation is O(slots).

namespace Ipc {

constexpr int MAX_BOXES = 16;   // concurrent mailboxes (>= max live tasks)
constexpr int BOX_CAP  = 16;    // messages buffered per mailbox

struct Box {
    // Default ctor: statically-stored boxes need owner=-1 (free slot) —
    // plain zero-init would make the table "owned" by kernel task #0.
    // The kernel's .init_array walk runs this at boot.
    Box() : owner(-1), head(0), count(0), waiter(-1) {}

    int owner;       // task id that owns this mailbox, -1 = free slot
    int head;        // next message to take out (ring index)
    int count;       // messages queued
    Msg ring[BOX_CAP];
    int waiter;      // task id blocked in recv (the owner; -1 = none)
};

static Box boxes[MAX_BOXES];
static const char* tag = "[Ipc] ";

static Box* find_box(int owner) {
    for (int i = 0; i < MAX_BOXES; i++) {
        if (boxes[i].owner == owner) return &boxes[i];
    }
    return nullptr;
}

// Get-or-create the box owned by 'owner'. nullptr if no free slot.
static Box* claim_box(int owner) {
    Box* b = find_box(owner);
    if (b != nullptr) return b;
    for (int i = 0; i < MAX_BOXES; i++) {
        if (boxes[i].owner == -1) {
            boxes[i].owner = owner;
            boxes[i].head = 0;
            boxes[i].count = 0;
            boxes[i].waiter = -1;
            return &boxes[i];
        }
    }
    return nullptr; // out of slots: 16 live-task mailboxes exceeded
}

int send(int dst, const Msg& m) {
    // Zero/negative ids are never userland tasks ('-1' would collide with
    // the kernel-post stamp, 0 is the kernel task itself).
    if (dst <= 0) return -1;
    if (!Sched::task_exists(dst)) return -1;
    Box* b = claim_box(dst);
    if (b == nullptr) return -1;
    if (b->count == BOX_CAP) return -2;

    Msg* slot = &b->ring[(b->head + b->count) % BOX_CAP];
    slot->from = m.from;
    slot->type = m.type;
    slot->a = m.a;
    slot->b = m.b;
    b->count++;

    // Wake a blocked receiver (one-shot: the mark is consumed here, the
    // task loops in recv and drains the rest itself).
    if (b->waiter >= 0) {
        Sched::wake_task(b->waiter);
        b->waiter = -1;
    }
    return 0;
}

int try_recv(Msg* out) {
    Box* b = find_box(Sched::current_task_id());
    if (b == nullptr || b->count == 0) return -1;

    Msg* slot = &b->ring[b->head];
    out->from = slot->from;
    out->type = slot->type;
    out->a = slot->a;
    out->b = slot->b;
    b->head = (b->head + 1) % BOX_CAP;
    b->count--;
    return 0;
}

void recv_wait() {
    // Caller holds cli and has just established the mailbox is empty.
    // Register as the box's one-and-only waiter, THEN sleep; a send that
    // lands after registration will wake us.
    Box* b = claim_box(Sched::current_task_id());
    if (b == nullptr) {
        // No slot left: report by leaving the task blocked-but-wakable at
        // the next timer tick instead of silently spinning. (Kernel
        // under-provisioning, not a userland error.)
        kprint(tag);
        kprint("no mailbox slot — sleeping on timer instead\n");
        Sched::sleep_current(2);
        return;
    }
    b->waiter = Sched::current_task_id();
    Sched::block_until_woken();
}

void post_kernel(int dst, uint64_t type, uint64_t a, uint64_t b) {
    Msg m;
    m.from = (uint64_t)-1; // kernel stamp: userland sees the truth
    m.type = type;
    m.a = a;
    m.b = b;
    send(dst, m);
}

void task_gone(int id) {
    Box* b = find_box(id);
    if (b == nullptr) return;
    if (b->count != 0) {
        kprint(tag);
        kprint("task died with pending messages (dropped): #");
        kprint_uint((uint64_t)id);
        kprint("\n");
    }
    b->owner = -1;
    b->waiter = -1;
    b->head = 0;
    b->count = 0;
}

} // namespace Ipc
