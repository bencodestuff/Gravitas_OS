#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "stdio_io.h"

// The FILE layer. A stream wraps a descriptor, which is one of two things:
//
//   * a STANDARD stream — stdin/stdout/stderr. The kernel gives every task
//     descriptors 0/1/2: a console by default, or whatever the shell pointed
//     them at (a file, a pipe end) with dup2. They are unbuffered, so a
//     terminal sees output the moment it is produced and a redirect lands in
//     the file at once.
//   * a FILE stream — a descriptor from fopen(). These buffer through one
//     BUFSIZ (or setvbuf) buffer, because that is where buffering pays.
//
// A single buffer serves reads and writes: a stream is read-or-write at a time,
// and switching directions flushes (write -> read) or rewinds the descriptor to
// the logical position (read -> write), which is what the C model expects.
//
// Every kernel call goes through the __stdio_* seam (stdio_io.h), so the
// buffering logic can be exercised on the host by tools/test_stdio.c.

#define KIND_FILE 1

#define FL_READ  0x1u
#define FL_WRITE 0x2u
#define FL_EOF   0x4u
#define FL_ERR   0x8u

struct _FILE {
    int kind;
    int which;           // the descriptor
    unsigned flags;
    int ungot;           // pushback byte, -1 when empty
    unsigned char* buf;  // file streams only
    size_t bufsize;
    int owns_buf;
    size_t rpos, rlen;   // read window: valid bytes are buf[rpos..rlen)
    size_t wlen;         // bytes pending in the write buffer
    long off;            // the descriptor's physical offset
};

static struct _FILE stdin_file  = { .kind = KIND_FILE, .which = 0,
                                    .flags = FL_READ, .ungot = -1 };
static struct _FILE stdout_file = { .kind = KIND_FILE, .which = 1,
                                    .flags = FL_WRITE, .ungot = -1 };
static struct _FILE stderr_file = { .kind = KIND_FILE, .which = 2,
                                    .flags = FL_WRITE, .ungot = -1 };

FILE* stdin  = &stdin_file;
FILE* stdout = &stdout_file;
FILE* stderr = &stderr_file;

static int is_std(const FILE* f) {
    return f == &stdin_file || f == &stdout_file || f == &stderr_file;
}

static void init_stream(FILE* f, int kind, int which, unsigned flags) {
    f->kind = kind;
    f->which = which;
    f->flags = flags;
    f->ungot = -1;
    f->buf = 0;
    f->bufsize = 0;
    f->owns_buf = 0;
    f->rpos = f->rlen = f->wlen = 0;
    f->off = 0;
}

// --- buffering ---------------------------------------------------------

static int flush_write(FILE* f) {
    if (f->wlen == 0) return 0;
    size_t done = 0;
    while (done < f->wlen) {
        long n = __stdio_write(f->which, f->buf + done, f->wlen - done);
        if (n <= 0) { f->flags |= FL_ERR; f->wlen = 0; return -1; }
        done += (size_t)n;
    }
    f->off += (long)f->wlen;
    f->wlen = 0;
    return 0;
}

// Drop the read window, moving the descriptor back onto the logical position so
// that a following write or seek starts where the last consumed byte ended.
static void discard_read(FILE* f) {
    if (f->rlen > f->rpos) {
        f->off -= (long)(f->rlen - f->rpos);
        __stdio_lseek(f->which, f->off, SEEK_SET);
    }
    f->rpos = f->rlen = 0;
}

