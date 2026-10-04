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
#define GRV_SYSCALL_SPAWN_BG 21
#define GRV_SYSCALL_WAIT   22
#define GRV_SYSCALL_KILL   23
#define GRV_SYSCALL_SEND   24
#define GRV_SYSCALL_RECV   25
#define GRV_SYSCALL_OPEN   7
#define GRV_SYSCALL_READFD  8
#define GRV_SYSCALL_WRITEFD 9
#define GRV_SYSCALL_CLOSE  10
#define GRV_SYSCALL_LISTDIR 11
#define GRV_SYSCALL_CHDIR  12
#define GRV_SYSCALL_GETCWD 13
#define GRV_SYSCALL_SLEEP  14
#define GRV_SYSCALL_MOUSERD 15
#define GRV_SYSCALL_FBINFO 16
#define GRV_SYSCALL_TICKS  17
#define GRV_SYSCALL_SURFACE_CREATE 18
#define GRV_SYSCALL_SURFACE_COMMIT 19
#define GRV_SYSCALL_SURFACE_MOVE   20
#define GRV_SYSCALL_SURFACE_DESTROY 26
#define GRV_SYSCALL_SURFACE_Z      27
#define GRV_SYSCALL_SURFACE_LIST   28

/* Introspection (the debugger/monitor) */
#define GRV_SYSCALL_SYSINFO        29
#define GRV_SYSCALL_TASK_LIST      30
#define GRV_SYSCALL_FAULT_LAST     31
#define GRV_SYSCALL_WM_CLAIM       32
#define GRV_SYSCALL_INPUT_FOCUS    33
#define GRV_SYSCALL_INPUT_MODE     34
#define GRV_SYSCALL_CONSOLE_ATTACH 35
#define GRV_SYSCALL_CONSOLE_DETACH 36
#define GRV_SYSCALL_CONSOLE_READ   37
#define GRV_SYSCALL_CONSOLE_WRITE  38
#define GRV_SYSCALL_WM_HOTKEY      39
#define GRV_SYSCALL_PUTS           40
#define GRV_SYSCALL_LSEEK          41
#define GRV_SYSCALL_STAT           42
#define GRV_SYSCALL_MKDIR          43
#define GRV_SYSCALL_UNLINK         44
#define GRV_SYSCALL_FTRUNCATE      45
#define GRV_SYSCALL_TIMER_SET      46

/* Event 'type' values the kernel posts to the display authority's mailbox
   (from = -1). Mirrors Compositor::EVT_* in the kernel. Distinct from the
   input event types so the authority can receive both. */
#define GRV_EVT_SURFACE_ADDED   0x20
#define GRV_EVT_SURFACE_REMOVED 0x21

/* Kernel post to a console owner when the tasks it hosts have output
   waiting. Mirrors Input::EV_CONSOLE_OUT; the message carries no payload. */
#define GRV_EVT_CONSOLE_OUT     3

/* Open flags (subset) */
#define GRV_O_RDONLY 0
#define GRV_O_WRONLY 1
#define GRV_O_RDWR   2
#define GRV_O_CREATE 4
#define GRV_O_TRUNC  8

/* --- Convenience wrappers --- */

static inline void grv_putc(char c) {
    syscall1(GRV_SYSCALL_PUTC, (unsigned char)c);
}

/* Write a NUL-terminated string to the caller's console in one call. The
   kernel emits it atomically, so a task switch cannot interleave another
   task's output into the middle of it — printf uses this. */
