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
    printf("Gravitas commands (commands are programs from the VFS):\n");
    printf("  ls [path]   - list a directory (default: cwd)\n");
    printf("  cat <file>  - print a file's contents\n");
    printf("  cd <dir>    - change directory (builtin)\n");
    printf("  pwd         - print working directory (builtin)\n");
    printf("  run <prog>  - spawn a program by exact name\n");
    printf("  help        - this list\n");
}

// --- Shell builtins that mutate the shell's OWN state ---
// cd must be a builtin (it changes this process's CWD — a child program
// could never do it), and pwd reads it. Everything else is a program.

static void cmd_cd(char* line) {
    while (*line == ' ') line++;
    if (*line == '\0') { printf("cd: missing directory\n"); return; }
    if (grv_chdir(line) != 0) {
        printf("cd: no such directory: %s\n", line);
    }
}

static void cmd_pwd() {
    static char cwd[64];
    grv_getcwd(cwd, sizeof(cwd));
    printf("%s\n", cwd);
}

// Startup listing: shows the CWD (the shell boots at "/")

static void execute(char* line) {
    while (*line == ' ') line++;
    if (*line == '\0') return;
    if (str_eq(line, "help")) { cmd_help(); return; }
    if (str_eq(line, "pwd")) { cmd_pwd(); return; }
    if (line[0] == 'c' && line[1] == 'd' && (line[2] == ' ' || line[2] == '\0')) {
        cmd_cd(line + 2);
        return;
    }

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

    // Startup listing: shows the CWD (the shell boots at "/")
    char* listbuf = (char*)malloc(2048);
    grv_readdir(".", listbuf, 2048);
    printf("[shell] cwd contents:\n%s", listbuf);
    printf("[shell] type 'help'\n");

    static char line[128];
    while (1) {
        // Prompt shows the CWD
        static char cwd[64];
        grv_getcwd(cwd, sizeof(cwd));
        printf("\n%s > ", cwd);

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
