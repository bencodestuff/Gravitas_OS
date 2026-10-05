#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <setjmp.h>

// libctest — exercises the libc basics (string, formatted output, allocator,
// parsing, sort). Prints PASS/FAIL for the smoke harness.

static int fails = 0;

static void check(int ok, const char* what) {
    if (!ok) { printf("libctest: FAIL: %s\n", what); fails++; }
}

static int cmp_int(const void* a, const void* b) {
    return *(const int*)a - *(const int*)b;
}

// --- setjmp/longjmp ---
static jmp_buf jb;
static int jump_seen = -1;

static void jump_deep(int n, int val) {
    if (n == 0) longjmp(jb, val);
    jump_deep(n - 1, val);
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    // --- string.h ---
    check(strcmp("abc", "abc") == 0, "strcmp equal");
    check(strcmp("abc", "abd") < 0, "strcmp less");
    check(strcmp("abd", "abc") > 0, "strcmp greater");
    check(strncmp("abcd", "abce", 3) == 0, "strncmp");

    char buf[64];
    strcpy(buf, "hello");
    check(strcmp(buf, "hello") == 0, "strcpy");
    strcat(buf, " world");
    check(strcmp(buf, "hello world") == 0, "strcat");
    check(strlen(buf) == 11, "strlen");
    check(strnlen("abc", 10) == 3, "strnlen");
    check(strnlen("abcdef", 4) == 4, "strnlen cap");
    check(strchr(buf, 'w') == buf + 6, "strchr");
    check(strrchr(buf, 'l') == buf + 9, "strrchr");
    check(strstr(buf, "lo w") == buf + 3, "strstr");
    check(strstr(buf, "zzz") == 0, "strstr miss");

    char* dc = strdup("dup");
    check(dc != 0 && strcmp(dc, "dup") == 0, "strdup");
    free(dc);

    // --- stdio (snprintf) ---
    snprintf(buf, sizeof(buf), "%d %x %s", 42, 0xbeef, "ok");
    check(strcmp(buf, "42 beef ok") == 0, "snprintf formats");
    snprintf(buf, 4, "%s", "abcdef");
    check(strcmp(buf, "abc") == 0, "snprintf truncates");
    snprintf(buf, sizeof(buf), "%u", 4294967295u);
    check(strcmp(buf, "4294967295") == 0, "snprintf unsigned");

    check(atoi("  -123") == -123, "atoi");
    check(atoi("+7") == 7, "atoi plus");
    check(strtol("ff", 0, 16) == 255, "strtol hex");
    check(strtol("0x1f", 0, 0) == 31, "strtol auto");
    char* end = 0;
    check(strtol("12ab", &end, 10) == 12 && *end == 'a', "strtol endptr");

    // --- floating point (SSE) ---
    double da = 1.5, db = 2.25;
    check(da + db == 3.75, "double add");
    check(da * 4.0 == 6.0, "double mul");
    check((int)(da * db) == 3, "double truncation");

    int* arr = (int*)calloc(5, sizeof(int));
    check(arr != 0 && arr[0] == 0 && arr[4] == 0, "calloc zeroes");
    arr = (int*)realloc(arr, 10 * sizeof(int));
    check(arr != 0 && arr[0] == 0, "realloc preserves");
    for (int i = 0; i < 10; i++) arr[i] = 10 - i;
    qsort(arr, 10, sizeof(int), cmp_int);
    check(arr[0] == 1 && arr[9] == 10, "qsort orders");
    free(arr);

    // --- setjmp/longjmp: unwind several frames and land back here ---
    int sv = setjmp(jb);
    if (sv == 0) jump_deep(6, 42);
    else jump_seen = sv;
    check(jump_seen == 42, "setjmp/longjmp deep unwind");

    sv = setjmp(jb);
    if (sv == 0) jump_deep(3, 0);
    else jump_seen = sv;
    check(jump_seen == 1, "longjmp(env, 0) returns 1");

    if (fails == 0) {
        printf("libctest: all checks pass\n");
        return 0;
    }
    printf("libctest: %d check(s) failed\n", fails);
    return 1;
}
