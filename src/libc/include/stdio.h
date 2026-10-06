#ifndef _STDIO_H
#define _STDIO_H 1

#include <sys/cdefs.h>
#include <stddef.h>
#include <stdarg.h>

#define EOF (-1)

/* tmpnam() needs at most this many bytes, and can produce TMP_MAX names. */
#define L_tmpnam 32
#define TMP_MAX  1000

/* setvbuf buffer modes. */
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2

/* fseek origins. */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define BUFSIZ 512

/* Streams. */
typedef struct _FILE FILE;

#ifdef __cplusplus
extern "C" {
#endif

/* --- formatted output --- */
int printf(const char* __restrict, ...);
int snprintf(char* __restrict, size_t, const char* __restrict, ...);
int vsnprintf(char* __restrict, size_t, const char* __restrict, va_list);
int sprintf(char* __restrict, const char* __restrict, ...);
int vsprintf(char* __restrict, const char* __restrict, va_list);
int fprintf(FILE* __restrict, const char* __restrict, ...);
int vfprintf(FILE* __restrict, const char* __restrict, va_list);

int putchar(int);
int puts(const char*);

/* --- streams ---
   A stream is either a console stream or a VFS file descriptor. The three
   standard streams (stdin/stdout/stderr) are console-backed and unbuffered:
   stdin reads keys, stdout/stderr write, and output appears immediately. A
   stream from fopen() is a file descriptor and is buffered (BUFSIZ, or a
   setvbuf buffer). fprintf() writes to a stream; printf() writes a whole line
   to the console atomically. */

extern FILE* stdin;
extern FILE* stdout;
extern FILE* stderr;

FILE* fopen(const char* __restrict, const char* __restrict);
FILE* freopen(const char* __restrict, const char* __restrict, FILE* __restrict);
FILE* tmpfile(void);
char* tmpnam(char*);
int   fclose(FILE*);

size_t fread(void* __restrict, size_t, size_t, FILE* __restrict);
size_t fwrite(const void* __restrict, size_t, size_t, FILE* __restrict);
int    fflush(FILE*);

int  fseek(FILE*, long, int);
long ftell(FILE*);
int  feof(FILE*);
int  ferror(FILE*);
void clearerr(FILE*);
int  fileno(FILE*);

int   fgetc(FILE*);
int   getc(FILE*);
int   getchar(void);
int   ungetc(int, FILE*);
int   fputc(int, FILE*);
int   putc(int, FILE*);
int   fputs(const char* __restrict, FILE* __restrict);
char* fgets(char* __restrict, int, FILE* __restrict);

int setvbuf(FILE*, char*, int, size_t);

/* --- filesystem, in its stdio spelling ---
   remove() unlinks a file or empty directory; rename() moves within one
   filesystem. */
int remove(const char*);
int rename(const char*, const char*);

#ifdef __cplusplus
}
#endif

#endif
