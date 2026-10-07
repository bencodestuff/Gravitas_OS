#pragma once

// Kernel-side error codes. The syscall ABI reports failure as a negated
// code; userland sees the same numbers in <errno.h> (libc), and
// docs/syscalls.md is the contract between the two.
//
// The values are the customary Unix numbers so that a ported program's
// expectations (ENOENT == 2, EAGAIN == 11, ...) hold.

namespace Errno {
    constexpr long EPERM   = 1;  // operation not permitted
    constexpr long ENOENT  = 2;  // no such file or directory / no such task
    constexpr long EIO     = 5;  // backend failure that has no better code
    constexpr long EBADF   = 9;  // bad file descriptor
    constexpr long EAGAIN  = 11; // would block (mailbox full / empty)
    constexpr long ENOMEM  = 12; // out of memory
    constexpr long EACCES  = 13; // permission denied
    constexpr long EFAULT  = 14; // bad address
    constexpr long EEXIST  = 17; // already exists
    constexpr long ENOTDIR = 20; // not a directory
    constexpr long EISDIR  = 21; // is a directory
    constexpr long EINVAL  = 22; // invalid argument
    constexpr long EMFILE  = 24; // per-task fd table full
    constexpr long ENOSPC  = 28; // no space left
    constexpr long EROFS   = 30; // read-only filesystem
    constexpr long ENAMETOOLONG = 36; // a name or path too long to represent
    constexpr long ENOSYS  = 38; // no such syscall (retired hole / unknown id)
}