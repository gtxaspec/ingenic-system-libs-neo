/*
 * sysutils.c -- System utility wrappers (base, ADC, keys, cipher, LED, PM).
 *
 * Drop-in replacement for Ingenic SDK libsysutils.so.
 * Runtime device detection -- single binary for all Ingenic SoCs.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <dirent.h>
#include <time.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/syscall.h>
#include <linux/input.h>
#include <linux/rtc.h>

#include "sysutils/su_base.h"
#include "sysutils/su_adc.h"
#include "sysutils/su_cipher.h"
#include "sysutils/su_misc.h"
#include "sysutils/su_pm.h"

#define EXPORT __attribute__((visibility("default")))

/* ------------------------------------------------------------------
 *  Constants and paths
 * ------------------------------------------------------------------ */

#define RTC_DEV            "/dev/rtc0"
#define POWER_STATE        "/sys/power/state"
#define WAKEUP_COUNT       "/sys/power/wakeup_count"
#define WAKE_LOCK          "/sys/power/wake_lock"
#define WAKE_UNLOCK        "/sys/power/wake_unlock"
#define CPU1_ONLINE        "/sys/devices/system/cpu/cpu1/online"
#define CPU_ONLINE         "/sys/devices/system/cpu/online"
#define CPUINFO            "/proc/cpuinfo"
#define DEV_MEM            "/dev/mem"
#define EFUSE_PROCFS       "/proc/jz/efuse/efuse_chip_id"
#define GPIO_KEYS_DIR      "/sys/devices/platform/gpio-keys"
#define GPIO_KEYS_LIST     GPIO_KEYS_DIR "/keys"
#define GPIO_KEYS_DIS      GPIO_KEYS_DIR "/disabled_keys"
#define INPUT_DIR          "/dev/input"
#define AES_DEV            "/dev/aes"
#define DES_DEV            "/dev/jz-des"
#define PM_DEV             "/dev/zboost"

#define ADC_CHANNELS_MAX   8
#define LED_FMT            "/proc/board/power/led%d"
#define EFUSE_PHYS_BASE    0x13540200
#define EFUSE_WORD_COUNT   3
#define CIPHER_BUF_MIN     16
#define CIPHER_BUF_MAX     0x100000
#define PM_IOCTL_CMD       0xc0087a06

/* RTC ioctl numbers -- standard Linux. */
#ifndef RTC_RD_TIME
#define RTC_RD_TIME    _IOR('p', 0x09, struct rtc_time)
#endif
#ifndef RTC_SET_TIME
#define RTC_SET_TIME   _IOW('p', 0x0a, struct rtc_time)
#endif
#ifndef RTC_WKALM_RD
#define RTC_WKALM_RD   _IOR('p', 0x10, struct rtc_wkalrm)
#endif
#ifndef RTC_WKALM_SET
#define RTC_WKALM_SET  _IOW('p', 0x0f, struct rtc_wkalrm)
#endif
#ifndef RTC_AIE_ON
#define RTC_AIE_ON     _IO('p', 0x01)
#endif
#ifndef RTC_AIE_OFF
#define RTC_AIE_OFF    _IO('p', 0x02)
#endif

/* PM ioctl sub-commands (passed in cmd field of pm_ioctl_arg). */
enum {
	PM_CMD_THREAD_RESUME  = 0,
	PM_CMD_INIT_LISTEN    = 2,
	PM_CMD_GET_LOCK_NUMS  = 3,
	PM_CMD_ADD_LISTEN     = 4,
	PM_CMD_DEL_LISTEN     = 5,
	PM_CMD_DUMP_LIST      = 7,
	PM_CMD_BOOT_COMPLETE  = 9,
};

/* ------------------------------------------------------------------
 *  Internal state
 * ------------------------------------------------------------------ */

static pthread_mutex_t g_suspend_lock = PTHREAD_MUTEX_INITIALIZER;

/* ADC state -- fds initialized to -1 (0 is a valid fd). */
static int g_adc_fds[ADC_CHANNELS_MAX] = {-1, -1, -1, -1, -1, -1, -1, -1};
static bool g_adc_init;

/* Key subsystem state. */
struct key_entry {
	uint16_t code;
	uint16_t disabled;
};
static struct key_entry *g_keys;
static int g_key_count;

/* CIPHER state. */
static int g_aes_fd = -1;
static int g_des_fd = -1;
static void *g_cipher_buf;
static int g_cipher_buf_len;

/* PM state. */
struct pm_ioctl_arg {
	int cmd;
	int value;
};

