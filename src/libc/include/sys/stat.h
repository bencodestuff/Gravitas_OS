#ifndef _SYS_STAT_H
#define _SYS_STAT_H 1

#include <sys/cdefs.h>
#include <sys/types.h>
#include <time.h>

/* File types and the permission bits a ported program tests for. */
#define S_IFMT  0170000
#define S_IFDIR 0040000
#define S_IFREG 0100000

#define S_ISUID 0004000
#define S_ISGID 0002000
#define S_ISVTX 0001000
#define S_IRWXU 0000700
#define S_IRUSR 0000400
#define S_IWUSR 0000200
#define S_IXUSR 0000100
#define S_IRWXG 0000070
#define S_IRWXO 0000007

#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)

/* What the platform knows: type (from st_mode), size, inode and mtime.
   Ownership and link counts are not modelled, so those fields read as zero. */
struct stat {
    dev_t     st_dev;
    ino_t     st_ino;
    nlink_t   st_nlink;
    mode_t    st_mode;
    uid_t     st_uid;
    gid_t     st_gid;
    off_t     st_size;
    time_t    st_atime;
    time_t    st_mtime;
    time_t    st_ctime;
    blksize_t st_blksize;
    blkcnt_t  st_blocks;
};

__BEGIN_DECLS

int stat(const char* __restrict, struct stat* __restrict);
int mkdir(const char* path, mode_t mode);   /* 'mode' is ignored */

__END_DECLS

#endif
