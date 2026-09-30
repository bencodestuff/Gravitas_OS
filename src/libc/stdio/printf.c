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
		} else if (*format == 'd' || *format == 'i') {
			format++;
			int value = va_arg(parameters, int);

			// Itoa into a local buffer (freestanding: no itoa in libc)
			char numbuf[12]; // -2147483648 is 11 chars + NUL
			int pos = 11;
			numbuf[pos] = '\0';
			bool negative = value < 0;
			unsigned int uv = negative ? (unsigned int)(-(long long)value) : (unsigned int)value;
			if (uv == 0) numbuf[--pos] = '0';
			while (uv > 0) {
				numbuf[--pos] = '0' + (uv % 10);
				uv /= 10;
			}
			if (negative) numbuf[--pos] = '-';

			const char* str = &numbuf[pos];
			size_t len = 11 - (size_t)pos;
			if (maxrem < len) return -1;
			if (!print(str, len))
				return -1;
			written += len;
		} else if (*format == 'x') {
			format++;
			unsigned int value = va_arg(parameters, unsigned int);

			char numbuf[9]; // 8 hex digits + NUL
			int pos = 8;
			numbuf[pos] = '\0';
			if (value == 0) numbuf[--pos] = '0';
			while (value > 0) {
				unsigned int digit = value % 16;
				numbuf[--pos] = (digit < 10) ? ('0' + digit) : ('a' + digit - 10);
				value /= 16;
			}

			const char* str = &numbuf[pos];
			size_t len = 8 - (size_t)pos;
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
