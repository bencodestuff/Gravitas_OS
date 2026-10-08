#include <errno.h>
#include <string.h>

// The message for each errno in <errno.h>. These are string literals, so the
// result is stable and no static buffer is needed (strerror's usual contract).
char* strerror(int err) {
    switch (err) {
    case 0:       return (char*)"Success";
    case EPERM:   return (char*)"Operation not permitted";
    case ENOENT:  return (char*)"No such file or directory";
    case EIO:     return (char*)"Input/output error";
    case EBADF:   return (char*)"Bad file descriptor";
    case EAGAIN:  return (char*)"Resource temporarily unavailable";
    case ENOMEM:  return (char*)"Out of memory";
    case EACCES:  return (char*)"Permission denied";
    case EFAULT:  return (char*)"Bad address";
    case EEXIST:  return (char*)"File exists";
    case ENOTDIR: return (char*)"Not a directory";
    case EISDIR:  return (char*)"Is a directory";
    case EINVAL:  return (char*)"Invalid argument";
    case EMFILE:  return (char*)"Too many open files";
    case ENOSPC:  return (char*)"No space left on device";
    case EROFS:   return (char*)"Read-only filesystem";
    case ENAMETOOLONG: return (char*)"File name too long";
    case ENOSYS:  return (char*)"Function not implemented";
    default:      return (char*)"Unknown error";
    }
}
