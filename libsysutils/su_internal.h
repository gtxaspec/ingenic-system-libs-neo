/*
 * su_internal.h -- Shared helpers for libsysutils modules.
 * SPDX-License-Identifier: MIT
 */

#ifndef SU_INTERNAL_H
#define SU_INTERNAL_H

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#define EXPORT __attribute__((visibility("default")))

static inline int open_cloexec(const char *path, int flags)
{
	int fd = open(path, flags | O_CLOEXEC);
	if (fd < 0)
		return -1;
	return fd;
}

static inline ssize_t read_file(const char *path, char *buf, size_t bufsz)
{
	int fd = open_cloexec(path, O_RDONLY);
	if (fd < 0)
		return -1;

	ssize_t n = read(fd, buf, bufsz - 1);
	close(fd);
	if (n < 0)
		return -1;

	buf[n] = '\0';
	return n;
}

static inline int write_file(const char *path, const char *data, size_t len)
{
	int fd = open_cloexec(path, O_WRONLY);
	if (fd < 0)
		return -1;

	ssize_t n;
	do {
		n = write(fd, data, len);
	} while (n < 0 && errno == EINTR);

	close(fd);
	return (n == (ssize_t)len) ? 0 : -1;
}

__attribute__((format(printf, 2, 3)))
static inline int write_file_fmt(const char *path, const char *fmt, ...)
{
	char buf[64];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n < 0 || n >= (int)sizeof(buf))
		return -1;
	return write_file(path, buf, (size_t)n);
}

static inline void chomp(char *s)
{
	size_t len = strlen(s);
	while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r'))
		s[--len] = '\0';
}

#endif /* SU_INTERNAL_H */
