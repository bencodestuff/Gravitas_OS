#ifndef _SETJMP_H
#define _SETJMP_H 1

#include <sys/cdefs.h>

__BEGIN_DECLS

/* Saved context: rbx, rbp, r12-r15, rsp, rip. */
typedef unsigned long jmp_buf[8];

/* Save the calling context and return 0. A later longjmp() to this buffer
   makes setjmp return its value (or 1 when that value is 0). */
int setjmp(jmp_buf env);

__attribute__((__noreturn__))
void longjmp(jmp_buf env, int val);

__END_DECLS

#endif