static int stream_write(FILE* f, const unsigned char* p, size_t n) {
    if (!(f->flags & FL_WRITE)) { f->flags |= FL_ERR; return -1; }

    if (f->rlen > f->rpos) discard_read(f);   // switching read -> write

    while (n > 0) {
        if (f->bufsize == 0) {                // unbuffered
            long r = __stdio_write(f->which, p, n);
            if (r <= 0) { f->flags |= FL_ERR; return -1; }
            f->off += (long)r; p += r; n -= (size_t)r;
            continue;
        }
        if (f->wlen == f->bufsize && flush_write(f) < 0) return -1;
        if (n >= f->bufsize && f->wlen == 0) {   // big write goes straight out
            long r = __stdio_write(f->which, p, n);
            if (r <= 0) { f->flags |= FL_ERR; return -1; }
            f->off += (long)r; p += r; n -= (size_t)r;
            continue;
        }
        size_t room = f->bufsize - f->wlen;
        size_t take = n < room ? n : room;
        memcpy(f->buf + f->wlen, p, take);
        f->wlen += take; p += take; n -= take;
    }
    return 0;
}

static size_t stream_read(FILE* f, unsigned char* dst, size_t want) {
    if (!(f->flags & FL_READ)) { f->flags |= FL_ERR; return 0; }
    if (want == 0) return 0;

    size_t got = 0;
    if (f->ungot >= 0) { dst[got++] = (unsigned char)f->ungot; f->ungot = -1; }

    while (got < want) {
        if (f->rpos < f->rlen) {
            size_t avail = f->rlen - f->rpos;
            size_t take = want - got < avail ? want - got : avail;
            memcpy(dst + got, f->buf + f->rpos, take);
            f->rpos += take; got += take;
            continue;
        }
        if (f->wlen > 0 && flush_write(f) < 0) break;   // write -> read
        if (f->bufsize == 0 || want - got >= f->bufsize) {
            long n = __stdio_read(f->which, dst + got, want - got);
            if (n <= 0) { f->flags |= n == 0 ? FL_EOF : FL_ERR; break; }
            f->off += n; got += (size_t)n;
            continue;
        }
        long n = __stdio_read(f->which, f->buf, f->bufsize);
        if (n <= 0) { f->flags |= n == 0 ? FL_EOF : FL_ERR; break; }
        f->off += n; f->rpos = 0; f->rlen = (size_t)n;
    }
    return got;
}

// --- open / close ------------------------------------------------------

// Parse a mode string. 'b' is accepted and ignored; 'x' (exclusive) is ignored
// because O_EXCL does not exist yet — fopen("wx") behaves like "w".
static int parse_mode(const char* mode, int* oflags, unsigned* pflags, int* append) {
    if (mode == 0 || mode[0] == '\0') return -1;

    int plus = 0;
    for (const char* p = mode + 1; *p; p++)
        if (*p == '+') plus = 1;

    *append = 0;
    switch (mode[0]) {
    case 'r':
        *oflags = plus ? GRV_O_RDWR : GRV_O_RDONLY;
        *pflags = FL_READ | (plus ? FL_WRITE : 0);
        return 0;
    case 'w':
        *oflags = (plus ? GRV_O_RDWR : GRV_O_WRONLY) | GRV_O_CREATE | GRV_O_TRUNC;
        *pflags = FL_WRITE | (plus ? FL_READ : 0);
        return 0;
    case 'a':
        *oflags = (plus ? GRV_O_RDWR : GRV_O_WRONLY) | GRV_O_CREATE;
        *pflags = FL_WRITE | (plus ? FL_READ : 0);
        *append = 1;
        return 0;
    default:
        return -1;
    }
}

static FILE* open_stream(const char* path, const char* mode) {
    int oflags, append;
    unsigned fl;
    if (parse_mode(mode, &oflags, &fl, &append) < 0) { errno = EINVAL; return 0; }

    int fd = __stdio_open(path, oflags);
    if (fd < 0) return 0;                 // errno came from the seam

    long start = 0;
    if (append) {
        start = __stdio_lseek(fd, 0, SEEK_END);
        if (start < 0) { __stdio_close(fd); return 0; }
    }

    FILE* f = (FILE*)malloc(sizeof(struct _FILE));
    if (f == 0) { __stdio_close(fd); errno = ENOMEM; return 0; }

    init_stream(f, KIND_FILE, fd, fl);
    f->off = start;
    f->buf = (unsigned char*)malloc(BUFSIZ);
    if (f->buf) { f->bufsize = BUFSIZ; f->owns_buf = 1; }
    // On allocation failure the stream is simply unbuffered — still correct.
    return f;
}

