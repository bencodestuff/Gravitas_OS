#pragma once

#include <stddef.h>
#include <grv/syscall.h>

/* The one place the FILE layer (file.c) touches the kernel. Everything else in
   the stream code is pure buffering logic, which is why tools/test_stdio.c can
   compile file.c unchanged against fake implementations of these. Not a public
   header. */

int  __stdio_open(const char* path, int flags);   /* fd, or -1 with errno */
long __stdio_read(int fd, void* buf, size_t n);
long __stdio_write(int fd, const void* buf, size_t n);
long __stdio_lseek(int fd, long off, int whence);
int  __stdio_close(int fd);

/* The console: stdin reads keys, stdout/stderr write. Unbuffered. The read
   echoes what it consumed — the kernel console does not echo, so each reader
   does. */
int  __stdio_putc(char c);
int  __stdio_getc(void);
