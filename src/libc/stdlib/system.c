#include <stdlib.h>

// No shell: the library cannot start a program, and there is no command
// interpreter to hand the string to. Returning -1 is the documented "cannot
// execute" answer (a NULL argument would be the "is a shell available" query).
int system(const char* command) {
    (void)command;
    return -1;
}
