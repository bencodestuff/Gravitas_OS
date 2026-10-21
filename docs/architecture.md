# Gravitas architecture

A monolithic x86_64 kernel with a userland of ordinary programs. Limine boots
the kernel; the kernel starts one program; that program runs a Lua manifest,
and the rest of the machine — the shell, the window manager, the terminal,
every app — is a task the manifest started. The subsystem reference for the
boundary between the two halves is `docs/syscalls.md`; this is the map of what
is on each side.

## The layer rule

**The kernel owns mechanisms; userland owns policy.** This is the rule the
whole tree is organised around, and it is repeated where it is enforced (for
example at the boot comments in `src/kernel/main.cpp` and at the top of
`docs/syscalls.md`). Concretely:

| the kernel provides | userland decides |
|---|---|
| bytes on a disk, sectors, a mount table, files and fds | what a file *means*, what runs at boot, the shell grammar |
| a task, a scheduler, memory, a program break | which programs exist, what they do |
| a surface: a w×h pixel buffer with an owner, a position and a z-order | what a "window" is, how it is decorated, dragged and closed |
| four-word messages moved verbatim, with a kernel-stamped sender | what a message `type` means; the WM protocol |
| `int $0x80` and a fixed ABI | the C library, the widget set, every policy above |

Nothing in the kernel reads a `type` it did not post, and nothing in the
kernel knows the word "window". When a piece of behaviour moves, it moves
across this line: the boot order used to be hardcoded in `kmain`, and is now a
Lua file (`files/init.lua`) that init runs.

## Boot path

```
Limine ──▶ kernel (kmain) ──▶ init.elf ──▶ /init.lua ──▶ shell / wm / term
```

`limine.conf` names three things the bootloader loads:

| entry | what it is |
|---|---|
| `kernel_path: boot():/gravitas.elf` | the kernel |
| `module_path: boot():/files.tar` | the userland image: `/bin`, `/fonts`, `/Pictures`, `/tmp`, `/env`, `/symbols.bin`, `/apps` |
| `module_path: boot():/disk.img` | the ext2 image mounted at `/disk` |

The kernel side is one function, `kmain` in `src/kernel/main.cpp`. In order:

1. **C++ runtime.** Global constructors (`__init_array`), then the
   stack-protector canary (`Fault::init`). Constructors must be
   self-contained — no allocation, no printing.
2. **Memory.** `PMM::init` (physical frames), `VMM::init` (paging + the HHDM
   window onto all physical memory), the kernel heap at physical 128 MB.
3. **CPU and interrupts.** `GDT::init`, `IDT::init`, `Cpu::enable_fpu_sse`,
   `Sched::init`. The PIC is remapped, every controller is silenced, then
   interrupts are enabled; the kernel unmasks exactly what it wants: the
   timer (IRQ0), the keyboard (IRQ1) and the mouse (IRQ12).
4. **Block layer self-test** on a small scratch RAM disk (`MemBlock`), no
   user's data.
5. **Filesystem.** `Tar::init` finds `files.tar`; `RamFS::init` and
   `Overlay::init(RamFS, Tar)` build the writable root, and `VFS::mount("/")`
   mounts it. `/` is therefore an overlay: the tar is the read-only lower
   layer, the ramfs the writable upper.
6. **Data disk.** `Ata::init` drives a real drive over PIO, or the kernel
   falls back to copying the `disk.img` module into a RAM disk; either way
   `Ext2::mount(dev, true)` puts it, read-write, at `/disk`.
7. **Presentation and data.** The console font (`Font::load_from_disk`), the
   fault-handler symbol table (`/symbols.bin`), and the environment block
   (`/env`).
8. **Userland.** `spawn_program("/bin/init.elf", "init")`. If that fails, the
   one fallback is a console shell.

`init.elf` (`src/user/init.cpp`) links in Lua 5.4 and runs the boot manifest,
`/init.lua` by default (source `files/init.lua`). The manifest is ordinary Lua
and is the machine's boot policy: the shipped one starts `shell.elf`, then
`wm.elf`, then `term.elf`, in that order. Because `/` is writable, a copy at
`/init.lua` in the overlay shadows the shipped one, so what starts can change
without a rebuild. `init` exposes a small `init` table (`spawn`, `run`, `wait`,
`kill`, `sleep`, `cwd`, `log` — documented at the top of `files/init.lua`).

