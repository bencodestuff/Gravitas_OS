#include <stdlib.h>

// The environment: a NULL-terminated array of "NAME=VALUE" strings that crt0
// points at the block the kernel laid out after argv (see
// src/kernel/core/env.cpp). Programs only read it; nothing here writes it.

extern char** environ;

char* getenv(const char* name) {
    if (name == 0 || environ == 0 || name[0] == '\0') return 0;

    int n = 0;
    while (name[n]) n++;

    for (char** e = environ; *e != 0; e++) {
        const char* s = *e;
        int i = 0;
        while (i < n && s[i] == name[i]) i++;
        if (i == n && s[n] == '=') return (char*)(s + n + 1);
    }
    return 0;
}
