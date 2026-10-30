# Gravitas limits

Every fixed capacity an external program can reach, where it is enforced, and
what happens at the edge. The numbers are deliberately small: they fit the
current desktop and they are stated here so a program can plan around them
rather than discover them as an unexplained failure.

Two conventions run through the table. A refusal is a negated `errno` (see
[`syscalls.md`](syscalls.md#errors)); a **silent** limit is one the kernel does
not report, and those are called out because they are the dangerous kind. The
storage backend is a `RamFS` or `ext2` depending on what is mounted, so the
filesystem rows name the backend as well as the limit.

## The limits

| limit | value | enforced in | at the boundary |
|---|---|---|---|
| Descriptors per task | 16 slots, of which 0, 1 and 2 are the standard streams, so **13 usable** | `VFS::MAX_FDS`, `fd_slot_alloc`/`fd_slot_new` in `src/fs/vfs.cpp` | `OPEN`, `DUP`, `DUP2`, `PIPE` return `-EMFILE` once the 13 are in use. The first `OPEN` is descriptor 3. |
| Open file descriptions, kernel-wide | **64**, counting file handles and pipe ends | `VFS::MAX_HANDLES`, `handle_alloc` in `src/fs/vfs.cpp` | `OPEN` and `PIPE` return `-EMFILE` even when the calling task still has free descriptor slots. The standard streams do not consume the pool. |
| Descriptor slots in a pipe | 2 per pipe, one read end and one write end | `pipe_fd` in `src/fs/vfs.cpp` | A pipe needs two free slots. It can fail with `-EMFILE` when only one is free, and the first end is reclaimed. |
| Pipes at once | **8**, shared by every task | `MAX_PIPES`, `pipe_alloc` in `src/fs/vfs.cpp` | `PIPE` returns `-EMFILE`. A pipe is freed when both ends close. |
| Bytes in a pipe | **4 KiB** ring, 4095 usable | `PIPE_BUF`, `pipe_write_fd` in `src/fs/vfs.cpp` | A write that meets a full pipe **blocks** until the reader drains it; it does not return `EAGAIN`. One byte is left unused so "full" and "empty" differ. |
| Blocked tasks per pipe end | **4** | `MAX_PIPE_WAITERS` in `src/fs/vfs.cpp` | Silently not registered: the fifth reader or writer on one end is woken by the timer rather than by the data. This is a kernel under-provisioning, not a program error. |
| Mailboxes at once | **16**, shared by every task | `MAX_BOXES`, `claim_box` in `src/kernel/core/ipc.cpp` | `SEND` returns `-ENOMEM` when no slot is free. There is **no cap on live tasks**, so a 17th task that wants a mailbox is refused. A `RECV` that cannot claim a slot falls back to a timer wakeup. |
| Messages per mailbox | **16** | `BOX_CAP`, `Ipc::send` in `src/kernel/core/ipc.cpp` | `SEND` returns `-EAGAIN`; sends never block. A non-blocking `RECV` on an empty box also returns `-EAGAIN`. |
| Path length | **512** bytes including the terminator | `VFS::PATH_MAX`, `normalize`/`resolve` in `src/fs/vfs.cpp` | `-ENAMETOOLONG`; never truncated. |
| Path components | **32** | `normalize` in `src/fs/vfs.cpp` | `-ENAMETOOLONG`. This is a separate cap from the byte length. |
| One name component | **256** bytes including the terminator (`NAME_MAX`); ext2 itself allows 255 bytes | `VFS::NAME_MAX`, `next_component`/`resolve` in `src/fs/vfs.cpp` | `-ENAMETOOLONG`; the VFS refuses the name before any backend sees it and never truncates it. |
| Working directory | **512** bytes, the same as a path | `Task::cwd` in `src/kernel/core/task.h`, `canonicalize` in `sys_chdir` (`src/arch/x86_64/idt.cpp`) | `CHDIR` returns `-ENAMETOOLONG` only for a path that genuinely does not fit. A directory whose path is at most 511 bytes can be entered. |
| Bytes per write call | **16 MB** | `VFS::MAX_WRITE_BYTES`, `write_fd` in `src/fs/vfs.cpp` | `-EINVAL`, before any backend or user-buffer page walk. This is a sanity cap, not a file-size cap. |
| `/tmp` file size | **1 MB** per file | `MAX_FILE_BYTES`, `reserve`/`ram_write` in `src/fs/ramfs.cpp` | A write that would pass the cap is refused with `-ENOSPC` (a short write is never returned). `FTRUNCATE` past it is `-ENOSPC`. |
| `/tmp` total file data | **8 MB** across the whole ramfs instance | `MAX_TOTAL_BYTES` in `src/fs/ramfs.cpp` | `-ENOSPC`. A diskless boot uses the same ramfs for the writable root, so `/tmp` shares this 8 MB there. With a disk, `/tmp` is its own mount but still this instance. |
| Mounts | **8**, including `/` | `VFS::MAX_MOUNTS`, `VFS::mount` in `src/fs/vfs.cpp` | `mount()` returns `false`; at boot the kernel prints that there was no room. There is no unmount. |
| Block devices | **4** | `Block::MAX_DEVICES`, `Block::add` in `src/kernel/core/block.h` | `add()` returns `false`; the device is simply not registered. |
| Block cache | **64 pages of 4 KiB**, 256 KiB | `BlockCache::PAGES` in `src/kernel/core/blockcache.h` | Not an error: the LRU evicts a clean page, or writes back a dirty one first. A working set larger than 256 KiB simply thrashes. |
| Kernel heap | **32 MB** | `HEAP_SIZE` in `src/kernel/main.cpp`, `Heap::malloc` in `src/kernel/core/heap.cpp` | An allocation returns `nullptr`; the caller reports `-ENOMEM` where it can. There is no growth and no reclaim pressure valve. |
| User virtual-address window | **256 MB to 1 GB** (768 MB) | `USER_VA_BASE`/`USER_VA_END`, `VMM::alloc_user_range` in `src/kernel/core/vmm.cpp` | `alloc_user_range` returns 0; the spawn, surface or mapping that needed it fails. The `brk` arena starts lower, at 128 MB. |
| Shared regions | **16** at once, each at most **64 MB**, each grantable to **8** tasks | `MAX_REGION`, `MAX_REGION_BYTES`, `MAX_GRANT` in `src/kernel/core/memmap.cpp` | `SHM_CREATE`/`GRANT` return `-1`. A zero-size or oversized region is refused before allocation. |
| Memory mappings | **32** kernel-wide, covering file mappings and attached regions | `MAX_MAP` in `src/kernel/core/memmap.cpp` | `MAP_FILE`/`SHM_MAP` return `-1`. |
| User stack | **32 KiB** plus one unmapped guard page below it | `USER_STACK_PAGES` in `src/kernel/core/proc.cpp` | The stack does not grow. Touching the guard page faults, and the fault kills the task with the stack-overflow report. |
| Kernel stack | **32 KiB** | stack allocation in `src/kernel/core/proc.cpp`; canary in `Sched::check_kernel_stack` (`src/kernel/core/sched.cpp`) | The canary is checked once per tick. An overflow halts the machine as a kernel bug; there is no guard page. |
| Surfaces | **32**, shared by every task | `MAX_SURFACES` in `src/kernel/gfx/compositor_internal.h` | `SURFACE_CREATE` returns `-1` (this call reports `-1`, not an `errno`). |
| Surface size | **64 MB** per surface | `MAX_SURFACE_BYTES` in `src/kernel/gfx/compositor_internal.h` | `SURFACE_CREATE` and `SURFACE_RESIZE` return `-1`. |
| `spawn` arguments | **8** entries, each truncated to **63 bytes** | `argv_stage[8][64]` and the copy loop in `src/arch/x86_64/idt.cpp` | **Silent**: extra arguments are dropped and longer ones are cut at 63 bytes. No error is reported. |
| `spawn` program name | **63 bytes** | `spawn_namebuf[64]` and the copy loop in `src/arch/x86_64/idt.cpp` | **Silent**: the name is cut at 63 bytes, so a longer path can name a different program. |
| Shipped ext2 image | **16 MB**, **512 inodes** | `BLOCKS`/`INODES` in `tools/mkext2.py`; mounted from `iso_root/disk.img` | `ENOSPC` from `CREATE`/`WRITE`/`MKDIR`; `df` reports both bytes and inodes. Inodes run out first. The driver also has no triple-indirect mapping and a 32-bit file size. |
| Permissions | **none** | not enforced anywhere; `grv_stat`/`grv_fstat` report the mode bits for information | Any task can read or write any file, kill any task, list another task's surface metadata, and map the scanout with `FBINFO`. `mode` is cosmetic; the only access refusals are `-EROFS` on a read-only backend. |

## Notes for a program

- Count descriptors, not buffers. A task's 13 usable descriptors are the usual
  wall, but the 64-handle pool is global: another program holding pipes can make
  `OPEN` fail with `EMFILE` while this task still has slots free. There is no
  retry story in the kernel; a program that needs many files should open, use
  and close them in turn.
- Treat `-EAGAIN` from `SEND` as "try later", and `-ENOMEM` from the first send
  as "no mailbox exists for this task yet and the table is full". The two are
  different failures even though both come from the same call.
- A pipe write blocks when the ring is full. If the reader is gone, the call
  returns `-EPIPE` (or a short count if some bytes already moved). There is no
  non-blocking mode.
- `ENOSPC` on `/tmp` is about the ramfs caps, not the disk. A 16 MB image can
  have free bytes and still refuse a new file because its 512 inodes are used;
  `df` reports both numbers.
- The `spawn` argument caps are the only limits on this page that fail without
  saying so. Keep argument strings short and pass at most 8 of them.

## Tests

`src/user/limittest.cpp` (group `tasks`) covers the limits a guest can drive
cheaply: 13 descriptors and `EMFILE`, a 16-message mailbox and `EAGAIN`, a
200-byte `/tmp` name that reopens, and `chdir` into a directory whose absolute
path is longer than the old 63-byte field. It deliberately does not fill the
shared `/disk` image, so ext2 `ENOSPC` is not covered here; a private small
image is the next case to add.
