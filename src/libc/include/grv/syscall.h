#ifndef _GRV_SYSCALL_H
#define _GRV_SYSCALL_H

/* Gravitas syscall ABI — the single contract between userland and kernel.
   All syscalls use int 0x80. Registers:
     rax = syscall number
     rdi, rsi, rdx = arguments 1..3
     return value in rax
   Kernel side: src/arch/x86_64/idt.cpp interrupt_dispatcher(). */

#include <sys/cdefs.h>

__BEGIN_DECLS

static inline long syscall1(long n, long a) {
    long ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(n), "D"(a) : "memory");
    return ret;
}

static inline long syscall2(long n, long a, long b) {
    long ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(n), "D"(a), "S"(b) : "memory");
    return ret;
}

static inline long syscall3(long n, long a, long b, long c) {
    long ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory");
    return ret;
}

/* Syscall numbers */
#define GRV_SYSCALL_PUTC   0
#define GRV_SYSCALL_GETC   1
#define GRV_SYSCALL_LIST   2
#define GRV_SYSCALL_READ   3
#define GRV_SYSCALL_SPAWN  4
#define GRV_SYSCALL_EXIT   5
#define GRV_SYSCALL_BRK    6

/* --- Convenience wrappers --- */

static inline void grv_putc(char c) {
    syscall1(GRV_SYSCALL_PUTC, (unsigned char)c);
}

static inline char grv_getc(void) {
    return (char)syscall1(GRV_SYSCALL_GETC, 0);
}

/* Fill buf with tarfs entry names, '\n'-separated, NUL-terminated. */
static inline void grv_list_files(char* buf, unsigned long max) {
    syscall2(GRV_SYSCALL_LIST, (long)buf, (long)max);
}

/* Read a file from the tarfs. Returns bytes copied, or -1 if not found. */
static inline long grv_read_file(const char* name, char* buf, unsigned long max) {
    return syscall3(GRV_SYSCALL_READ, (long)name, (long)buf, (long)max);
}

/* Spawn a program from the tarfs (blocks until it exits).
   Returns child pid, or -1. */
static inline long grv_spawn(const char* name, const char* const* argv, int argc) {
    return syscall3(GRV_SYSCALL_SPAWN, (long)name, (long)argv, (long)argc);
}

static inline __attribute__((__noreturn__)) void grv_exit(int code) {
    syscall1(GRV_SYSCALL_EXIT, code);
    __builtin_unreachable();
}

/* Move the program break. brk(0) returns the current break.
   Returns the new break, or (void*)-1 on failure. */
static inline void* grv_brk(void* addr) {
    return (void*)syscall1(GRV_SYSCALL_BRK, (long)addr);
}

__END_DECLS

#endif