## Major subsystems

### VFS and its filesystems

`src/fs/vfs.h` (design), `src/fs/vfs.cpp`. The layer above works in *nodes*,
not path strings; path walking happens in one place and each backend answers
per-node questions (`Filesystem` ops table). Backends are registered into a
mount table (`MAX_MOUNTS` 8) and the VFS hands out integer file descriptors
backed by per-task handle tables (`MAX_FDS` 16; 0–2 are the standard streams).
A name is at most `NAME_MAX` 256 bytes and a path `PATH_MAX` 512. A bare name
is a lookup convenience: the CWD first, then `/bin`, then `/bin/tests` for the
test programs.

| backend | source | role |
|---|---|---|
| tarfs | `src/fs/tar.cpp` | ustar archive from Limine, read-only lower layer |
| ramfs | `src/fs/ramfs.cpp` | writable, in-memory upper layer; refcounted nodes |
| overlay | `src/fs/overlay.cpp` | the writable root: ramfs over tarfs, copy-up on write, whiteouts on delete |
| ext2 | `src/kernel/dev/ext2.cpp` | read-write ext2 on the data disk, mounted at `/disk` |

The overlay is what makes `/` writable at all: a write to an image file is
copied up first (or shadowed by `O_TRUNC`), a listing is the union of both
layers, and deleting an image file leaves a `.wh.<name>` marker in the upper
rather than touching the immutable tar. A read-only backend has no writer, so
a write to `/disk` fails at the VFS with `EROFS`.

### Block layer and the disk

`src/kernel/core/block.h`. A filesystem should not care whether its bytes come
from RAM, a bootloader module or a real drive, so everything below the VFS
talks in 512-byte sectors through `Block::Device` — a name, a size, and
read/write of a sector run. It is a registry and a contract, not a cache, and
a request that runs past the end moves nothing (a caller asking past the end is
broken, and a short read would hide it). There is no syscall here; the layer is
internal.

| device | source | notes |
|---|---|---|
| RAM disk | `src/kernel/dev/memblock.cpp` | heap buffer behind the contract; optionally seeded from a host image |
| ATA PIO | `src/kernel/dev/ata.cpp` | polled PIO on the primary channel's legacy task file; no interrupts, LBA only, spin cap on every poll |
| Limine modules | `src/kernel/dev/modules.cpp` | finds the `.tar` and `.img` modules |
| ext2 | `src/kernel/dev/ext2.cpp` | read-write ext2 over a `Block::Device` |

`disk.img` is made on the host with `tools/mkext2.py`. The kernel prefers a
real ATA drive and falls back to the module, so the guest boots either way and
the mount above cannot tell which it got. The ext2 reader covers the superblock,
group descriptors, inode tables, directory records and the direct/indirect
block pointers. The writer allocates and frees blocks and inodes through the
bitmaps and group counts, writes partial blocks, keeps directories' `rec_len`
records tidy, and refuses a filesystem that would need symlinks, extents or
64-bit support.

### Scheduler, tasks and file descriptors

`src/kernel/core/sched.h` (public API), `task.h` (the `Task` object),
`sched.cpp` (ready queue and context switch), `proc.cpp` (lifecycle), and
`src/arch/x86_64/sched_asm.S` (the switch). Tasks are scheduled round-robin and
preempted by the 50 Hz timer. A task carries its own address space (CR3), a
heap-allocated kernel stack with a canary, an FPU/SSE save area, its program
break (`brk`), its working directory, its fd table, and (for a hosted console)
its stdio rings.

`create_user_task` builds a ring-3 task from an ELF entry point; the kernel
loads the program into a freshly cloned address space with `ELF::load_elf`
(`src/fs/elf.cpp`) and lays `argc`/`argv`/`environ` onto the new stack in
System V order. Spawning returns a task id; a task can `WAIT` on it, or be
`KILL`ed. Exits are recorded so `WAIT` can reap an already-exited child, and
faults leave a distinctive exit code (`EXIT_FAULT`, `EXIT_STACK_SMASH`).

### IPC and mailboxes

