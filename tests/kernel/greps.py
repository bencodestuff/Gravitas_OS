#!/usr/bin/env python3
"""grep: line selection on a file and on the read end of a pipe.

The matching engine has a host test (tools/test_grep.cpp) for read-boundary
splits and the print window. What only a guest can show is the wiring: the
program is in the image, options reach it as argv words, a file argument is
opened and read in chunks, and stdin is a pipe end when the shell puts it in a
pipeline.

    python3 tests/run.py greps
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from harness import run_module, test


@test
def grep_reads_a_file(g):
    """Options are argv words; the file is read to its last newline."""
    g.shell("run grep.elf Gravitas hello.txt")
    g.wait_for(r"^Hello from the Gravitas tar filesystem!$")

    # -n numbers from one, using the source line number not the match count.
    g.shell("run grep.elf -n syscalls hello.txt")
    g.wait_for(r"^2:If cat can read this, file syscalls work\.$")

    # -c prints the number of selected lines and nothing else.
    g.shell("run grep.elf -c Gravitas hello.txt")
    g.wait_for(r"^1$")

    # -v inverts the selection, so the other line survives.
    g.shell("run grep.elf -v Gravitas hello.txt")
    g.wait_for(r"^If cat can read this, file syscalls work\.$")


@test
def grep_reads_a_pipe(g):
    """In a pipeline grep reads stdin; the producer's bytes are not reordered."""
    g.shell("cat hello.txt | run grep.elf cat")
    g.wait_for(r"^If cat can read this, file syscalls work\.$")

    # A directory listing is a second, differently shaped producer.
    g.shell("ls /bin | run grep.elf grep")
    g.wait_for(r"^grep\.elf$")

    # A pattern that selects nothing must print nothing at all, so the only
    # output between the two commands is the shell's own completion marker.
    g.shell("run grep.elf no-such-word hello.txt")
    g.wait_after("run grep.elf no-such-word", "program finished", timeout=30.0)


if __name__ == "__main__":
    run_module(__file__)
