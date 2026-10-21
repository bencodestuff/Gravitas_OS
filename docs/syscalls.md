# Gravitas syscall ABI

The single contract between userland and the kernel. Everything crosses
through `int $0x80`:

| register | role |
|---|---|
| `rax` | syscall number (in), return value (out) |
| `rdi` | argument 1 |
| `rsi` | argument 2 |
| `rdx` | argument 3 |
| `r10` | argument 4 (only `SURFACE_RESIZE` uses it) |

Calls needing more arguments than the registers hold widen one into a
userland struct pointer. Userland wrappers live in
`src/libc/include/grv/syscall.h` (the structs there mirror the kernel's
layouts byte for byte); the kernel side is `src/arch/x86_64/idt.cpp`.

The kernel follows the project rule: **mechanisms only**. Syscalls move
bytes, map frames and schedule tasks. What a message `type` means, which
surface is a "window", and what is drawn are all userland policy.

## Errors

A syscall reports failure by returning a **negated errno**. The C wrappers in
`grv/syscall.h` turn that into `-1`/`NULL` and set the global `errno`, so a
program uses the usual C idiom. The complete set lives in `<errno.h>`
(userland) and `src/kernel/core/errno.h` (kernel); the two must match this
table.

| code | value | meaning |
|---|---|---|
| `EPERM` | 1 | operation not permitted |
| `ENOENT` | 2 | no such file/directory, or no such task (IPC) |
| `EIO` | 5 | backend failure with no better code |
| `EBADF` | 9 | bad file descriptor |
| `EAGAIN` | 11 | would block (mailbox full, or an empty non-blocking poll) |
| `ENOMEM` | 12 | out of memory |
| `EACCES` | 13 | not readable with this handle |
| `EFAULT` | 14 | bad address |
| `EEXIST` | 17 | already exists |
| `ENOTDIR` | 20 | not a directory |
| `EISDIR` | 21 | is a directory |
| `EINVAL` | 22 | invalid argument |
| `EMFILE` | 24 | per-task fd table full |
| `ENOSPC` | 28 | no space left |
| `EROFS` | 30 | read-only filesystem |
| `EPIPE` | 32 | write to a pipe whose reader has gone |
| `ENOSYS` | 38 | no such syscall (a retired hole or an unknown number) |

A successful call never clears `errno` (check the return value, as in C).
Not every syscall distinguishes every reason yet: the filesystem backends
report 0/−1, so the VFS maps their failures to the closest code above.

## Bad addresses

Every pointer and length argument is checked at the boundary, before the
kernel touches it, against the calling task's page tables. A syscall rejects
a null pointer, an address at or above the user/kernel split
(`0x0000800000000000`), a range that wraps or crosses the split, and a range
whose pages are not mapped (or not writable, for an output buffer) with
`-EFAULT`. The whole `[ptr, ptr + len)` range is checked page by page, so a
buffer that starts in one page and ends in an unmapped one is refused instead
of faulting in ring 0 (where the fault contract halts the machine). Strings are
scanned to their terminator inside the VFS's path limit, and an argv array and
each string it points at are checked before a spawn copies them. Optional
output pointers (a null struct pointer meaning "just do the work") keep
working; a bad non-null one is `-EFAULT`.

A write count above the per-call cap (`16 MB`, see
`VFS::MAX_WRITE_BYTES`) is `-EINVAL`, not `-EFAULT`: such a count is an
impossible request rather than a bad buffer, and it is refused before the page
walk. A pointer or range that is genuinely bad or unmapped within the cap stays
`-EFAULT`.

## Paths

A path is absolute (`/Pictures/x.bmp`) or relative to the task's working
directory, which children inherit when they are spawned. A **bare name** (no
`/`) is a lookup convenience for existing files: `OPEN` and `STAT` try the
CWD first and then each program directory on the search path — `/bin`, then
`/bin/tests` for the test suite (`run ls` finds `ls.elf`).
`MKDIR`, `UNLINK`, `RENAME` and `OPEN` with `O_CREATE` never search: they
resolve against the CWD (or an absolute path) only, so a typo cannot quietly
create or delete something under a program directory.

