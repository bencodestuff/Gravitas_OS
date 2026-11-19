// Host test for the bounded line engine in src/user/grep.cpp.
//
// The engine touches no libc and no syscalls, so the program can be included
// here with GREP_HOST_TEST and driven over an in-memory byte stream. What
// matters on a host is the part a guest boot is bad at proving: a line split
// across arbitrary read boundaries, a match that spans two reads, a match past
// the print window, and the final line with no newline.
//
// Build and run (not wired into tests/host/run.sh yet):
//   c++ -O1 -std=c++17 -Wall -Wextra tools/test_grep.cpp -o /tmp/test_grep
//   /tmp/test_grep

#define GREP_HOST_TEST
#include "../src/user/grep.cpp"

#include <stdio.h>
#include <string.h>

static char g_out[16384];
static long g_out_len;

static void capture(void* ctx, const char* p, long n) {
    (void)ctx;
    for (long i = 0; i < n; i++) {
        if (g_out_len < (long)sizeof(g_out) - 1) g_out[g_out_len++] = p[i];
    }
}

static int g_failures;

static void reset_out() {
    g_out_len = 0;
    g_out[0] = '\0';
}

static void check(const char* label, long selected, long want_selected,
                  const char* want) {
    bool ok = true;
    if (selected != want_selected) {
        printf("FAIL %s: selected=%ld want=%ld\n", label, selected, want_selected);
        ok = false;
    }
    long want_len = (long)strlen(want);
    if (g_out_len != want_len || memcmp(g_out, want, (size_t)want_len) != 0) {
        printf("FAIL %s: output %ld bytes, want %ld\n", label, g_out_len, want_len);
        printf("  want: ");
        for (long i = 0; i < want_len; i++)
            putchar(want[i] == '\n' ? '$' : want[i]);
        printf("\n  got:  ");
        for (long i = 0; i < g_out_len && i < 400; i++)
            putchar(g_out[i] == '\n' ? '$' : g_out[i]);
        printf("\n");
        ok = false;
    }
    if (!ok) g_failures++;
}

static GrepState g_st;
static GrepSink g_sink = { capture, 0 };

// Run one case with the input delivered in `chunk`-sized reads (0 = one read).
static void run_case(const char* label, const char* pattern, const char* input,
                     int chunk, bool fold, bool invert, bool number,
                     bool count_only, long want_selected, const char* want) {
    reset_out();
    int patlen = (int)strlen(pattern);
    grep_init(&g_st, pattern, patlen, fold, invert, number, count_only);

    long len = (long)strlen(input);
    if (chunk <= 0) {
        grep_feed(&g_st, &g_sink, input, len);
    } else {
        for (long off = 0; off < len; off += chunk) {
            long n = len - off;
            if (n > chunk) n = chunk;
            grep_feed(&g_st, &g_sink, input + off, n);
        }
    }
    grep_finish(&g_st, &g_sink);

    long selected = g_st.selected;
    check(label, selected, want_selected, want);
}

int main() {
    const char* listing = "lua\nlua.elf\nhello\n";

    run_case("plain match", "lua", listing, 0, false, false, false, false,
             2, "lua\nlua.elf\n");
    run_case("byte at a time", "lua", listing, 1, false, false, false, false,
             2, "lua\nlua.elf\n");
    run_case("odd chunks", "lua", listing, 3, false, false, false, false,
             2, "lua\nlua.elf\n");
    run_case("no match", "zzz", listing, 0, false, false, false, false,
             0, "");
    run_case("match spans reads", "ell", "hello\n", 2,
             false, false, false, false, 1, "hello\n");
    run_case("match at line end", "llo", "hello", 2, false, false, false, false,
             1, "hello\n");
    run_case("ignore case", "LUA", listing, 2, true, false, false, false,
             2, "lua\nlua.elf\n");
    run_case("invert", "lua", listing, 0, false, true, false, false,
             1, "hello\n");
    run_case("number", "lua", listing, 0, false, false, true, false,
             2, "1:lua\n2:lua.elf\n");
    run_case("count", "lua", listing, 0, false, false, false, true,
             2, "");
    run_case("empty pattern", "", "a\n\nb\n", 0, false, false, false, false,
             3, "a\n\nb\n");
    run_case("overlap", "aa", "aaa\n", 0, false, false, false, false,
             1, "aaa\n");
    run_case("pattern longer than line", "abcdef", "abc\n", 0,
             false, false, false, false, 0, "");
    run_case("crlf kept", "lua", "lua\r\nx\n", 0, false, false, false, false,
             1, "lua\r\n");

    // A match past the print window still selects the line, and the printed
    // line stops at GREP_LINE_MAX. Both properties are the bounded contract.
    static char long_line[GREP_LINE_MAX + 600];
    memset(long_line, 'x', sizeof(long_line));
    long tail = GREP_LINE_MAX + 300;
    memcpy(long_line + tail, "needle", 6);
    long_line[tail + 6] = '\n';
    long_line[tail + 7] = '\0';

    reset_out();
    grep_init(&g_st, "needle", 6, false, false, false, false);
    grep_feed(&g_st, &g_sink, long_line, tail + 7);
    grep_finish(&g_st, &g_sink);
    if (g_st.selected != 1) {
        printf("FAIL long line match beyond window: selected=%ld\n", g_st.selected);
        g_failures++;
    }
    if (g_out_len != GREP_LINE_MAX + 1) {
        printf("FAIL long line print bound: %ld bytes, want %d\n",
               g_out_len, GREP_LINE_MAX + 1);
        g_failures++;
    }

    // The same long line with no match selects nothing.
    reset_out();
    grep_init(&g_st, "absent", 6, false, false, false, false);
    grep_feed(&g_st, &g_sink, long_line, tail + 7);
    grep_finish(&g_st, &g_sink);
    if (g_st.selected != 0 || g_out_len != 0) {
        printf("FAIL long line without match: selected=%ld out=%ld\n",
               g_st.selected, g_out_len);
        g_failures++;
    }

    if (g_failures) {
        printf("%d grep engine check(s) failed\n", g_failures);
        return 1;
    }
    printf("test_grep: all checks passed\n");
    return 0;
}
