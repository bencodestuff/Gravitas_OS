# Adding an app

A Gravitas app is one `.cpp` file under `src/user/`. The build finds it
automatically, links it against the shared libc, and packs the resulting ELF
into `/bin`; to put it in the window manager's launcher you add one line to
`files/apps`. No kernel change, and no change to the window manager.

## The three steps

1. **Write the program** — `src/user/myapp.cpp`, with a plain
   `int main(int argc, char** argv)`.
2. **Re-run the build configuration** — `python3 configure.py`. It globs
   `src/` at configure time, so a source file that is added or removed does not
   exist as far as `ninja` is concerned until this runs. It emits a
   `bin/myapp.elf` rule with the program's own load base.
3. **Put it in the launcher** — add a line to `files/apps`:

   ```
   myapp.elf       My App
   ```

   The file is the WM launcher menu, one app per line: the program in `/bin`,
   whitespace, then the label to show. Lines beginning with `#` are ignored.
   The WM reads it at startup (`load_apps` in `src/user/wm.cpp`), so a new app
   appears in the Apps menu with no WM change.

Then build: `ninja iso` builds the kernel, every userland program and the ISO.

| to touch | for what |
|---|---|
| `src/user/myapp.cpp` | the program |
| `python3 configure.py` | make the build see the new source |
| `files/apps` | show it in the WM launcher |
| `files/init.lua` | (optional) start it at boot |

## What the build makes of it

`configure.py` walks `src/` for `*.cpp` and treats every file under
`src/user/` as a separate program. For each one it emits a rule that links
`crt0.cpp` and the shared libc objects (compiled once into `obj/libc/`) at the
program's own load base, producing `bin/<stem>.elf`. `build_iso.sh` copies
every `bin/*.elf` into the image's `/bin`, and `files/*` 1:1 into the image
root — which is why `files/apps` is `/apps` in the guest, and why a file you
add under `files/` is present at the path you put it.

Two sources are special:

- `src/user/init.cpp` links the embedded Lua interpreter and gets its own rule;
  it is not built like the others.
- A source whose stem is listed in `TEST_PROGRAMS` in `configure.py` is a
  platform test and lands in `bin/tests/` (and `/bin/tests`, which is on the
  program search path) instead of `bin/`.

Programs are freestanding, `-fno-rtti -fno-exceptions`, statically linked, and
built for SSE (the kernel builds `-mno-sse`; keep the two halves that way).
There is no dynamic linking and no `dlopen` — one ELF per program.

## A console program

The libc gives you the usual C surface (`printf`, `open`, `read`, `write`,
`malloc`, `opendir`, …) plus the `grv_*` wrappers for anything the libc does
not cover:

```c
#include <stdio.h>

int main(int argc, char** argv) {
    printf("myapp: %d argument(s)\n", argc - 1);
    for (int i = 1; i < argc; i++) printf("  %s\n", argv[i]);
    return 0;
}
```

`argc`/`argv` are laid out by the kernel in System V order (see
`src/libc/cxx/crt0.cpp`), and `environ` points at the environment block the
kernel copied from `/env`.

## A windowed (Gravity) program

For a window, build on the Gravity library (`<gravity/gravity.h>`); it owns
the surface, the event loop and damage-tracked present, and the app supplies
two callbacks. `src/user/win.cpp` is the smallest complete example:

```c
#include <stdio.h>
#include <gravity/gravity.h>

static struct gravity_app app;

static void on_draw(struct gravity_app* a) {
    struct gravity_canvas* c = &a->canvas;
    gravity_clear(c, 0xFF203040);
    gravity_text(c, 12, 12, "myapp", 0xFFFFFFFF);
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    app.on_draw = on_draw;
    app.title = "My App";
    gravity_run(&app, 360, 220);
    return 0;
}
```

`gravity_run` creates the window surface, opts into message-delivered input,
answers the WM's hello with `title`, and loops on the mailbox. Optional
callbacks (`on_event`, `on_popup_draw`, `on_poll`, `on_resize`, `on_close`)
and the `--geometry WxH` flag are documented in `<gravity/app.h>`; the canvas
primitives are in `<gravity/canvas.h>`. If the program needs a syscall the
library does not wrap, drop to the matching `<grv/...>` header — see the
`grv` vs `gravity` split in `architecture.md`.

## Running it

- In a booted guest, from the shell: `run myapp` (a bare name is looked up in
  the CWD, then `/bin`, then `/bin/tests`; `run myapp.elf` works too).
- At boot: append `init.spawn("myapp.elf")` to `files/init.lua` (or to
  `/init.lua` in the overlay, to change a running machine without rebuilding).
- Host tests (no QEMU): `ninja check`. The tree must stay green before a
  commit.
