#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <grv/syscall.h>

// Gravitas userland shell — a pure program spawner.
// Built against the libc: printf/strlen/malloc + grv syscall wrappers.
// Entry: main(argc, argv) via crt0 (not _start).

static bool str_eq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == *b;
}

static bool str_ends_with_elf(const char* s) {
    size_t len = strlen(s);
    if (len < 4) return false;
    return s[len-4] == '.' && s[len-3] == 'e' && s[len-2] == 'l' && s[len-1] == 'f';
}

static void cmd_help() {
    printf("Gravitas commands (commands are programs from the tarfs):\n");
    printf("  ls          - list files in the tarfs\n");
    printf("  cat <file>  - print a file's contents\n");
    printf("  run <prog>  - spawn a program by exact name\n");
    printf("  help        - this list\n");
}

static void execute(char* line) {
    while (*line == ' ') line++;
    if (*line == '\0') return;
    if (str_eq(line, "help")) { cmd_help(); return; }

    // Split into words
    static char* words[8];
    static char storage[8][64];
    int nwords = 0;
    char* p = line;
    while (*p && nwords < 8) {
        while (*p == ' ') p++;
        if (*p == '\0') break;
        int i = 0;
        while (*p && *p != ' ' && i < 63) storage[nwords][i++] = *p++;
        storage[nwords][i] = '\0';
        words[nwords] = storage[nwords];
        nwords++;
    }
    if (nwords == 0) return;

    // Pick the program: 'run' takes an exact tarfs name, everything else
    // gets ".elf" appended ('ls' -> 'ls.elf').
    const char* prog;
    int arg_start;
    static char progbuf[64];
    if (str_eq(words[0], "run")) {
        if (nwords < 2) { printf("run: missing program name\n"); return; }
        prog = words[1];
        arg_start = 2;
    } else {
        if (str_ends_with_elf(words[0])) {
            prog = words[0];
        } else {
            // progbuf = words[0] + ".elf"
            int len = 0;
            const char* w = words[0];
            while (*w && len < 58) progbuf[len++] = *w++;
            progbuf[len++] = '.'; progbuf[len++] = 'e';
            progbuf[len++] = 'l'; progbuf[len++] = 'f';
            progbuf[len] = '\0';
            prog = progbuf;
        }
        arg_start = 1;
    }

    // Build argv: argv[0] = program name, argv[1..] = extra words
    const char* argv[9];
    int argc = 0;
    argv[argc++] = prog;
    for (int i = arg_start; i < nwords && argc < 8; i++) {
        argv[argc++] = words[i];
    }

    // Spawn blocks us until the child exits
    long pid = grv_spawn(prog, argv, argc);
    if (pid == -1) {
        printf("unknown command, try 'help'\n");
        return;
    }
    printf("[shell] program finished\n");
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    printf("[shell] Gravitas userland shell v0.2 (libc)\n");

    // Startup listing: proves tarfs + list syscall work end to end
    char* listbuf = (char*)malloc(2048);
    grv_readdir("/bin", listbuf, 2048);
    printf("[shell] tarfs contents:\n%s", listbuf);
    printf("[shell] type 'help'\n");

    static char line[128];
    while (1) {
        printf("\n> ");

        int len = 0;
        while (1) {
            char c = grv_getc();

            putchar(c); // echo

            if (c == '\n') { line[len] = '\0'; break; }
            if (c == '\b') { if (len > 0) len--; continue; }
            if (len < 126) line[len++] = c;
        }
        execute(line);
    }
}
