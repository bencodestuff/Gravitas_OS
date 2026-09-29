#include <stdio.h>

#include <grv/syscall.h>

int putchar(int ic) {
    grv_putc((char)ic);
    return (unsigned char)ic;
}
