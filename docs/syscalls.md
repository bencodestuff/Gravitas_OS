# Gravitas syscall ABI

The single contract between userland and the kernel. Everything crosses
through `int $0x80`:

| register | role |
|---|---|
| `rax` | syscall number (in), return value (out) |
| `rdi` | argument 1 |
| `rsi` | argument 2 |
| `rdx` | argument 3 |

There are no other argument registers; calls needing more widen one
argument into a userland struct pointer. Userland wrappers live in
`src/libc/include/grv/syscall.h` (the structs there mirror the kernel's
layouts byte for byte); the kernel side is `src/arch/x86_64/idt.cpp`.

The kernel follows the project rule: **mechanisms only**. Syscalls move
bytes, map frames and schedule tasks. What a message `type` means, which
surface is a "window", and what is drawn are all userland policy.

## Calls

| # | name | args | returns |
|---|---|---|---|
| 0 | `PUTC` | `c` | — |
| 1 | `GETC` | — | key (blocks until one arrives) |
| 4 | `SPAWN` | `name, argv, argc` | child id (blocks until it exits) |
| 5 | `EXIT` | `code` | never returns |
| 6 | `BRK` | `addr` (0 = query) | new break, or `(void*)-1` |
| 7 | `OPEN` | `path, flags` | fd, or −1 |
| 8 | `READFD` | `fd, buf, count` | bytes read |
| 9 | `WRITEFD` | `fd, buf, count` | bytes written |
| 10 | `CLOSE` | `fd` | — |
| 11 | `LISTDIR` | `path, buf, max` | fills `buf` (`\n`-separated, NUL-terminated) |
| 12 | `CHDIR` | `path` | 0 or −1 |
| 13 | `GETCWD` | `buf, max` | fills `buf` |
| 14 | `SLEEP` | `ms` | — (woken by the timer) |
| 15 | `MOUSERD` | `pkt*` | 1 if a packet was drained, else 0 |
| 16 | `FBINFO` | `info*` | 0 on success (maps the framebuffer into the caller), −1 |
| 17 | `TICKS` | — | timer ticks since boot (50 Hz) |
| 18 | `SURFACE_CREATE` | `w, h, out*` | surface id, or −1 |
| 19 | `SURFACE_COMMIT` | — | 0 (repaints the caller's surfaces) |
| 20 | `SURFACE_MOVE` | `id, x, y` | — |
| 21 | `SPAWN_BG` | `name, argv, argc` | child id (returns immediately) |
| 22 | `WAIT` | `pid` | exit code (each id once) |
| 23 | `KILL` | `pid, code` | 0 or −1 |
| 24 | `SEND` | `dst, msg*` | 0 sent, −1 no/dead target, −2 mailbox full |
| 25 | `RECV` | `msg*, block` | 0 + `msg`, or −1 |
| 26 | `SURFACE_DESTROY` | `id` | 0 or −1 (owner-checked) |
| 27 | `SURFACE_Z` | `id, pos` | 0 or −1 (owner-checked) |
| 28 | `SURFACE_LIST` | `out*, max` | count written, or −1 |
| 29 | `SYSINFO` | `out*` | — |
| 30 | `TASK_LIST` | `out*, max` | count written |
| 31 | `FAULT_LAST` | `out*` | 1 if a fault was recorded, else 0 |

Numbers 2 and 3 are retired (the flat-tarfs list/read-by-name, superseded
by the VFS).

## Structures

Layouts are shared with `grv/syscall.h`; on x86_64 LP64 every field is 8
bytes unless noted.

### Message (`SEND` / `RECV`)

```
struct grv_msg {
    unsigned long from;   // kernel-stamped sender id (-1 = kernel post)
    unsigned long type;   // free-form tag; meaning is userland policy
    unsigned long a;      // payload word 1
    unsigned long b;      // payload word 2
};
```

### Framebuffer info (`FBINFO`)

```
struct grv_fb_info {
    unsigned long width;
    unsigned long height;
    unsigned long pitch;    // bytes per row
    unsigned long bpp;
    unsigned long address;  // userland VA of the pixels (mapped on success)
    unsigned long size;     // total bytes
};
```

### Surface record (`SURFACE_CREATE` out, `SURFACE_LIST` entries)

```
struct grv_surface_info {
    unsigned long address;  // owner-space VA of the pixels (output)
    unsigned long width;
    unsigned long height;
    unsigned long id;
    unsigned long owner;    // owning task id
    long          x;        // screen position (top-left; may be negative)
    long          y;
    unsigned long z;        // draw order, 0 = bottom
};
```

`SURFACE_CREATE` fills the record for the new surface (its pixels are at
`address`, `w*h*4` bytes, 32bpp, top byte = alpha). `SURFACE_LIST` copies
records bottom-to-top, so the array index equals `z`.

### System snapshot (`SYSINFO`)

```
struct grv_sysinfo {
    unsigned long ticks;
    unsigned long tasks;
    unsigned long mem_free;    // PMM free frames (4 KiB)
    unsigned long mem_total;
    unsigned long heap_used;   // kernel heap, bytes
    unsigned long heap_peak;
    unsigned long heap_total;
    unsigned long surfaces;
    unsigned long comp_presents;
    unsigned long comp_full;   // full-screen paints
    unsigned long comp_rects;  // rectangles painted
    unsigned long comp_pixels; // pixels written
};
```

### Task record (`TASK_LIST`)

```
struct grv_taskinfo {
    int           id;
    int           state;       // 0 ready, 1 sleeping, 2 running
    unsigned long cr3;
    unsigned long brk;
    long          waiting_on;  // task being waited on, or -1
    char          name[16];
};
```

### Crash record (`FAULT_LAST`)

```
struct grv_fault_record {
    unsigned long valid;
    long          task_id;
    char          task_name[16];
    unsigned long vector;      // CPU exception vector (e.g. 14 = #PF)
    unsigned long error_code;
    unsigned long cr2;         // meaningful for #PF
    unsigned long rip, rsp, rflags, cs;
    unsigned long rax, rbx, rcx, rdx, rsi, rdi, rbp;
    unsigned long r8, r9, r10, r11, r12, r13, r14, r15;
};
```

## Related kernel debug keys

- **F11** — put the console layer above the surfaces (and back), so kernel
  output is readable while a window owns the screen.
- **F12** — dump the task table and compositor counters to the console.