`src/kernel/core/ipc.cpp`, `src/kernel/core/ipc.h`. Every task has a FIFO
mailbox carrying four-word messages `{from, type, a, b}`. The kernel moves the
words verbatim and stamps the true sender in `from`, so userland cannot spoof
it; a kernel post uses `from == -1`. Send is non-blocking (`-1` no/dead
target, `-2` mailbox full); receive blocks. Nobody may receive from another
task's mailbox, and a task's mailbox dies with it.

The four words are the whole wire format, so the meaning of `type` is
userland policy. The reserved ranges — kernel input/console, the WM protocol,
surface lifecycle, applications — are tabulated in `docs/syscalls.md`.
Message payloads cannot move a buffer; where tasks must share bytes they use a
surface or a shared region (`SHM_*`) instead.

### Surfaces, the compositor and the window manager

`src/kernel/gfx/compositor.cpp`, `src/kernel/gfx/compositor.h`. A **surface**
is a w×h 32bpp pixel buffer the kernel tracks but never interprets: scattered
physical frames (the PMM makes no contiguity promise), mapped into the owner's
address space and read back by the kernel through the HHDM so compositing
works whichever CR3 is active. The kernel knows geometry, ownership and draw
order; it has no idea what a window is. Every op except `SURFACE_LIST` is
owner-checked against the kernel-stamped task id. `present()` paints only the
damaged rectangles, and `SURFACE_COMMIT` with a rect damages one surface-local
rectangle, so a partial redraw is a partial repaint.

The **display authority** is the set of mechanisms a window manager needs
without the kernel knowing about windows: one task may `WM_CLAIM`, and it can
then move and restack *any* surface, owns the mouse (`MOUSERD` returns packets
to it alone), sets keyboard focus (`INPUT_FOCUS`) and reserves a global hotkey
chord (`WM_HOTKEY`, e.g. Alt). When a non-authority task creates or loses a
surface the kernel posts a message to the authority with `type` =
`GRV_EVT_SURFACE_ADDED` / `GRV_EVT_SURFACE_REMOVED`. Closing a window is
`KILL`ing its owner; the kernel's teardown removes the surface.

The **window manager** itself is userland: `src/user/wm.cpp`. It draws the
taskbar, the decorations and the launcher, hit-tests the pointer and turns it
into `GRV_EV_MOUSE` messages with window-local coordinates, implements Alt+Tab,
and speaks the WM protocol in `<grv/wm.h>` (`GRV_WM_HELLO`, `SET_TITLE`,
`SET_ROLE`, `CLOSE`, `RESIZE`, …) over the ordinary mailbox. A Gravity app's
window comes from `gravity_run` in `<gravity/app.h>`.

Input enters the system through `src/kernel/dev/keyboard.cpp` and
`mouse.cpp`; the event types and key/button codes are mirrored in
`src/kernel/core/input.h` and `<grv/input.h>`, and their full behaviour is in
`docs/syscalls.md`.

### Console, terminal and stdio

The kernel console (`src/kernel/gfx/console.cpp`) is the default byte stream
for a task's `GETC`/`PUTC`. A task's console is a pair of byte rings, and
`CONSOLE_ATTACH`/`CONSOLE_READ`/`CONSOLE_WRITE` let one task become another's
console owner — which is how the userland terminal (`src/user/term.cpp`) hosts
a real shell in a window without the kernel knowing what a terminal is.
Children inherit the owner, so a hosted shell's own children report to the
same terminal. With no owner (the default, `-1`) the kernel console is used
exactly as before. A console is a byte stream that behaves like a terminal's:
`'\n'` advances, `'\r'` returns to the line start, and `'\b'` moves left
without erasing.

### libc and the POSIX facade

`src/libc/` is a freestanding C library, compiled once into `obj/libc` and
linked into every userland program. It covers the pieces Lua and the apps
need: string/ctype, the `FILE*` stdio layer with `printf`, stdlib (`malloc`
over `brk`), time, libm, `setjmp`, and the remaining Lua-required entry points.

`src/libc/posix/` is the facade that gives those mechanisms POSIX names and
types: `unistd.c` (`read`, `write`, `close`, `dup`, `dup2`, `pipe`, `lseek`,
`chdir`, `getcwd`, `sleep`, …), `stat.c` (`stat`, `mkdir`), `dirent.c`
(`opendir`/`readdir`, with real `d_ino`/`d_type`), and `fcntl.c`. Each is a
thin translation of a `grv_*` call, and it is deliberately partial — ownership
and link counts are not modelled, so those fields read as zero.

