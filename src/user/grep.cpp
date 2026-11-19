#ifdef GREP_HOST_TEST
#include <stddef.h>
#else
#include <stdio.h>
#include <grv/syscall.h>
#endif

// grep: prints the lines of its input that contain a fixed string.
// Usage: grep [-i] [-v] [-n] [-c] [--] PATTERN [file...]
// With no file, or with "-", it reads stdin, so it works in a pipe:
//   ls | grep lua
//
// The match is a plain substring, never a regular expression, and both the
// pattern and the line kept for printing are bounded. Matching itself is
// streamed byte by byte, so a match past the print window still selects its
// line; only the printed line stops at GREP_LINE_MAX bytes. That keeps the
// program's memory fixed no matter how long a pipe write or an input line is.
//
// Exit status: 0 if a line was selected, 1 if none, 2 on a usage or read
// error, which is the convention the other programs here follow.

#define GREP_PATTERN_MAX 64
#define GREP_LINE_MAX    2048

// The engine below touches no libc and no syscalls, so the host test can
// include this file and drive it directly.
#ifdef GREP_HOST_TEST
#define GREP_API
#else
#define GREP_API static
#endif

struct GrepSink {
    void (*write)(void* ctx, const char* p, long n);
    void* ctx;
};

struct GrepState {
    // Pattern, folded once at startup when -i is set.
    char pat[GREP_PATTERN_MAX];
    int  patlen;
    bool fold;
    bool invert;
    bool number;
    bool count_only;

    // Streaming matcher: a ring of the last patlen bytes seen on this line.
    // When the ring is full it holds exactly one window to compare.
    char ring[GREP_PATTERN_MAX];
    int  rpos;
    long seen;
    bool matched;

    // The line as kept for printing. The tail past GREP_LINE_MAX is consumed
    // but not stored; `overlong` records that so the caveat is deliberate.
    char line[GREP_LINE_MAX];
    long llen;
    bool overlong;

    long lineno;    // lines completed so far
    long selected;  // lines this run will print or count
};

