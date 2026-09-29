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

static void cmd_cat(const char* name) {
    static char filebuf[4096];
    uint64_t n = syscall3(3, (uint64_t)name, (uint64_t)filebuf, sizeof(filebuf));
    if (n == (uint64_t)-1) {
        print("cat: no such file\n");
        return;
    }
    for (uint64_t i = 0; i < n; i++) sys_putc(filebuf[i]);
    sys_putc('\n');
}

static void cmd_help() {
    print("Gravitas userland commands:\n");
    print("  ls          - list files in the tarfs\n");
    print("  cat <file>  - print a file's contents\n");
    print("  run <prog>  - spawn a program from the tarfs\n");
    print("  help        - this list\n");
}

static void execute(char* line) {
    // Trim leading spaces
    while (*line == ' ') line++;

    if (str_eq(line, "")) return;
    if (str_eq(line, "help")) { cmd_help(); return; }

    if (line[0] == 'c' && line[1] == 'a' && line[2] == 't' && (line[3] == ' ' || line[3] == '\0')) {
        char* name = line + 3;
        while (*name == ' ') name++;
        if (*name == '\0') { print("cat: missing file name\n"); return; }
        cmd_cat(name);
        return;
    }

    if (line[0] == 'r' && line[1] == 'u' && line[2] == 'n' && (line[3] == ' ' || line[3] == '\0')) {
        char* name = line + 3;
        while (*name == ' ') name++;
        if (*name == '\0') { print("run: missing program name\n"); return; }

        // Spawn blocks us until the child exits
        uint64_t pid = syscall2(4, (uint64_t)name, 0);
        if (pid == (uint64_t)-1) { print("run: no such program\n"); return; }
        print("[shell] program finished\n");
        return;
    }

    // Not a builtin: commands ARE programs. Try spawning <word>.elf from
    // the tarfs. (This is why 'ls' works — it spawns ls.elf.)
    static char progname[48];
    int i = 0;
    while (line[i] != '\0' && line[i] != ' ' && i < 40) {
        progname[i] = line[i];
        i++;
    }
    progname[i] = '\0';

    // Don't double-suffix if the user already typed the .elf
    int len = i;
    if (!(len >= 4 && progname[len-4] == '.' && progname[len-3] == 'e'
          && progname[len-2] == 'l' && progname[len-1] == 'f')) {
        progname[len++] = '.';
        progname[len++] = 'e';
        progname[len++] = 'l';
        progname[len++] = 'f';
        progname[len] = '\0';
    }

    uint64_t pid = syscall2(4, (uint64_t)progname, 0);
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
