#!/usr/bin/env python3
"""Boot the machine over and over, looking for the one that does not come up.

The boot fault (see the roadmap) is intermittent and load-dependent: it appears
roughly once in fifty boots, and mostly when several guests are running at once.
A scenario cannot find that, so this is repetition instead.

It is deliberately NOT part of `ninja test`: it is a minute of booting with
nothing to say unless something breaks. Run it as `ninja soak`, or directly:

    tests/soak.py                 # 40 boots, as many at once as memory allows
    tests/soak.py -n 200 -j 8     # longer, harder

Every boot is a plain QEMU with the same devices the suite uses by default (the
IDE controller and one data disk), and the whole test is that it reaches the
shell prompt. That is the realistic machine: it is also the configuration a
drive-select bug hid in, because a second drive changes what the driver has to
do. A boot that does not gets its serial log kept in
test_artifacts/soak/, because that log is the only evidence there is.
"""
import argparse
import concurrent.futures
import os
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ART = os.path.join(ROOT, "test_artifacts", "soak")

# The shell prints this once it is up and listing the root. Reaching it means the
# whole boot worked: kernel, filesystems, spawn, console.
MARKER = "type 'help'"

# Generous: a healthy boot is ~4s, so anything near this is a hang, not slowness.
BOOT_TIMEOUT = 40.0


def default_jobs():
    """As many guests at once as memory allows, since load is what provokes it."""
    try:
        for line in open("/proc/meminfo"):
            if line.startswith("MemAvailable:"):
                mb = int(line.split()[1]) // 1024
                return max(1, min(8, mb // 800))
    except (OSError, ValueError):
        pass
    return 2


def boot(index):
    """One boot. Returns (index, ok, seconds)."""
    tmp = tempfile.mkdtemp(prefix="gravitas-soak-")
    log = os.path.join(tmp, "serial.log")
    try:
        accel = ["-enable-kvm"] if os.path.exists("/dev/kvm") else []
        qemu = subprocess.Popen([
            "qemu-system-x86_64", "-cdrom", os.path.join(ROOT, "gravitas.iso"),
            "-m", "512M", "-machine", "q35", "-display", "none", "-no-reboot",
            "-device", "piix4-ide,id=ideb",
            "-drive", "if=none,id=disk0,file=%s,format=raw,snapshot=on"
                      % os.path.join(ROOT, "iso_root", "disk.img"),
            "-device", "ide-hd,drive=disk0,bus=ideb.0",
            *accel, "-serial", "file:" + log,
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        start = time.time()
        ok = False
        while time.time() - start < BOOT_TIMEOUT:
            if qemu.poll() is not None:
                break
            try:
                if MARKER in open(log, errors="ignore").read():
                    ok = True
                    break
            except OSError:
                pass
            time.sleep(0.05)
        elapsed = time.time() - start

        qemu.terminate()
        try:
            qemu.wait(timeout=5)
        except subprocess.TimeoutExpired:
            qemu.kill()

        if not ok:
            os.makedirs(ART, exist_ok=True)
            shutil.copy(log, os.path.join(ART, "boot-%04d.log" % index))
        return index, ok, elapsed
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("-n", "--boots", type=int, default=40,
                    help="how many boots (default 40)")
    ap.add_argument("-j", "--jobs", type=int, default=0,
                    help="how many at once (default: as memory allows)")
    args = ap.parse_args()

    for need in ("gravitas.iso", "iso_root/disk.img"):
        if not os.path.exists(os.path.join(ROOT, need)):
            print("soak: %s missing - run 'ninja iso' first" % need)
            return 2
    if shutil.which("qemu-system-x86_64") is None:
        print("soak: qemu-system-x86_64 not installed")
        return 2

    jobs = args.jobs or default_jobs()
    print("soak: %d boots, %d at a time" % (args.boots, jobs), flush=True)

    start = time.time()
    failed = []
    done = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        futures = [pool.submit(boot, i) for i in range(args.boots)]
        for f in concurrent.futures.as_completed(futures):
            index, ok, secs = f.result()
            done += 1
            if not ok:
                failed.append(index)
                print("  boot %4d FAILED after %.1fs (log kept)" % (index, secs),
                      flush=True)
            elif done % 10 == 0 or done == args.boots:
                print("  %d/%d ok (%.0fs)" % (done, args.boots, time.time() - start),
                      flush=True)

    print("soak: %d/%d boots came up in %.0fs"
          % (args.boots - len(failed), args.boots, time.time() - start))
    if failed:
        print("soak: %d did not; logs in test_artifacts/soak/ (boots %s)"
              % (len(failed), ", ".join(str(i) for i in failed[:10])))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