struct pm_state {
	pthread_t        thread;
	pthread_rwlock_t rwlock;
	sem_t            suspend_sem;
	sem_t            resume_sem;
	int              event_state;
	int              event_mode;
	int              cpu_mode;
	int              initialized;
	int              legacy_mode;
	int              pm_fd;
	void            *suspend_fn;
	void            *resume_fn;
};

static struct pm_state g_pm;

/* T41 global exported symbol. */
EXPORT int mem_user_v;

/* ------------------------------------------------------------------
 *  Internal helpers
 * ------------------------------------------------------------------ */

static int open_cloexec(const char *path, int flags)
{
	int fd = open(path, flags | O_CLOEXEC);
	if (fd < 0)
		return -1;
	return fd;
}

static ssize_t read_file(const char *path, char *buf, size_t bufsz)
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

static int write_file(const char *path, const char *data, size_t len)
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

static int write_file_fmt(const char *path, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

static int write_file_fmt(const char *path, const char *fmt, ...)
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

static void chomp(char *s)
{
	size_t len = strlen(s);
	while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r'))
		s[--len] = '\0';
}

static void su_to_rtc(const SUTime *su, struct rtc_time *rtc)
{
	rtc->tm_sec  = su->sec;
	rtc->tm_min  = su->min;
	rtc->tm_hour = su->hour;
	rtc->tm_mday = su->mday;
	rtc->tm_mon  = su->mon - 1;
	rtc->tm_year = su->year - 1900;
	rtc->tm_wday = 0;
	rtc->tm_yday = 0;
	rtc->tm_isdst = 0;
}

static void rtc_to_su(const struct rtc_time *rtc, SUTime *su)
{
	su->sec  = rtc->tm_sec;
	su->min  = rtc->tm_min;
	su->hour = rtc->tm_hour;
	su->mday = rtc->tm_mday;
	su->mon  = rtc->tm_mon + 1;
	su->year = rtc->tm_year + 1900;
}

static int pm_ioctl(int cmd, int *value)
{
	if (g_pm.pm_fd < 0)
		return -1;
	struct pm_ioctl_arg arg = { cmd, value ? *value : 0 };
	int ret = ioctl(g_pm.pm_fd, PM_IOCTL_CMD, &arg);
	if (ret < 0)
		return -1;
	if (value)
		*value = arg.value;
	return 0;
}

/* ------------------------------------------------------------------
 *  SU_Base -- System information, time, alarm, power
 * ------------------------------------------------------------------ */

EXPORT int SU_Base_GetModelNumber(SUModelNum *modelNum)
{
	if (!modelNum)
		return -1;

	char buf[256];
	ssize_t n = read_file(CPUINFO, buf, sizeof(buf));
	if (n < 0)
		return -1;

	char *colon = strchr(buf, ':');
	if (!colon)
		return -1;

	const char *val = colon + 1;
	while (*val == ' ')
		val++;
	chomp((char *)val);

	snprintf(modelNum->chr, MAX_INFO_LEN, "%s", val);
	return 0;
}

EXPORT int SU_Base_GetVersion(SUVersion *version)
{
	if (!version)
		return -1;

	snprintf(version->chr, MAX_INFO_LEN, "neo-sysutils-1.0");
	return 0;
}

EXPORT int SU_Base_GetDevID(SUDevID *devID)
{
	if (!devID)
		return -1;

	memset(devID, 0, sizeof(*devID));

	char raw[256];
	ssize_t n = read_file(EFUSE_PROCFS, raw, sizeof(raw));
	if (n > 0) {
		chomp(raw);
		snprintf(devID->chr, MAX_INFO_LEN, "%s", raw);
		return 0;
	}

	int fd = open_cloexec(DEV_MEM, O_RDONLY | O_SYNC);
	if (fd < 0)
		return -1;

	long pagesz = sysconf(_SC_PAGESIZE);
	if (pagesz <= 0) {
		close(fd);
		return -1;
	}

	off_t base = EFUSE_PHYS_BASE & ~(pagesz - 1);
	size_t offset = EFUSE_PHYS_BASE - (size_t)base;
	size_t mapsz = (size_t)pagesz;

	if (offset + EFUSE_WORD_COUNT * 4 > mapsz)
		mapsz += (size_t)pagesz;

	void *map = mmap(NULL, mapsz, PROT_READ, MAP_SHARED, fd, base);
	close(fd);
	if (map == MAP_FAILED)
		return -1;

	volatile uint32_t *regs = (volatile uint32_t *)((char *)map + offset);
	char part[32];
	devID->chr[0] = '\0';

	for (int i = 0; i < EFUSE_WORD_COUNT; i++) {
		uint32_t val = regs[i];
		snprintf(part, sizeof(part), "%08x", val);

		char swapped[9];
		swapped[0] = part[2]; swapped[1] = part[3];
		swapped[2] = part[0]; swapped[3] = part[1];
		swapped[4] = part[6]; swapped[5] = part[7];
		swapped[6] = part[4]; swapped[7] = part[5];
		swapped[8] = '\0';

		size_t cur = strlen(devID->chr);
		snprintf(devID->chr + cur, MAX_INFO_LEN - cur, "%s", swapped);
	}

	munmap(map, mapsz);
	return 0;
}

EXPORT int SU_Base_GetTime(SUTime *time)
{
	if (!time)
		return -1;

	int fd = open_cloexec(RTC_DEV, O_RDONLY);
	if (fd < 0)
		return -1;

	struct rtc_time rtc;
	int ret = ioctl(fd, RTC_RD_TIME, &rtc);
	close(fd);

	if (ret < 0)
		return -1;

	rtc_to_su(&rtc, time);
	return 0;
}

EXPORT int SU_Base_SetTime(SUTime *time)
{
	if (!time)
		return -1;

	int fd = open_cloexec(RTC_DEV, O_RDONLY);
	if (fd < 0)
		return -1;

	struct rtc_time rtc;
	memset(&rtc, 0, sizeof(rtc));
	su_to_rtc(time, &rtc);

	int ret = ioctl(fd, RTC_SET_TIME, &rtc);
	close(fd);
	return (ret < 0) ? -1 : 0;
}

EXPORT int SU_Base_GetTimeMs(int *ms)
{
	if (!ms)
		return -1;

	int fd = open_cloexec(RTC_DEV, O_RDONLY);
	if (fd < 0)
		return -1;

	int val = 0;
	int ret = ioctl(fd, RTC_RD_TIME, &val);
	close(fd);
	if (ret < 0)
		return -1;

	*ms = val;
	return 0;
}

EXPORT int SU_Base_SUTime2Raw(SUTime *suTime, uint32_t *rawTime)
{
	if (!suTime || !rawTime)
		return -1;

	struct tm tm_val = {
		.tm_sec  = suTime->sec,
		.tm_min  = suTime->min,
		.tm_hour = suTime->hour,
		.tm_mday = suTime->mday,
		.tm_mon  = suTime->mon - 1,
		.tm_year = suTime->year - 1900,
		.tm_isdst = 0,
	};

	time_t t = timegm(&tm_val);
	if (t == (time_t)-1)
		return -1;

	*rawTime = (uint32_t)t;
	return 0;
}

EXPORT int SU_Base_Raw2SUTime(uint32_t *rawTime, SUTime *suTime)
{
	if (!rawTime || !suTime)
		return -1;

	time_t t = (time_t)*rawTime;
	struct tm tm_val;
	if (!gmtime_r(&t, &tm_val))
		return -1;

	suTime->sec  = tm_val.tm_sec;
	suTime->min  = tm_val.tm_min;
	suTime->hour = tm_val.tm_hour;
	suTime->mday = tm_val.tm_mday;
	suTime->mon  = tm_val.tm_mon + 1;
	suTime->year = tm_val.tm_year + 1900;
	return 0;
}

EXPORT int SU_Base_SetAlarm(SUTime *time)
{
	if (!time)
		return -1;

	int fd = open_cloexec(RTC_DEV, O_RDONLY);
	if (fd < 0)
		return -1;

	struct rtc_wkalrm alrm;
	memset(&alrm, 0, sizeof(alrm));
	su_to_rtc(time, &alrm.time);

	int ret = ioctl(fd, RTC_WKALM_SET, &alrm);
	close(fd);
	return (ret < 0) ? -1 : 0;
}

EXPORT int SU_Base_GetAlarm(SUTime *time)
{
	if (!time)
		return -1;

	int fd = open_cloexec(RTC_DEV, O_RDONLY);
	if (fd < 0)
		return -1;

	struct rtc_wkalrm alrm;
	int ret = ioctl(fd, RTC_WKALM_RD, &alrm);
	close(fd);

	if (ret < 0)
		return -1;

	rtc_to_su(&alrm.time, time);
	return 0;
}

EXPORT int SU_Base_SetAlarmTimeMs(int ms)
{
	if (ms < 1)
		return -1;

	int fd = open_cloexec(RTC_DEV, O_RDWR);
	if (fd < 0)
		return -1;

	struct rtc_wkalrm alrm;
	memset(&alrm, 0, sizeof(alrm));
	alrm.enabled = 1;

	int ret = ioctl(fd, RTC_WKALM_SET, &alrm);
	close(fd);
	return (ret < 0) ? -1 : 0;
}

EXPORT int SU_Base_GetAlarmTimeMs(uint32_t *ms)
{
	if (!ms)
		return -1;

	int fd = open_cloexec(RTC_DEV, O_RDONLY);
	if (fd < 0)
		return -1;

	struct rtc_wkalrm alrm;
	int ret = ioctl(fd, RTC_WKALM_RD, &alrm);
	close(fd);

	if (ret < 0)
		return -1;

	*ms = (uint32_t)(alrm.time.tm_hour * 3600000 +
	                 alrm.time.tm_min * 60000 +
	                 alrm.time.tm_sec * 1000);
	return 0;
}

EXPORT int SU_Base_EnableAlarm(void)
{
	SUTime now, alrm;
	if (SU_Base_GetTime(&now) < 0)
		return -1;
	if (SU_Base_GetAlarm(&alrm) < 0)
		return -1;

	uint32_t now_raw, alrm_raw;
	if (SU_Base_SUTime2Raw(&now, &now_raw) < 0)
		return -1;
	if (SU_Base_SUTime2Raw(&alrm, &alrm_raw) < 0)
		return -1;

	if (alrm_raw <= now_raw)
		return -1;

	int fd = open_cloexec(RTC_DEV, O_RDONLY);
	if (fd < 0)
		return -1;

	int ret = ioctl(fd, RTC_AIE_ON, 0);
	close(fd);
	return (ret < 0) ? -1 : 0;
}

EXPORT int SU_Base_DisableAlarm(void)
{
	int fd = open_cloexec(RTC_DEV, O_RDONLY);
	if (fd < 0)
		return -1;

	int ret = ioctl(fd, RTC_AIE_OFF, 0);
	close(fd);
	return (ret < 0) ? -1 : 0;
}

EXPORT int SU_Base_EnableAlarmTimeMs(void)
{
	int fd = open_cloexec(RTC_DEV, O_RDWR);
	if (fd < 0)
		return -1;

	int ret = ioctl(fd, RTC_AIE_ON, 0);
	close(fd);
	return (ret < 0) ? -1 : 0;
}

EXPORT int SU_Base_DisableAlarmTimeMs(void)
{
	int fd = open_cloexec(RTC_DEV, O_RDWR);
	if (fd < 0)
		return -1;

	int ret = ioctl(fd, RTC_AIE_OFF, 0);
	close(fd);
	return (ret < 0) ? -1 : 0;
}

EXPORT int SU_Base_PollingAlarm(uint32_t timeoutMsec)
{
	int fd = open_cloexec(RTC_DEV, O_RDONLY);
	if (fd < 0)
		return -1;

	fd_set rfds;
	FD_ZERO(&rfds);
	FD_SET(fd, &rfds);

	struct timeval tv;
	tv.tv_sec  = timeoutMsec / 1000;
	tv.tv_usec = (timeoutMsec % 1000) * 1000;

	int ret = select(fd + 1, &rfds, NULL, NULL, &tv);
	close(fd);

	if (ret <= 0)
		return -1;
	return 0;
}

EXPORT int SU_Base_Shutdown(void)
{
	sync();
	kill(1, SIGCHLD);
	return 0;
}

EXPORT int SU_Base_Reboot(void)
{
	sync();
	kill(1, SIGTERM);
	return 0;
}

EXPORT int SU_Base_Suspend(void)
{
	pthread_mutex_lock(&g_suspend_lock);
	int ret = write_file(POWER_STATE, "mem", 3);
	pthread_mutex_unlock(&g_suspend_lock);
	return ret;
}

EXPORT int SU_Base_SetWkupMode(SUWkup mode)
{
	return write_file_fmt(POWER_STATE, "%d", (int)mode);
}

EXPORT int SU_Base_GetWkupMode(void)
{
	int fd = open_cloexec(RTC_DEV, O_RDWR);
	if (fd < 0)
		return -1;

	struct {
		int cmd;
		int mode;
		int result;
	} arg = { 1, 0, 0 };

	int ret = ioctl(fd, RTC_RD_TIME, &arg);
	close(fd);
	return (ret < 0) ? -1 : arg.result;
}

EXPORT int SU_Base_GetWakeupCount(void)
{
	char buf[64];
	ssize_t n = read_file(WAKEUP_COUNT, buf, sizeof(buf));
	if (n <= 0)
		return -1;
	return 0;
}

EXPORT int SU_Base_CtlPwrDown(void)
{
	pthread_mutex_lock(&g_suspend_lock);

	int fd = open_cloexec(RTC_DEV, O_RDWR);
	if (fd < 0) {
		pthread_mutex_unlock(&g_suspend_lock);
		return -1;
	}

	struct {
		int cmd;
		int pad;
		int val;
	} arg = { 0, 0x20, 0 };

	int ret = ioctl(fd, RTC_WKALM_SET, &arg);
	close(fd);
	pthread_mutex_unlock(&g_suspend_lock);
	return (ret < 0) ? -1 : 0;
}

/* ------------------------------------------------------------------
 *  SU_ADC -- Analog to digital conversion
 * ------------------------------------------------------------------ */

static const char *adc_dev_prefix(void)
{
	static const char *prefix;
	if (!prefix) {
		if (access("/dev/ingenic_adc_aux_0", F_OK) == 0)
			prefix = "/dev/ingenic_adc_aux_";
		else
			prefix = "/dev/jz_adc_aux_";
	}
	return prefix;
}

EXPORT int SU_ADC_Init(void)
{
	if (g_adc_init)
		return 0;

	const char *prefix = adc_dev_prefix();

	for (unsigned i = 0; i < ADC_CHANNELS_MAX; i++) {
		char path[64];
		snprintf(path, sizeof(path), "%s%u", prefix, i);

		g_adc_fds[i] = open_cloexec(path, O_RDONLY);
		if (g_adc_fds[i] < 0) {
			if (i == 0)
				return -1;
			break;
		}
	}

	g_adc_init = true;
	return 0;
}

EXPORT int SU_ADC_Exit(void)
{
	if (!g_adc_init)
		return 0;

	for (unsigned i = 0; i < ADC_CHANNELS_MAX; i++) {
		if (g_adc_fds[i] >= 0) {
			close(g_adc_fds[i]);
			g_adc_fds[i] = -1;
		}
	}

	g_adc_init = false;
	return 0;
}

EXPORT int SU_ADC_EnableChn(uint32_t chn_num)
{
	if (!g_adc_init || chn_num >= ADC_CHANNELS_MAX)
		return -1;
	if (g_adc_fds[chn_num] < 0)
		return -1;

	return ioctl(g_adc_fds[chn_num], 0) < 0 ? -1 : 0;
}

EXPORT int SU_ADC_DisableChn(uint32_t chn_num)
{
	if (!g_adc_init || chn_num >= ADC_CHANNELS_MAX)
		return -1;
	if (g_adc_fds[chn_num] < 0)
		return -1;

	return ioctl(g_adc_fds[chn_num], 1) < 0 ? -1 : 0;
}

EXPORT int SU_ADC_GetChnValue(uint32_t chn_num, int *value)
{
	if (!g_adc_init || chn_num >= ADC_CHANNELS_MAX || !value)
		return -1;
	if (g_adc_fds[chn_num] < 0)
		return -1;

	ssize_t n = read(g_adc_fds[chn_num], value, sizeof(*value));
	return (n == sizeof(*value)) ? 0 : -1;
}

/* ------------------------------------------------------------------
 *  SU_Key -- Input event handling
 * ------------------------------------------------------------------ */

static int load_keys(void)
{
	if (g_keys)
		return 0;

	char buf[128];
	ssize_t n = read_file(GPIO_KEYS_LIST, buf, sizeof(buf));
	if (n <= 0)
		return -1;

	chomp(buf);

	int count = 0;
	char *tmp = buf;
	while (*tmp) {
		count++;
		char *comma = strchr(tmp, ',');
		if (!comma)
			break;
		tmp = comma + 1;
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

EXPORT int SU_Key_OpenEvent(void)
{
	DIR *dir = opendir(INPUT_DIR);
	if (!dir)
		return -1;

	int result_fd = -1;
	struct dirent *ent;

	while ((ent = readdir(dir)) != NULL) {
		if (ent->d_name[0] == '.')
			continue;

		int fd = openat(dirfd(dir), ent->d_name, O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			continue;

		char name[80];
		memset(name, 0, sizeof(name));
		if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) > 0) {
			if (strcmp(name, "gpio-keys") == 0) {
				result_fd = fd;
				break;
			}
		}
		close(fd);
	}

	closedir(dir);
	return result_fd;
}

EXPORT int SU_Key_CloseEvent(int evfd)
{
	if (evfd < 0)
		return -1;
	return close(evfd);
}

EXPORT int SU_Key_ReadEvent(int evfd, int *keyCode, SUKeyEvent *event)
{
	if (evfd < 0 || !keyCode || !event)
		return -1;

	struct input_event ev;
	for (;;) {
		ssize_t n = read(evfd, &ev, sizeof(ev));
		if (n < (ssize_t)sizeof(ev))
			return -1;

		if (ev.type == EV_KEY) {
			*keyCode = ev.code;
			*event = ev.value ? KEY_PRESSED : KEY_RELEASED;
			return 0;
		}
	}
}

static int update_key_sysfs(const char *sysfs_path, uint16_t want_flag)
{
	if (g_key_count <= 0)
		return -1;

	char list[128];
	int pos = 0;

	for (int i = 0; i < g_key_count; i++) {
		if (g_keys[i].disabled != want_flag)
			continue;

		int n = snprintf(list + pos, sizeof(list) - (size_t)pos,
		                 "%s%u", pos > 0 ? "," : "",
		                 g_keys[i].code);
		if (n < 0 || pos + n >= (int)sizeof(list))
			break;
		pos += n;
	}

	if (pos == 0)
		return 0;

	return write_file(sysfs_path, list, (size_t)pos);
}

EXPORT int SU_Key_EnableEvent(int keyCode)
{
	if (load_keys() < 0)
		return -1;

	for (int i = 0; i < g_key_count; i++) {
		if (g_keys[i].code == (uint16_t)keyCode) {
			g_keys[i].disabled = 0;
			break;
		}
	}

	return update_key_sysfs(GPIO_KEYS_LIST, 0);
}

EXPORT int SU_Key_DisableEvent(int keyCode)
{
	if (load_keys() < 0)
		return -1;

	for (int i = 0; i < g_key_count; i++) {
		if (g_keys[i].code == (uint16_t)keyCode) {
			g_keys[i].disabled = 1;
			break;
		}
	}

	return update_key_sysfs(GPIO_KEYS_DIS, 1);
}

/* ------------------------------------------------------------------
 *  SU_CIPHER -- Hardware AES/DES + buffer management
 * ------------------------------------------------------------------ */

EXPORT int SU_CIPHER_Init(void)
{
	if (g_aes_fd >= 0)
		return REINIT;

	g_aes_fd = open_cloexec(AES_DEV, O_RDONLY);
	if (g_aes_fd < 0)
		return INIT_FAILED;

	return 0;
}

EXPORT int SU_CIPHER_Exit(void)
{
	if (g_aes_fd < 0)
		return EXIT_ERR;

	close(g_aes_fd);
	g_aes_fd = -1;
	return 0;
}

EXPORT int SU_CIPHER_DES_Init(void)
{
	if (g_des_fd >= 0)
		return REINIT;

	g_des_fd = open_cloexec(DES_DEV, O_RDONLY);
	if (g_des_fd < 0)
		return INIT_FAILED;

	return 0;
}

EXPORT int SU_CIPHER_DES_Exit(void)
{
	if (g_des_fd < 0)
		return EXIT_ERR;

	close(g_des_fd);
	g_des_fd = -1;
	return 0;
}

EXPORT int SU_CIPHER_DES_Test(void)
{
	if (g_des_fd < 0)
		return UNINIT;
	return 0;
}

EXPORT int SU_CIPHER_CreateHandle(void)
{
	if (g_aes_fd < 0)
		return UNINIT;

	int fd = dup(g_aes_fd);
	if (fd < 0)
		return FAILED_GETHANDLE;

	return fd;
}

EXPORT int SU_CIPHER_DestroyHandle(int fd)
{
	if (fd < 0)
		return FAILED_DESHANDLE;
	close(fd);
	return 0;
}

EXPORT int SU_CIPHER_ConfigHandle(int hCipher, IN_UNF_CIPHER_CTRL *Ctrl)
{
	if (hCipher < 0 || !Ctrl)
		return INVALID_PARA;

	return ioctl(hCipher, 0, Ctrl) < 0 ? SET_PARA_FAILED : 0;
}

EXPORT int SU_CIPHER_Encrypt(int hCipher, unsigned int *srcAddr,
                             unsigned int *dstAddr, unsigned int dataLen)
{
	if (hCipher < 0 || !srcAddr || !dstAddr)
		return INVALID_PARA;
	if (dataLen == 0 || dataLen > 1024 * 1024)
		return SET_DATALEN_ERR;

	struct {
		unsigned int *src;
		unsigned int *dst;
		unsigned int  len;
	} args = { srcAddr, dstAddr, dataLen };

	return ioctl(hCipher, 1, &args) < 0 ? FAILURE : 0;
}

EXPORT int SU_CIPHER_Decrypt(int hCipher, unsigned int *srcAddr,
                             unsigned int *dstAddr, unsigned int dataLen)
{
	if (hCipher < 0 || !srcAddr || !dstAddr)
		return INVALID_PARA;
	if (dataLen == 0 || dataLen > 1024 * 1024)
		return SET_DATALEN_ERR;

	struct {
		unsigned int *src;
		unsigned int *dst;
		unsigned int  len;
	} args = { srcAddr, dstAddr, dataLen };

	return ioctl(hCipher, 2, &args) < 0 ? FAILURE : 0;
}

EXPORT int SU_CIPHER_Malloc_Buffer(int size)
{
	if (size < CIPHER_BUF_MIN || size > CIPHER_BUF_MAX)
		return -1;

	void *buf = malloc((size_t)size);
	if (!buf)
		return -1;

	g_cipher_buf = buf;
	g_cipher_buf_len = size;
	return 0;
}

EXPORT int SU_CIPHER_Free_Buffer(void)
{
	if (!g_cipher_buf)
		return -1;

	free(g_cipher_buf);
	g_cipher_buf = NULL;
	g_cipher_buf_len = 0;
	return 0;
}

EXPORT int cipher_htonl(const void *src, void *dst, unsigned int len)
{
	if (len < CIPHER_BUF_MIN || len > CIPHER_BUF_MAX)
		return -1;

	const uint8_t *s = src;
	uint8_t *d = dst;
	unsigned int aligned = len & ~3u;

	for (unsigned int i = 0; i < aligned; i += 4) {
		d[i + 0] = s[i + 3];
		d[i + 1] = s[i + 2];
		d[i + 2] = s[i + 1];
		d[i + 3] = s[i + 0];
	}

	return 0;
}

/* ------------------------------------------------------------------
 *  SU_LED -- LED control via regulator sysfs
 * ------------------------------------------------------------------ */

EXPORT int SU_LED_Command(int ledNum, SULedCmd cmd)
{
	char path[64];
	snprintf(path, sizeof(path), LED_FMT, ledNum);

	const char *val = (cmd == LED_ON) ? "1" : "0";
	return write_file(path, val, 1);
}

/* ------------------------------------------------------------------
 *  SU_PM -- Power management (T32/T33)
 *
 *  Thread-aware suspend/resume framework using /dev/zboost ioctls,
 *  sysfs wake locks, and CPU hotplug. Gracefully returns -1 on SoCs
 *  without /dev/zboost.
 * ------------------------------------------------------------------ */

static void *pm_listener_thread(void *arg)
{
	(void)arg;
	while (g_pm.initialized) {
		sem_wait(&g_pm.suspend_sem);
		if (!g_pm.initialized)
			break;
		g_pm.event_state = 1;
		sem_post(&g_pm.resume_sem);
	}
	return NULL;
}

EXPORT int SU_PM_Init(int *config)
{
	if (g_pm.initialized)
		return -1;

	pthread_rwlock_init(&g_pm.rwlock, NULL);
	sem_init(&g_pm.suspend_sem, 0, 0);
	sem_init(&g_pm.resume_sem, 0, 0);

	g_pm.pm_fd = open_cloexec(PM_DEV, O_RDWR | O_NONBLOCK);
	if (g_pm.pm_fd < 0)
		return -1;

	if (config) {
		int v = *config;
		g_pm.event_state = (v >> 2) & 3;
		g_pm.event_mode  = (v >> 2) & 3;
		g_pm.cpu_mode    = (v >> 4) & 3;
		g_pm.legacy_mode = (v >> 0) & 3;
	} else {
		g_pm.event_state = 1;
		g_pm.event_mode  = 1;
		g_pm.cpu_mode    = 0;
		g_pm.legacy_mode = 0;
	}

	if (pthread_create(&g_pm.thread, NULL, pm_listener_thread, NULL) != 0) {
		close(g_pm.pm_fd);
		g_pm.pm_fd = -1;
		return -1;
	}

	int val = 0;
	pm_ioctl(PM_CMD_BOOT_COMPLETE, &val);

	g_pm.initialized = 1;
	return 0;
}

EXPORT int SU_PM_DeInit(void)
{
	if (!g_pm.initialized)
		return -1;

	g_pm.initialized = 0;
	sem_post(&g_pm.suspend_sem);

	pthread_join(g_pm.thread, NULL);
	pthread_rwlock_destroy(&g_pm.rwlock);
	sem_destroy(&g_pm.suspend_sem);
	sem_destroy(&g_pm.resume_sem);

	if (g_pm.pm_fd >= 0) {
		close(g_pm.pm_fd);
		g_pm.pm_fd = -1;
	}

	return 0;
}

EXPORT void SU_PM_Sleep(int ms)
{
	struct timespec ts;
	ts.tv_sec  = ms / 1000;
	ts.tv_nsec = (ms % 1000) * 1000000L;
	nanosleep(&ts, NULL);
}

EXPORT int SU_PM_RequestWakeLock(const char *name)
{
	if (!name)
		return -1;
	return write_file_fmt(WAKE_LOCK, "%s", name);
}

EXPORT int SU_PM_ReleaseWakeLock(const char *name)
{
	if (!name)
		return -1;
	return write_file_fmt(WAKE_UNLOCK, "%s", name);
}

EXPORT int SU_PM_Set_CPUOnline(int online)
{
	return write_file_fmt(CPU1_ONLINE, "%d", online);
}

EXPORT int SU_PM_Get_CPUOnlineNums(void)
{
	char buf[16];
	ssize_t n = read_file(CPU_ONLINE, buf, sizeof(buf));
	if (n <= 0)
		return -1;

	chomp(buf);
	char *dash = strchr(buf, '-');
	if (dash)
		return atoi(dash + 1) + 1;
	return 1;
}

EXPORT int SU_PM_AddThreadListen(void)
{
	if (!g_pm.initialized || g_pm.legacy_mode)
		return -1;

	int val = 0;
	if (pm_ioctl(PM_CMD_ADD_LISTEN, &val) < 0)
		return -1;

	return (val == 0) ? -1 : val;
}

EXPORT int SU_PM_DelThreadListen(int handle)
{
	if (!g_pm.initialized || g_pm.legacy_mode)
		return -1;
	if (handle < 0)
		return -1;

	int val = handle;
	return pm_ioctl(PM_CMD_DEL_LISTEN, &val);
}

EXPORT int SU_PM_TheadSuspend(void)
{
	if (!g_pm.initialized || g_pm.legacy_mode)
		return -1;

	sem_wait(&g_pm.suspend_sem);
	return 0;
}

EXPORT int SU_PM_ThreadResume(void)
{
	if (!g_pm.initialized || g_pm.legacy_mode)
		return -1;

	int val = 0;
	return pm_ioctl(PM_CMD_THREAD_RESUME, &val);
}

EXPORT int SU_PM_WaitThreadSuspend(int timeout_ms)
{
	if (!g_pm.initialized || g_pm.legacy_mode)
		return -1;
	if (timeout_ms <= 0)
		return -1;

	if (g_pm.pm_fd >= 0) {
		ssize_t n;
		do {
			n = write(g_pm.pm_fd, &timeout_ms, sizeof(timeout_ms));
		} while (n < 0 && errno == EINTR);
		return (n < 0) ? -1 : 0;
	}

	return -1;
}

EXPORT int SU_PM_EventSend(int event, int mode)
{
	if (!g_pm.initialized || g_pm.legacy_mode)
		return -1;
	if (event > 1 || mode > 1)
		return -1;

	if (event != g_pm.event_mode) {
		g_pm.event_state = event;
		if (event == 0)
			sem_post(&g_pm.resume_sem);
		else
			sem_post(&g_pm.suspend_sem);
	}

	return 0;
}

EXPORT int SU_PM_GetEvent(void)
{
	if (!g_pm.initialized || g_pm.legacy_mode)
		return 0;

	return g_pm.event_state;
}

EXPORT int SU_PM_InitListenLock(int count)
{
	if (!g_pm.initialized || g_pm.legacy_mode)
		return -1;

	int val = count;
	return pm_ioctl(PM_CMD_INIT_LISTEN, &val);
}

EXPORT int SU_PM_GetListenLockNums(void)
{
	if (!g_pm.initialized || g_pm.legacy_mode)
		return 0;

	int val = 0;
	if (pm_ioctl(PM_CMD_GET_LOCK_NUMS, &val) < 0)
		return 0;

	return val;
}

EXPORT void SU_PM_DumpList(void)
{
	int val = 0;
	pm_ioctl(PM_CMD_DUMP_LIST, &val);
}
