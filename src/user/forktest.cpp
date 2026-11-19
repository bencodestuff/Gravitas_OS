// forktest: exercises fork and parent-owned wait, where a task comes back out
// of the syscall it was copied from instead of starting at a fresh entry point.
//
// The contract under test:
//   1. the child resumes at the instruction after the call, not somewhere in
//      the kernel, and sees 0; the parent sees the child's task id;
//   2. the child's user stack is the same stack at the same addresses, with
//      the same contents;
//   3. memory is private after the fork: a write in one side is invisible to
//      the other, and the parent's pages survive the child's exit;
//   4. the child is a separate task with a separate address space, and both
//      can be inside syscalls at the same time (separate kernel stacks);
//   5. open descriptors are shared with the parent (a pipe end written by the
//      child is read by the parent, and the offset is one description);
//   6. a child that exits before the wait keeps its status, and that status is
//      collected exactly once; a stranger's id cannot be waited for;
//   7. a fork is refused while a shared-memory region is mapped, before any
//      task is published, and works again once the mapping is gone;
//   8. ordinary spawn still works afterwards.

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <grv/syscall.h>

#define CHILD_EXIT_BASE 10
#define MAGIC_SHARED 0x1111UL
#define MAGIC_STACK  0x2222UL

static volatile unsigned long shared_word = MAGIC_SHARED;
static int failures = 0;

static void fail(const char* what) {
    printf("[forktest] FAIL %s\n", what);
    failures++;
}

// Which address spaces are alive for this program name. Used while the child
// is still runnable: the two tasks must be listed separately and must not
// share a CR3.
static int named_cr3s(unsigned long out[4], int cap) {
    struct grv_taskinfo tasks[32];
    int n = grv_task_list(tasks, 32);
    int found = 0;
    for (int i = 0; i < n && found < cap; ++i) {
        if (strcmp(tasks[i].name, "forktest.elf") == 0)
            out[found++] = tasks[i].cr3;
    }
    return found;
}