FILE* fopen(const char* path, const char* mode) {
    return open_stream(path, mode);
}

FILE* freopen(const char* path, const char* mode, FILE* f) {
    if (f == 0 || path == 0) return 0;

    // Release what the stream holds, but keep the FILE object itself.
    if (f->flags & FL_WRITE) flush_write(f);
    if (f->kind == KIND_FILE && f->which >= 0) __stdio_close(f->which);
    if (f->owns_buf) { free(f->buf); f->owns_buf = 0; }

    int oflags, append;
    unsigned fl;
    int fd = -1;
    long start = 0;
    if (parse_mode(mode, &oflags, &fl, &append) < 0) {
        errno = EINVAL;
    } else if ((fd = __stdio_open(path, oflags)) >= 0) {
        if (append) {
            start = __stdio_lseek(fd, 0, SEEK_END);
            if (start < 0) start = 0;
        }
    }

    if (fd < 0) { init_stream(f, -1, -1, 0); return 0; }

    init_stream(f, KIND_FILE, fd, fl);
    f->off = start;
    f->buf = (unsigned char*)malloc(BUFSIZ);
    if (f->buf) { f->bufsize = BUFSIZ; f->owns_buf = 1; }
    return f;
}

int fclose(FILE* f) {
    if (f == 0) return EOF;

    int rc = 0;
    if (f->flags & FL_WRITE) { if (flush_write(f) < 0) rc = -1; }
    if (f->kind == KIND_FILE && f->which >= 0) {
        if (__stdio_close(f->which) < 0) rc = -1;
    }

    int dynamic = !is_std(f);             // decide before touching the object
    if (f->owns_buf) { free(f->buf); f->owns_buf = 0; }
    f->buf = 0;
    f->bufsize = 0;
    f->rpos = f->rlen = f->wlen = 0;
    f->flags = 0;
    f->kind = -1;
    f->which = -1;
    f->ungot = -1;

    if (dynamic) free(f);
    return rc < 0 ? EOF : 0;
}

// --- reads and writes --------------------------------------------------

size_t fread(void* ptr, size_t size, size_t nmemb, FILE* f) {
    if (f == 0 || size == 0 || nmemb == 0) return 0;
    size_t want = size * nmemb;
    if (want / size != nmemb) { f->flags |= FL_ERR; return 0; }
    return stream_read(f, (unsigned char*)ptr, want) / size;
}

size_t fwrite(const void* ptr, size_t size, size_t nmemb, FILE* f) {
    if (f == 0 || size == 0 || nmemb == 0) return 0;
    size_t want = size * nmemb;
    if (want / size != nmemb) { f->flags |= FL_ERR; return 0; }
    return stream_write(f, (const unsigned char*)ptr, want) < 0 ? 0 : nmemb;
}

int fflush(FILE* f) {
    if (f == 0) {
        // No stream registry: flush the two console outputs, which are
        // unbuffered anyway, so this is trivially satisfied.
        int rc = 0;
        if (flush_write(stdout) < 0) rc = -1;
        if (flush_write(stderr) < 0) rc = -1;
        return rc < 0 ? EOF : 0;
    }
    if (f->flags & FL_WRITE) return flush_write(f) < 0 ? EOF : 0;
    discard_read(f);                      // input: drop the buffer
    return 0;
}

// --- character I/O -----------------------------------------------------

int fgetc(FILE* f) {
    if (f == 0) return EOF;
    unsigned char c;
    return stream_read(f, &c, 1) == 1 ? (int)c : EOF;
}

int getc(FILE* f) { return fgetc(f); }
int getchar(void) { return fgetc(stdin); }

int ungetc(int c, FILE* f) {
    if (f == 0 || c == EOF) return EOF;
    if (f->ungot >= 0) return EOF;        // one byte of pushback is all we keep
    f->ungot = (unsigned char)c;
    f->flags &= ~FL_EOF;
    return (unsigned char)c;
}

