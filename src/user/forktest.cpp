// forktest: exercises the child-return path, where a task comes back out of
// the syscall it was copied from, instead of starting at a fresh entry point.
//
// The contract under test:
//   1. the child resumes at the instruction after the call, not somewhere in
//      the kernel, and sees rax = 0; the parent sees the child's task id;
//   2. the child's user stack is the same stack at the same addresses, with
//      the same contents;
//   3. memory is private after the fork: a write in one side is invisible to
//      the other, and the parent's pages survive the child's exit;
//   4. the child is a separate task with a separate address space, and both
//      can be inside syscalls at the same time (separate kernel stacks);
//   5. ordinary spawn still works afterwards.
//
// The private syscall hook exists only in a kernel built with
// `configure.py --fork-test`. Without it the call reports ENOSYS and this
// program says so and exits 0, so it is harmless in an ordinary image.

#include <stdio.h>
#include <string.h>
#include <grv/syscall.h>

// Not a published syscall: a test-only hook in interrupt_dispatcher, compiled
// in only when the kernel is built with GRV_TEST_FORK.
#define FORK_HOOK 0xFFFF

#define CHILD_EXIT_BASE 10
#define MAGIC_SHARED 0x1111UL
#define MAGIC_STACK  0x2222UL

static volatile unsigned long shared_word = MAGIC_SHARED;
static int failures = 0;

static void fail(const char* what) {
    printf("[forktest] FAIL %s\n", what);
    failures++;
}

// The fork call. The child's copy of this returns 0; the parent's returns the
// new task's id, or a negative errno when the kernel has no hook built in.
static long fork_hook(void) {
    return syscall1(FORK_HOOK, 0);
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

    long child = fork_hook();
    if (child < 0) {
        // -ENOSYS: this kernel was built without the hook.
        printf("[forktest] no child-return hook in this kernel (%d); "
               "build with configure.py --fork-test\n", (int)child);
        return 0;
    }

    if (child == 0) {
        printf("[forktest] child %d: resumed with rax=0\n", 0);
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

    // More than one fork from the same task, each with a distinct id. All
    // three children stay alive at once, so several copies of the same
    // program are inside syscalls on their own kernel stacks.
    long ids[3];
    for (int i = 0; i < 3; ++i) {
        long c = fork_hook();
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
    long p = grv_spawn_bg("sleeper.elf", sleeper_argv, 1);
    if (p <= 0) {
        printf("[forktest] spawn_bg after fork failed (%d)\n", (int)p);
        fail("spawn after fork");
    } else {
        long c = grv_wait(p);
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
           "own address space, spawn intact\n");
    return 0;
}
