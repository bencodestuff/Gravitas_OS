#include <limits.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <grv/syscall.h>

// One formatter behind printf and snprintf/sprintf. Each call supplies an
// emit function; printf's emit batches into a chunk and prints it with
// grv_puts() (which the kernel prints atomically), so a task switch cannot
// interleave another task's characters into the middle of a line.

typedef void (*emit_fn)(void* ctx, const char* s, size_t len);

static int vformat(emit_fn emit, void* ctx, const char* format,
                   va_list parameters) {
	int written = 0;

	while (*format != '\0') {
		size_t maxrem = (size_t)(INT_MAX - written);

		if (format[0] != '%' || format[1] == '%') {
			if (format[0] == '%')
				format++;
			size_t amount = 1;
			while (format[amount] && format[amount] != '%')
				amount++;
			if (maxrem < amount) return -1;
			emit(ctx, format, amount);
			format += amount;
			written += (int)amount;
			continue;
		}

		const char* format_begun_at = format++;

		// Length modifier (only 'l'): make d/i/u/x 64-bit.
		bool long_val = false;
		if (*format == 'l') { long_val = true; format++; }

		if (*format == 'c') {
			format++;
			char c = (char)va_arg(parameters, int);
			if (!maxrem) return -1;
			emit(ctx, &c, 1);
			written++;
		} else if (*format == 's') {
			format++;
			const char* str = va_arg(parameters, const char*);
			if (str == 0) str = "(null)";
			size_t len = strlen(str);
			if (maxrem < len) return -1;
			emit(ctx, str, len);
			written += (int)len;
		} else if (*format == 'd' || *format == 'i' || *format == 'u') {
			format++;
			bool is_unsigned = (*(format - 1) == 'u');

			char numbuf[24];
			int pos = 23;
			numbuf[pos] = '\0';

			unsigned long long uv;
			if (is_unsigned) {
				uv = long_val ? va_arg(parameters, unsigned long)
				              : (unsigned int)va_arg(parameters, unsigned int);
			} else {
				long long sv = long_val ? va_arg(parameters, long)
				                        : (long long)(int)va_arg(parameters, int);
				if (sv < 0) {
					uv = (unsigned long long)(-(sv + 1)) + 1ULL;
					numbuf[--pos] = '-';
				} else {
					uv = (unsigned long long)sv;
				}
			}
			if (uv == 0) numbuf[--pos] = '0';
			while (uv > 0) {
				numbuf[--pos] = '0' + (uv % 10);
				uv /= 10;
			}

			const char* str = &numbuf[pos];
			size_t len = (size_t)(23 - pos); // digits only (numbuf[23] = NUL)
			if (maxrem < len) return -1;
			emit(ctx, str, len);
			written += (int)len;
		} else if (*format == 'x') {
			format++;
			unsigned long long uv = long_val ? va_arg(parameters, unsigned long)
			                                 : (unsigned int)va_arg(parameters, unsigned int);

			char numbuf[17];
			int pos = 16;
			numbuf[pos] = '\0';
			while (uv > 0) {
				unsigned int digit = uv % 16;
				numbuf[--pos] = (digit < 10) ? ('0' + digit) : ('a' + digit - 10);
				uv /= 16;
			}
			if (pos == 16) numbuf[--pos] = '0';

			const char* str = &numbuf[pos];
			size_t len = (size_t)(16 - pos); // digits only (numbuf[16] = NUL)
			if (maxrem < len) return -1;
			emit(ctx, str, len);
			written += (int)len;
		} else {
			format = format_begun_at;
			size_t len = strlen(format);
			if (maxrem < len) return -1;
			emit(ctx, format, len);
			written += (int)len;
			format += len;
		}
	}

	return written;
}

// --- printf: chunked, atomic -------------------------------------------------

#define PBUF 256

struct put_ctx { char b[PBUF]; int n; };

static void put_flush(struct put_ctx* o) {
	if (o->n == 0) return;
	o->b[o->n] = '\0';
	grv_puts(o->b);
	o->n = 0;
}

static void put_emit(void* ctx, const char* s, size_t len) {
	struct put_ctx* o = (struct put_ctx*)ctx;
	for (size_t i = 0; i < len; i++) {
		if (o->n == PBUF - 1) put_flush(o);
		o->b[o->n++] = s[i];
	}
}

int printf(const char* restrict format, ...) {
	struct put_ctx o;
	o.n = 0;
	va_list parameters;
	va_start(parameters, format);
	int n = vformat(put_emit, &o, format, parameters);
	va_end(parameters);
	put_flush(&o);
	return n;
}

// --- snprintf / vsnprintf ----------------------------------------------------

struct sn_ctx { char* buf; size_t cap; size_t len; };

static void sn_emit(void* ctx, const char* s, size_t len) {
	struct sn_ctx* o = (struct sn_ctx*)ctx;
	for (size_t i = 0; i < len; i++) {
		if (o->cap && o->len + 1 < o->cap) o->buf[o->len] = s[i];
		o->len++;
	}
}

int vsnprintf(char* buf, size_t size, const char* format, va_list parameters) {
	struct sn_ctx o = { buf, size, 0 };
	int n = vformat(sn_emit, &o, format, parameters);
	if (size) {
		size_t end = (o.len < size) ? o.len : size - 1;
		buf[end] = '\0';
	}
	return n;
}

int snprintf(char* buf, size_t size, const char* format, ...) {
	va_list parameters;
	va_start(parameters, format);
	int n = vsnprintf(buf, size, format, parameters);
	va_end(parameters);
	return n;
}

// --- sprintf / vsprintf (unbounded; the caller owns sizing) -----------------

struct sp_ctx { char* buf; size_t len; };

static void sp_emit(void* ctx, const char* s, size_t len) {
	struct sp_ctx* o = (struct sp_ctx*)ctx;
	for (size_t i = 0; i < len; i++) o->buf[o->len++] = s[i];
}

int vsprintf(char* buf, const char* format, va_list parameters) {
	struct sp_ctx o = { buf, 0 };
	int n = vformat(sp_emit, &o, format, parameters);
	buf[o.len] = '\0';
	return n;
}

int sprintf(char* buf, const char* format, ...) {
	va_list parameters;
	va_start(parameters, format);
	int n = vsprintf(buf, format, parameters);
	va_end(parameters);
	return n;
}
