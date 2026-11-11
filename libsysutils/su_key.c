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

static int load_keys(void)
{
	if (g_keys)
		return 0;

	char buf[128], path[96];
	if (gpio_keys_path(path, sizeof(path), "keys") < 0)
		return -1;
	if (read_file(path, buf, sizeof(buf)) <= 0)
		return -1;
	chomp(buf);

	int count = 0;
	for (char *p = buf; *p; ) {
		count++;
		char *comma = strchr(p, ',');
		if (!comma) break;
		p = comma + 1;
	}
	if (count == 0)
		return -1;

	g_keys = calloc((size_t)count, sizeof(*g_keys));
	if (!g_keys)
		return -1;

	char *saveptr;
	char *tok = strtok_r(buf, ",", &saveptr);
	int i = 0;
	while (tok && i < count) {
		g_keys[i].code = (uint16_t)atoi(tok);
		g_keys[i].disabled = 0;
		i++;
		tok = strtok_r(NULL, ",", &saveptr);
	}
	g_key_count = i;
	return 0;
}

static int update_key_sysfs(const char *file, uint16_t want_flag)
{
	if (g_key_count <= 0)
		return -1;

	char list[128];
	int pos = 0;
	for (int i = 0; i < g_key_count; i++) {
		if (g_keys[i].disabled != want_flag)
			continue;
		int n = snprintf(list + pos, sizeof(list) - (size_t)pos,
		                 "%s%u", pos > 0 ? "," : "", g_keys[i].code);
		if (n < 0 || pos + n >= (int)sizeof(list))
			break;
		pos += n;
	}
	if (pos == 0)
		return 0;
	char path[96];
	if (gpio_keys_path(path, sizeof(path), file) < 0)
		return -1;
	return write_file(path, list, (size_t)pos);
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
	if (load_keys() < 0) return -1;
	for (int i = 0; i < g_key_count; i++)
		if (g_keys[i].code == (uint16_t)keyCode) { g_keys[i].disabled = 0; break; }
	return update_key_sysfs("keys", 0);
}

EXPORT int SU_Key_DisableEvent(int keyCode)
{
	if (load_keys() < 0) return -1;
	for (int i = 0; i < g_key_count; i++)
		if (g_keys[i].code == (uint16_t)keyCode) { g_keys[i].disabled = 1; break; }
	return update_key_sysfs("disabled_keys", 1);
}
