/*
 * test_su_key.c -- host test for SU_Key_EnableEvent/SU_Key_DisableEvent.
 * Links su_key.c with --wrap=access,open,read,write,close and models the
 * gpio_keys sysfs attributes "keys" and "disabled_keys", including the two
 * kernel behaviours the library has to cope with:
 *  - sysfs drops zero-length writes before the driver sees them and still
 *    reports success;
 *  - a store that is refused (key not can_disable, or the vendor 4.4.94
 *    gpio_keys, whose store helper returns -EINVAL for every list).
 * Build and run: make -f tests/su_key.mk
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sysutils/su_misc.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

#define DIR_   "/sys/devices/platform/gpio-keys"
#define FD_KEYS 200
#define FD_DIS  201

#define NKEYS 3
static const int codes[NKEYS] = { 28, 29, 31 };
static int k_disabled[NKEYS];        /* state inside the "kernel" */
static int k_can_disable[NKEYS];
static int k_reject_all;             /* vendor 4.4.94 store helper */
static int k_stores;                 /* stores that reached the driver */
static int k_empty_writes;           /* zero-length writes (never reach it) */
static size_t rd_pos;
static int k_empty_list;             /* gpio-keys without keys (T33L) */

int __wrap_access(const char *path, int mode)
{
	(void)mode;
	return strcmp(path, DIR_) == 0 ? 0 : -1;
}

int __real_open(const char *path, int flags, ...);
int __wrap_open(const char *path, int flags, ...)
{
	(void)flags;
	rd_pos = 0;
	if (strcmp(path, DIR_ "/keys") == 0) return FD_KEYS;
	if (strcmp(path, DIR_ "/disabled_keys") == 0) return FD_DIS;
	errno = ENOENT;
	return -1;
}

int __real_close(int fd);
int __wrap_close(int fd) { return (fd == FD_KEYS || fd == FD_DIS) ? 0 : __real_close(fd); }

ssize_t __real_read(int fd, void *buf, size_t len);
ssize_t __wrap_read(int fd, void *buf, size_t len)
{
	if (fd != FD_KEYS && fd != FD_DIS) return __real_read(fd, buf, len);
	char out[64] = "";
	if (fd == FD_KEYS) {
		strcpy(out, k_empty_list ? "\n" : "28-29,31\n");  /* kernel prints ranges */
	} else {
		/* like "%*pbl": 28-29 collapse into a range */
		if (k_disabled[0] && k_disabled[1]) strcat(out, "28-29");
		else if (k_disabled[0]) strcat(out, "28");
		else if (k_disabled[1]) strcat(out, "29");
		if (k_disabled[2]) strcat(out, out[0] ? ",31" : "31");
		strcat(out, "\n");
	}
	size_t n = strlen(out) - rd_pos;
	if (n > len) n = len;
	memcpy(buf, out + rd_pos, n);
	rd_pos += n;
	return (ssize_t)n;
}

ssize_t __real_write(int fd, const void *buf, size_t len);
ssize_t __wrap_write(int fd, const void *buf, size_t len)
{
	if (fd != FD_DIS) return __real_write(fd, buf, len);
	if (len == 0) { k_empty_writes++; return 0; }   /* sysfs_kf_write */
	k_stores++;
	char list[64];
	if (len >= sizeof(list)) { errno = EINVAL; return -1; }
	memcpy(list, buf, len);
	list[len] = '\0';
	int want[NKEYS] = { 0 };
	for (char *tok = strtok(list, ",\n"); tok; tok = strtok(NULL, ",\n")) {
		int found = 0;
		for (int i = 0; i < NKEYS; i++)
			if (atoi(tok) == codes[i]) { want[i] = 1; found = 1; }
		if (!found) { errno = EINVAL; return -1; }
	}
	for (int i = 0; i < NKEYS; i++)
		if (want[i] && !k_can_disable[i]) { errno = EINVAL; return -1; }
	if (k_reject_all) { errno = EINVAL; return -1; }
	memcpy(k_disabled, want, sizeof(want));
	return (ssize_t)len;
}

int main(int argc, char **argv)
{
	/* 0. gpio-keys device without keys (vendor: loads 0 keys, update
	 *    returns 0 without sysfs I/O).  Own run: the key list is cached. */
	if (argc > 1 && strcmp(argv[1], "empty") == 0) {
		k_empty_list = 1;
		CHECK(SU_Key_DisableEvent(28) == 0);
		CHECK(SU_Key_EnableEvent(28) == 0);
		CHECK(k_stores == 0 && k_empty_writes == 0);
		printf("%s empty-list (%d failures)\n", failures ? "FAILED" : "OK", failures);
		return failures != 0;
	}

	/* 1. working kernel, all keys can_disable */
	for (int i = 0; i < NKEYS; i++) k_can_disable[i] = 1;
	CHECK(SU_Key_DisableEvent(28) == 0 && k_disabled[0] && !k_disabled[1]);
	CHECK(SU_Key_DisableEvent(31) == 0 && k_disabled[0] && k_disabled[2]);
	CHECK(SU_Key_EnableEvent(28) == 0 && !k_disabled[0] && k_disabled[2]);
	/* the last enable writes the empty list: it has to reach the driver */
	CHECK(SU_Key_EnableEvent(31) == 0 && !k_disabled[2]);
	CHECK(k_stores == 4 && k_empty_writes == 0);

	/* 2. key 29 is not can_disable: refused, cache not poisoned */
	k_can_disable[1] = 0;
	CHECK(SU_Key_DisableEvent(29) == -1 && !k_disabled[1]);
	CHECK(SU_Key_DisableEvent(28) == 0 && k_disabled[0] && !k_disabled[1]);
	CHECK(SU_Key_EnableEvent(29) == 0);   /* it is enabled */
	k_can_disable[1] = 1;

	/* 3. the kernel refuses every store while keys 28+29 are disabled
	 *    (state changed behind the library's back): Enable must not
	 *    report success, the keys stay disabled */
	k_disabled[0] = k_disabled[1] = 1;
	k_reject_all = 1;
	CHECK(SU_Key_EnableEvent(28) == -1 && k_disabled[0]);
	CHECK(SU_Key_EnableEvent(29) == -1 && k_disabled[1]);
	CHECK(SU_Key_DisableEvent(28) == 0);  /* already disabled */
	CHECK(SU_Key_DisableEvent(31) == -1 && !k_disabled[2]);
	CHECK(SU_Key_EnableEvent(31) == 0);   /* already enabled */

	/* 4. same kernel, nothing disabled (vendor 4.4.94 after boot) */
	k_disabled[0] = k_disabled[1] = 0;
	CHECK(SU_Key_EnableEvent(28) == 0);
	CHECK(SU_Key_DisableEvent(28) == -1 && !k_disabled[0]);
	CHECK(SU_Key_EnableEvent(28) == 0);
	CHECK(k_empty_writes == 0);

	/* 5. back on a working kernel the cache is still usable */
	k_reject_all = 0;
	CHECK(SU_Key_DisableEvent(29) == 0 && !k_disabled[0] && k_disabled[1] && !k_disabled[2]);
	CHECK(SU_Key_EnableEvent(29) == 0 && !k_disabled[1]);

	printf("%s (%d failures)\n", failures ? "FAILED" : "OK", failures);
	return failures != 0;
}
