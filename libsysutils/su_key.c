/* su_key.c -- GPIO key/button input events. SPDX-License-Identifier: MIT */

#include <stdbool.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <linux/input.h>

#include "sysutils/su_misc.h"
#include "su_internal.h"

/* Boards register the platform device / input name either as "gpio-keys"
 * or "gpio_keys" (e.g. T41), so accept both. */
#define GPIO_KEYS_DIR_A "/sys/devices/platform/gpio-keys"
#define GPIO_KEYS_DIR_B "/sys/devices/platform/gpio_keys"
#define INPUT_DIR       "/dev/input"

static const char *gpio_keys_dir(void)
{
	if (access(GPIO_KEYS_DIR_A, F_OK) == 0)
		return GPIO_KEYS_DIR_A;
	if (access(GPIO_KEYS_DIR_B, F_OK) == 0)
		return GPIO_KEYS_DIR_B;
	return NULL;
}

static int gpio_keys_path(char *out, size_t len, const char *file)
{
	const char *dir = gpio_keys_dir();
	if (!dir)
		return -1;
	int n = snprintf(out, len, "%s/%s", dir, file);
	return (n < 0 || (size_t)n >= len) ? -1 : 0;
}

struct key_entry {
	uint16_t code;
	uint16_t disabled;
};

static struct key_entry *g_keys;
static int g_key_count;
static int g_keys_loaded;	/* a gpio-keys device may have no keys */

/* Next item of a kernel bitmap list ("28", "28-30"; items separated by
 * commas, list ended by a newline).  Returns 1 with the range, 0 at the end
 * of the list, -1 on malformed input. */
static int next_range(char **pos, unsigned long *lo, unsigned long *hi)
{
	char *s = *pos, *end;
	while (*s == ',' || *s == '\n' || *s == ' ')
		s++;
	if (!*s)
		return 0;
	*lo = *hi = strtoul(s, &end, 10);
	if (end == s)
		return -1;
	if (*end == '-') {
		s = end + 1;
		*hi = strtoul(s, &end, 10);
		if (end == s)
			return -1;
	}
	if (*hi < *lo || *hi > KEY_MAX)
		return -1;
	*pos = end;
	return 1;
}

static int load_keys(void)
{
	if (g_keys || g_keys_loaded)
		return 0;

	char buf[128], path[96];
	if (gpio_keys_path(path, sizeof(path), "keys") < 0)
		return -1;
	/* an empty list is valid: the vendor load_globle_keys loads 0 keys */
	if (read_file(path, buf, sizeof(buf)) < 0)
		return -1;

	/* the kernel prints the key codes as a bitmap list, so neighbouring
	 * codes come as a range */
	unsigned long lo, hi;
	size_t count = 0;
	int r;
	char *p = buf;
	while ((r = next_range(&p, &lo, &hi)) > 0)
		count += hi - lo + 1;
	if (r < 0)
		return -1;
	if (count == 0) {
		g_key_count = 0;
		g_keys_loaded = 1;
		return 0;
	}

	struct key_entry *keys = calloc(count, sizeof(*keys));
	if (!keys)
		return -1;

	size_t i = 0;
	p = buf;
	while (next_range(&p, &lo, &hi) > 0)
		for (unsigned long c = lo; c <= hi; c++)
			keys[i++].code = (uint16_t)c;
	g_keys = keys;
	g_key_count = (int)i;
	g_keys_loaded = 1;
	return 0;
}

