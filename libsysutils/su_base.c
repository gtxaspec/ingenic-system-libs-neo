/* su_base.c -- System info, time, alarm, power. SPDX-License-Identifier: MIT */

#include <stdbool.h>
#include <stdlib.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <linux/rtc.h>

#include "sysutils/su_base.h"
#include "su_internal.h"

#define RTC_DEV        "/dev/rtc0"
#define POWER_STATE    "/sys/power/state"
#define WAKEUP_COUNT   "/sys/power/wakeup_count"
#define CPUINFO        "/proc/cpuinfo"
#define DEV_MEM        "/dev/mem"
#define EFUSE_PROCFS   "/proc/jz/efuse/efuse_chip_id"
#define EFUSE_PHYS_BASE  0x13540200
#define EFUSE_WORD_COUNT 3

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

static pthread_mutex_t g_suspend_lock = PTHREAD_MUTEX_INITIALIZER;

EXPORT int mem_user_v;

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

EXPORT int SU_Base_GetModelNumber(SUModelNum *modelNum)
{
	if (!modelNum)
		return -1;

	char buf[256];
	if (read_file(CPUINFO, buf, sizeof(buf)) < 0)
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
	if (read_file(EFUSE_PROCFS, raw, sizeof(raw)) > 0) {
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
	devID->chr[0] = '\0';

	for (int i = 0; i < EFUSE_WORD_COUNT; i++) {
		uint32_t val = regs[i];
		char part[32];
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
	if (!time) return -1;
	int fd = open_cloexec(RTC_DEV, O_RDONLY);
	if (fd < 0) return -1;
	struct rtc_time rtc;
	int ret = ioctl(fd, RTC_RD_TIME, &rtc);
	close(fd);
	if (ret < 0) return -1;
	rtc_to_su(&rtc, time);
	return 0;
}

EXPORT int SU_Base_SetTime(SUTime *time)
{
	if (!time) return -1;
	int fd = open_cloexec(RTC_DEV, O_RDONLY);
	if (fd < 0) return -1;
	struct rtc_time rtc;
	memset(&rtc, 0, sizeof(rtc));
	su_to_rtc(time, &rtc);
	int ret = ioctl(fd, RTC_SET_TIME, &rtc);
	close(fd);
	return (ret < 0) ? -1 : 0;
}

EXPORT int SU_Base_GetTimeMs(int *ms)
{
	if (!ms) return -1;
	int fd = open_cloexec(RTC_DEV, O_RDONLY);
	if (fd < 0) return -1;
	int val = 0;
	int ret = ioctl(fd, RTC_RD_TIME, &val);
	close(fd);
	if (ret < 0) return -1;
	*ms = val;
	return 0;
}

EXPORT int SU_Base_SUTime2Raw(SUTime *suTime, uint32_t *rawTime)
{
	if (!suTime || !rawTime) return -1;
	struct tm tm_val = {
		.tm_sec = suTime->sec, .tm_min = suTime->min,
		.tm_hour = suTime->hour, .tm_mday = suTime->mday,
		.tm_mon = suTime->mon - 1, .tm_year = suTime->year - 1900,
		.tm_isdst = 0,
	};
	time_t t = timegm(&tm_val);
	if (t == (time_t)-1) return -1;
	*rawTime = (uint32_t)t;
	return 0;
}

EXPORT int SU_Base_Raw2SUTime(uint32_t *rawTime, SUTime *suTime)
{
	if (!rawTime || !suTime) return -1;
	time_t t = (time_t)*rawTime;
	struct tm tm_val;
	if (!gmtime_r(&t, &tm_val)) return -1;
	suTime->sec = tm_val.tm_sec; suTime->min = tm_val.tm_min;
	suTime->hour = tm_val.tm_hour; suTime->mday = tm_val.tm_mday;
	suTime->mon = tm_val.tm_mon + 1; suTime->year = tm_val.tm_year + 1900;
	return 0;
}

EXPORT int SU_Base_SetAlarm(SUTime *time)
{
	if (!time) return -1;
	int fd = open_cloexec(RTC_DEV, O_RDONLY);
	if (fd < 0) return -1;
	struct rtc_wkalrm alrm;
	memset(&alrm, 0, sizeof(alrm));
	su_to_rtc(time, &alrm.time);
	int ret = ioctl(fd, RTC_WKALM_SET, &alrm);
	close(fd);
	return (ret < 0) ? -1 : 0;
}

EXPORT int SU_Base_GetAlarm(SUTime *time)
{
	if (!time) return -1;
	int fd = open_cloexec(RTC_DEV, O_RDONLY);
	if (fd < 0) return -1;
	struct rtc_wkalrm alrm;
	int ret = ioctl(fd, RTC_WKALM_RD, &alrm);
	close(fd);
	if (ret < 0) return -1;
	rtc_to_su(&alrm.time, time);
	return 0;
}

EXPORT int SU_Base_SetAlarmTimeMs(int ms)
{
	if (ms < 1) return -1;
	int fd = open_cloexec(RTC_DEV, O_RDWR);
	if (fd < 0) return -1;
	struct rtc_wkalrm alrm;
	memset(&alrm, 0, sizeof(alrm));
	alrm.enabled = 1;
	int ret = ioctl(fd, RTC_WKALM_SET, &alrm);
	close(fd);
	return (ret < 0) ? -1 : 0;
}

EXPORT int SU_Base_GetAlarmTimeMs(uint32_t *ms)
{
	if (!ms) return -1;
	int fd = open_cloexec(RTC_DEV, O_RDONLY);
	if (fd < 0) return -1;
	struct rtc_wkalrm alrm;
	int ret = ioctl(fd, RTC_WKALM_RD, &alrm);
	close(fd);
	if (ret < 0) return -1;
	*ms = (uint32_t)(alrm.time.tm_hour * 3600000 +
	                 alrm.time.tm_min * 60000 +
	                 alrm.time.tm_sec * 1000);
	return 0;
}

EXPORT int SU_Base_EnableAlarm(void)
{
	SUTime now, alrm;
	if (SU_Base_GetTime(&now) < 0) return -1;
	if (SU_Base_GetAlarm(&alrm) < 0) return -1;
	uint32_t now_raw, alrm_raw;
	if (SU_Base_SUTime2Raw(&now, &now_raw) < 0) return -1;
	if (SU_Base_SUTime2Raw(&alrm, &alrm_raw) < 0) return -1;
	if (alrm_raw <= now_raw) return -1;
	int fd = open_cloexec(RTC_DEV, O_RDONLY);
	if (fd < 0) return -1;
	int ret = ioctl(fd, RTC_AIE_ON, 0);
	close(fd);
	return (ret < 0) ? -1 : 0;
}

EXPORT int SU_Base_DisableAlarm(void)
{
	int fd = open_cloexec(RTC_DEV, O_RDONLY);
	if (fd < 0) return -1;
	int ret = ioctl(fd, RTC_AIE_OFF, 0);
	close(fd);
	return (ret < 0) ? -1 : 0;
}

EXPORT int SU_Base_EnableAlarmTimeMs(void)
{
	int fd = open_cloexec(RTC_DEV, O_RDWR);
	if (fd < 0) return -1;
	int ret = ioctl(fd, RTC_AIE_ON, 0);
	close(fd);
	return (ret < 0) ? -1 : 0;
}

EXPORT int SU_Base_DisableAlarmTimeMs(void)
{
	int fd = open_cloexec(RTC_DEV, O_RDWR);
	if (fd < 0) return -1;
	int ret = ioctl(fd, RTC_AIE_OFF, 0);
	close(fd);
	return (ret < 0) ? -1 : 0;
}

EXPORT int SU_Base_PollingAlarm(uint32_t timeoutMsec)
{
	int fd = open_cloexec(RTC_DEV, O_RDONLY);
	if (fd < 0) return -1;
	fd_set rfds;
	FD_ZERO(&rfds);
	FD_SET(fd, &rfds);
	struct timeval tv = { timeoutMsec / 1000, (timeoutMsec % 1000) * 1000 };
	int ret = select(fd + 1, &rfds, NULL, NULL, &tv);
	close(fd);
	return (ret <= 0) ? -1 : 0;
}

EXPORT int SU_Base_Shutdown(void) { sync(); kill(1, SIGCHLD); return 0; }
EXPORT int SU_Base_Reboot(void)   { sync(); kill(1, SIGTERM); return 0; }

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
	if (fd < 0) return -1;
	struct { int cmd, mode, result; } arg = { 1, 0, 0 };
	int ret = ioctl(fd, RTC_RD_TIME, &arg);
	close(fd);
	return (ret < 0) ? -1 : arg.result;
}

EXPORT int SU_Base_GetWakeupCount(void)
{
	char buf[64];
	return (read_file(WAKEUP_COUNT, buf, sizeof(buf)) <= 0) ? -1 : 0;
}

EXPORT int SU_Base_CtlPwrDown(void)
{
	pthread_mutex_lock(&g_suspend_lock);
	int fd = open_cloexec(RTC_DEV, O_RDWR);
	if (fd < 0) { pthread_mutex_unlock(&g_suspend_lock); return -1; }
	struct { int cmd, pad, val; } arg = { 0, 0x20, 0 };
	int ret = ioctl(fd, RTC_WKALM_SET, &arg);
	close(fd);
	pthread_mutex_unlock(&g_suspend_lock);
	return (ret < 0) ? -1 : 0;
}
