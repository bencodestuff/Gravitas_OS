#include <stdio.h>
#include <grv/syscall.h>

// remove() and rename() are the stdio spellings of the VFS operations. The
// grv_ wrappers already set errno and return -1, which is the stdio contract
// too. remove() handles empty directories as well, since the backend unlinks
// them.
int remove(const char* path) {
    return grv_unlink(path);
}

int rename(const char* from, const char* to) {
    return grv_rename(from, to);
}
