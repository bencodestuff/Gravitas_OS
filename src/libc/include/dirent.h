#ifndef _DIRENT_H
#define _DIRENT_H 1

#include <sys/cdefs.h>
#include <sys/types.h>

/* d_type values (the common subset). */
#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
#define DT_REG     8
#define DT_LNK    10
#define DT_SOCK   12

struct dirent {
    ino_t         d_ino;      /* always 0: there are no inode numbers */
    unsigned char d_type;
    char          d_name[256];
};

typedef struct __dirstream DIR;

__BEGIN_DECLS

/* opendir snapshots the directory listing; readdir walks it. Names longer
   than the buffer are truncated. */
DIR* opendir(const char* name);
struct dirent* readdir(DIR* dirp);
int closedir(DIR* dirp);

__END_DECLS

#endif
