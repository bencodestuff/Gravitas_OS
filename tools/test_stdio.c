/* Host validation harness for the FILE layer (src/libc/stdio/file.c).
   Compiles the real file.c against fake implementations of the __stdio_* seam
   (stdio_io.h), backed by a tiny in-memory filesystem, so the buffering logic
   can be exercised without a kernel:

       cc -O2 -fno-builtin -Isrc/libc/include tools/test_stdio.c -o /tmp/test_stdio

   Run by tests/host/run.sh (`ninja check`). The seam is the only place file.c
   touches the kernel; everything else here is pure buffering. */

/* Compile our FILE layer under my_* names so it cannot interpose on the host
   libc's own FILE machinery (the host's printf is this test's only output).
   errno is renamed too: glibc's errno is not a plain symbol. */
#define fopen     my_fopen
#define freopen   my_freopen
#define fclose    my_fclose
#define fread     my_fread
#define fwrite    my_fwrite
#define fflush    my_fflush
#define fseek     my_fseek
#define ftell     my_ftell
#define feof      my_feof
#define ferror    my_ferror
#define clearerr  my_clearerr
#define fileno    my_fileno
#define fgetc     my_fgetc
#define getc      my_getc
#define getchar   my_getchar
#define ungetc    my_ungetc
#define fputc     my_fputc
#define putc      my_putc
#define fputs     my_fputs
#define fgets     my_fgets
#define setvbuf   my_setvbuf
#define tmpnam    my_tmpnam
#define tmpfile   my_tmpfile
#define stdin     my_stdin
#define stdout    my_stdout
#define stderr    my_stderr
#define errno     my_errno

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "../src/libc/stdio/stdio_io.h"

/* Our errno object (see the rename above). */
int my_errno = 0;

/* --- a tiny in-memory "filesystem" ------------------------------------ */

#define NFILES 3
#define FCAP   8192

static struct ff {
    const char* name;
    unsigned char data[FCAP];
    long len;
    int exists;
} fs[NFILES] = {
    { "a.txt",  {0}, 0, 0 },
    { "b.txt",  {0}, 0, 0 },
    { "ro.txt", {0}, 0, 1 },   /* pre-existing, used for read-only tests */
};

static struct ff* find_file(const char* name) {
    for (int i = 0; i < NFILES; i++)
        if (fs[i].name && strcmp(fs[i].name, name) == 0) return &fs[i];
    return 0;
}

#define NOPEN 8
static struct {
    struct ff* file;
    int mayread, maywrite;
    long pos;
    int used;
} opens[NOPEN];

int __stdio_open(const char* path, int flags) {
    struct ff* f = find_file(path);
    if (f == 0) { errno = ENOENT; return -1; }
    if (!f->exists) {
        if (!(flags & GRV_O_CREATE)) { errno = ENOENT; return -1; }
        f->exists = 1;
        f->len = 0;
    }
    if (flags & GRV_O_TRUNC) f->len = 0;

    int wr = (flags & GRV_O_WRONLY) != 0;
    int rw = (flags & GRV_O_RDWR) != 0;
    for (int i = 0; i < NOPEN; i++) {
        if (opens[i].used) continue;
        opens[i].used = 1;
        opens[i].file = f;
        opens[i].pos = 0;
        opens[i].maywrite = wr || rw;
        opens[i].mayread = !wr || rw;
        return i;
    }
    errno = EMFILE;
    return -1;
}

long __stdio_read(int fd, void* buf, size_t n) {
    if (fd < 0 || fd >= NOPEN || !opens[fd].used) { errno = EBADF; return -1; }
    if (!opens[fd].mayread) { errno = EACCES; return -1; }
    struct ff* f = opens[fd].file;
    long avail = f->len - opens[fd].pos;
    if (avail <= 0) return 0;
    long take = (long)n < avail ? (long)n : avail;
    memcpy(buf, f->data + opens[fd].pos, (size_t)take);
    opens[fd].pos += take;
    return take;
}