static inline void grv_puts(const char* s) {
    syscall1(GRV_SYSCALL_PUTS, (long)s);
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

/* Spawn in the background: returns immediately with the child pid (blocks
   nothing). Child exit is observable via grv_wait(pid). */
static inline long grv_spawn_bg(const char* name, const char* const* argv, int argc) {
    return syscall3(GRV_SYSCALL_SPAWN_BG, (long)name, (long)argv, (long)argc);
}

/* Wait for the task to exit; returns its exit code, or -1 if it never
   existed or was already reaped (each pid can be waited on ONCE — like
   Unix waitpid once the child is reaped). */
static inline long grv_wait(long pid) {
    return syscall1(GRV_SYSCALL_WAIT, pid);
}

/* Forcibly terminate another task; its exit code becomes 'code'.
   Returns 0 or -1. */
static inline long grv_kill(long pid, long code) {
    return syscall2(GRV_SYSCALL_KILL, pid, code);
}

/* --- IPC (M3): per-task mailboxes --- */

struct grv_msg {
    unsigned long from;   /* kernel-stamped sender id (-1 = kernel) */
    unsigned long type;   /* free-form tag, meaning is caller's policy */
    unsigned long a;      /* payload word 1 */
    unsigned long b;      /* payload word 2 */
};

/* Send to task 'dst'. The kernel overwrites m->from with our pid.
   Returns 0 delivered, -1 no such/dead task, -2 mailbox full. */
static inline long grv_send(long dst, struct grv_msg* m) {
    return syscall2(GRV_SYSCALL_SEND, dst, (long)m);
}

/* Take the next message addressed to us. When 'block' is nonzero, sleep
   until one arrives. Returns 0 and fills m, or -1 (empty/none). */
static inline long grv_recv(struct grv_msg* m, long block) {
    return syscall2(GRV_SYSCALL_RECV, (long)m, block);
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

/* --- File positioning / metadata --- */
/* Seek an fd's offset. 'whence': 0=SET, 1=CUR, 2=END. Returns the new
   offset (clamped to the file), or -1. */
static inline long grv_lseek(int fd, long offset, int whence) {
    return syscall3(GRV_SYSCALL_LSEEK, (long)fd, offset, (long)whence);
}

struct grv_stat {
    unsigned long size;
    long          is_dir;
};

/* Metadata for a path. Returns 0 on success and fills *out, else -1. */
static inline int grv_stat(const char* path, struct grv_stat* out) {
    return (int)syscall2(GRV_SYSCALL_STAT, (long)path, (long)out);
}

/* Create a directory / remove a file, by path. 0 or -1 (read-only fs
   refuses). */
static inline int grv_mkdir(const char* path) {
    return (int)syscall1(GRV_SYSCALL_MKDIR, (long)path);
}
static inline int grv_unlink(const char* path) {
    return (int)syscall1(GRV_SYSCALL_UNLINK, (long)path);
}

/* Set an open file's length. Returns 0 or -1. */
static inline int grv_ftruncate(int fd, unsigned long size) {
    return (int)syscall2(GRV_SYSCALL_FTRUNCATE, (long)fd, (long)size);
}

/* chdir: change the process working directory (0 = ok, -1 = no such dir).
   getcwd: copies the CWD into buf, returns its length. */
static inline int grv_chdir(const char* path) {
    return (int)syscall1(GRV_SYSCALL_CHDIR, (long)path);
}

static inline long grv_getcwd(char* buf, unsigned long max) {
    return syscall2(GRV_SYSCALL_GETCWD, (long)buf, (long)max);
}

/* Sleep for ms milliseconds (blocked, not busy-waited). */
static inline void grv_sleep(unsigned long ms) {
    syscall1(GRV_SYSCALL_SLEEP, (long)ms);
}

/* --- Mouse (PS/2, relative) --- */
struct grv_mouse_packet {
    short dx, dy;         // movement since last packet
    unsigned char buttons; // bit0 left, bit1 right, bit2 middle
};

/* Drain one mouse packet. Returns 1 if a packet was read, 0 if none. */
static inline int grv_mouse_read(struct grv_mouse_packet* pkt) {
    return (int)syscall1(GRV_SYSCALL_MOUSERD, (long)pkt);
}

/* --- Framebuffer --- */
struct grv_fb_info {
    unsigned long width;
    unsigned long height;
    unsigned long pitch;    // bytes per row
    unsigned long bpp;
    unsigned long address;  // USERLAND virtual address of the pixels
    unsigned long size;     // total bytes
};

/* Map the framebuffer into this process and return its geometry.
   Returns 0 on success, -1 on failure. After success, write pixels
   directly to (address + y*pitch + x*4). */
static inline int grv_fb_info(struct grv_fb_info* info) {
    return (int)syscall1(GRV_SYSCALL_FBINFO, (long)info);
}

/* Timer ticks since boot (50Hz). For honest FPS: (t2-t1)*20 = ms elapsed. */
static inline unsigned long grv_ticks(void) {
    return (unsigned long)syscall1(GRV_SYSCALL_TICKS, 0);
}

/* --- Surfaces (compositor) ---
   The record the kernel fills at create and in list: identical layout on
   both sides of the syscall boundary. 'address' is the owner-space VA;
   x/y are screen position; z is the draw position (0 = bottom). */
struct grv_surface_info {
    unsigned long address;
    unsigned long width;
    unsigned long height;
    unsigned long id;
    unsigned long owner;
    long          x;
    long          y;
    unsigned long z;
};

static inline int grv_surface_create(unsigned long w, unsigned long h,
                                     struct grv_surface_info* out) {
    return (int)syscall3(GRV_SYSCALL_SURFACE_CREATE, (long)w, (long)h, (long)out);
}

static inline void grv_surface_commit(void) {
    syscall1(GRV_SYSCALL_SURFACE_COMMIT, 0);
}

/* Commit only a surface-local rectangle (0,0 = the surface's top-left).
   Lets a full-screen overlay repaint just what changed. */
struct grv_rect { long x, y, w, h; };
static inline void grv_surface_commit_rect(long x, long y, long w, long h) {
    struct grv_rect r = { x, y, w, h };
    syscall1(GRV_SYSCALL_SURFACE_COMMIT, (long)&r);
}

/* Commit one owned surface (by id) with a surface-local rectangle. Needed
   when a task owns more than one surface (e.g. a window and a popup). */
static inline void grv_surface_commit_rect_id(int id, long x, long y,
                                              long w, long h) {
    struct grv_rect r = { x, y, w, h };
    syscall2(GRV_SYSCALL_SURFACE_COMMIT, (long)&r, (long)id);
}

static inline void grv_surface_move(int id, int x, int y) {
    syscall3(GRV_SYSCALL_SURFACE_MOVE, (long)id, (long)x, (long)y);
}

/* Destroy an owned surface. 0 ok, -1 unknown/not ours. */
static inline int grv_surface_destroy(int id) {
    return (int)syscall1(GRV_SYSCALL_SURFACE_DESTROY, (long)id);
}

/* Restack an owned surface: pos < 0 = bottom, pos huge = top, otherwise
   the exact draw position. 0 ok, -1 unknown/not ours. */
static inline int grv_surface_z(int id, long pos) {
    return (int)syscall2(GRV_SYSCALL_SURFACE_Z, (long)id, pos);
}

static inline int grv_surface_raise(int id) {
    return grv_surface_z(id, 0x7FFFFFFF);
}

static inline int grv_surface_lower(int id) {
    return grv_surface_z(id, -1);
}

/* Copy up to 'max' records, bottom-to-top (index = z). Returns the count
   written, or -1 for a null buffer. */
static inline int grv_surface_list(struct grv_surface_info* out, long max) {
    return (int)syscall2(GRV_SYSCALL_SURFACE_LIST, (long)out, max);
}

/* --- Introspection (the debugger/monitor) --- */
struct grv_sysinfo {
    unsigned long ticks;
    unsigned long tasks;
    unsigned long mem_free;
    unsigned long mem_total;
    unsigned long heap_used;
    unsigned long heap_peak;
    unsigned long heap_total;
    unsigned long surfaces;
    unsigned long comp_presents;
    unsigned long comp_full;
    unsigned long comp_rects;
    unsigned long comp_pixels;
    unsigned long screen_w;
    unsigned long screen_h;
    unsigned long cursor_x;
    unsigned long cursor_y;
};

static inline void grv_sysinfo(struct grv_sysinfo* out) {
    syscall1(GRV_SYSCALL_SYSINFO, (long)out);
}

struct grv_taskinfo {
    int id;
    int state;          /* 0 ready, 1 sleeping, 2 running */
    unsigned long cr3;
    unsigned long brk;
    long waiting_on;    /* -1 = none */
    char name[16];
};

static inline int grv_task_list(struct grv_taskinfo* out, long max) {
    return (int)syscall2(GRV_SYSCALL_TASK_LIST, (long)out, max);
}

struct grv_fault_record {
    unsigned long valid;
    long task_id;
    char task_name[16];
    unsigned long vector;
    unsigned long error_code;
    unsigned long cr2;
    unsigned long rip, rsp, rflags, cs;
    unsigned long rax, rbx, rcx, rdx, rsi, rdi, rbp;
    unsigned long r8, r9, r10, r11, r12, r13, r14, r15;
};

static inline int grv_fault_last(struct grv_fault_record* out) {
    return (int)syscall1(GRV_SYSCALL_FAULT_LAST, (long)out);
}

/* --- Display authority (window-manager mechanisms) --- */
/* Claim the one display-authority role: the first claimant holds it, the
   same task may re-claim, another task gets -1. Returns 0 or -1. */
static inline int grv_wm_claim(void) {
    return (int)syscall1(GRV_SYSCALL_WM_CLAIM, 0);
}

/* Set the keyboard focus (authority only): only this task's grv_getc()
   receives keys; -1 clears focus (back to "any waiter"). Returns 0 or -1. */
static inline int grv_input_focus(long task_id) {
    return (int)syscall1(GRV_SYSCALL_INPUT_FOCUS, task_id);
}

/* --- Console endpoint (per-task stdio) --- */
/* Bind another task's console to this task: its output arrives in our
   console ring, and we inject its input with grv_console_write(). Children
   inherit the binding. This is how a userland terminal hosts a shell.
   attach/detach return 0 or -1. */
static inline int grv_console_attach(long task_id) {
    return (int)syscall1(GRV_SYSCALL_CONSOLE_ATTACH, task_id);
}

static inline int grv_console_detach(long task_id) {
    return (int)syscall1(GRV_SYSCALL_CONSOLE_DETACH, task_id);
}

/* Owner: drain pending output from the hosted tasks into buf. Non-blocking;
   returns the byte count (0 when nothing is pending). */
static inline long grv_console_read(char* buf, unsigned long max) {
    return syscall2(GRV_SYSCALL_CONSOLE_READ, (long)buf, (long)max);
}

/* Owner: inject input bytes into task 'task_id' (as if typed). Returns the
   bytes accepted (the ring may be full), or -1 if we are not its owner. */
static inline long grv_console_write(long task_id, const char* buf,
                                     unsigned long n) {
    return syscall3(GRV_SYSCALL_CONSOLE_WRITE, task_id, (long)buf, (long)n);
}

/* Authority only: reserve a modifier chord (e.g. GRV_MOD_ALT) so keys with
   it are delivered to us instead of the focused task — global hotkeys.
   0 clears it. Returns 0, or -1 if we are not the authority. */
static inline int grv_wm_hotkey(long mask) {
    return (int)syscall1(GRV_SYSCALL_WM_HOTKEY, mask);
}

__END_DECLS

#endif
