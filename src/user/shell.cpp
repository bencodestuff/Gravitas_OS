#include <stdint.h>

// Gravitas userland shell.
// Runs in ring 3, talks to the kernel only through int 0x80 syscalls.
//
// Syscall ABI (matches kernel interrupt_dispatcher):
//   0 = putc          rdi = char
//   1 = getc          -> rax = char (0 = no key pressed)
//   2 = list files    rdi = buf, rsi = max -> names '\n'-separated
//   3 = read file     rdi = name, rsi = buf, rdx = max -> rax = bytes (or -1)
//   4 = spawn         (not implemented yet — needs task exit/wait)
//
// Returns the char, or 0 if buffer is empty

static void sys_putc(char c) {
    asm volatile(
        "mov $0, %%rax\n"
        "mov %0, %%rdi\n"
        "int $0x80\n"
        : : "r"((uint64_t)c) : "rax", "rdi"
    );
}

static char sys_getc() {
    uint64_t ret;
    asm volatile(
        "mov $1, %%rax\n"
        "int $0x80\n"
        : "=a"(ret)
        :
        : "memory"
    );
    return (char)ret;
}

// Generic 2-arg syscall (rdi, rsi), result in rax
static uint64_t syscall2(uint64_t id, uint64_t a, uint64_t b) {
    uint64_t ret;
    asm volatile(
        "int $0x80\n"
        : "=a"(ret)
        : "a"(id), "D"(a), "S"(b)
        : "memory"
    );
    return ret;
}

// Generic 3-arg syscall (rdi, rsi, rdx), result in rax
static uint64_t syscall3(uint64_t id, uint64_t a, uint64_t b, uint64_t c) {
    uint64_t ret;
    asm volatile(
        "int $0x80\n"
        : "=a"(ret)
        : "a"(id), "D"(a), "S"(b), "d"(c)
        : "memory"
    );
    return ret;
}

static void print(const char* s) {
    while (*s) sys_putc(*s++);
}

// --- tiny local string helpers ---
static bool str_eq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == *b;
}

static void cmd_help() {
    print("Gravitas commands (commands are programs from the tarfs):\n");
    print("  ls          - list files in the tarfs\n");
    print("  cat <file>  - print a file's contents\n");
    print("  run <prog>  - spawn a program by exact name\n");
    print("  help        - this list\n");
}

// Split a line into words (max 'max' words, 63 chars each)
static int split_words(char* line, char words[][64], int max) {
    int count = 0;
    while (*line && count < max) {
        while (*line == ' ') line++;
        if (*line == '\0') break;
        int i = 0;
        while (*line && *line != ' ' && i < 63) words[count][i++] = *line++;
        words[count][i] = '\0';
        count++;
    }
    return count;
}

// Copy src into dst (max 63 chars + NUL), returns length
static int str_copy(char* dst, const char* src) {
    int i = 0;
    while (src[i] && i < 63) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
    return i;
}

static bool str_ends_with_elf(const char* s) {
    while (*s) s++;
    if (s[-4] != '.' || s[-3] != 'e' || s[-2] != 'l' || s[-1] != 'f') return false;
    return true;
}

static void execute(char* line) {
    // Trim leading spaces
    while (*line == ' ') line++;

    if (str_eq(line, "")) return;
    if (str_eq(line, "help")) { cmd_help(); return; }

    static char words[8][64];
    int nwords = split_words(line, words, 8);
    if (nwords == 0) return;

    // Pick the program: 'run' takes an exact tarfs name, everything else
    // gets ".elf" appended ('ls' -> 'ls.elf').
    const char* prog;
    int arg_start;
    static char progbuf[64];
    if (str_eq(words[0], "run")) {
        if (nwords < 2) { print("run: missing program name\n"); return; }
        prog = words[1];
        arg_start = 2;
    } else {
        if (str_ends_with_elf(words[0])) {
            prog = words[0];
        } else {
            int len = str_copy(progbuf, words[0]);
            progbuf[len++] = '.';
            progbuf[len++] = 'e';
            progbuf[len++] = 'l';
            progbuf[len++] = 'f';
            progbuf[len] = '\0';
            prog = progbuf;
        }
        arg_start = 1;
    }

    // Build argv: argv[0] = program name, argv[1..] = extra words
    static char argstrings[8][64];
    static const char* argvp[9];
    int argc = 0;
    str_copy(argstrings[argc], prog);
    argvp[argc] = argstrings[argc];
    argc++;
    for (int i = arg_start; i < nwords && argc < 8; i++) {
        str_copy(argstrings[argc], words[i]);
        argvp[argc] = argstrings[argc];
        argc++;
    }

    // Spawn blocks us until the child exits
    uint64_t pid = syscall3(4, (uint64_t)prog, (uint64_t)argvp, argc);
    if (pid == (uint64_t)-1) {
        print("unknown command, try 'help'\n");
        return;
    }
    print("[shell] program finished\n");
}

extern "C" void _start() {
    print("[shell] Gravitas userland shell v0.1\n");

    // Startup listing: proves tarfs + list syscall work end to end
    static char listbuf[2048];
    syscall2(2, (uint64_t)listbuf, sizeof(listbuf));
    print("[shell] tarfs contents:\n");
    print(listbuf);
    print("[shell] type 'help'\n");

    static char line[128];
    while (1) {
        print("\n> ");

        int len = 0;
        while (1) {
            char c = sys_getc();
            if (c == 0) continue; // poll: no key yet (should block eventually)

            // Echo what was typed
            sys_putc(c);

            if (c == '\n') { line[len] = '\0'; break; }
            if (c == '\b') { if (len > 0) len--; continue; }
            if (len < 126) line[len++] = c;
        }
        execute(line);
    }
}
