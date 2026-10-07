# Gravitas OS

An x86_64 hobby operating system. Limine boots a monolithic kernel. The
userland is ordinary programs linked against a small libc.

## Build requirements

Host tools, with the Arch package that provides each:

| tool | package | needed for |
|---|---|---|
| `clang++`, `clang` | `clang` | compiling C++ and assembly |
| `ld.lld` | `lld` | linking |
| `xorriso` | `xorriso` | making the ISO |
| `tar` | `tar` | packing the image |
| `mke2fs` | `e2fsprogs` | making the ext2 test image |
| `python3` | `python` | configure.py, the build helpers, the tests |
| `qemu-system-x86_64` | `qemu-system-x86` | running and testing (not needed to build) |

Needed only on the first configure, because Limine and Lua are fetched then:
`git`, `make`, `curl`.

`configure.py` checks all of the above and stops with a message if something is
missing.

## Build and run

```sh
python3 configure.py
ninja               # the kernel only
ninja iso           # gravitas.iso
ninja run           # build and boot it in QEMU
```

## Test

```sh
ninja check         # host tests, a couple of seconds, no QEMU
ninja test          # host tests plus the guest groups
ninja test-kernel   # guest groups only
ninja test-ui       # the window/desktop groups only
```

## Layout

| path | what it is |
|---|---|
| `src/kernel/` | the kernel |
| `src/libc/` | the C library and the `gravity` widget library |
| `src/user/` | userland programs |
| `src/fs/` | ramfs, tarfs, the overlay, and the VFS |
| `src/arch/` | x86_64 specific code |
| `files/` | what gets packed into the image (`/bin`, `/fonts`, ...) |
| `tools/` | host-side build helpers |
| `docs/syscalls.md` | the syscall ABI and the mechanisms below it |
| `tests/` | the test suite; see `tests/README.md` |

## Notes

TODO: write this.