long __stdio_write(int fd, const void* buf, size_t n) {
    if (fd < 0 || fd >= NOPEN || !opens[fd].used) { errno = EBADF; return -1; }
    if (!opens[fd].maywrite) { errno = EACCES; return -1; }
    struct ff* f = opens[fd].file;
    if (opens[fd].pos + (long)n > FCAP) { errno = ENOSPC; return -1; }
    memcpy(f->data + opens[fd].pos, buf, n);
    opens[fd].pos += (long)n;
    if (opens[fd].pos > f->len) f->len = opens[fd].pos;
    return (long)n;
}

long __stdio_lseek(int fd, long off, int whence) {
    if (fd < 0 || fd >= NOPEN || !opens[fd].used) { errno = EBADF; return -1; }
    struct ff* f = opens[fd].file;
    long base = whence == SEEK_SET ? 0
              : whence == SEEK_CUR ? opens[fd].pos
              : whence == SEEK_END ? f->len : -1;
    if (base < 0) { errno = EINVAL; return -1; }
    long p = base + off;
    if (p < 0) { errno = EINVAL; return -1; }
    opens[fd].pos = p;
    return p;
}

int __stdio_close(int fd) {
    if (fd < 0 || fd >= NOPEN || !opens[fd].used) { errno = EBADF; return -1; }
    opens[fd].used = 0;
    return 0;
}

/* Console capture: writes land in a buffer, reads come from a fixed string. */
static char cout[4096];
static size_t cout_len = 0;

int __stdio_putc(char c) {
    if (cout_len < sizeof(cout) - 1) cout[cout_len++] = c;
    return (unsigned char)c;
}

static const char* cin = "";
static size_t cin_pos = 0;

int __stdio_getc(void) {
    if (cin[cin_pos] == '\0') return -1;   /* our console never EOFs in practice */
    return (unsigned char)cin[cin_pos++];
}

/* --- the real FILE layer ---------------------------------------------- */

#include "../src/libc/stdio/file.c"

/* --- checks ----------------------------------------------------------- */

static int checks = 0, fails = 0;

static void ck(int ok, const char* what) {
    checks++;
    if (!ok) { printf("FAIL %s\n", what); fails++; }
}

static long file_len(const char* name) {
    struct ff* f = find_file(name);
    return f ? f->len : -1;
}

