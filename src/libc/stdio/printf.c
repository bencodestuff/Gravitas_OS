#include <limits.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static bool print(const char* data, size_t length) {
	const unsigned char* bytes = (const unsigned char*) data;
	for (size_t i = 0; i < length; i++)
		if (putchar(bytes[i]) == EOF)
			return false;
	return true;
}

int printf(const char* restrict format, ...) {
	va_list parameters;
	va_start(parameters, format);

	int written = 0;

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
			if (!print(format, amount))
				return -1;
			format += amount;
			written += amount;
			continue;
		}

		const char* format_begun_at = format++;

		if (*format == 'c') {
			format++;
			char c = (char) va_arg(parameters, int /* char promotes to int */);
			if (!maxrem) {
				// TODO: Set errno to EOVERFLOW.
				return -1;
			}
			if (!print(&c, sizeof(c)))
				return -1;
			written++;
		} else if (*format == 's') {
			format++;
			const char* str = va_arg(parameters, const char*);
			size_t len = strlen(str);
			if (maxrem < len) {
				// TODO: Set errno to EOVERFLOW.
				return -1;
			}
			if (!print(str, len))
				return -1;
			written += len;
		} else if (*format == 'd' || *format == 'i' || *format == 'u') {
			format++;
			bool is_unsigned = (*(format-1) == 'u');

			// Length modifier: 'l' selects long (64-bit) — without one the
			// value is read as int (32-bit). long long is overkill for an
			// OS where u64 counts ticks, not calendar years.
			bool long_val = false;
			if (*format == 'l') { long_val = true; format++; }

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
			size_t len = 24 - (size_t)pos;
			if (maxrem < len) return -1;
			if (!print(str, len))
				return -1;
			written += len;
		} else if (*format == 'x') {
			format++;
			bool long_val = (*format == 'l');
			if (long_val) format++;

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
			size_t len = 17 - (size_t)pos;
			if (maxrem < len) return -1;
			if (!print(str, len))
				return -1;
			written += len;
		} else {
			format = format_begun_at;
			size_t len = strlen(format);
			if (maxrem < len) {
				// TODO: Set errno to EOVERFLOW.
				return -1;
			}
			if (!print(format, len))
				return -1;
			written += len;
			format += len;
		}
	}

	va_end(parameters);
	return written;
}