static char grep_fold_char(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

// Longest decimal form of a long, plus slack; written without printf so the
// engine stays free of the libc.
static long grep_format_number(long v, char* out) {
    char tmp[24];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    return n;
}

GREP_API void grep_reset_line(GrepState* s) {
    for (int i = 0; i < s->patlen; i++) s->ring[i] = 0;
    s->rpos = 0;
    s->seen = 0;
    s->matched = (s->patlen == 0);   // the empty pattern matches every line
    s->llen = 0;
    s->overlong = false;
}

GREP_API void grep_init(GrepState* s, const char* pattern, int patlen,
                        bool fold, bool invert, bool number, bool count_only) {
    s->patlen = patlen;
    for (int i = 0; i < patlen; i++)
        s->pat[i] = fold ? grep_fold_char(pattern[i]) : pattern[i];
    s->fold = fold;
    s->invert = invert;
    s->number = number;
    s->count_only = count_only;
    s->rpos = 0;
    s->lineno = 0;
    s->selected = 0;
    grep_reset_line(s);
}

// Feed one non-newline byte: keep it for printing and advance the window.
GREP_API void grep_match_byte(GrepState* s, char c) {
    if (s->patlen == 0) return;    // selected already by grep_reset_line
    if (s->fold) c = grep_fold_char(c);

    s->ring[s->rpos] = c;
    s->rpos = (s->rpos + 1) % s->patlen;
    if (s->seen < s->patlen) s->seen++;
    if (s->seen < s->patlen) return;   // no full window yet

    int j = s->rpos;                   // oldest byte in the ring
    int k = 0;
    while (k < s->patlen && s->ring[j] == s->pat[k]) {
        k++;
        j = (j + 1) % s->patlen;
    }
    if (k == s->patlen) s->matched = true;
}

// Close the current line: count it, and print it if it was selected.
GREP_API void grep_complete_line(GrepState* s, const GrepSink* sink) {
    s->lineno++;
    bool selected = s->matched != s->invert;
    if (selected) s->selected++;

    if (selected && !s->count_only && sink) {
        if (s->number) {
            char num[24];
            long n = grep_format_number(s->lineno, num);
            sink->write(sink->ctx, num, n);
            sink->write(sink->ctx, ":", 1);
        }
        if (s->llen > 0) sink->write(sink->ctx, s->line, s->llen);
        // An overlong line prints its stored prefix only; the rest was read
        // and matched, but there is nowhere bounded to keep it.
        sink->write(sink->ctx, "\n", 1);
    }
    grep_reset_line(s);
}

// One read's worth of bytes. A line may span any number of calls.
GREP_API void grep_feed(GrepState* s, const GrepSink* sink,
                        const char* data, long n) {
    for (long i = 0; i < n; i++) {
        char c = data[i];
        if (c == '\n') {
            grep_complete_line(s, sink);
            continue;
        }
        if (s->llen < GREP_LINE_MAX) s->line[s->llen++] = c;
        else s->overlong = true;
        grep_match_byte(s, c);
    }
}

// End of input: a file that does not end in a newline still has a last line.
GREP_API void grep_finish(GrepState* s, const GrepSink* sink) {
    if (s->llen > 0 || s->overlong) grep_complete_line(s, sink);
}

#ifndef GREP_HOST_TEST

static void grep_write(void* ctx, const char* p, long n) {
    (void)ctx;
    grv_write_fd(1, p, (unsigned long)n);
}

// Read one descriptor to EOF through the bounded engine.
static int grep_read_fd(int fd, GrepState* st, const GrepSink* sink) {
    char buf[256];
    for (;;) {
        long n = grv_read_fd(fd, buf, sizeof(buf));
        if (n < 0) return -1;
        if (n == 0) return 0;
        grep_feed(st, sink, buf, n);
    }
}

int main(int argc, char** argv) {
    bool fold = false, invert = false, number = false, count_only = false;
    bool end_of_options = false;
    int i = 1;

    for (; i < argc; i++) {
        const char* a = argv[i];
        if (!end_of_options && a[0] == '-' && a[1] != '\0') {
            if (a[1] == '-' && a[2] == '\0') { end_of_options = true; continue; }
            bool ok = true;
            for (int k = 1; a[k] != '\0'; k++) {
                switch (a[k]) {
                    case 'i': fold = true; break;
                    case 'v': invert = true; break;
                    case 'n': number = true; break;
                    case 'c': count_only = true; break;
                    default:
                        fprintf(stderr, "grep: unknown option -%c\n", a[k]);
                        ok = false;
                        break;
                }
                if (!ok) break;
            }
            if (!ok) return 2;
            continue;
        }
        break;   // the first non-option word is the pattern
    }

    if (i >= argc) {
        fprintf(stderr,
                "grep: usage: grep [-i] [-v] [-n] [-c] [--] PATTERN [file...]\n");
        return 2;
    }

    const char* pattern = argv[i++];
    int patlen = 0;
    while (pattern[patlen] != '\0') patlen++;
    if (patlen > GREP_PATTERN_MAX) {
        fprintf(stderr, "grep: pattern longer than %d bytes\n", GREP_PATTERN_MAX);
        return 2;
    }

    static GrepState st;
    GrepSink sink = { grep_write, 0 };
    grep_init(&st, pattern, patlen, fold, invert, number, count_only);

    bool failed = false;
    if (i >= argc) {
        if (grep_read_fd(0, &st, &sink) < 0) {
            fprintf(stderr, "grep: cannot read stdin\n");
            failed = true;
        }
    } else {
        for (; i < argc; i++) {
            const char* name = argv[i];
            int fd;
            if (name[0] == '-' && name[1] == '\0') {
                fd = 0;
            } else {
                fd = grv_open(name, GRV_O_RDONLY);
                if (fd < 0) {
                    fprintf(stderr, "grep: cannot open %s\n", name);
                    failed = true;
                    continue;
                }
            }
            if (grep_read_fd(fd, &st, &sink) < 0) {
                fprintf(stderr, "grep: cannot read %s\n", name);
                failed = true;
            }
            if (fd != 0) grv_close(fd);
        }
    }
    grep_finish(&st, &sink);

    if (count_only) printf("%ld\n", st.selected);
    if (failed) return 2;
    return st.selected > 0 ? 0 : 1;
}

#endif  // GREP_HOST_TEST
