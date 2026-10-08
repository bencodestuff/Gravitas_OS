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

// A bare program word ("ls") means the program in /bin ("ls.elf"); a name that
// already ends in .elf (or is a path) is passed through.
static void resolve_prog(const char* word, char* out, int cap) {
    if (str_ends_with_elf(word)) {
        int i = 0;
        while (word[i] && i < cap - 1) { out[i] = word[i]; i++; }
        out[i] = '\0';
        return;
    }
    int len = 0;
    while (word[len] && len < cap - 5) { out[len] = word[len]; len++; }
    out[len++] = '.'; out[len++] = 'e'; out[len++] = 'l'; out[len++] = 'f';
    out[len] = '\0';
}

static void cmd_help() {
    printf("Gravitas commands (commands are programs from the VFS):\n");
    printf("  ls [path]   - list a directory (default: cwd)\n");
    printf("  cat [file]  - print a file, or copy stdin\n");
    printf("  edit <file> - open a file in the text editor\n");
    printf("  bg <prog>   - spawn a program in the background\n");
    printf("  cd <dir>    - change directory (builtin)\n");
    printf("  pwd         - print working directory (builtin)\n");
    printf("  run <prog>  - spawn a program by exact name\n");
    printf("  kill <pid>  - terminate a background task\n");
    printf("  help        - this list\n");
    printf("  Redirection: cmd < in, cmd > out, cmd >> out\n");
    printf("  Pipelines:   a | b | c\n");
}

// --- Shell builtins that mutate the shell's OWN state ---
// cd must be a builtin (it changes this process's CWD — a child program
// could never do it), and pwd reads it. Everything else is a program.

