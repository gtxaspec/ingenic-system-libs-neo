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

/*
 * Ingenic private RTC ioctls, as used by the vendor libsysutils.so (T33
 * sdk 2.0.2.1).  Not to be confused with the generic RTC_RD_TIME/RTC_SET_TIME
 * ('p', 0x09/0x0a), which transfer a 36-byte struct rtc_time.
 *
 * REG_ACCESS (CTRL_RTC_REG) is known to both vendor RTC drivers: "jz-rtc"
 * (rtc-jz.c, 3.10 T32/T33 kernels) and "rtc-ingenic" (rtc-ingenic.c, 4.4 and
 * 5.15).  RD_MS (INGENIC_RTC_READ_TIME_MS) exists only in "jz-rtc".
 */
#define INGENIC_RTC_REG_ACCESS _IOW('R', 0x09, int)   /* 0x80045209 */
#define INGENIC_RTC_RD_MS      _IOWR('R', 0x0a, int)  /* 0xc004520a */

#define RTC_DRIVER_LINK "/sys/class/rtc/rtc0/device/driver"
#define RTC_DRIVER_MS   "jz-rtc"

/* argument of INGENIC_RTC_REG_ACCESS (layout as in the vendor lib) */
struct ingenic_rtc_reg {
	int cmd;    /* 0 = write, 1 = read */
	int reg;
	int val;
};

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

	char *val = colon + 1;
	while (*val == ' ' || *val == '\t')
		val++;
	/* only the first cpuinfo line is the model name */
	val[strcspn(val, "\r\n")] = '\0';

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

	/* all-zero means no ID could be read (e.g. no efuse): report failure */
	if (strspn(devID->chr, "0") == strlen(devID->chr)) {
		memset(devID, 0, sizeof(*devID));
		return -1;
	}
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

/*
 * The sub-second ioctl must only reach a driver that implements it.
 * rtc-ingenic.c (4.4/5.15) answers every unknown private ioctl with 0 and
 * returns with its ioctl mutex still held, so trying the ioctl there would
 * report a bogus 0 ms and block the next REG_ACCESS for good.  Only "jz-rtc"
 * is known to have it (the T31 3.10 jz-rtc lacks it but rejects it with
 * ENOTTY), so ask sysfs which driver is bound to rtc0 first.
 */
static bool rtc_has_ms_ioctl(void)
{
	char link[128];
	ssize_t n = readlink(RTC_DRIVER_LINK, link, sizeof(link) - 1);
	if (n <= 0)
		return false;
	link[n] = '\0';
	const char *name = strrchr(link, '/');
	name = name ? name + 1 : link;
	return strcmp(name, RTC_DRIVER_MS) == 0;
}

EXPORT int SU_Base_GetTimeMs(int *ms)
{
	if (!ms) return -1;
	if (!rtc_has_ms_ioctl()) return -1;
	int fd = open_cloexec(RTC_DEV, O_RDONLY);
	if (fd < 0) return -1;
	/* 32768 Hz sub-second counter, converted like the vendor lib */
	uint32_t cnt = 0;
	int ret = ioctl(fd, INGENIC_RTC_RD_MS, &cnt);
	close(fd);
	if (ret < 0) return -1;
	*ms = (int)((cnt * 1000u + 0x4000u) >> 15);
	return 0;
}

/*
 * SU_Base_SUTime2Raw / SU_Base_Raw2SUTime as the vendor libsysutils answers
 * them.  Checked against the vendor code under unicorn (OpenIMP
 * tests/su_time, issue Lu-Fi/openimp#321), first for 1.3.0 (T40 SDK) and then
 * for every uClibc build at hand: T20 3.9.0/3.12.0, T21 1.0.33, T23
 * 1.1.0-1.3.0, T30 1.0.5, T31 1.1.1-1.1.6, T32 1.0.4-2.2.0, T33 2.0.2.1, T40
 * 1.0.2-1.3.1, T41 1.0.1-1.2.6, C100 2.1.0.  All of them give the same
 * results:
 *  - SUTime2Raw is the kernel mktime() formula in 32-bit unsigned
 *    arithmetic. It never fails and never normalises: out-of-range fields
 *    (month 13, day 0, year 1969 ...) simply flow through the formula.
 *  - Raw2SUTime writes plain struct tm fields (mon 0..11, year - 1900),
 *    NOT the SUTime convention (mon 1..12, year > 1900) that SUTime2Raw
 *    reads, so it is not the inverse of SUTime2Raw. Kept on purpose.
 * Struct layout: SUTime and struct tm share the field order sec, min, hour,
 * mday, mon, year.
 */
