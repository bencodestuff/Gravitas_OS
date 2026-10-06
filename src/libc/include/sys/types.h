#ifndef _SYS_TYPES_H
#define _SYS_TYPES_H 1

#include <stddef.h>

/* The POSIX-ish integer types. Gravitas is LP64, so the widths are the
   familiar ones. */
typedef long           ssize_t;
typedef long           off_t;
typedef long           pid_t;
typedef long           time_t;
typedef unsigned int   mode_t;
typedef unsigned long  ino_t;
typedef unsigned long  dev_t;
typedef unsigned long  nlink_t;
typedef unsigned int   uid_t;
typedef unsigned int   gid_t;
typedef long           blksize_t;
typedef long           blkcnt_t;

#endif
