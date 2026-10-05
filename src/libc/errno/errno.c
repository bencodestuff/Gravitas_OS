#include <errno.h>

/* Userland is single-threaded, so a plain global is the whole implementation
   (no per-thread errno location). */
int errno = 0;