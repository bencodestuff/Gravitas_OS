# Contributing to Gravitas

Gravitas is a small system with a strict boundary. Changes are easier to review
when they respect it and arrive with evidence that they work.

## The layer rule

**The kernel owns mechanisms; userland owns policy.** Before adding anything,
decide which side it belongs to:

- The kernel may know tasks, memory, files, descriptors, surfaces and
  four-word messages. It may not know what a window is, what runs at boot, what
  a message `type` means, or what a shell command looks like.
- Userland decides all of that. A new policy belongs above the syscall line,
  usually in `src/user/` or `src/libc/gravity/`.

`docs/architecture.md` explains the boundary and why it is enforced where it is.
If a change has to cross it, say so in the commit body and in a comment at the
crossing.

## Before you commit

The tree must be green. At minimum:

```sh
ninja check
```

Run the guest group that covers your change:

```sh
GROUPS=init ninja test       # one group
ninja test-kernel            # every kernel group
ninja test-ui                # the desktop groups
```

A change to the kernel, a filesystem, the ABI or the harness should also run
`ninja test` before it lands. `ninja test` boots the full guest suite; it is the
last check, not the first one.

**A change lands with tests.** New behaviour needs an assertion that would fail
without it. Prefer a host test when the logic can run without the kernel;
otherwise add a test to the relevant group (see `tests/README.md`). A bug fix
needs a test that reproduces the bug.

## Commit contract

- Write the subject in the imperative mood: `Fix the ext2 directory split`, not
  `Fixed` or `Fixes`. Keep it short and do not end it with a full stop.
- The body says **why**, not what the diff already shows. A few lines is enough.
  Record a non-obvious decision, a trade-off, or a limit you accepted.
- Use British English: behaviour, colour, initialise, licence, centre.
- Do not use em dashes or en dashes. Use a comma, a colon, a semicolon or a full
  stop.
- One logical change per commit. Do not reformat unrelated files in the same
  commit.
- Do not commit generated output (`bin/`, `obj/`, `iso_root/`, `build.ninja`,
  `test_artifacts/`, `limine/`, `third_party/lua-*`).

## Build

```sh
python3 configure.py     # first time, and after changing build prerequisites
ninja                    # the kernel ELF only
ninja iso                # every userland program and the bootable ISO
ninja run                # boot the ISO in QEMU
```

`ninja` regenerates `build.ninja` when `configure.py` changes, but a fresh clone
still needs one `python3 configure.py` because there is no graph to regenerate.
The host tools and their packages are listed in `README.md`.

## Adding an app

1. Add one `.cpp` file under `src/user/` with `int main(int argc, char** argv)`.
2. Put it in the launcher by adding a line to `files/apps`.
3. Run `ninja iso`.

The build finds the source automatically, links it against the shared libc and
packs the ELF into `/bin`. Add a test under `src/user/` and a group assertion if
it is a platform program rather than an application. The full tutorial,
including windowed programs, is in `docs/adding-an-app.md`.

## Documentation

User-visible behaviour, new syscalls, new limits and new debugging tricks belong
in `README.md`, `docs/syscalls.md`, `docs/architecture.md` or
`docs/debugging.md` in the same change. `tests/README.md` documents the harness
rules that keep the suite honest. If you correct an old claim, check it against
the source first.
