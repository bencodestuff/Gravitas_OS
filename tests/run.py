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
"""
import glob
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CATEGORIES = ("kernel", "ui")


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


def main(argv):
    selectors = [s for s in argv[1:] if s]
    if os.environ.get("GROUPS"):
        selectors = [s for s in os.environ["GROUPS"].split(",") if s]

    groups = discover()
    if selectors:
        groups = [g for g in groups if matched(g, selectors)]
    if not groups:
        print("no test groups selected (have: %s)"
              % ", ".join(os.path.splitext(os.path.basename(g))[0] for g in discover()))
        return 1

    env = dict(os.environ)
    env["PYTHONPATH"] = os.path.join(ROOT, "tests") + os.pathsep + env.get("PYTHONPATH", "")

    results = []
    for g in groups:
        name = os.path.splitext(os.path.basename(g))[0]
        rc = subprocess.call([sys.executable, g], env=env)
        results.append((name, rc == 0))

    print("\n=== summary ===")
    for name, ok in results:
        print("  %-4s %s" % ("PASS" if ok else "FAIL", name))
    bad = [n for n, ok in results if not ok]
    print("\n%d/%d groups passed" % (len(results) - len(bad), len(results)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
