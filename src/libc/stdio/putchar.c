#include <stdio.h>

/* putchar writes stdout, not "the console": when the shell redirects fd 1 the
   character follows it, exactly as printf's output does. */
int putchar(int ic) {
    return fputc(ic, stdout);
}
