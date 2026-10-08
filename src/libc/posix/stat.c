#include <sys/stat.h>
#include <grv/syscall.h>

// Fill a POSIX struct stat from the platform record: the type, the size, the
// inode, the permission bits and the mtime. Ownership and link counts are not
// modelled, so those fields read as zero.
static void fill_stat(struct stat* st, const struct grv_stat* gs) {
    unsigned char* p = (unsigned char*)st;
    for (size_t i = 0; i < sizeof(*st); i++) p[i] = 0;

    mode_t mode = (mode_t)gs->mode;
    if (mode == 0) mode = gs->is_dir ? 0755 : 0644;   // the filesystem's default
    st->st_mode = (gs->is_dir ? S_IFDIR : S_IFREG) | mode;
    st->st_ino = (ino_t)gs->ino;
    st->st_size = (off_t)gs->size;
    st->st_nlink = 1;
    st->st_mtime = (time_t)gs->mtime;
    st->st_blksize = 512;
}

// stat by path.
int stat(const char* path, struct stat* st) {
    struct grv_stat gs;
    if (grv_stat(path, &gs) != 0) return -1;
    fill_stat(st, &gs);
    return 0;
}

// fstat by descriptor: the same record for a file the caller already has open.
int fstat(int fd, struct stat* st) {
    struct grv_stat gs;
    if (grv_fstat(fd, &gs) != 0) return -1;
    fill_stat(st, &gs);
    return 0;
}

int mkdir(const char* path, mode_t mode) {
    (void)mode;
    return grv_mkdir(path);
}
