#include <stdio.h>
#include <string.h>
#include <errno.h>

// stdiotest — exercises the FILE* layer in the real environment: buffered
// reads/writes over the ramfs, fgets, fseek/ftell, append, the console
// streams, and the error paths (ENOENT on the ramfs, EROFS on the tarfs).
// Prints PASS/FAIL for the test harness.

static int fails = 0;
static void check(int ok, const char* what) {
    if (!ok) { printf("stdiotest: FAIL: %s\n", what); fails++; }
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    const char* path = "/tmp/stdio.txt";
    remove(path);   // clean slate if a previous run left it (may fail)

    // --- write with fprintf, check the size via ftell ------------------
    FILE* f = fopen(path, "w");
    check(f != 0, "fopen w");
    if (f == 0) { printf("stdiotest: %d check(s) failed\n", fails); return 1; }

    check(fprintf(f, "line one\n") == 9, "fprintf to file");
    check(fprintf(f, "value=%d %.2f\n", 42, 1.5) == 14, "fprintf formats to file");
    check(ftell(f) == 23, "ftell after buffered writes");
    check(fclose(f) == 0, "fclose");

    // --- read it back -------------------------------------------------
    char buf[64];
    f = fopen(path, "r");
    check(f != 0, "fopen r");
    check(fread(buf, 1, 23, f) == 23, "fread whole file");
    check(memcmp(buf, "line one\nvalue=42 1.50\n", 23) == 0, "fread contents");
    check(fgetc(f) == EOF && feof(f) == 1, "fgetc at eof + feof");
    fclose(f);

    // --- fgets, including a line longer than one buffer fill ----------
    f = fopen(path, "r");
    check(fgets(buf, sizeof buf, f) != 0 && strcmp(buf, "line one\n") == 0,
          "fgets line 1");
    check(fgets(buf, 9, f) != 0 && strcmp(buf, "value=42") == 0, "fgets size cap");
    check(fgets(buf, sizeof buf, f) != 0 && strcmp(buf, " 1.50\n") == 0, "fgets tail");
    fclose(f);

    // --- fseek / ftell -------------------------------------------------
    f = fopen(path, "r");
    check(fgetc(f) == 'l', "first byte");
    check(ftell(f) == 1, "ftell after one byte");
    check(fseek(f, 5, SEEK_SET) == 0 && fgetc(f) == 'o', "fseek SET");
    check(fseek(f, -1, SEEK_END) == 0 && fgetc(f) == '\n', "fseek END");
    check(fseek(f, -5, SEEK_CUR) == 0 && ftell(f) == 18, "fseek CUR + ftell");
    check(ungetc('Z', f) == 'Z' && fgetc(f) == 'Z', "ungetc");
    fclose(f);

    // --- append mode ---------------------------------------------------
    f = fopen(path, "a");
    check(f != 0, "fopen a");
    check(fwrite("tail\n", 1, 5, f) == 5, "append write");
    check(ftell(f) == 28, "append lands at end");
    fclose(f);

    f = fopen(path, "r");
    check(fseek(f, 0, SEEK_END) == 0 && ftell(f) == 28, "size after append");
    fseek(f, 0, SEEK_SET);
    check(fread(buf, 4, 6, f) == 6, "fread item count");
    fclose(f);

    // --- unbuffered mode ----------------------------------------------
    f = fopen("/tmp/stdio2.txt", "w");
    check(f != 0, "fopen w (2)");
    check(setvbuf(f, 0, _IONBF, 0) == 0, "setvbuf _IONBF");
    check(fwrite("xy", 1, 2, f) == 2, "unbuffered write");
    fclose(f);
    check(remove("/tmp/stdio2.txt") == 0, "remove");

    // --- errors --------------------------------------------------------
    errno = 0;
    check(fopen("/tmp/no-such-file", "r") == 0 && errno == ENOENT,
          "fopen missing -> ENOENT");
    errno = 0;
    check(fopen("/hello.txt", "w") == 0 && errno == EROFS,
          "fopen for write on the read-only tarfs -> EROFS");

    // --- read a file from the read-only tarfs --------------------------
    f = fopen("/hello.txt", "r");
    check(f != 0, "fopen a tarfs file");
    check(fgets(buf, sizeof buf, f) != 0 && strlen(buf) > 0, "fgets from tarfs");
    fclose(f);

    // --- the console streams ------------------------------------------
    check(fprintf(stdout, "stdiotest: console via stdout\n") > 0, "fprintf stdout");
    check(fputs("stdiotest: via fputs\n", stdout) == 0, "fputs stdout");
    check(fflush(stdout) == 0, "fflush stdout");

    // --- temporary names ----------------------------------------------
    char tn[64];
    check(tmpnam(tn) == tn && tn[0] == '/', "tmpnam");
    f = tmpfile();
    check(f != 0, "tmpfile");
    if (f) { check(fwrite("t", 1, 1, f) == 1, "tmpfile write"); fclose(f); }

    check(remove(path) == 0, "remove the test file");
    check(fopen(path, "r") == 0, "file is gone after remove");

    if (fails == 0) { printf("stdiotest: all checks pass\n"); return 0; }
    printf("stdiotest: %d check(s) failed\n", fails);
    return 1;
}