// Runs on the child's copy of the user stack. 'slot' points into the parent's
// main() frame, which the child inherits at the same address.
static void child_side(int round, unsigned long* slot) {
    if (shared_word != MAGIC_SHARED) {
        printf("[forktest] child %d: global is %lx, want %lx\n",
               round, shared_word, MAGIC_SHARED);
        fail("child did not inherit the global");
    }
    if (*slot != MAGIC_STACK) {
        printf("[forktest] child %d: stack slot is %lx, want %lx\n",
               round, *slot, MAGIC_STACK);
        fail("child did not inherit its user stack");
    }

    // Write both copies through the inherited addresses. The parent must still
    // see its own values afterwards.
    shared_word = 0x3333UL + (unsigned long)round;
    *slot = 0x4444UL + (unsigned long)round;

    // Sleep so the parent runs while this task is parked, and so the parent
    // can see both address spaces in the task list.
    printf("[forktest] child %d running\n", round);
    grv_sleep(150);
    printf("[forktest] child %d done\n", round);
    grv_exit(CHILD_EXIT_BASE + round);
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    unsigned long stack_word = MAGIC_STACK;
    unsigned long* slot = &stack_word;
    shared_word = MAGIC_SHARED;

    // --- fork, private memory, blocked wait ---
    long child = grv_fork();
    if (child < 0) {
        printf("[forktest] fork failed (%ld)\n", child);
        return 1;
    }
    if (child == 0) {
        printf("[forktest] child 0: resumed with rax=0\n");
        child_side(0, slot);
        return 1;   // not reached: child_side exits
    }

    printf("[forktest] parent: child is task %d\n", (int)child);
    if (shared_word != MAGIC_SHARED) fail("parent saw the child's write early");
    if (stack_word != MAGIC_STACK) fail("parent stack changed across fork");

    // Both tasks exist right now, each in its own address space.
    unsigned long cr3s[4];
    int found = named_cr3s(cr3s, 4);
    if (found != 2) {
        printf("[forktest] parent: %d tasks named forktest.elf, want 2\n", found);
        fail("child is not a separate listed task");
    } else if (cr3s[0] == cr3s[1]) {
        fail("child shares the parent's address space");
    }

    long code = grv_wait(child);
    if (code != CHILD_EXIT_BASE) {
        printf("[forktest] child exited %d, want %d\n", (int)code, CHILD_EXIT_BASE);
        fail("child exit status");
    }
    // The child wrote through its copy-on-write mapping and then exited; the
    // parent's pages must be unchanged and still mapped.
    if (shared_word != MAGIC_SHARED) fail("child's write reached the parent");
    if (*slot != MAGIC_STACK) fail("child's stack write reached the parent");
    if (shared_word != MAGIC_SHARED || *slot != MAGIC_STACK) {
        printf("[forktest] parent after child exit: global=%lx slot=%lx\n",
               shared_word, *slot);
    }

    // Status is collected exactly once, and a stranger's id is not ours.
    if (grv_wait(child) != -1) fail("a child could be waited for twice");
    if (grv_wait(9999) != -1) fail("wait on an unknown id");

    // --- a child that exits before the parent waits ---
    long quick = grv_fork();
    if (quick == 0) grv_exit(70);
    if (quick < 0) {
        printf("[forktest] fork for the early exit failed (%d)\n", (int)quick);
        fail("fork for the early exit");
    } else {
        grv_sleep(150);                 // let it exit and be cleaned up
        long c = grv_wait(quick);
        if (c != 70) {
            printf("[forktest] early child exited %d, want 70\n", (int)c);
            fail("status of a child that exited first");
        }
        if (grv_wait(quick) != -1) fail("early child waited for twice");
    }

    // --- descriptors survive the fork ---
    // The child writes into a pipe; the parent closes its own write end and
    // reads, so it sees the bytes and then EOF. That only works if the child
    // holds the same open description.
    int p[2];
    if (grv_pipe(p) != 0) {
        fail("could not create a pipe");
    } else {
        long c = grv_fork();
        if (c == 0) {
            grv_close(p[0]);
            long w = grv_write_fd(p[1], "AB", 2);
            grv_exit(w == 2 ? 40 : 41);
        }
        if (c < 0) {
            printf("[forktest] fork for the pipe failed (%d)\n", (int)c);
            fail("fork with a pipe open");
            grv_close(p[0]);
            grv_close(p[1]);
        } else {
            grv_close(p[1]);            // the child's copy keeps it open
            char buf[4] = {0, 0, 0, 0};
            long n = grv_read_fd(p[0], buf, 3);
            grv_close(p[0]);
            long cc = grv_wait(c);
            if (n != 2 || buf[0] != 'A' || buf[1] != 'B') {
                printf("[forktest] pipe read %d bytes (%d,%d), want 2 (A,B)\n",
                       (int)n, (int)buf[0], (int)buf[1]);
                fail("child did not inherit the descriptors");
            }
            if (cc != 40) {
                printf("[forktest] pipe child exited %d, want 40\n", (int)cc);
                fail("pipe child status");
            }
        }
    }

    // --- shared-memory mappings are refused, not silently unshared ---
    int region = grv_shm_create(4096);
    struct grv_map m;
    if (region <= 0 || grv_shm_map(region, &m) != 0) {
        printf("[forktest] could not map a shared region (%d)\n", region);
        fail("shared region setup");
    } else {
        long refused = grv_fork();
        if (refused == 0) {
            grv_exit(60);               // a child must not exist at all
        }
        if (refused >= 0) {
            fail("fork succeeded with a shared region mapped");
            grv_wait(refused);          // do not leave the stray child behind
        } else if (refused != -EOPNOTSUPP) {
            printf("[forktest] fork refused with %d, want %d\n",
                   (int)refused, -EOPNOTSUPP);
            fail("wrong refusal for a shared mapping");
        } else {
            unsigned long only[4];
            if (named_cr3s(only, 4) != 1)
                fail("a refused fork published a task");
        }
        grv_munmap(m.address);
        grv_shm_close(region);

        long again = grv_fork();
        if (again == 0) grv_exit(61);
        if (again < 0) {
            printf("[forktest] fork still refused after unmap (%d)\n",
                   (int)again);
            fail("refusal outlived the mapping");
        } else if (grv_wait(again) != 61) {
            fail("child after the mapping was dropped");
        }
    }

    // --- several forks from one task, each with a distinct id ---
    // All three children stay alive at once, so several copies of the same
    // program are inside syscalls on their own kernel stacks.
    long ids[3] = {0, 0, 0};
    for (int i = 0; i < 3; ++i) {
        long c = grv_fork();
        if (c == 0) {
            // Child: prove the same continuation works repeatedly, then exit
            // with a code unique to this round.
            grv_sleep(50);
            grv_exit(20 + i);
        }
        if (c < 0) {
            fail("extra fork failed");
            break;
        }
        ids[i] = c;
    }
    for (int i = 0; i < 3; ++i) {
        if (ids[i] <= 0) continue;
        for (int j = 0; j < i; ++j)
            if (ids[j] == ids[i]) fail("two forks returned the same task id");
        long c = grv_wait(ids[i]);
        if (c != 20 + i) {
            printf("[forktest] round %d child exited %d, want %d\n",
                   i, (int)c, 20 + i);
            fail("extra fork exit status");
        }
    }

    // Ordinary spawn must still work after all that: a fresh entry point on a
    // fresh stack, not a resumed trap frame.
    const char* sleeper_argv[] = {"sleeper"};
    long p2 = grv_spawn_bg("sleeper.elf", sleeper_argv, 1);
    if (p2 <= 0) {
        printf("[forktest] spawn_bg after fork failed (%d)\n", (int)p2);
        fail("spawn after fork");
    } else {
        long c = grv_wait(p2);
        if (c != 0) {
            printf("[forktest] spawned child exited %d, want 0\n", (int)c);
            fail("spawned child after fork");
        }
    }

    if (failures != 0) {
        printf("[forktest] %d check(s) failed\n", failures);
        return 1;
    }
    printf("[forktest] PASS: child returned from the same syscall, own stack, "
           "own address space, shared descriptors, parent-owned status, "
           "spawn intact\n");
    return 0;
}
