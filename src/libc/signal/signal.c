#include <signal.h>
#include <stdlib.h>

// There is no userland signal delivery yet: the kernel's Ctrl+C interrupts the
// foreground job directly (default action), and there is no mechanism to run a
// handler in a task. Refuse a real handler honestly; accept the two special
// values, which mean "default" and "ignore".
sighandler_t signal(int sig, sighandler_t handler) {
    (void)sig;
    if (handler == SIG_DFL || handler == SIG_IGN) return SIG_DFL;
    return SIG_ERR;
}

// The default action of every signal we define is to terminate the task.
int raise(int sig) {
    exit(128 + sig);
}
