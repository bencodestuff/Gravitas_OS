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
| 19 | `SURFACE_COMMIT` | `rect*` (optional) | 0 (repaints the caller's surfaces, or only `rect` if given) |
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
| 32 | `WM_CLAIM` | — | 0 if the caller is/becomes the display authority, else −1 |
| 33 | `INPUT_FOCUS` | `task_id` | 0 or −1 (authority only); −1 clears focus |
| 34 | `INPUT_MODE` | `events` | 0 (opt into message-delivered input) |

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
    unsigned long screen_w;    // framebuffer size (for overlay creation)
    unsigned long screen_h;
    unsigned long cursor_x;    // pointer position (screen pixels)
    unsigned long cursor_y;
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

## Display authority and input

The mechanisms a window manager needs, without the kernel knowing what a
"window" is:

- **`WM_CLAIM` (32)** makes the caller the one **display authority**. The
  authority may `SURFACE_MOVE`/`SURFACE_Z` **any** surface (not just its
  own) — how a WM places, raises and drags windows it does not own. It also
  owns the mouse: once an authority exists, `MOUSERD` returns packets to it
  alone. The authority is cleared automatically if that task dies.
- **`INPUT_FOCUS` (33)**, authority-only, sets which task's `GETC` receives
  keys. `-1` clears focus (back to "whichever task waits"). Focus clears if
  the focused task dies. Clients use `GETC` unchanged.
- **Window lifecycle events**: when a non-authority task creates or loses a
  surface, the kernel posts a message to the authority's mailbox
  (`from == -1`) with `type` = `GRV_EVT_SURFACE_ADDED` /
  `GRV_EVT_SURFACE_REMOVED` and `a` = the surface id. The authority also
  scans `SURFACE_LIST` once at startup for pre-existing surfaces.
- **`SURFACE_COMMIT` with a rect** damages only that surface-local
  rectangle, so a full-screen overlay can commit just its change:

```
struct grv_rect { long x, y, w, h; };   // surface-local; top-left = 0,0
```

Closing a window is `KILL`ing its owner — the kernel's teardown removes the
surface (the authority cannot `SURFACE_DESTROY` another task's surface,
since that would unmap memory the owner still holds).

## Input events

A task that calls **`INPUT_MODE(1)`** receives input as messages in its
mailbox instead of the blocking `GETC` character ring. Read them with
`SEND`/`RECV`; the two event `type` values are reserved, so an app can mix
input with its own IPC traffic. The console never opts in, so `GETC` keeps
working for it.

| event | `type` | `a` | `b` |
|---|---|---|---|
| key | `GRV_EV_KEY` (1) | key value | modifier bits |
| mouse | `GRV_EV_MOUSE` (2) | button bits | `(x<<32) \| y`, window-local |

- **Key values**: printable ASCII as-is; specials above ASCII
  (`GRV_KEY_ESCAPE` 0x100, `GRV_KEY_UP/DOWN/LEFT/RIGHT` 0x101–0x104).
  Ctrl+letter arrives as its control code (^C = 3) with `GRV_MOD_CTRL` set.
- **Modifiers**: `GRV_MOD_SHIFT/CTRL/ALT/CAPS` (1/2/4/8).
- **Buttons**: `GRV_BTN_LEFT/RIGHT/MIDDLE` (1/2/4).

The keyboard IRQ delivers keys to the **focused** event-mode task. Mouse
events are produced by the WM (it alone has the pointer): it hit-tests the
window under the cursor and sends `GRV_EV_MOUSE` with coordinates relative
to that window's surface.

## Related kernel debug keys

- **F11** — put the console layer above the surfaces (and back), so kernel
  output is readable while a window owns the screen.
- **F12** — dump the task table and compositor counters to the console.
