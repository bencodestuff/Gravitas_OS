# The test suite

Two halves, one command each:

    ninja check     host tests only — no QEMU, ~2s, most regressions
    ninja test      host tests + the guest groups (~40s)
    ninja test-kernel / test-ui    a subset of the groups
    GROUPS=apps ninja test         one group (or `tests/run.py apps`)

`ninja check` runs `tests/host/run.sh`: the libc pieces that build and run
natively (buffer, string, stdio, time, printf, libm) plus `tools/check_syscalls.py`,
which checks the syscall ABI against its documentation. Prefer it while working;
run the guest groups before a commit.

## Guest groups

A *group* is a Python module that registers tests with `@test`. Running it boots
one guest and runs the tests in order, so a group that wedges or crashes the
machine cannot hide the others.

    tests/kernel/console.py     the boot log, the console, the shell's echo
    tests/kernel/contracts.py   surfaces and the compositor, on unmanaged surfaces
    tests/kernel/faults.py      the fault contract (the kernel kills the task)
    tests/kernel/lua.py         the interpreter, REPL and scripts
    tests/kernel/power.py       leaving: poweroff and reset end the guest
    tests/kernel/tasks.py       scheduling, IPC, the fd/VFS surface, mappings
    tests/ui/windows.py         WM geometry and lifecycle
    tests/ui/apps.py            the apps and the IDE (the slowest group)

Groups boot **in parallel** (`tests/run.py -j N`; the default is as many as the
machine's free memory holds, capped at 4 — each guest wants 512MB, and a machine
that swaps makes a parallel run slower *and* flakier, not faster). Output is
buffered per group and printed when it finishes, so a run still reads
top-to-bottom.

## Adding a test

    @test
    def my_thing(g):
        g.shell("run mytest.elf")
        g.wait_for("mytest: all checks pass")

`g` is a `Guest`: a booted machine with an HMP monitor socket (keys, pointer,
screendumps) and a QMP socket (the wheel, which the HMP cannot express). Put a
group in `tests/kernel/` or `tests/ui/` and it is picked up automatically.

The rules that keep it from being flaky:

  * **Wait for the log, never a fixed sleep.** `wait_for` / `wait_count` /
    `wait_more` anchor on the serial log; a wedged step then fails on its
    timeout instead of passing on a lucky sleep. Where a test needs "the guest
    has painted", use `await_change` (dump until the region differs) rather than
    sleeping and hoping.
  * **Anchor repeated waits on a count.** `wait_for` matches the first occurrence
    in the whole log, so waiting for a string that is already there returns
    immediately. `n = g.count(pat); ...; g.wait_more(pat, n + 1)` is the safe
    shape.
  * **A shell hosted by a terminal prints into its window, not the log.** Those
    tests have to sync on something else — a spawn, an exit, or a repaint.
  * The harness types at a paced rate, because the guest's PS/2 controller drops
    keys that arrive faster than its IRQ handler drains them. `PACE` is a
    trade-off: 40ms is conservative, 30ms is what the suite runs at, and 15ms
    drops keys as soon as the machine is busy.

`test_artifacts/<group>/` keeps each run's serial log and any screendumps (a
failing test dumps extra ones for the moment it failed).