static void cmd_cd(const char* dir) {
    if (dir == 0 || *dir == '\0') { printf("cd: missing directory\n"); return; }
    if (grv_chdir(dir) != 0) {
        printf("cd: no such directory: %s\n", dir);
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

/* bg <prog> [args...] — spawn in the background, remember the pid.
   'words' is the parsed command, so words[0] == "bg". */
static void cmd_bg(char** words, int nwords) {
    if (nwords < 2) { printf("bg: usage: bg <prog> [args...]\n"); return; }

    // Resolve the program name ('bg plasma' -> 'plasma.elf')
    static char progbuf[64];
    resolve_prog(words[1], progbuf, sizeof(progbuf));
    const char* prog = progbuf;

    const char* argv[9];
    int argc = 1;
    argv[0] = prog;
    for (int i = 2; i < nwords && argc < 8; i++) argv[argc++] = words[i];

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
static void cmd_kill(const char* arg) {
    if (arg == 0) { printf("kill: usage: kill <pid>\n"); return; }
    long pid = 0;
    const char* p = arg;
    while (*p >= '0' && *p <= '9') { pid = pid * 10 + (*p - '0'); p++; }
    if (p == arg || *p != '\0') { printf("kill: usage: kill <pid>\n"); return; }

    long r = grv_kill(pid, 0);
    if (r != 0) { printf("kill: no such task (or refused)\n"); return; }
    printf("kill: terminated %d\n", (int)pid);
    for (int i = 0; i < 8; i++) {
        if (g_bg_pid[i] == pid) { g_bg_pid[i] = 0; break; }
    }
}

// --- Command line parsing ------------------------------------------------
// A line is one or more commands separated by '|'. Each command is a program
// (or a builtin) with whitespace-separated arguments and optional
// redirections: '< file', '> file' (truncate), '>> file' (append). The
// operators are always separate tokens, so 'cat<in>out' means the same as
// 'cat < in > out'. There is no quoting: this shell is a spawner, and
// everything on the line is a word.

#define SH_MAX_CMDS 8
#define SH_MAX_ARGS 8
#define SH_WORD_MAX 64

struct sh_cmd {
    char words[SH_MAX_ARGS][SH_WORD_MAX];
    int  nwords;
    int  rin;                    // 0 none, else '<' present
    char in_path[SH_WORD_MAX];
    int  rout;                   // 0 none, 1 '>', 2 '>>'
    char out_path[SH_WORD_MAX];
    char prog[SH_WORD_MAX];      // resolved program name
    int  arg_start;              // index of argv[1] in 'words'
};

static bool is_builtin(const char* w) {
    return str_eq(w, "help") || str_eq(w, "pwd") || str_eq(w, "cd") ||
           str_eq(w, "bg")   || str_eq(w, "jobs") || str_eq(w, "kill");
}

// Read one word (up to whitespace or an operator) into 'out'. Returns its
// length, 0 when there was nothing to read.
static int sh_read_word(const char** pp, char* out) {
    const char* p = *pp;
    while (*p == ' ' || *p == '\t') p++;
    int n = 0;
    while (*p && *p != ' ' && *p != '\t' && *p != '<' && *p != '>' && *p != '|') {
        if (n < SH_WORD_MAX - 1) out[n++] = *p;
        p++;
    }
    out[n] = '\0';
    *pp = p;
    return n;
}

// Parse 'line' into at most SH_MAX_CMDS commands. Returns the static array (and
// the count through *out_n), or nullptr with *err set. An empty line yields a
// count of 0.
static struct sh_cmd* parse_pipeline(char* line, int* out_n, const char** err) {
    static struct sh_cmd cmds[SH_MAX_CMDS];
    *err = 0;
    *out_n = 0;

    const char* p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0') return cmds;

    int nc = 0;
    struct sh_cmd* c = &cmds[0];
    c->nwords = 0; c->rin = 0; c->rout = 0;

    while (1) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;

        if (*p == '|') {
            if (c->nwords == 0) { *err = "syntax error: empty command"; return 0; }
            if (nc + 1 >= SH_MAX_CMDS) { *err = "too many commands in the pipeline"; return 0; }
            nc++;
            c = &cmds[nc];
            c->nwords = 0; c->rin = 0; c->rout = 0;
            p++;
            continue;
        }
        if (*p == '<') {
            p++;
            if (sh_read_word(&p, c->in_path) == 0) { *err = "syntax error: '<' needs a file name"; return 0; }
            c->rin = 1;
            continue;
        }
        if (*p == '>') {
            p++;
            int app = 0;
            if (*p == '>') { app = 1; p++; }
            if (sh_read_word(&p, c->out_path) == 0) { *err = "syntax error: '>' needs a file name"; return 0; }
            c->rout = app ? 2 : 1;
            continue;
        }

        if (c->nwords >= SH_MAX_ARGS) { *err = "too many arguments"; return 0; }
        sh_read_word(&p, c->words[c->nwords]);
        c->nwords++;
    }

    if (c->nwords == 0) { *err = "syntax error: empty command"; return 0; }
    *out_n = nc + 1;
    return cmds;
}

// Resolve a command's program: 'run' takes an exact name, anything else gets
// ".elf" appended unless it already ends in one. Sets arg_start.
static int resolve_command(struct sh_cmd* c, int* arg_start) {
    const char* word = c->words[0];
    *arg_start = 1;
    if (str_eq(word, "run")) {
        if (c->nwords < 2) return -1;
        word = c->words[1];
        *arg_start = 2;
    }
    resolve_prog(word, c->prog, (int)sizeof(c->prog));
    return 0;
}

// Apply a command's redirections to the shell's own descriptors. On success
// *save0/*save1 hold descriptors for restore_fds (or -1). The target of an
// output redirection is created even if the command writes nothing, as a shell
// does. Returns 0, or -1 with a message printed.
static int apply_redirs(struct sh_cmd* c, int* save0, int* save1) {
    *save0 = -1;
    *save1 = -1;

    if (c->rin) {
        int fd = grv_open(c->in_path, GRV_O_RDONLY);
        if (fd < 0) { printf("shell: cannot open '%s' to read\n", c->in_path); return -1; }
        *save0 = grv_dup(0);
        if (*save0 < 0 || grv_dup2(fd, 0) < 0) {
            if (*save0 >= 0) { grv_close(*save0); *save0 = -1; }
            grv_close(fd);
            printf("shell: cannot redirect input\n");
            return -1;
        }
        grv_close(fd);
    }

    if (c->rout) {
        int flags = GRV_O_WRONLY | GRV_O_CREATE | (c->rout == 1 ? GRV_O_TRUNC : 0);
        int fd = grv_open(c->out_path, flags);
        if (fd < 0) {
            printf("shell: cannot open '%s' to write\n", c->out_path);
            if (*save0 >= 0) { grv_dup2(*save0, 0); grv_close(*save0); *save0 = -1; }
            return -1;
        }
        if (c->rout == 2) grv_lseek(fd, 0, 2);   // append: start at the end (no O_APPEND)
        *save1 = grv_dup(1);
        if (*save1 < 0 || grv_dup2(fd, 1) < 0) {
            if (*save1 >= 0) { grv_close(*save1); *save1 = -1; }
            grv_close(fd);
            if (*save0 >= 0) { grv_dup2(*save0, 0); grv_close(*save0); *save0 = -1; }
            printf("shell: cannot redirect output\n");
            return -1;
        }
        grv_close(fd);
    }
    return 0;
}

static void restore_fds(int save0, int save1) {
    if (save0 >= 0) { grv_dup2(save0, 0); grv_close(save0); }
    if (save1 >= 0) { grv_dup2(save1, 1); grv_close(save1); }
}

// A builtin runs in the shell itself (cd/bg/kill/jobs change the shell's own
// state, so they cannot be a child), with its redirections applied around it.
static void run_builtin(struct sh_cmd* c) {
    int save0, save1;
    if (apply_redirs(c, &save0, &save1) < 0) return;

    const char* w = c->words[0];
    if (str_eq(w, "help"))       cmd_help();
    else if (str_eq(w, "pwd"))   cmd_pwd();
    else if (str_eq(w, "jobs"))  cmd_jobs();
    else if (str_eq(w, "cd"))    cmd_cd(c->nwords > 1 ? c->words[1] : "");
    else if (str_eq(w, "bg")) {
        char* wv[SH_MAX_ARGS];
        for (int i = 0; i < c->nwords; i++) wv[i] = c->words[i];
        cmd_bg(wv, c->nwords);
    }
    else if (str_eq(w, "kill"))  cmd_kill(c->nwords > 1 ? c->words[1] : "");

    restore_fds(save0, save1);
}

// One program, spawned in the foreground: the shell blocks on it, so it owns
// the console (input and Ctrl+C) exactly as before.
static void run_single(struct sh_cmd* c) {
    if (resolve_command(c, &c->arg_start) < 0) {
        printf("run: missing program name\n");
        return;
    }
    const char* argv[9];
    int argc = 0;
    argv[argc++] = c->prog;
    for (int i = c->arg_start; i < c->nwords && argc < 8; i++) argv[argc++] = c->words[i];

    int save0, save1;
    if (apply_redirs(c, &save0, &save1) < 0) return;

    long pid = grv_spawn(c->prog, argv, argc);
    restore_fds(save0, save1);

    if (pid == -1) {
        printf("shell: cannot run '%s' (try 'help', 'ls')\n", c->prog);
        return;
    }
    printf("[shell] program finished\n");
}

// A pipeline: every stage is spawned in the background with its descriptors
// wired to the pipe, then the shell waits for them all. One pipe is created as
// we go and the parent-side ends are closed as soon as the child that owns them
// exists — that closure is what lets a reader see EOF.
static void run_pipeline(struct sh_cmd* cmds, int ncmds) {
    for (int i = 0; i < ncmds; i++) {
        if (is_builtin(cmds[i].words[0])) {
            printf("shell: '%s' cannot be used in a pipeline\n", cmds[i].words[0]);
            return;
        }
        // A stage's input comes from the pipe before it and its output goes to
        // the pipe after it. A redirection that would fight the pipe is refused
        // rather than silently ignored (which would leave the other stage's
        // end unwired, and a writer blocked on a pipe nobody reads).
        if (i > 0 && cmds[i].rin) {
            printf("shell: cannot read '<' into a piped command\n");
            return;
        }
        if (i < ncmds - 1 && cmds[i].rout) {
            printf("shell: cannot write '>' out of a piped command\n");
            return;
        }
        if (resolve_command(&cmds[i], &cmds[i].arg_start) < 0) {
            printf("run: missing program name\n");
            return;
        }
    }

    long pids[SH_MAX_CMDS];
    for (int i = 0; i < ncmds; i++) pids[i] = -1;

    int prev_read = -1;                  // read end feeding the next stage
    for (int i = 0; i < ncmds; i++) {
        struct sh_cmd* c = &cmds[i];
        int is_last = (i == ncmds - 1);
        int pipe_r = -1, pipe_w = -1;
        if (!is_last) {
            int pf[2];
            if (grv_pipe(pf) != 0) { printf("shell: cannot create a pipe\n"); break; }
            pipe_r = pf[0];
            pipe_w = pf[1];
        }

        bool ok = true;
        int save0 = -1, save1 = -1;

        // stdin: the previous pipe, or this command's '< file', or the console.
        if (i > 0) {
            if (prev_read < 0) {
                ok = false;
            } else {
                save0 = grv_dup(0);
                if (save0 < 0 || grv_dup2(prev_read, 0) < 0) ok = false;
            }
        } else if (c->rin) {
            int fd = grv_open(c->in_path, GRV_O_RDONLY);
            if (fd < 0) { printf("shell: cannot open '%s' to read\n", c->in_path); ok = false; }
            else {
                save0 = grv_dup(0);
                if (save0 < 0 || grv_dup2(fd, 0) < 0) ok = false;
                grv_close(fd);
            }
        }

        // stdout: the next pipe, or this command's '> file', or the console.
        if (ok) {
            if (!is_last) {
                save1 = grv_dup(1);
                if (save1 < 0 || grv_dup2(pipe_w, 1) < 0) ok = false;
            } else if (c->rout) {
                int flags = GRV_O_WRONLY | GRV_O_CREATE | (c->rout == 1 ? GRV_O_TRUNC : 0);
                int fd = grv_open(c->out_path, flags);
                if (fd < 0) { printf("shell: cannot open '%s' to write\n", c->out_path); ok = false; }
                else {
                    if (c->rout == 2) grv_lseek(fd, 0, 2);
                    save1 = grv_dup(1);
                    if (save1 < 0 || grv_dup2(fd, 1) < 0) ok = false;
                    grv_close(fd);
                }
            }
        }

        if (ok) {
            const char* argv[9];
            int argc = 0;
            argv[argc++] = c->prog;
            for (int a = c->arg_start; a < c->nwords && argc < 8; a++) argv[argc++] = c->words[a];
            long pid = grv_spawn_bg(c->prog, argv, argc);
            if (pid < 0) printf("shell: cannot run '%s'\n", c->prog);
            else pids[i] = pid;
        }

        restore_fds(save0, save1);
        if (i > 0 && prev_read >= 0) { grv_close(prev_read); prev_read = -1; }
        if (!is_last) {
            grv_close(pipe_w);             // the child holds the write end now
            if (ok) prev_read = pipe_r;    // the read end feeds the next stage
            else grv_close(pipe_r);
        }
    }
    if (prev_read >= 0) grv_close(prev_read);

    int started = 0;
    for (int i = 0; i < ncmds; i++) if (pids[i] > 0) started++;
    for (int i = 0; i < ncmds; i++) if (pids[i] > 0) grv_wait(pids[i]);
    if (started > 0) printf("[shell] program finished\n");
}

static void execute(char* line) {
    const char* err = 0;
    int ncmds = 0;
    struct sh_cmd* cmds = parse_pipeline(line, &ncmds, &err);
    if (cmds == 0) { printf("shell: %s\n", err ? err : "syntax error"); return; }
    if (ncmds == 0) return;
    // Builtins change the shell's own state and cannot run in a child, so they
    // are only meaningful as a lone command.
    if (is_builtin(cmds[0].words[0])) {
        if (ncmds > 1) {
            printf("shell: '%s' cannot be used in a pipeline\n", cmds[0].words[0]);
            return;
        }
        run_builtin(&cmds[0]);
        return;
    }

    if (ncmds == 1) run_single(&cmds[0]);
    else run_pipeline(cmds, ncmds);
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

// Print a directory's entries, one per line, directories with a trailing '/'.
// Bounded: a bigger directory lists its first SHELL_LIST_MAX entries.
#define SHELL_LIST_MAX 64
static void list_dir(const char* path) {
    static struct grv_dirent ents[SHELL_LIST_MAX];
    long n = grv_readdir(path, ents, SHELL_LIST_MAX);
    if (n < 0) {
        printf("%s: not a directory\n", path);
        return;
    }
    for (long i = 0; i < n; i++)
        printf("%s%s\n", ents[i].name, ents[i].type == GRV_DT_DIR ? "/" : "");
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    printf("[shell] Gravitas userland shell v0.2 (libc)\n");

    // Startup listing: shows the CWD (the shell boots at "/")
    printf("[shell] cwd contents:\n");
    list_dir(".");
    printf("[shell] type 'help'\n");

    static char line[128];
    while (1) {
        // Prompt shows the CWD
        static char cwd[64];
        grv_getcwd(cwd, sizeof(cwd));
        // The prompt is printed once, on a line of its own; the *text* is what
        // a redraw reprints, so it must not carry that leading newline (or
        // every redraw would start a new line).
        static char prompt[96];
        snprintf(prompt, sizeof(prompt), "%s root@gravitas# ", cwd);
        printf("\n%s", prompt);

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