/* write the list of all keys flagged disabled to disabled_keys */
static int write_disabled_keys(void)
{
	/* vendor update_enabled_keys: key count <= 0 -> 0, no sysfs I/O */
	if (g_key_count <= 0)
		return 0;

	char list[128];
	int pos = 0;
	for (int i = 0; i < g_key_count; i++) {
		if (!g_keys[i].disabled)
			continue;
		int n = snprintf(list + pos, sizeof(list) - (size_t)pos,
		                 "%s%u", pos > 0 ? "," : "", g_keys[i].code);
		if (n < 0 || pos + n >= (int)sizeof(list))
			return -1;
		pos += n;
	}
	char path[96];
	if (gpio_keys_path(path, sizeof(path), "disabled_keys") < 0)
		return -1;
	if (pos == 0) {
		/* nothing disabled: a lone newline is the empty list.  It must
		 * not be a zero-length write, sysfs drops those before the
		 * driver sees them and still reports success. */
		list[0] = '\n';
		pos = 1;
	}
	return write_file(path, list, (size_t)pos);
}

/* take the disabled flags from the kernel ("3,5-7\n", empty = none) */
static int read_disabled_keys(void)
{
	char buf[128], path[96];
	if (gpio_keys_path(path, sizeof(path), "disabled_keys") < 0)
		return -1;
	if (read_file(path, buf, sizeof(buf)) < 0)
		return -1;

	for (int i = 0; i < g_key_count; i++)
		g_keys[i].disabled = 0;

	unsigned long lo, hi;
	int r;
	char *p = buf;
	while ((r = next_range(&p, &lo, &hi)) > 0)
		for (int i = 0; i < g_key_count; i++)
			if (g_keys[i].code >= lo && g_keys[i].code <= hi)
				g_keys[i].disabled = 1;
	return r;
}

static int set_key_disabled(int keyCode, uint16_t want)
{
	if (load_keys() < 0) return -1;
	int idx = -1;
	for (int i = 0; i < g_key_count; i++)
		if (g_keys[i].code == (uint16_t)keyCode) { idx = i; break; }
	if (idx >= 0)
		g_keys[idx].disabled = want;
	if (write_disabled_keys() == 0)
		return 0;
	/* The kernel refused the list: the key is not can_disable, or it is a
	 * gpio_keys that rejects every write (vendor 4.4.94).  Take the real
	 * state back from the kernel so the cache is not poisoned, and report
	 * success only if the key already is in the requested state. */
	if (read_disabled_keys() < 0) {
		if (idx >= 0)
			g_keys[idx].disabled = !want;
		return -1;
	}
	return (idx >= 0 && g_keys[idx].disabled == want) ? 0 : -1;
}

EXPORT int SU_Key_OpenEvent(void)
{
	DIR *dir = opendir(INPUT_DIR);
	if (!dir) return -1;

	int result_fd = -1;
	struct dirent *ent;
	while ((ent = readdir(dir)) != NULL) {
		if (ent->d_name[0] == '.') continue;
		int fd = openat(dirfd(dir), ent->d_name, O_RDONLY | O_CLOEXEC);
		if (fd < 0) continue;
		char name[80];
		memset(name, 0, sizeof(name));
		if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) > 0 &&
		    (strcmp(name, "gpio-keys") == 0 ||
		     strcmp(name, "gpio_keys") == 0)) {
			result_fd = fd;
			break;
		}
		close(fd);
	}
	closedir(dir);
	return result_fd;
}

EXPORT int SU_Key_CloseEvent(int evfd)
{
	if (evfd < 0) return -1;
	return close(evfd);
}

EXPORT int SU_Key_ReadEvent(int evfd, int *keyCode, SUKeyEvent *event)
{
	if (evfd < 0 || !keyCode || !event) return -1;
	struct input_event ev;
	for (;;) {
		ssize_t n = read(evfd, &ev, sizeof(ev));
		if (n < (ssize_t)sizeof(ev)) return -1;
		if (ev.type == EV_KEY) {
			*keyCode = ev.code;
			*event = ev.value ? KEY_PRESSED : KEY_RELEASED;
			return 0;
		}
	}
}

EXPORT int SU_Key_EnableEvent(int keyCode)
{
	return set_key_disabled(keyCode, 0);
}

EXPORT int SU_Key_DisableEvent(int keyCode)
{
	return set_key_disabled(keyCode, 1);
}
