/* Host test for src/libc/gravity/text.c (the editable text buffer).
   Build: cc -O2 -Isrc/libc/include tools/test_text.c -o /tmp/test_text */

#include <stdio.h>
#include <string.h>

#include "../src/libc/gravity/text.c"

static int fails = 0;
static int checks = 0;

static void ok(int cond, const char* what) {
    checks++;
    if (!cond) { printf("FAIL: %s\n", what); fails++; }
}

static void eqs(const struct gravity_text* t, const char* want, const char* what) {
    checks++;
    int n = (int)strlen(want);
    if (t->len != n || memcmp(t->buf, want, (size_t)n) != 0) {
        printf("FAIL: %s: got %.*s (%d), want %s (%d)\n",
               what, t->len, t->buf, t->len, want, n);
        fails++;
    }
}

int main(void) {
    struct gravity_text t;
    ok(gravity_text_init(&t, 64) == 0, "init");
    eqs(&t, "", "init empty");

    gravity_text_set(&t, "hello", 5);
    eqs(&t, "hello", "set");
    ok(gravity_text_line_count(&t) == 1, "one line");

    gravity_text_insert(&t, 5, " world", 6);
    eqs(&t, "hello world", "append");
    gravity_text_insert(&t, 0, ">> ", 3);
    eqs(&t, ">> hello world", "prepend");
    gravity_text_insert(&t, 3, "X", 1);
    eqs(&t, ">> Xhello world", "insert middle");

    gravity_text_erase(&t, 3, 1);
    eqs(&t, ">> hello world", "erase middle");
    gravity_text_erase(&t, 0, 3);
    eqs(&t, "hello world", "erase head");
    gravity_text_erase(&t, 5, 100); // clamps
    eqs(&t, "hello", "erase clamps");

    gravity_text_set(&t, "ab\ncd\n", 6);
    ok(gravity_text_line_count(&t) == 3, "three lines");
    ok(gravity_text_line_start(&t, 4) == 3, "line_start of line 2");
    ok(gravity_text_line_end(&t, 0) == 2, "line_end of line 1");
    ok(gravity_text_next_line(&t, 0) == 3, "next_line");
    ok(gravity_text_prev_line(&t, 3) == 0, "prev_line");
    ok(gravity_text_next_line(&t, 3) == 6, "next_line to empty last");
    ok(gravity_text_line_start(&t, 6) == 6, "start of trailing empty line");
    ok(gravity_text_prev_line(&t, 6) == 3, "prev from trailing empty line");

    // Tabs expand to GRAVITY_TEXT_TAB stops for column maths.
    gravity_text_set(&t, "a\tb\n\tX", 5);
    ok(gravity_text_col(&t, 0) == 0, "col 0");
    ok(gravity_text_col(&t, 1) == 1, "col after a");
    ok(gravity_text_col(&t, 2) == 4, "col after tab stop");
    ok(gravity_text_col(&t, 3) == 5, "col after b");
    ok(gravity_text_offset_at_col(&t, 0, 0) == 0, "offset col 0");
    ok(gravity_text_offset_at_col(&t, 0, 4) == 2, "offset to tab end");
    ok(gravity_text_offset_at_col(&t, 4, 4) == 5, "offset col 4 on line 2");
    ok(gravity_text_offset_at_col(&t, 0, 3) == 1, "offset inside a tab -> tab start");

    // Growth past the initial capacity.
    char big[900];
    for (int i = 0; i < 899; i++) big[i] = (char)('a' + (i % 26));
    big[899] = '\0';
    gravity_text_set(&t, big, 899);
    eqs(&t, big, "grow/set large");
    gravity_text_insert(&t, 899, big, 899);
    ok(t.len == 1798 && t.cap >= 1798, "grow on insert");
    for (int i = 0; i < 899; i++)
        if (t.buf[899 + i] != big[i]) { ok(0, "grown content"); break; }

    gravity_text_free(&t);
    printf("text buffer host test: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}