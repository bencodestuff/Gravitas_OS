#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>
#include <locale.h>
#include <time.h>
#include <signal.h>
#include <setjmp.h>
#include <errno.h>
#include <grv/syscall.h>

// libctest — exercises the libc basics (string, formatted output, allocator,
// parsing, sort). Prints PASS/FAIL for the test harness.

static int fails = 0;

static void check(int ok, const char* what) {
    if (!ok) { printf("libctest: FAIL: %s\n", what); fails++; }
}

static int cmp_int(const void* a, const void* b) {
    return *(const int*)a - *(const int*)b;
}

// A handler the signal() stub must refuse (there is no userland delivery yet).
static void dummy_handler(int sig) { (void)sig; }

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

    // --- Lua-port helpers: the rest of the string/ctype/stdlib surface ---
    check(memchr(buf, 'o', sizeof(buf)) == buf + 4, "memchr");
    check(memchr(buf, 'z', 5) == 0, "memchr miss");
    {
        char sb[8];
        char* se = stpcpy(sb, "hi");
        check(se == sb + 2 && *se == '\0' && strcmp(sb, "hi") == 0, "stpcpy");
    }
    check(strspn("abcXY", "abc") == 3, "strspn");
    check(strcspn("abc:def", ":") == 3, "strcspn");
    check(strpbrk("hello", "xyzl") != 0 && *strpbrk("hello", "xyzl") == 'l', "strpbrk");
    check(strpbrk("hello", "xyz") == 0, "strpbrk miss");
    check(strcasecmp("HeLLo", "hello") == 0, "strcasecmp");
    check(strcasecmp("abc", "abd") < 0, "strcasecmp order");
    check(strncasecmp("HELLO", "help", 3) == 0, "strncasecmp");
    check(strcoll("abc", "abd") < 0, "strcoll");
    check(isgraph('!') && isgraph('0') && !isgraph(' ') && !isgraph('\n'), "isgraph");
    check(abs(-7) == 7 && abs(4) == 4, "abs");
    check(strtoul("4294967295", 0, 10) == 4294967295UL, "strtoul");
    check(strtoul("-1", 0, 10) == (unsigned long)-1, "strtoul wraps");
    check(ifloor(2.7) == 2 && iceil(2.1) == 3, "ifloor/iceil");
    check(ifloor(-2.1) == -3 && iceil(-2.7) == -2, "ifloor/iceil negative");
    {
        static char toks[] = "a,b,,c";
        char* sp = toks;
        check(strcmp(strsep(&sp, ","), "a") == 0, "strsep 1");
        check(strcmp(strsep(&sp, ","), "b") == 0, "strsep 2");
        check(strcmp(strsep(&sp, ","), "") == 0, "strsep empty field");
        check(strcmp(strsep(&sp, ","), "c") == 0, "strsep 3");
        check(strsep(&sp, ",") == 0, "strsep end");
    }

    // --- stdio (snprintf) ---
    snprintf(buf, sizeof(buf), "%d %x %s", 42, 0xbeef, "ok");
    check(strcmp(buf, "42 beef ok") == 0, "snprintf formats");
    snprintf(buf, 4, "%s", "abcdef");
    check(strcmp(buf, "abc") == 0, "snprintf truncates");
    snprintf(buf, sizeof(buf), "%u", 4294967295u);
    check(strcmp(buf, "4294967295") == 0, "snprintf unsigned");

    // Float formatting (%.14g is Lua's number format).
    snprintf(buf, sizeof(buf), "%.14g", 1.5);
    check(strcmp(buf, "1.5") == 0, "snprintf %.14g");
    snprintf(buf, sizeof(buf), "%.2f", 3.14159);
    check(strcmp(buf, "3.14") == 0, "snprintf %.2f");
    snprintf(buf, sizeof(buf), "%e", 12345.678);
    check(strcmp(buf, "1.234568e+04") == 0, "snprintf %e");
    snprintf(buf, sizeof(buf), "%5.2f", -1.5);
    check(strcmp(buf, "-1.50") == 0, "snprintf width/precision");
    snprintf(buf, sizeof(buf), "%.14g", 1.0 / 3.0);
    check(strcmp(buf, "0.33333333333333") == 0, "snprintf 1/3");
    snprintf(buf, sizeof(buf), "%lld", 1234567890123LL);
    check(strcmp(buf, "1234567890123") == 0, "snprintf lld");

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

    // --- strtod ---
    char* endp = 0;
    check(strtod("3.5", 0) == 3.5, "strtod simple");
    check(strtod("-2.5e2", 0) == -250.0, "strtod exponent");
    check(strtod("1e-3", 0) == 0.001, "strtod negative exponent");
    endp = 0;
    check(strtod("abc", &endp) == 0.0 && endp != 0 && *endp == 'a', "strtod no digits");
    endp = 0;
    check(strtod("12.5xyz", &endp) == 12.5 && *endp == 'x', "strtod endptr");

    // --- math (x87-backed) ---
    check(sqrt(4.0) == 2.0, "sqrt");
    check(pow(2.0, 10.0) == 1024.0, "pow integer exponent");
    check(fmod(10.0, 3.0) == 1.0, "fmod");
    check(floor(-1.5) == -2.0 && ceil(-1.5) == -1.0, "floor/ceil");
    check(trunc(-1.9) == -1.0 && round(2.5) == 3.0, "trunc/round");
    check(fabs(-3.25) == 3.25, "fabs");
    check(sin(0.0) == 0.0 && cos(0.0) == 1.0, "sin/cos at 0");
    check(exp(0.0) == 1.0 && log(1.0) == 0.0, "exp/log at 1");
    check(fabs(log10(1000.0) - 3.0) < 1e-14, "log10");
    check(atan2(0.0, 1.0) == 0.0, "atan2");
    double d2 = sqrt(2.0);
    check(fabs(d2 * d2 - 2.0) < 1e-15, "sqrt(2) squared");
    check(fabs(pow(2.0, 0.5) - d2) < 1e-15, "pow(2, 0.5)");
    check(fabs(sin(1.0) - 0.8414709848078965) < 1e-14, "sin(1)");

    // --- errno: syscalls return -errno; wrappers map to -1 + errno ---
    struct grv_stat st;
    errno = 0;
    check(grv_open("/no/such/file", GRV_O_RDONLY) == -1 && errno == ENOENT,
          "open missing -> ENOENT");
    errno = 0;
    check(grv_stat("/no/such/file", &st) == -1 && errno == ENOENT,
          "stat missing -> ENOENT");
    errno = 0;
    check(grv_read_fd(99, buf, 4) == -1 && errno == EBADF,
          "read bad fd -> EBADF");
    errno = 0;
    check(grv_mkdir("/bin/nope") == -1 && errno == EROFS,
          "mkdir on the read-only tarfs -> EROFS");
    errno = 0;
    struct grv_msg em;
    check(grv_recv(&em, 0) == -1 && errno == EAGAIN,
          "empty poll -> EAGAIN");
    // The dispatcher is a number->handler table: a retired or unknown number
    // is reported rather than echoed back.
    check(syscall1(999, 0) == -ENOSYS, "unknown syscall -> ENOSYS");
    check(syscall1(2, 0) == -ENOSYS, "retired syscall 2 -> ENOSYS");

    // --- strerror / getenv / locale (the C-locale surface) ---
    check(strcmp(strerror(ENOENT), "No such file or directory") == 0, "strerror ENOENT");
    check(strcmp(strerror(EROFS), "Read-only filesystem") == 0, "strerror EROFS");
    check(strerror(9999) != 0 && strerror(9999)[0] != '\0', "strerror unknown readable");
    check(getenv("PATH") == 0, "getenv miss -> NULL (no environment)");
    check(setlocale(LC_ALL, NULL) != 0, "setlocale query");
    check(setlocale(LC_ALL, "C") != 0, "setlocale C");
    check(setlocale(LC_ALL, "POSIX") != 0, "setlocale POSIX");
    check(setlocale(LC_ALL, "") != 0, "setlocale empty -> C");
    check(setlocale(LC_ALL, "xx_YY") == 0, "setlocale unknown -> NULL");
    check(localeconv() != 0 && localeconv()->decimal_point[0] == '.',
          "localeconv decimal point");

    // --- time / signal / system ---
    time_t now = time(0);
    check(now > 1000000000, "time() is a real epoch (RTC read at boot)");
    time_t now2 = 0;
    check(time(&now2) == now2, "time(tloc)");

    time_t zero = 0;
    struct tm* epoch = gmtime(&zero);
    check(epoch != 0 && epoch->tm_year == 70 && epoch->tm_mon == 0 &&
          epoch->tm_mday == 1 && epoch->tm_wday == 4 && epoch->tm_yday == 0,
          "gmtime(0) = 1970-01-01 Thursday");
    struct tm* lt = localtime(&zero);
    check(lt != 0 && lt->tm_year == 70 && lt->tm_mday == 1, "localtime == UTC");

    struct tm rt = *epoch;
    check(mktime(&rt) == 0 && rt.tm_wday == 4, "mktime round-trip + wday");

    char tbuf[64];
    check(strftime(tbuf, sizeof tbuf, "%Y-%m-%d %H:%M:%S", epoch) > 0 &&
          strcmp(tbuf, "1970-01-01 00:00:00") == 0, "strftime date+time");
    check(strftime(tbuf, sizeof tbuf, "%a %b %d", epoch) > 0 &&
          strcmp(tbuf, "Thu Jan 01") == 0, "strftime names");
    check(difftime(100, 40) == 60.0, "difftime");
    check(clock() >= 0, "clock()");

    check(signal(SIGINT, SIG_IGN) != SIG_ERR, "signal SIG_IGN accepted");
    check(signal(SIGINT, SIG_DFL) != SIG_ERR, "signal SIG_DFL accepted");
    check(signal(SIGINT, dummy_handler) == SIG_ERR, "signal handler refused");
    check(system("true") == -1, "system has no shell");

    int* arr = (int*)calloc(5, sizeof(int));
    check(arr != 0 && arr[0] == 0 && arr[4] == 0, "calloc zeroes");
    arr = (int*)realloc(arr, 10 * sizeof(int));
    check(arr != 0 && arr[0] == 0, "realloc preserves");
    for (int i = 0; i < 10; i++) arr[i] = 10 - i;
    qsort(arr, 10, sizeof(int), cmp_int);
    check(arr[0] == 1 && arr[9] == 10, "qsort orders");
    free(arr);

    // --- allocator: split on reuse, coalesce, realloc in place ---
    // A freed 32K block must serve 512 small requests without growing the
    // break: that is the split path (a big free chunk handed out in pieces).
    void* big = malloc(32768);
    check(big != 0, "malloc 32K");
    free(big);
    void* brk_a = grv_brk((void*)0);
    void* small[512];
    for (int i = 0; i < 512; i++) small[i] = malloc(48);
    check(small[0] != 0 && small[511] != 0, "many small from one big");
    check((char*)grv_brk((void*)0) <= (char*)brk_a + 4096, "split reuses a freed block");
    for (int i = 0; i < 512; i++) free(small[i]);

    // Two freed neighbours must coalesce back into one usable block.
    void* na = malloc(1000);
    void* nb = malloc(1000);
    void* brk_b = grv_brk((void*)0);
    free(na);
    free(nb);
    void* merged = malloc(2000);
    check(merged != 0, "neighbours coalesce");
    check((char*)grv_brk((void*)0) <= (char*)brk_b, "coalesced without growing");
    free(merged);

    // realloc keeps contents across grow (in place or copied) and shrink.
    char* rp = (char*)malloc(64);
    check(rp != 0, "malloc 64");
    for (int i = 0; i < 64; i++) rp[i] = (char)i;
    rp = (char*)realloc(rp, 4096);
    check(rp != 0, "realloc grow");
    int intact = 1;
    for (int i = 0; i < 64; i++) if (rp[i] != (char)i) intact = 0;
    check(intact, "realloc grow preserves contents");
    rp = (char*)realloc(rp, 32);
    check(rp != 0, "realloc shrink");
    intact = 1;
    for (int i = 0; i < 32; i++) if (rp[i] != (char)i) intact = 0;
    check(intact, "realloc shrink preserves contents");
    free(rp);

    // Churn: allocate/free mixed sizes in rounds; every block must keep its
    // own pattern (catches merge/split mistakes that corrupt neighbours).
    static unsigned char* blk[128];
    for (int round = 0; round < 4; round++) {
        for (int i = 0; i < 128; i++) {
            int n = 8 + (i % 7) * 16;
            blk[i] = (unsigned char*)malloc((size_t)n);
            if (!blk[i]) break;
            for (int k = 0; k < n; k++) blk[i][k] = (unsigned char)(i + k);
        }
        int bad = 0;
        for (int i = 0; i < 128; i++) {
            int n = 8 + (i % 7) * 16;
            if (!blk[i]) continue;
            for (int k = 0; k < n; k++)
                if (blk[i][k] != (unsigned char)(i + k)) bad = 1;
        }
        check(!bad, "heap churn keeps blocks intact");
        for (int i = 0; i < 128; i++) { free(blk[i]); blk[i] = 0; }
    }

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
