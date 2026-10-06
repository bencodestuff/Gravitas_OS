#include <sys/stat.h>
#include <grv/syscall.h>

// stat by path: the platform reports type and size, nothing else (no owner,
// no links, no timestamps), so the rest of the struct reads as zero.
int stat(const char* path, struct stat* st) {
    struct grv_stat gs;
    if (grv_stat(path, &gs) != 0) return -1;

    unsigned char* p = (unsigned char*)st;
    for (size_t i = 0; i < sizeof(*st); i++) p[i] = 0;

    st->st_mode = gs.is_dir ? (S_IFDIR | 0755) : (S_IFREG | 0644);
    st->st_size = (off_t)gs.size;
    st->st_nlink = 1;
    st->st_blksize = 512;
    return 0;
}

int mkdir(const char* path, mode_t mode) {
    (void)mode;
    return grv_mkdir(path);
}
