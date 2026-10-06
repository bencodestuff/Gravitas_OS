#ifndef _SIGNAL_H
#define _SIGNAL_H 1

#include <sys/cdefs.h>

__BEGIN_DECLS

typedef void (*sighandler_t)(int);

/* The C standard requires <signal.h> to define this. */
typedef int sig_atomic_t;

#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
#define SIG_ERR ((sighandler_t)-1)

#define SIGHUP  1
#define SIGINT  2
#define SIGQUIT 3
#define SIGILL  4
#define SIGABRT 6
#define SIGFPE  8
#define SIGKILL 9
#define SIGSEGV 11
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15

/* Install a handler for 'sig'. There is no userland signal delivery yet, so a
   real handler is refused (SIG_ERR); SIG_DFL and SIG_IGN are accepted. The
   kernel's Ctrl+C interrupt is a default-action terminate and does not consult
   this table. */
sighandler_t signal(int sig, sighandler_t handler);

/* Raise a signal in the current task. Every signal we define has the default
   action "terminate", so this does not return (exit code 128 + sig). */
int raise(int sig);

__END_DECLS

#endif
