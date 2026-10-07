#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <grv/syscall.h>

// Gravitas userland shell — a pure program spawner.
// Built against the libc: printf/strlen/malloc + grv syscall wrappers.
// Entry: main(argc, argv) via crt0 (not _start).

static bool str_eq(const char* a, const char* b) {
    return strcmp(a, b) == 0;
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
    printf("  edit <file> - open a file in the text editor\n");
    printf("  bg <prog>   - spawn a program in the background\n");
    printf("  cd <dir>    - change directory (builtin)\n");
    printf("  pwd         - print working directory (builtin)\n");
    printf("  run <prog>  - spawn a program by exact name\n");
    printf("  kill <pid>  - terminate a background task\n");
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

/* --- Background job table --- */
static long g_bg_pid[8];      /* 0 = empty slot */
static char g_bg_name[8][32]; /* program name of the job */

/* bg <prog> [args...] — spawn in the background, remember the pid. */
static void cmd_bg(char* line) {
    while (*line == ' ') line++;
    if (*line == '\0') { printf("bg: usage: bg <prog> [args...]\n"); return; }

    // Word split (same rules as execute(): max 8 words of max 63 chars)
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

    // Resolve the program name ('bg plasma' -> 'plasma.elf')
    const char* prog;
    static char progbuf[64];
    if (str_ends_with_elf(words[0])) {
        prog = words[0];
    } else {
        int len = 0;
        const char* w = words[0];
        while (*w && len < 58) progbuf[len++] = *w++;
        progbuf[len++] = '.'; progbuf[len++] = 'e';
        progbuf[len++] = 'l'; progbuf[len++] = 'f';
        progbuf[len] = '\0';
        prog = progbuf;
    }

    const char* argv[9];
    int argc = 1;
    argv[0] = prog;
    for (int i = 1; i < nwords && argc < 8; i++) argv[argc++] = words[i];

    long pid = grv_spawn_bg(prog, argv, argc);
    if (pid == -1) { printf("bg: cannot start '%s'\n", prog); return; }

    // Remember it in a free slot (jobs/kill refer to tasks by this pid)
    for (int i = 0; i < 8; i++) {
        if (g_bg_pid[i] == 0) {
            g_bg_pid[i] = pid;
            const char* s = prog; int c = 0;
            while (s[c] && c < 31) { g_bg_name[i][c] = s[c]; c++; }
            g_bg_name[i][c] = '\0';
            break;
        }
    }
    printf("bg: started '%s' as task %d\n", prog, (int)pid);
}

/* jobs — list the pids we spawned in the background. */
static void cmd_jobs() {
    bool any = false;
    for (int i = 0; i < 8; i++) {
        if (g_bg_pid[i] != 0) {
            printf("  %d  %s\n", (int)g_bg_pid[i], g_bg_name[i]);
            any = true;
        }
    }
    if (!any) printf("no background tasks\n");
}

/* kill <pid> — forcibly terminate a background task. */
static void cmd_kill(char* line) {
    while (*line == ' ') line++;
    long pid = 0;
    const char* p = line;
    while (*p >= '0' && *p <= '9') { pid = pid * 10 + (*p - '0'); p++; }
    if (p == line || *p != '\0') { printf("kill: usage: kill <pid>\n"); return; }

    long r = grv_kill(pid, 0);
    if (r != 0) { printf("kill: no such task (or refused)\n"); return; }
    printf("kill: terminated %d\n", (int)pid);
    for (int i = 0; i < 8; i++) {
        if (g_bg_pid[i] == pid) { g_bg_pid[i] = 0; break; }
    }
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

    // bg <prog> [args...] — spawn in the background, keep the pid so the
    // prompt stays usable and `jobs`/`kill` can refer to it later.
    if (line[0] == 'b' && line[1] == 'g' && (line[2] == ' ' || line[2] == '\0')) {
        cmd_bg(line + 3);
        return;
    }

    // kill <pid> — forcibly terminate a spawned background task
    if (line[0] == 'k' && line[1] == 'i' && line[2] == 'l' && line[3] == 'l' &&
        (line[4] == ' ' || line[4] == '\0')) {
        cmd_kill(line + 5);
        return;
    }

    // jobs — list spawned background tasks
    if (str_eq(line, "jobs")) { cmd_jobs(); return; }

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
        if (str_ends_with_elf(words[1])) {
            prog = words[1];
        } else {
            // accept 'run plasma' the same as 'run plasma.elf'
            int len = 0;
            const char* w = words[1];
            while (*w && len < 58) progbuf[len++] = *w++;
            progbuf[len++] = '.'; progbuf[len++] = 'e';
            progbuf[len++] = 'l'; progbuf[len++] = 'f';
            progbuf[len] = '\0';
            prog = progbuf;
        }
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
        printf("shell: cannot run '%s' (try 'help', 'ls')\n", prog);
        return;
    }
    printf("[shell] program finished\n");
}

// --- the line reader ---
// A byte-stream line editor: the console gives bytes, so the arrows arrive as
// the readline codes (^P/^N for up/down, ^B/^F for left/right, ^A/^E for
// home/end) and the editing chords arrive as themselves.
//
// The console's '\b' moves without erasing (as a terminal's does), so the
// cursor can sit anywhere in the line: moving is one '\b', and erasing is the
// three-byte idiom. Redrawing goes back to the start of the prompt and pads
// over a longer previous line, because there is no erase-to-end-of-line.

