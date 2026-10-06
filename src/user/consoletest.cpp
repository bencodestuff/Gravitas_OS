#include <stdio.h>
#include <grv/syscall.h>

// consoletest — exercise the console endpoint (syscalls 35-38): spawn a
// program, bind its console to us, send it a line, and confirm the reply
// arrives here instead of the kernel console. This covers both directions:
// owner -> task (CONSOLE_WRITE feeding GETC) and task -> owner (output
// captured by CONSOLE_READ). Prints PASS/FAIL for the test harness.

static int contains(const char* hay, const char* needle) {
    for (const char* h = hay; *h; h++) {
        const char* a = h;
        const char* b = needle;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return 1;
    }
    return 0;
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    const char* prog = "echoprog.elf";
    const char* cargv[1] = { prog };
    long pid = grv_spawn_bg(prog, cargv, 1);
    if (pid < 0) { printf("consoletest: FAIL (spawn)\n"); return 1; }
    if (grv_console_attach(pid) != 0) {
        printf("consoletest: FAIL (attach)\n");
        return 1;
    }

    // Send a line into the hosted program's GETC ring.
    grv_console_write(pid, "ping\n", 5);

    static char buf[256];
    int total = 0;
    int ok = 0;
    for (int spins = 0; spins < 400; spins++) {
        struct grv_msg m;
        while (grv_recv(&m, 0) == 0) { /* drain wakeups */ }
        long n = grv_console_read(buf + total, sizeof(buf) - 1 - total);
        if (n > 0) total += (int)n;
        buf[total] = '\0';
        if (contains(buf, "echo:ping")) { ok = 1; break; }
        grv_sleep(5);
    }
    grv_console_detach(pid);

    if (ok) {
        printf("consoletest: PASS (captured %d bytes)\n", total);
        return 0;
    }
    printf("consoletest: FAIL, got '%s'\n", buf);
    return 1;
}
