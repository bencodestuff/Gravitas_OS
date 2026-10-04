#include <limits.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <grv/syscall.h>

// printf accumulates into a chunk buffer and emits it with grv_puts(), which
// the kernel prints atomically — so a task switch cannot interleave another
// task's characters into the middle of our line.

#define PBUF 256

struct pout {
	char b[PBUF];
	int n;
};

static void pflush(struct pout* o) {
	if (o->n == 0)
		return;
	o->b[o->n] = '\0';
	grv_puts(o->b);
	o->n = 0;
}

static void pwrite(struct pout* o, const char* data, size_t length) {
	for (size_t i = 0; i < length; i++) {
		if (o->n == PBUF - 1)
			pflush(o);
		o->b[o->n++] = data[i];
	}
}

int printf(const char* restrict format, ...) {
	va_list parameters;
	va_start(parameters, format);

	int written = 0;
	struct pout o;
	o.n = 0;

	while (*format != '\0') {
		size_t maxrem = INT_MAX - written;

		if (format[0] != '%' || format[1] == '%') {
			if (format[0] == '%')
				format++;
			size_t amount = 1;
			while (format[amount] && format[amount] != '%')
				amount++;
			if (maxrem < amount) {
				// TODO: Set errno to EOVERFLOW.
				return -1;
			}
			pwrite(&o, format, amount);
			format += amount;
			written += amount;
			continue;
		}

		const char* format_begun_at = format++;

		// Length modifier (only 'l' is supported): make d/i/u/x 64-bit.
		// Must be read before the conversion char so "%lu"/"%lx" parse —
		// otherwise the modifier falls through to the literal path below
		// and the rest of the format string is printed verbatim.
		bool long_val = false;
		if (*format == 'l') { long_val = true; format++; }

		if (*format == 'c') {
			format++;
			char c = (char) va_arg(parameters, int /* char promotes to int */);
			if (!maxrem) {
				// TODO: Set errno to EOVERFLOW.
				return -1;
			}
			pwrite(&o, &c, sizeof(c));
			written++;
		} else if (*format == 's') {
			format++;
			const char* str = va_arg(parameters, const char*);
			size_t len = strlen(str);
			if (maxrem < len) {
				// TODO: Set errno to EOVERFLOW.
				return -1;
			}
			pwrite(&o, str, len);
			written += len;
		} else if (*format == 'd' || *format == 'i' || *format == 'u') {
			format++;
			bool is_unsigned = (*(format-1) == 'u');

			// Itoa into a local buffer (freestanding: no itoa in libc)
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
					// safe magnitude even at the negative extreme
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
			pwrite(&o, str, len);
			written += len;
		} else if (*format == 'x') {
			format++;

			unsigned long long uv = long_val ? va_arg(parameters, unsigned long)
			                                 : (unsigned int)va_arg(parameters, unsigned int);

			char numbuf[17]; // 16 hex digits + NUL
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
			pwrite(&o, str, len);
			written += len;
		} else {
			format = format_begun_at;
			size_t len = strlen(format);
			if (maxrem < len) {
				// TODO: Set errno to EOVERFLOW.
				return -1;
			}
			pwrite(&o, format, len);
			written += len;
			format += len;
		}
	}

	pflush(&o);
	va_end(parameters);
	return written;
}