`OPEN`'s flags are the VFS subset: `O_CREATE` creates a missing file, `O_TRUNC`
empties one that exists, `O_APPEND` makes every write land at the current end
(so two appenders do not overwrite each other), and `O_EXCL` with `O_CREATE`
refuses a file that is already there — the safe create, `EEXIST` rather than a
truncate. `O_EXCL` without `O_CREATE` has no effect.

The root is an **overlay**: the tar image is the read-only lower layer and a
ramfs is the writable upper layer over it, so the whole tree is writable. A
write to an image file is copied up first (or shadowed by `O_TRUNC`), the
listing is the union of both layers, and deleting an image file leaves a
whiteout marker (`.wh.<name>`) in the upper rather than touching the image.

## Environment

Every program starts with an environment: the kernel reads `/env` at boot
(`NAME=VALUE` per line) and copies it onto each new task's stack, right after
`argv` — the System V layout, so `crt0` points `environ` at `argv + argc + 1`
and `getenv` just walks it. It is a file rather than a kernel constant, so what a
program starts with is something you can read and change; `LUA_PATH` (where Lua
looks for modules) is the first thing to use it. There is no `setenv`: nothing
writes the block after the kernel builds it.

## Block devices

The storage direction, below the VFS: a filesystem should not care whether its
bytes come from RAM, a Limine module or a real disk, so everything above talks in
512-byte sectors through `Block::Device` (`src/kernel/core/block.h`) — a name, a
size, and read/write of a sector run. It is a registry and a contract, not a
cache. A request that runs past the end moves nothing, in either direction,
because a caller asking past the end is broken and a short read would hide it.

There is no syscall here; the layer is internal. The first device is a
**RAM disk** (`src/kernel/dev/memblock.cpp`): a heap buffer with the contract in
front of it, optionally seeded from a host-made image, which is how a filesystem
can be written and tested before any driver exists. It passes its own
read/write/bounds self-test at boot.

There is one disk image, and it is writable. `disk.img` is made on the host with
`mke2fs` (`tools/mkext2.py`) and reaches the guest two ways: as a Limine module
(alongside `files.tar`, see `src/kernel/dev/modules.h`) copied into a RAM disk,
or as a real drive the kernel drives over **ATA PIO** (`src/kernel/dev/ata.cpp`)
or **virtio-blk**. The kernel prefers virtio, then a real ATA drive, then the
module, so the guest boots either way, and the mount above cannot tell which it
got. It is then mounted read-write as **ext2** at `/disk`
(`src/kernel/dev/ext2.cpp`).

The writer allocates and frees blocks and inodes through the group bitmaps,
grows and truncates files across the direct and indirect pointers, and keeps
directory records tidy. `FSYNC` flushes one descriptor's filesystem and `SYNC`
flushes every mount, which is also what `poweroff` and `reboot` do before the
machine stops; on a virtio drive that means a real `FLUSH` request when the host
offers the feature. It does not model symlinks, extents, extended attributes or
64-bit filesystems, and refuses a filesystem that would need them or that was
not cleanly unmounted.

The ATA driver is polled PIO on the primary channel's legacy task file: no
interrupts, and a spin cap on every status poll so a device that never answers
cannot hang the kernel. It does not use the secondary channel, and it does not
fall back to CHS — a drive that cannot do LBA is reported unusable rather than
driven wrongly. One thing worth knowing: a controller that is AHCI only (QEMU's
q35, for instance) has no legacy task file at all, so the test harness adds a
`piix4-ide` device for the driver to talk to.

## Message types

`SEND`/`RECV` carry a free-form `type`; the kernel only reserves the values
it posts itself, and everything in between is userland policy. The registry:

| range | owner | values |
|---|---|---|
| `0x01–0x0F` | kernel input/console | `EV_KEY` 1, `EV_MOUSE` 2, `EV_CONSOLE_OUT` 3, `EV_TIMER` 4, `EV_SCROLL` 5 |
| `0x10–0x1F` | WM protocol (`grv/wm.h`) | `HELLO` 0x10, `SET_TITLE` 0x11, `SET_ROLE` 0x12, `DISMISS` 0x13, `CLOSE` 0x14, `CLOSE_ACK` 0x15, `SET_TITLE2` 0x16 |
| `0x20–0x2F` | kernel surface lifecycle | `EVT_SURFACE_ADDED` 0x20, `EVT_SURFACE_REMOVED` 0x21 |
| `0x100+` | applications | anything (no registry) |