EXPORT int SU_Base_SUTime2Raw(SUTime *suTime, uint32_t *rawTime)
{
	if (!suTime || !rawTime) return -1;
	int32_t mon = (int32_t)suTime->mon - 2;
	uint32_t year = (uint32_t)suTime->year;
	if (mon <= 0) {
		mon += 12;
		year -= 1;
	}
	uint32_t days = (year >> 2) + year / 400u + (uint32_t)suTime->mday;
	days += year * 365u;
	days += (uint32_t)(mon * 367) / 12u;
	days -= year / 100u;
	uint32_t t = ((days * 24u + (uint32_t)suTime->hour) * 60u +
		      (uint32_t)suTime->min) * 60u + (uint32_t)suTime->sec;
	*rawTime = t + 0x86b1bf80u; /* -719499 days * 86400 s, mod 2^32 */
	return 0;
}

EXPORT int SU_Base_Raw2SUTime(uint32_t *rawTime, SUTime *suTime)
{
	if (!rawTime || !suTime) return -1;
	/* UTC calendar of the full unsigned 32-bit range (1970 .. 2106), done
	 * by hand: a 32-bit time_t would turn raw >= 2^31 negative. */
	uint32_t raw = *rawTime;
	uint32_t days = raw / 86400u, rem = raw % 86400u;
	int64_t z = (int64_t)days + 719468;
	int64_t era = z / 146097;
	int64_t doe = z - era * 146097;
	int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	int64_t mp = (5 * doy + 2) / 153;
	int64_t d = doy - (153 * mp + 2) / 5 + 1;
	int64_t m = mp < 10 ? mp + 3 : mp - 9;
	int64_t y = yoe + era * 400 + (m <= 2);
	suTime->sec = (int)(rem % 60);
	suTime->min = (int)(rem / 60 % 60);
	suTime->hour = (int)(rem / 3600);
	suTime->mday = (int)d;
	suTime->mon = (int)(m - 1);
	suTime->year = (int)(y - 1900);
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

/*
 * Vendor lib: sync(); kill(1, 17).  17 is SIGUSR2 on MIPS (SIGCHLD is 18
 * there), which busybox init handles as "poweroff".  SIGCHLD is ignored
 * by init, so the previous code never shut the device down.
 */
EXPORT int SU_Base_Shutdown(void) { sync(); kill(1, SIGUSR2); return 0; }
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
	struct ingenic_rtc_reg arg = { 1, 0x30, 0x0f };
	int ret = ioctl(fd, INGENIC_RTC_REG_ACCESS, &arg);
	close(fd);
	if (ret < 0) return -1;
	int mode = arg.val & 3;
	return (mode == 3) ? 0 : mode;
}

/*
 * Vendor libsysutils (T33 2.0.2.1 0x1d14, T32 2.1.0.0 0x1d14, T32 1.0.6
 * 0x1ae0): fopen of wakeup_count fails -> 1; fread gives 0 bytes -> -1;
 * otherwise 0.  The count itself is not returned.
 */
EXPORT int SU_Base_GetWakeupCount(void)
{
	char buf[64];
	int fd = open_cloexec(WAKEUP_COUNT, O_RDONLY);
	if (fd < 0)
		return 1;
	ssize_t n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	return (n <= 0) ? -1 : 0;
}

EXPORT int SU_Base_CtlPwrDown(void)
{
	pthread_mutex_lock(&g_suspend_lock);
	int fd = open_cloexec(RTC_DEV, O_RDWR);
	if (fd < 0) { pthread_mutex_unlock(&g_suspend_lock); return -1; }
	struct ingenic_rtc_reg arg = { 0, 0x20, 1 };
	int ret = ioctl(fd, INGENIC_RTC_REG_ACCESS, &arg);
	close(fd);
	pthread_mutex_unlock(&g_suspend_lock);
	return (ret < 0) ? -1 : 0;
}
