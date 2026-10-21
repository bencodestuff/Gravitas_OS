#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <grv/syscall.h>

// df [path...] — how much room there is, and what kind of filesystem it is.
//
// Worth having because the caps are otherwise invisible: ramfs will refuse a
// write with ENOSPC long before the machine is out of memory, and nothing said
// so until you hit it. A read-only filesystem reports nothing free, which is
// the honest answer — the medium may be large, but nothing can be written.
//
// The inode count gets its own line, and only when the backend knows it: a disk
// runs out of inodes long before it runs out of bytes, and "no space" from an
// entry limit is otherwise indistinguishable from one from a full medium. A
// backend without an inode limit (ramfs) reports 0 and prints nothing, and the
// one-line summary keeps its shape either way.

static int show(const char* path) {
    struct grv_statfs fs;
    if (grv_statfs(path, &fs) != 0) {
        printf("df: %s: %s\n", path, strerror(errno));
        return 1;
    }

    printf("%-14s %-9s %8luK %8luK  %s\n", path, fs.type,
           fs.total / 1024, fs.free / 1024,
           fs.writable ? "" : "(read-only)");

    if (fs.inodes_total != 0)
        printf("Inodes: %lu/%lu used\n",
               fs.inodes_total - fs.inodes_free, fs.inodes_total);
    return 0;
}

int main(int argc, char** argv) {
    printf("%-14s %-9s %8s %8s\n", "Path", "Type", "Total", "Free");

    if (argc > 1) {
        int failed = 0;
        for (int i = 1; i < argc; i++) failed |= show(argv[i]);
        return failed;
    }

    static char cwd[64];
    grv_getcwd(cwd, sizeof(cwd));
    return show(cwd);
}
