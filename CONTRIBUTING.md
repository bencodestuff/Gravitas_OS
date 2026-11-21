# Contributing to Gravitas

## Build

Install the [build dependencies](README.md#build-requirements), then run:

```sh
python3 configure.py     # set up a fresh checkout
ninja                    # build the kernel
ninja iso                # build the programs and bootable ISO
ninja run                # boot the ISO in QEMU
```

Run `python3 configure.py` again if you add or remove a source file. Ninja
regenerates `build.ninja` when `configure.py` itself changes.

## Kernel and userland

The kernel provides tasks, memory, files, descriptors, drawing surfaces and
four-word messages. The window manager decides where those surfaces appear,
init decides what starts at boot, and the shell parses commands. Programs
decide what their message types mean.

Most changes to those programs go in `src/user/` or `src/libc/gravity/`.
See [the architecture notes](docs/architecture.md) for how they use the kernel.
If a change needs an exception, explain why in the commit message and leave a
comment where the code crosses that boundary.

## Tests

Run the host checks before committing:

```sh
ninja check
```

For changes that run inside Gravitas, run the relevant QEMU tests too:

```sh
GROUPS=init ninja test       # one group
ninja test-kernel            # every kernel group
ninja test-ui                # the desktop groups
```

Run the full `ninja test` suite for changes to the kernel, filesystems, syscall
ABI or test harness.

Add a test for new behaviour or a bug fix. Check that it fails without the
change. Use a host test if it can run without the kernel; otherwise add it to
the relevant guest group. See the [test guide](tests/README.md).

## Commits

Use a short subject such as `Fix the ext2 directory split`. In the body, explain
what prompted the change and any trade-offs a reviewer needs to know about.
Keep unrelated changes and generated files out of the commit.

## Adding an app

1. Add one `.cpp` file under `src/user/` with `int main(int argc, char** argv)`.
2. If the app should appear in the desktop launcher, add it to `files/apps`.
3. Run `ninja iso`.

The build links the program with libc and puts it in `/bin` on the ISO.
See [Adding an app](docs/adding-an-app.md) for a windowed example.

## Documentation

Update the docs in the same change. Put user-facing changes in the
[README](README.md), syscall and limit changes in the [syscall reference](docs/syscalls.md)
and [limits](docs/limits.md), and debugging instructions in the
[debugging guide](docs/debugging.md).