int main(void) {
    char buf[1100];
    FILE* f;

    /* write then read back, through the buffer */
    f = fopen("a.txt", "w");
    ck(f != 0, "fopen w");
    ck(fwrite("hello world", 1, 11, f) == 11, "fwrite");
    ck(ftell(f) == 11, "ftell after write");
    ck(fclose(f) == 0, "fclose");
    ck(file_len("a.txt") == 11, "file length after close");

    f = fopen("a.txt", "r");
    ck(f != 0, "fopen r");
    memset(buf, 0, sizeof buf);
    ck(fread(buf, 1, 11, f) == 11, "fread");
    ck(memcmp(buf, "hello world", 11) == 0, "fread contents");
    ck(fgetc(f) == EOF, "fgetc at eof");
    ck(feof(f) == 1, "feof set");
    ck(fclose(f) == 0, "fclose r");

    /* a write larger than the buffer, read back in awkward chunks */
    f = fopen("b.txt", "w");
    for (int i = 0; i < 1000; i++) buf[i] = (char)(i * 7);
    ck(fwrite(buf, 1, 1000, f) == 1000, "fwrite 1000");
    ck(ftell(f) == 1000, "ftell after big write");
    fclose(f);
    ck(file_len("b.txt") == 1000, "big length");

    f = fopen("b.txt", "r");
    int pos = 0, chunk[3] = {1, 3, 7};
    int ci = 0;
    for (;;) {
        int n = chunk[ci++ % 3];
        int got = (int)fread(buf, 1, (size_t)n, f);
        for (int i = 0; i < got; i++)
            if ((unsigned char)buf[i] != (unsigned char)((pos + i) * 7)) {
                ck(0, "chunked read contents");
                goto chunk_done;
            }
        pos += got;
        if (got < n) break;
    }
chunk_done:
    ck(pos == 1000, "chunked read total");
    ck(feof(f) == 1, "chunked read eof");
    fclose(f);

    /* fseek / ftell relative to the buffered logical position */
    f = fopen("b.txt", "r");
    ck(fgetc(f) == (char)(0 * 7), "seek: first byte");
    ck(ftell(f) == 1, "ftell after 1 byte");
    ck(fseek(f, 10, SEEK_SET) == 0, "fseek SET");
    ck(ftell(f) == 10, "ftell after SET");
    ck(fgetc(f) == (char)(10 * 7), "read after SET");
    ck(fseek(f, -5, SEEK_CUR) == 0 && ftell(f) == 6, "fseek CUR");
    ck(fgetc(f) == (char)(6 * 7), "read after CUR");
    ck(fseek(f, -4, SEEK_END) == 0 && ftell(f) == 996, "fseek END");
    ck(fgetc(f) == (char)(996 * 7), "read after END");
    fclose(f);

    /* ungetc */
    f = fopen("a.txt", "r");
    ck(fgetc(f) == 'h', "ungetc: read h");
    ck(ungetc('H', f) == 'H', "ungetc");
    ck(fgetc(f) == 'H', "ungetc returned");
    ck(ungetc('x', f) == 'x', "ungetc again");
    ck(ungetc('y', f) == EOF, "ungetc second fails");
    fclose(f);

    /* fgets: lines, a long line, and the size limit */
    f = fopen("a.txt", "w");
    fputs("one\ntwo\nthree long enough to matter\n", f);
    fclose(f);
    f = fopen("a.txt", "r");
    ck(fgets(buf, sizeof buf, f) != 0 && strcmp(buf, "one\n") == 0, "fgets 1");
    ck(fgets(buf, 4, f) != 0 && strcmp(buf, "two") == 0, "fgets size cap");
    ck(fgets(buf, 6, f) != 0 && strcmp(buf, "\n") == 0, "fgets stops at newline");
    ck(fgets(buf, sizeof buf, f) != 0 &&
       strcmp(buf, "three long enough to matter\n") == 0, "fgets long line");
    ck(fgets(buf, sizeof buf, f) == 0 && feof(f), "fgets eof -> NULL");
    fclose(f);

    /* append mode */
    f = fopen("a.txt", "a");
    ck(f != 0, "fopen a");
    ck(fwrite("X", 1, 1, f) == 1, "append write");
    fclose(f);
    f = fopen("a.txt", "r");
    long L = file_len("a.txt");
    fseek(f, L - 1, SEEK_SET);
    ck(fgetc(f) == 'X', "append landed at end");
    fclose(f);

    /* fread with size > 1 returns whole items */
    f = fopen("b.txt", "r");
    ck(fread(buf, 4, 3, f) == 3, "fread items");
    ck((unsigned char)buf[11] == (unsigned char)(11 * 7), "fread item bytes");
    fclose(f);

    /* ferror: writing to a read-only stream */
    f = fopen("ro.txt", "r");
    ck(fwrite("nope", 1, 4, f) == 0, "fwrite on read-only returns 0");
    ck(ferror(f) == 1, "ferror set");
    clearerr(f);
    ck(ferror(f) == 0, "clearerr");
    fclose(f);

    /* stdout goes to the console, unbuffered */
    cout_len = 0;
    ck(fwrite("out", 1, 3, stdout) == 3, "stdout fwrite");
    ck(cout_len == 3 && memcmp(cout, "out", 3) == 0, "stdout reaches console");

    /* setvbuf(_IONBF) writes straight through */
    f = fopen("b.txt", "w");
    ck(setvbuf(f, 0, _IONBF, 0) == 0, "setvbuf IONBF");
    ck(fwrite("abc", 1, 3, f) == 3, "unbuffered write");
    ck(file_len("b.txt") == 3, "unbuffered write is immediately visible");
    fclose(f);

    /* reading stdin from the fake console */
    cin = "hi!";
    cin_pos = 0;
    ck(fgetc(stdin) == 'h' && fgetc(stdin) == 'i' && fgetc(stdin) == '!',
       "stdin fgetc");

    /* errors: missing file */
    errno = 0;
    ck(fopen("nope.txt", "r") == 0 && errno == ENOENT, "fopen missing -> ENOENT");

    /* fileno + freopen */
    f = fopen("a.txt", "r");
    ck(fileno(f) >= 0, "fileno");
    ck(freopen("b.txt", "r", f) == f, "freopen returns the stream");
    ck(fgetc(f) == 'a', "freopen reads the new file");
    fclose(f);

    printf("stdio host test: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
