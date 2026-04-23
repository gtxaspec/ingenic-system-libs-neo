/* su_pm.c -- Power management (T32/T33). SPDX-License-Identifier: MIT */

#include <stdbool.h>
#include <stdlib.h>
#include <time.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/ioctl.h>

#include "sysutils/su_pm.h"
#include "su_internal.h"

#define PM_DEV         "/dev/zboost"
#define WAKE_LOCK      "/sys/power/wake_lock"
#define WAKE_UNLOCK    "/sys/power/wake_unlock"
#define CPU1_ONLINE    "/sys/devices/system/cpu/cpu1/online"
#define CPU_ONLINE     "/sys/devices/system/cpu/online"
#define PM_IOCTL_CMD   0xc0087a06

enum {
	PM_CMD_THREAD_RESUME  = 0,
	PM_CMD_INIT_LISTEN    = 2,
	PM_CMD_GET_LOCK_NUMS  = 3,
	PM_CMD_ADD_LISTEN     = 4,
	PM_CMD_DEL_LISTEN     = 5,
	PM_CMD_DUMP_LIST      = 7,
	PM_CMD_BOOT_COMPLETE  = 9,
};

struct pm_ioctl_arg {
	int cmd;
	int value;
};

static struct {
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
} g_pm;

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
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
	nanosleep(&ts, NULL);
}

EXPORT int SU_PM_RequestWakeLock(const char *name)
{
	if (!name) return -1;
	return write_file_fmt(WAKE_LOCK, "%s", name);
}

EXPORT int SU_PM_ReleaseWakeLock(const char *name)
{
	if (!name) return -1;
	return write_file_fmt(WAKE_UNLOCK, "%s", name);
}

EXPORT int SU_PM_Set_CPUOnline(int online)
{
	return write_file_fmt(CPU1_ONLINE, "%d", online);
}

EXPORT int SU_PM_Get_CPUOnlineNums(void)
{
	char buf[16];
	if (read_file(CPU_ONLINE, buf, sizeof(buf)) <= 0)
		return -1;
	chomp(buf);
	char *dash = strchr(buf, '-');
	return dash ? atoi(dash + 1) + 1 : 1;
}

EXPORT int SU_PM_AddThreadListen(void)
{
	if (!g_pm.initialized || g_pm.legacy_mode) return -1;
	int val = 0;
	if (pm_ioctl(PM_CMD_ADD_LISTEN, &val) < 0) return -1;
	return (val == 0) ? -1 : val;
}

EXPORT int SU_PM_DelThreadListen(int handle)
{
	if (!g_pm.initialized || g_pm.legacy_mode || handle < 0) return -1;
	int val = handle;
	return pm_ioctl(PM_CMD_DEL_LISTEN, &val);
}

EXPORT int SU_PM_TheadSuspend(void)
{
	if (!g_pm.initialized || g_pm.legacy_mode) return -1;
	sem_wait(&g_pm.suspend_sem);
	return 0;
}

EXPORT int SU_PM_ThreadResume(void)
{
	if (!g_pm.initialized || g_pm.legacy_mode) return -1;
	int val = 0;
	return pm_ioctl(PM_CMD_THREAD_RESUME, &val);
}

EXPORT int SU_PM_WaitThreadSuspend(int timeout_ms)
{
	if (!g_pm.initialized || g_pm.legacy_mode || timeout_ms <= 0) return -1;
	if (g_pm.pm_fd < 0) return -1;
	ssize_t n;
	do { n = write(g_pm.pm_fd, &timeout_ms, sizeof(timeout_ms)); }
	while (n < 0 && errno == EINTR);
	return (n < 0) ? -1 : 0;
}

EXPORT int SU_PM_EventSend(int event, int mode)
{
	if (!g_pm.initialized || g_pm.legacy_mode) return -1;
	if (event > 1 || mode > 1) return -1;
	if (event != g_pm.event_mode) {
		g_pm.event_state = event;
		if (event == 0) sem_post(&g_pm.resume_sem);
		else            sem_post(&g_pm.suspend_sem);
	}
	return 0;
}

EXPORT int SU_PM_GetEvent(void)
{
	if (!g_pm.initialized || g_pm.legacy_mode) return 0;
	return g_pm.event_state;
}

EXPORT int SU_PM_InitListenLock(int count)
{
	if (!g_pm.initialized || g_pm.legacy_mode) return -1;
	int val = count;
	return pm_ioctl(PM_CMD_INIT_LISTEN, &val);
}

EXPORT int SU_PM_GetListenLockNums(void)
{
	if (!g_pm.initialized || g_pm.legacy_mode) return 0;
	int val = 0;
	if (pm_ioctl(PM_CMD_GET_LOCK_NUMS, &val) < 0) return 0;
	return val;
}

EXPORT void SU_PM_DumpList(void)
{
	int val = 0;
	pm_ioctl(PM_CMD_DUMP_LIST, &val);
}