`src/libc/cxx/crt0.cpp` is the real entry point: it points `environ` at the
kernel-built block, runs constructors, calls `main`, and exits with its return
value. Programs just write `int main(int argc, char** argv)`.

### The syscall ABI

Everything crosses through `int $0x80`. `rax` is the number in and the return
value out; `rdi`, `rsi`, `rdx`, `r10` are the arguments; failure is a negated
errno. The userland wrappers live in `src/libc/include/grv/syscall.h`, the
kernel side is the dispatcher in `src/arch/x86_64/idt.cpp`, and the two must
agree with the documentation — a host test, `tools/check_syscalls.py` in
`ninja check`, enforces that. **`docs/syscalls.md` is the reference**: the
register convention, the errno table, the path and environment rules, the
message-type registry, the numbered call table, and the shared struct layouts.

## The `grv` and `gravity` split

The userland side of the ABI is split into two namespaces with a strict
direction: `grv` is the contract, `gravity` is convenience built on it. They
live side by side under `src/libc`:

| | `<grv/...>` | `<gravity/...>` |
|---|---|---|
| what it is | the kernel-facing ABI surface | the higher-level userland library |
| source | headers only, `src/libc/include/grv/` | headers + compiled code, `src/libc/include/gravity/` and `src/libc/gravity/` |
| includes | `<sys/cdefs.h>`, `<errno.h>` | `grv` and each other |
| contents | syscall numbers and inline wrappers (`syscall.h`), the input-event wire format (`input.h`), the WM protocol (`wm.h`), the clipboard convention (`clipboard.h`) | a surface-backed canvas, an event loop, a small widget set, and the shared font/image/text-buffer/editor/filer/term primitives |
| who uses it | both halves of the wire: the libc wrappers and any program that talks to the kernel directly | applications, via `<gravity/gravity.h>` |

There is **no compiled `grv`** — every header under `src/libc/include/grv/`
is declarations and `static inline` wrappers over `int $0x80` or over the WM
message format. Its job is to be the one place a kernel layout, a syscall
number, an event type or a wire convention is written down on the userland
side, mirroring the kernel byte for byte. `src/libc/gravity/` is where the
actual library code lives (canvas, app, widget, editor, filer, term, list,
font, image, buffer, popup).

The rule in practice:

- **Kernel-facing or shared-wire → `grv`.** If the kernel defines it, or the
  value has to match the kernel's record, it belongs in `grv` and nowhere else.
- **A userland convenience on top → `gravity`.** A canvas primitive, a widget,
  an event loop: none of it is ABI, and none of it belongs in `grv`.
- **Direction is one-way.** `gravity` includes `grv`; `grv` includes neither
  `gravity` nor any policy. An application that only draws uses
  `<gravity/gravity.h>`; one that needs a syscall the library does not wrap
  drops to the matching `<grv/...>` header.

This is exactly why the two are separate directories and not two names for the
same thing: `grv` is small, stable and mirrored by a host test, while
`gravity` can grow widgets without touching the ABI.

## Where things live

| path | what it is |
|---|---|
| `src/kernel/` | the kernel: `core/` (PMM, VMM, heap, sched, IPC, timer, fault), `dev/` (ATA, ext2, keyboard, mouse, RTC, RAM disk, modules), `gfx/` (framebuffer, console, compositor, font), `klib/` |
| `src/arch/x86_64/` | GDT, IDT + the syscall dispatcher, PIC/IOAPIC, context-switch and user-mode assembly, the linker script |
| `src/fs/` | VFS, ramfs, tarfs, the overlay, and the ELF loader |
| `src/libc/` | the C library, the POSIX facade, and the `gravity` library; `include/grv/` is the ABI surface |
| `src/user/` | userland programs (one `.cpp` per program) |
| `files/` | what is packed into the image root: `bin/` is built into it, plus `apps`, `init.lua`, `env`, `fonts/`, `Pictures/`, `lua/` |
| `docs/` | this file, `syscalls.md`, and `adding-an-app.md` |
| `tests/` | the test suite; see `tests/README.md` |
