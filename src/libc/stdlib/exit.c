#include <stdlib.h>
#include <grv/syscall.h>

void exit(int code) {
    grv_exit(code);
}