Add new platform messages inside the range they belong to; never reuse a
value with a different meaning.

## Calls

| # | name | args | returns |
|---|---|---|---|
| 0 | `PUTC` | `c` | — |
| 1 | `GETC` | — | key (blocks until one arrives) |
| 4 | `SPAWN` | `name, argv, argc` | child id (blocks until it exits) |
| 5 | `EXIT` | `code` | never returns |
| 6 | `BRK` | `addr` (0 = query) | new break, or `(void*)-1` |
| 7 | `OPEN` | `path, flags` | fd (≥ 3; 0–2 are the standard streams), or −1 |
| 8 | `READFD` | `fd, buf, count` | bytes read |
| 9 | `WRITEFD` | `fd, buf, count` | bytes written |
| 10 | `CLOSE` | `fd` | — |
| 11 | `LISTDIR` | `path, ents*, max` | entries written (typed: name, `DT_*` type, inode), or −1 |
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
| 35 | `CONSOLE_ATTACH` | `task_id` | 0 or −1 (become its console owner) |
| 36 | `CONSOLE_DETACH` | `task_id` | 0 or −1 (owner only) |
| 37 | `CONSOLE_READ` | `buf, max` | bytes drained (owner; non-blocking) |
| 38 | `CONSOLE_WRITE` | `task_id, buf, n` | bytes accepted, or −1 (owner only) |
| 39 | `WM_HOTKEY` | `modifier mask` | 0 or −1 (`INPUT_FOCUS`-style authority check) |
| 40 | `PUTS` | `str` | length written (whole string emitted atomically) |
| 41 | `LSEEK` | `fd, offset, whence` | new offset, or −1 |
| 42 | `STAT` | `path, out*` | 0/−1; fills `{size, is_dir, ino, mode, mtime}` |
| 43 | `MKDIR` | `path` | 0 or −1 |
| 44 | `UNLINK` | `path` | 0 or −1 |
| 45 | `FTRUNCATE` | `fd, size` | 0 or −1 |
| 46 | `TIMER_SET` | `ms` (0 cancels) | 0; posts `GRV_EV_TIMER` on expiry |
| 47 | `RENAME` | `from, to` | 0 or −1 (both sides must be on the same filesystem) |
| 48 | `TIME` | — | wall-clock seconds since the Unix epoch (RTC-anchored) |
| 49 | `SURFACE_RESIZE` | `id, w, h, out*` | 0 or −1 (owner only; the buffer is reallocated, so the caller must re-bind to the new `address`) |
| 50 | `MAP_FILE` | `path, out*` | 0 or −1; fills `{address, size}` with a read-only, zero-copy mapping |
| 51 | `MUNMAP` | `address` | 0 or −1 (the address a mapping call returned) |
| 52 | `SHM_CREATE` | `size` | region id (> 0) or −1 |
| 53 | `SHM_GRANT` | `id, task` | 0 or −1 (owner only) |
| 54 | `SHM_MAP` | `id, out*` | 0 or −1; fills `{address, size}` |
| 55 | `SHM_CLOSE` | `id` | 0 or −1 (owner only) |
| 56 | `POWEROFF` | — | does not return: the machine switches off |
| 57 | `REBOOT` | — | does not return: the machine resets |
| 58 | `STATFS` | `path, out*` | 0/−1; fills `{total, free, writable, type}` |
| 59 | `DUP` | `fd` | new descriptor (≥ 3) sharing the open file description, or −1 |
| 60 | `DUP2` | `oldfd, newfd` | `newfd` now names `oldfd`'s description (closing what it held), or −1 |
| 61 | `PIPE` | `fds*` | 0 and `fds = {read end, write end}`, or −1 |
| 62 | `FSTAT` | `fd, out*` | 0/−1; the same `{size, is_dir, ino, mode, mtime}` as `STAT`, for an open descriptor |
| 63 | `SYNC` | — | 0, or −1 when a filesystem flush failed; flushes every mounted filesystem |
| 64 | `FSYNC` | `fd` | 0/−1; flushes the filesystem holding an open descriptor |