#define HIST_MAX 16

static char hist[HIST_MAX][128];
static int  hist_count = 0;   // how many are stored
static int  hist_next = 0;    // the ring slot the next one goes in

static void hist_add(const char* s) {
    if (s[0] == '\0') return;
    int prev = (hist_next + HIST_MAX - 1) % HIST_MAX;
    if (hist_count > 0 && str_eq(hist[prev], s)) return; // not twice in a row
    int i = 0;
    while (s[i] && i < 127) { hist[hist_next][i] = s[i]; i++; }
    hist[hist_next][i] = '\0';
    hist_next = (hist_next + 1) % HIST_MAX;
    if (hist_count < HIST_MAX) hist_count++;
}

// The n-th most recent entry (1 = the previous command), or 0.
static const char* hist_back(int n) {
    if (n < 1 || n > hist_count) return 0;
    return hist[(hist_next + HIST_MAX - n) % HIST_MAX];
}

// Repaint the line with the cursor at 'cur'.
static void line_draw(const char* prompt, const char* line, int len, int cur,
                      int* drawn) {
    putchar('\r');
    for (const char* p = prompt; *p; p++) putchar(*p);
    for (int i = 0; i < len; i++) putchar(line[i]);
    for (int i = len; i < *drawn; i++) putchar(' '); // a longer line's tail
    *drawn = len;
    for (int i = len; i > cur; i--) putchar('\b');   // moves, does not erase
}

// Put 's' on the line, cursor at the end.
static void line_set(char* line, int* len, int* cur, const char* prompt,
                     const char* s, int* drawn) {
    int n = 0;
    while (s[n] && n < 120) { line[n] = s[n]; n++; }
    *len = n;
    *cur = n;
    line_draw(prompt, line, *len, *cur, drawn);
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
        static char prompt[96];
        snprintf(prompt, sizeof(prompt), "\n%s root@gravitas# ", cwd);
        printf("%s", prompt);

        // The reader: a line editor. Backspace is swallowed once everything
        // typed is gone, so the cursor can never ride back across the prompt.
        int len = 0, cur = 0, drawn = 0, back = 0;
        while (1) {
            char c = grv_getc();

            if (c == '\n') { putchar('\n'); line[len] = '\0'; break; }

            if (c == '\b') {                  // rub out before the cursor
                if (cur > 0) {
                    for (int i = cur - 1; i < len - 1; i++) line[i] = line[i + 1];
                    len--;
                    cur--;
                    line_draw(prompt, line, len, cur, &drawn);
                    back = 0;
                }
                continue;
            }
            if (c == 1) {                     // ^A: start of the line
                cur = 0;
                line_draw(prompt, line, len, cur, &drawn);
                continue;
            }
            if (c == 5) {                     // ^E: end of it
                cur = len;
                line_draw(prompt, line, len, cur, &drawn);
                continue;
            }
            if (c == 2) {                     // ^B: one left
                if (cur > 0) cur--;
                line_draw(prompt, line, len, cur, &drawn);
                continue;
            }
            if (c == 6) {                     // ^F: one right
                if (cur < len) cur++;
                line_draw(prompt, line, len, cur, &drawn);
                continue;
            }
            if (c == 11) {                    // ^K: kill to the end
                len = cur;
                line_draw(prompt, line, len, cur, &drawn);
                back = 0;
                continue;
            }
            if (c == 21) {                    // ^U: kill the whole line
                len = 0;
                cur = 0;
                line_draw(prompt, line, len, cur, &drawn);
                back = 0;
                continue;
            }
            if (c == 23) {                    // ^W: kill the word before
                int start = cur;
                while (start > 0 && line[start - 1] == ' ') start--;
                while (start > 0 && line[start - 1] != ' ') start--;
                for (int i = start; i + (cur - start) < len; i++)
                    line[i] = line[i + (cur - start)];
                len -= cur - start;
                cur = start;
                line_draw(prompt, line, len, cur, &drawn);
                back = 0;
                continue;
            }
            if (c == 16) {                    // ^P: back through the history
                const char* h = hist_back(back + 1);
                if (h != 0) {
                    back++;
                    line_set(line, &len, &cur, prompt, h, &drawn);
                }
                continue;
            }
            if (c == 14) {                    // ^N: forward through it
                if (back > 1) {
                    back--;
                    line_set(line, &len, &cur, prompt, hist_back(back), &drawn);
                } else if (back == 1) {
                    back = 0;
                    line_set(line, &len, &cur, prompt, "", &drawn);
                }
                continue;
            }
            if (c >= 32 && c < 127) {         // a printable character
                if (len < 120) {
                    for (int i = len; i > cur; i--) line[i] = line[i - 1];
                    line[cur++] = c;
                    len++;
                    line_draw(prompt, line, len, cur, &drawn);
                    back = 0;
                } else putchar(0x07); // alert on overflow
                continue;
            }
            // anything else: swallowed
        }
        hist_add(line);
        execute(line);
    }
}
