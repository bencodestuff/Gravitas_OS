#include <stdlib.h>

// There is no environment block yet: the kernel does not hand one to a task, so
// every lookup misses. An honest stub (always NULL) rather than a guess; Lua's
// os.getenv returns nil.
char* getenv(const char* name) {
    (void)name;
    return 0;
}