Numbers 2 and 3 are retired (the flat-tarfs list/read-by-name, superseded
by the VFS). The kernel dispatches through a number→handler table
(`syscall_table` in `src/arch/x86_64/idt.cpp`); a retired hole or an unknown
number returns `-ENOSYS`, and adding a call is one handler plus one row.

### The interrupt key

`0x03` (Ctrl+C) in a task's console input is not text: it terminates that
task's **foreground job** — the task it is blocked on (a foreground `SPAWN` or
`WAIT`), which resumes it with exit code 130. `CONSOLE_WRITE` applies this rule
to the bytes it injects, so a terminal that forwards Ctrl+C to its hosted shell
makes it work end to end; the kernel console applies it to the focused task.

### The clipboard

There is no clipboard syscall: it is a file (`/tmp/.clipboard`, see
`<grv/clipboard.h>`) that any app can read and write, and Ctrl+Shift+C /
Ctrl+Shift+V is the convention for using it. The Shift is what separates the
chord from the interrupt key, so the keyboard keeps the letter intact for it.

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

The kernel's dump names the RIP's function (`RIP=0x... (main(int, char**)+0x2C)`)
using a symbol table generated at build time by `tools/mksyms.py` and read from
the tar at boot. It covers the kernel **and** every program — a fault is usually
in a program's own code, and the table cannot be linked into the kernel it
describes, because adding it would move the addresses it names.

## Leaving

`POWEROFF` and `REBOOT` are one-way: by the time the machine answered, it is
already going down, so anything that needs flushing belongs *before* the call
(`poweroff.elf` is the userland command; `poweroff -r` resets). On QEMU the
shutdown is a write to ACPI's PM1a control register and the reset is the 8042's
pulse-reset line; on real hardware the port and sleep type come from the FADT,
which is a Phase 6 concern along with the flushing.

## Memory mappings

Surfaces are one kind of shared memory; these are the other two. Both map frames
into a task's address space that the task does **not** own, which is why the
kernel marks those leaves with a software bit (`PTE_SHARED`, bit 9): the
address-space teardown clears the mapping and leaves the frames alone, so
whoever owns them frees them.

- **A file mapping** (`MAP_FILE`) is a read-only, zero-copy view of a file. It
  works when the backend can promise the bytes stay put: a tarfs member is a
  slice of the immutable image, so the mapping is just its physical frames —
  nothing is copied, and nothing can change under the reader. The overlay
  forwards it to the lower layer, and refuses when the file has been copied up
  (a ramfs buffer can be reallocated). A file rarely starts on a page boundary,
  so the returned address points at its real first byte inside the mapping.
- **A shared region** (`SHM_*`) is anonymous pages one task creates and others
  attach. The id is a handle, but knowing it is not enough: the owner *grants*
  the region to a task (`SHM_GRANT`), so a region is handed over deliberately.
  It is zeroed on creation and reference counted, so its frames outlive the
  owner if another task still has it attached.

This is what four-word messages cannot do: a producer and a consumer share a
buffer instead of copying through the mailbox. `MUNMAP` drops either kind, and a
task's mappings and handles are released when it dies.

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
- **`WM_HOTKEY` (39)**, authority-only, reserves a modifier chord (e.g.
  `GRV_MOD_ALT`): keys pressed with it are posted to the authority as
  `GRV_EV_KEY` regardless of focus — global hotkeys (Alt+Tab). `0` clears it.
- **Window lifecycle events**: when a non-authority task creates or loses a
  surface, the kernel posts a message to the authority's mailbox
  (`from == -1`) with `type` = `GRV_EVT_SURFACE_ADDED` /
  `GRV_EVT_SURFACE_REMOVED` and `a` = the surface id. The authority also
  scans `SURFACE_LIST` once at startup for pre-existing surfaces.
- **`SURFACE_RESIZE` (49)**, owner-only like the rest: a surface's buffer is
  allocated once, and this reallocates it at a new size. The pixels move to a
  new VA, so the owner has to re-read the geometry and re-bind. It is the
  mechanism behind resizable windows: the WM *asks* a client for a size
  (`GRV_WM_RESIZE`, userland policy in `<grv/wm.h>`) and the client answers with
  the size it took (`GRV_WM_RESIZED`), which is how the decoration stays in step
  with a client that clamps.
- **`SURFACE_COMMIT` with a rect** damages only that surface-local
  rectangle, so a full-screen overlay can commit just its change:

```
struct grv_rect { long x, y, w, h; };   // surface-local; top-left = 0,0
```

Closing a window is `KILL`ing its owner — the kernel's teardown removes the
surface (the authority cannot `SURFACE_DESTROY` another task's surface,
since that would unmap memory the owner still holds).

The **userland** half of this — the WM protocol (`GRV_WM_HELLO`, `SET_TITLE`,
`SET_ROLE`, `CLOSE`) — is `<grv/wm.h>`, and the kernel knows nothing about any
of it. A Gravity app's `--geometry WxH` flag is `<gravity/app.h>`, and the
widgets the apps and the IDE are built from are `<gravity/editor.h>`,
`<gravity/filer.h>`, `<gravity/term.h>` and `<gravity/list.h>`.

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
  (`GRV_KEY_ESCAPE` 0x100, `GRV_KEY_UP/DOWN/LEFT/RIGHT` 0x101–0x104,
  `GRV_KEY_HOME/END/DELETE` 0x107–0x109, `GRV_KEY_F1`–`GRV_KEY_F10`
  0x10A–0x113). Ctrl+letter arrives as its control code (^C = 3) with
  `GRV_MOD_CTRL` set — **except** Ctrl+Shift+letter, which keeps the letter
  (uppercase) so an app can tell Ctrl+Shift+C (copy) from Ctrl+C (interrupt).
- **A console reader gets bytes, not events.** The shell reads `GETC`, so the
  console translates the keys a byte-stream line editor needs: the arrows arrive
  as the readline codes (^P/^N up/down, ^B/^F left/right, ^A/^E home/end) and
  the editing chords (^A/^B/^E/^F/^K/^N/^P/^U/^W) pass through as themselves.
  Ctrl+C does not: it stays the interrupt key.
- **Modifiers**: `GRV_MOD_SHIFT/CTRL/ALT/CAPS` (1/2/4/8).
- **Buttons**: `GRV_BTN_LEFT/RIGHT/MIDDLE` (1/2/4).

The keyboard IRQ delivers keys to the **focused** event-mode task. Mouse
events are produced by the WM (it alone has the pointer): it hit-tests the
window under the cursor and sends `GRV_EV_MOUSE` with coordinates relative
to that window's surface.

**A console is a byte stream, and it behaves like a terminal's.** `'\n'` moves
to the next line, `'\r'` returns to the start of the line, and `'\b'` *moves*
left without erasing — so a reader that keeps a cursor anywhere in a line (the
shell) can move it without destroying what it passes. Erasing is the usual
three-byte idiom, `'\b' ' ' '\b'`. There is no erase-to-end-of-line, so a
redraw pads over a longer previous line.

## Console endpoint (per-task stdio)

How a userland terminal hosts a real program without the kernel knowing
what a terminal is. A task may bind another task's console to itself; the
bound task's `PUTC` bytes then land in the owner's ring, and the owner
injects the bound task's input with `CONSOLE_WRITE`.

- **`CONSOLE_ATTACH` (35)** makes the caller the **console owner** of
  `task_id` (a live task, not the caller, not already owned). Children
  **inherit** the owner, so a hosted shell's own children report to the
  same terminal. Returns 0 or −1.
- **`CONSOLE_READ` (37)**, owner-only, drains pending output into `buf`.
  Non-blocking; 0 means nothing is pending. When output arrives the kernel
  posts a coalesced `GRV_EVT_CONSOLE_OUT` message (no payload) to the
  owner's mailbox, so a terminal blocked in `RECV` wakes to drain it.
- **`CONSOLE_WRITE` (38)**, owner-only, injects bytes into `task_id`'s
  `GETC` ring (as if typed). Returns the bytes accepted (a full ring takes
  fewer) or −1 if the caller is not the owner.
- **`CONSOLE_DETACH` (36)** unbinds (owner only). The binding also clears  itself if the owner dies; a ring that fills simply drops (a full tty).

With no owner (the default, `-1`) `PUTC`/`GETC` keep using the kernel
console exactly as before, so nothing changes for tasks that never opt in.

## Related kernel debug keys

- **F11** — put the console layer above the surfaces (and back), so kernel
  output is readable while a window owns the screen.
- **F12** — dump the task table and compositor counters to the console.