int fputc(int c, FILE* f) {
    if (f == 0) return EOF;
    unsigned char b = (unsigned char)c;
    return stream_write(f, &b, 1) < 0 ? EOF : b;
}

int putc(int c, FILE* f) { return fputc(c, f); }

int fputs(const char* s, FILE* f) {
    if (s == 0 || f == 0) return EOF;
    size_t n = strlen(s);
    if (n == 0) return 0;
    return stream_write(f, (const unsigned char*)s, n) < 0 ? EOF : 0;
}

char* fgets(char* s, int n, FILE* f) {
    if (s == 0 || n <= 0 || f == 0) return 0;
    int i = 0;
    while (i < n - 1) {
        int c = fgetc(f);
        if (c == EOF) break;
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    if (i == 0) return 0;
    s[i] = '\0';
    return s;
}

// --- position and state ------------------------------------------------

long ftell(FILE* f) {
    if (f == 0 || f->kind != KIND_FILE) { errno = EINVAL; return -1; }
    return f->off - (long)(f->rlen - f->rpos) + (long)f->wlen;
}

int fseek(FILE* f, long offset, int whence) {
    if (f == 0 || f->kind != KIND_FILE) {
        if (f) f->flags |= FL_ERR;
        errno = EINVAL;
        return -1;
    }

    long logical = f->off - (long)(f->rlen - f->rpos) + (long)f->wlen;
    long target;
    if (whence == SEEK_SET) {
        target = offset;
    } else if (whence == SEEK_CUR) {
        target = logical + offset;
    } else if (whence == SEEK_END) {
        long end = __stdio_lseek(f->which, 0, SEEK_END);
        if (end < 0) { f->flags |= FL_ERR; return -1; }
        target = end + offset;
    } else {
        errno = EINVAL;
        f->flags |= FL_ERR;
        return -1;
    }

    if (flush_write(f) < 0) return -1;
    f->rpos = f->rlen = 0;                // drop any read window
    long r = __stdio_lseek(f->which, target, SEEK_SET);
    if (r < 0) { f->flags |= FL_ERR; return -1; }
    f->off = r;
    f->ungot = -1;
    f->flags &= ~FL_EOF;                  // C: a successful seek clears EOF
    return 0;
}

int feof(FILE* f) { return (f && (f->flags & FL_EOF)) ? 1 : 0; }
int ferror(FILE* f) { return (f && (f->flags & FL_ERR)) ? 1 : 0; }

void clearerr(FILE* f) {
    if (f) f->flags &= ~(FL_EOF | FL_ERR);
}

int fileno(FILE* f) {
    if (f == 0 || f->kind != KIND_FILE) { errno = EINVAL; return -1; }
    return f->which;
}

int setvbuf(FILE* f, char* buf, int mode, size_t size) {
    if (f == 0) return -1;

    if (f->owns_buf) { free(f->buf); f->owns_buf = 0; }
    f->buf = 0;
    f->bufsize = 0;
    f->rpos = f->rlen = f->wlen = 0;

    if (mode == _IONBF) return 0;

    size_t n = size ? size : BUFSIZ;
    if (buf != 0) {
        f->buf = (unsigned char*)buf;
        f->bufsize = n;
        return 0;
    }
    f->buf = (unsigned char*)malloc(n);
    if (f->buf == 0) return -1;
    f->bufsize = n;
    f->owns_buf = 1;
    return 0;
}

// --- temporary names ---------------------------------------------------

// There is no O_EXCL yet, so these are best-effort unique (a per-process
// counter), not the race-free names the standard promises. Adequate for a
// single-user system.
static unsigned long tmp_counter = 0;

char* tmpnam(char* s) {
    static char buf[32];
    char* out = s ? s : buf;
    snprintf(out, 32, "/tmp/tmp%lu", ++tmp_counter);
    return out;
}

FILE* tmpfile(void) {
    char name[32];
    tmpnam(name);
    return fopen(name, "w+");
}
