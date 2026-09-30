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
/* 2, 3 retired (flat-tarfs list/read by name) — superseded by the VFS */
#define GRV_SYSCALL_SPAWN  4
#define GRV_SYSCALL_EXIT   5
#define GRV_SYSCALL_BRK    6
#define GRV_SYSCALL_OPEN   7
#define GRV_SYSCALL_READFD  8
#define GRV_SYSCALL_WRITEFD 9
#define GRV_SYSCALL_CLOSE  10
#define GRV_SYSCALL_LISTDIR 11
#define GRV_SYSCALL_CHDIR  12
#define GRV_SYSCALL_GETCWD 13

/* Open flags (subset) */
#define GRV_O_RDONLY 0
#define GRV_O_WRONLY 1
#define GRV_O_RDWR   2
#define GRV_O_CREATE 4

/* --- Convenience wrappers --- */

static inline void grv_putc(char c) {
    syscall1(GRV_SYSCALL_PUTC, (unsigned char)c);
}

static inline char grv_getc(void) {
    return (char)syscall1(GRV_SYSCALL_GETC, 0);
}

/* Fill buf with the entries of directory 'path', '\n'-separated,
   NUL-terminated. */
static inline void grv_readdir(const char* path, char* buf, unsigned long max) {
    syscall3(GRV_SYSCALL_LISTDIR, (long)path, (long)buf, (long)max);
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

/* --- File descriptors --- */

static inline int grv_open(const char* path, int flags) {
    return (int)syscall2(GRV_SYSCALL_OPEN, (long)path, (long)flags);
}

static inline long grv_read_fd(int fd, void* buf, unsigned long count) {
    return syscall3(GRV_SYSCALL_READFD, (long)fd, (long)buf, (long)count);
}

static inline long grv_write_fd(int fd, const void* buf, unsigned long count) {
    return syscall3(GRV_SYSCALL_WRITEFD, (long)fd, (long)buf, (long)count);
}

static inline void grv_close(int fd) {
    syscall1(GRV_SYSCALL_CLOSE, (long)fd);
}

/* chdir: change the process working directory (0 = ok, -1 = no such dir).
   getcwd: copies the CWD into buf, returns its length. */
static inline int grv_chdir(const char* path) {
    return (int)syscall1(GRV_SYSCALL_CHDIR, (long)path);
}

static inline long grv_getcwd(char* buf, unsigned long max) {
    return syscall2(GRV_SYSCALL_GETCWD, (long)buf, (long)max);
}

__END_DECLS

#endif
