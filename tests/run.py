#!/usr/bin/env python3
"""Run the Gravitas guest test groups.

Each group (tests/<category>/<group>.py) gets its own QEMU boot, so a group
that wedges or crashes the guest cannot hide the others. Selectors on the
command line — or in GROUPS=a,b — match a group name, a category, or any
substring of the path.

    tests/run.py                 # every group
    tests/run.py kernel          # just the kernel groups
    tests/run.py windows editor  # two named groups
    GROUPS=ui tests/run.py       # just the ui groups

Groups boot in parallel (-j, default 4). Each one is an independent QEMU, and
the output is buffered per group so a run stays readable: a group's lines are
printed together when it finishes.
"""
import concurrent.futures
import glob
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CATEGORIES = ("kernel", "ui")


def default_jobs(n):
    """How many groups to boot at once.

    Each guest wants 512MB, and the boots are CPU-heavy in bursts, so this is
    bounded by memory rather than by cores. On a machine with less to spare it
    degrades to fewer (or one) instead of swapping, which is what makes a
    parallel run *slow* and flaky rather than fast.
    """
    try:
        for line in open("/proc/meminfo"):
            if line.startswith("MemAvailable:"):
                mb = int(line.split()[1]) // 1024
                return max(1, min(4, mb // 800, n))
    except (OSError, ValueError):
        pass
    return 2


def discover():
    groups = []
    for cat in CATEGORIES:
        groups += sorted(glob.glob(os.path.join(ROOT, "tests", cat, "*.py")))
    return groups


def matched(group, selectors):
    rel = os.path.relpath(group, ROOT)
    name = os.path.splitext(os.path.basename(group))[0]
    cat = os.path.basename(os.path.dirname(group))
    return any(s in (name, cat) or s in rel for s in selectors)


def parse(argv, ngroups):
    """Split -j N / -jN out of the selectors."""
    jobs = default_jobs(ngroups)
    selectors = []
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "-j" and i + 1 < len(argv):
            jobs = max(1, int(argv[i + 1]))
            i += 2
        elif a.startswith("-j") and len(a) > 2:
            jobs = max(1, int(a[2:]))
            i += 1
        else:
            if a:
                selectors.append(a)
            i += 1
    if os.environ.get("GROUPS"):
        selectors = [s for s in os.environ["GROUPS"].split(",") if s]
    return jobs, selectors


def main(argv):
    groups = discover()
    jobs, selectors = parse(argv[1:], len(groups))

    if selectors:
        groups = [g for g in groups if matched(g, selectors)]
    if not groups:
        print("no test groups selected (have: %s)"
              % ", ".join(os.path.splitext(os.path.basename(g))[0] for g in discover()))
        return 1

    env = dict(os.environ)
    env["PYTHONPATH"] = os.path.join(ROOT, "tests") + os.pathsep + env.get("PYTHONPATH", "")

    def run_one(group):
        name = os.path.splitext(os.path.basename(group))[0]
        fd, path = tempfile.mkstemp(prefix="gravitas-group-", suffix=".log")
        with os.fdopen(fd, "wb") as out:
            rc = subprocess.call([sys.executable, group], env=env,
                                 stdout=out, stderr=subprocess.STDOUT)
        return name, rc == 0, path

    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        futures = [pool.submit(run_one, g) for g in groups]
        for fut in concurrent.futures.as_completed(futures):
            name, ok, path = fut.result()
            with open(path, errors="replace") as f:   # a group's lines, together
                print(f.read(), end="", flush=True)
            os.unlink(path)
            results.append((name, ok))

    results.sort()
    print("\n=== summary ===")
    for name, ok in results:
        print("  %-4s %s" % ("PASS" if ok else "FAIL", name))
    bad = [n for n, ok in results if not ok]
    print("\n%d/%d groups passed" % (len(results) - len(bad), len(results)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
