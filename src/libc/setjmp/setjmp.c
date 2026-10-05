#include <setjmp.h>

/* --- setjmp/longjmp (x86_64 SysV) ---
   Slots: 0=rbx 1=rbp 2=r12 3=r13 4=r14 5=r15 6=rsp 7=rip.

   Both are naked so the stack pointer and return address are read exactly as
   the caller left them (a compiler prologue would move rsp first). */

__attribute__((naked, returns_twice))
int setjmp(jmp_buf env) {
    __asm__ volatile(
        "movq %rbx, 0(%rdi)\n\t"
        "movq %rbp, 8(%rdi)\n\t"
        "movq %r12, 16(%rdi)\n\t"
        "movq %r13, 24(%rdi)\n\t"
        "movq %r14, 32(%rdi)\n\t"
        "movq %r15, 40(%rdi)\n\t"
        "leaq 8(%rsp), %rax\n\t"      // rsp once this "call" has returned
        "movq %rax, 48(%rdi)\n\t"
        "movq (%rsp), %rax\n\t"       // the return address to resume at
        "movq %rax, 56(%rdi)\n\t"
        "xorl %eax, %eax\n\t"         // setjmp returns 0
        "ret\n\t");
}

__attribute__((naked, noreturn))
void longjmp(jmp_buf env, int val) {
    __asm__ volatile(
        "movq 48(%rdi), %rsp\n\t"
        "movq 56(%rdi), %rdx\n\t"
        "movq 0(%rdi), %rbx\n\t"
        "movq 8(%rdi), %rbp\n\t"
        "movq 16(%rdi), %r12\n\t"
        "movq 24(%rdi), %r13\n\t"
        "movq 32(%rdi), %r14\n\t"
        "movq 40(%rdi), %r15\n\t"
        "movl %esi, %eax\n\t"
        "testl %eax, %eax\n\t"
        "jne 1f\n\t"
        "movl $1, %eax\n\t"           // longjmp(env, 0) still returns 1
        "1:\n\t"
        "jmp *%rdx\n\t");
}