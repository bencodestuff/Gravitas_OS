# Debugging Gravitas

The kernel writes everything to the serial port: the boot log, faults, the F12
dump and the monitor's output. That log is the first place to look, and usually
the only one. When it is not enough, the kernel and every program are built with
debug symbols, so GDB can attach to a booted (or frozen) machine.

## Where the evidence is

Each guest group keeps its last run in `test_artifacts/<group>/`:

| file | what it is |
|---|---|
| `serial.log` | every byte the guest wrote to the serial port, from the first boot message to the point the guest closed or the test failed |
| `<tag>.ppm` | a screendump a failing test took at the moment it failed |

Group names are the module names in `tests/kernel/` and `tests/ui/`, so a
failure in `tests/kernel/init.py` is in `test_artifacts/init/serial.log`. `soak`
keeps the logs of boots that did not come up in `test_artifacts/soak/`; that log
is the only evidence of an intermittent boot hang. The directory is ignored by
git.

The log has carriage returns in it. Read it with `less -R`, or remove them:

```sh
tr -d '\r' < test_artifacts/init/serial.log | less
grep -n "FAULT\|\[dbg\]\|\[Ext2\]\|\[VFS\]" test_artifacts/init/serial.log
```

A failing test also prints the tail of the log it was reading, so start with
the test output; the artifact is for the context around it.

## Run the machine

```sh
ninja iso
ninja run
```

`ninja run` boots `gravitas.iso` with q35, 512 MB, the SDL display and the serial
multiplexed onto the terminal, using KVM when `/dev/kvm` exists. The window
takes the keyboard and mouse; the terminal shows the serial log and accepts
serial input.

`ctrl-a c` switches the multiplexed terminal to the QEMU monitor; `ctrl-a c`
again switches back to the guest serial. The monitor is useful without a
debugger:

```
(qemu) info registers
(qemu) info block
(qemu) info qtree
(qemu) screendump /tmp/guest.ppm
(qemu) quit
```

`quit` kills the machine immediately, without flushing the page cache or the
disk. That is useful when testing what a power cut does, and the wrong way to
leave a machine whose filesystem you want intact.

The equivalent manual command, when you need to add flags:

```sh
qemu-system-x86_64 -cdrom gravitas.iso -m 512M -machine q35 \
  -serial mon:stdio -display sdl
```

The ISO ships `disk.img` as a Limine module, so it boots without a drive. To
exercise the ATA driver the way the test harness does, attach a real drive.
q35's own SATA controller is AHCI, which has no legacy task file, so add a
PIIX4 IDE controller for `ata.cpp` to drive:

```sh
qemu-system-x86_64 -cdrom gravitas.iso -m 512M -machine q35 -display sdl \
  -serial mon:stdio \
  -device piix4-ide,id=ideb \
  -drive if=none,id=disk0,file=iso_root/disk.img,format=raw,snapshot=on \
  -device ide-hd,drive=disk0,bus=ideb.0
```

`snapshot=on` keeps the host image as the starting state for the next boot.

## Attach GDB

The kernel and userland objects are compiled with `-g`, and `bin/gravitas.elf`
carries the kernel symbols. QEMU's `-s` opens a GDB stub on TCP port 1234; `-S`
freezes the CPU at reset so the debugger is attached before Limine runs:

```sh
qemu-system-x86_64 -cdrom gravitas.iso -m 512M -machine q35 \
  -serial mon:stdio -display sdl -s -S
```

In another terminal:

```sh
gdb bin/gravitas.elf
(gdb) target remote :1234
(gdb) hbreak kmain
(gdb) continue
```

A hardware breakpoint (`hbreak`) is the one that works for the first stop: at
reset the kernel's higher-half address is not mapped, so GDB cannot write a
software breakpoint there, but the CPU compares the breakpoint address against
RIP and hits `kmain` after Limine has entered long mode. `bt`, `info registers`
and `x/10i $pc` work as usual once it stops. Omit `-S` to attach to a machine
that is already booting or running; GDB's interrupt then stops it.

The kernel runs at `0xffffffff80000000` and above. A faulting RIP in userland is
in a program's own load base (each program has one, assigned by
`configure.py`); the fault dump names the function even though GDB only has the
kernel's symbols loaded.

If the machine boots but a program misbehaves, the fastest loop is usually the
guest itself: run it from the shell, read the serial log, and use `mon` or F12
rather than single-stepping the kernel.

## Read a fault dump

A user-mode fault looks like this in the serial log:

```
=== FAULT: Page fault in task 'crash.elf' (task #7) ===
[fault] vector 14 Page fault
[fault] RIP=0x0000000002000123 (crash(int, char**)+0x2C)
[fault] CS=0x23 RFLAGS=0x10202
[fault] CR2=0x0000000000000000 (not-present write user)
[fault] RAX=0x0000000000000000 RBX=0x0000000000000001 ...
```

- The `vector` line names the exception. `14` is a page fault, `13` a general
  protection fault, `8` a double fault, `6` an invalid opcode.
- `RIP` is the faulting instruction, followed by the function it belongs to and
  the offset into it. The symbol table is built from the kernel and every
  program by `tools/mksyms.py` at ISO time (about 6000 functions), so rebuild
  with `ninja iso` if a name is missing or points at the previous build.
- For a page fault, `CR2` is the address that was touched and the flags say why:
  present or not-present, a read or a write, from user or kernel mode, and
  whether it was an instruction fetch. A user-mode fault on the unmapped page
  below a task's stack is recognised and reported as
  `stack overflow detected (user stack guard page)`.
- For a double fault, read the `the fault before it` line first: the double
  fault itself is the consequence, not the cause.
- The register lines are the interrupted context, so they are the values the
  faulting code had, not the handler's.

A user-mode fault follows the fault contract: the task is killed, the rest of
the machine carries on, and the shell prompt returns. The last fault stays
available to userland (`FAULT_LAST`, printed by `mon`). A kernel-mode fault
dumps the same information and halts the CPU, because there is nothing safe to
return to.

## F12 and F11

These are handled by the kernel's keyboard driver, not by the window manager,
so they work whatever has focus:

- **F12** dumps the task table and the compositor counters to the kernel console
  and the serial log, without stopping anything. Each task line has its id,
  name, state (`RUNNING`, `ready`, `sleeping`, `blocked-on-task-N`) and CR3.
  The `[dbg] compositor:` line reports presents, full paints, damage rectangles,
  painted pixels and whether the double buffer is on.
- **F11** brings the kernel console above the windows, and puts it back. Use it
  when a window covers a fault dump or the boot log on screen.

## The mon app

`mon.elf` is the userland half of the debugger. Run it from the shell:

```
run mon
```

It prints a snapshot and exits, so run it again for a fresh one. It uses only
the introspection syscalls (`SYSINFO`, `TASK_LIST`, `SURFACE_LIST`,
`FAULT_LAST`) and reports:

- ticks, live task and surface counts, and the pointer position;
- free and total physical frames, and heap used, peak and total;
- compositor presents, full paints, rectangles and pixels;
- the task table with ids, names, states and CR3s;
- the surface registry with owner, size, position and z-order;
- the last fault, including the task it happened in.

Use F12 for the state at the instant a key is pressed and `mon` for a readable
snapshot you can scroll through or copy. Neither changes kernel state.

## When the boot hangs

Find the last line in the serial log. A boot that stops after `[VFS] Mounted /`
is waiting on the next subsystem named in the boot path, and one that reaches
the desktop without a shell prompt is waiting on init or on a task it started.
`ninja soak` is the tool for a hang that only happens under load: it boots
repeatedly and
keeps the log of each boot that did not reach the shell prompt. With QEMU's
monitor, `info status` says whether the CPU is still running; with GDB attached,
interrupt it and take a backtrace. See `tests/README.md` for the harness rules
that make an intermittent failure reproducible.
