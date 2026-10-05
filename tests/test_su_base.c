/*
 * test_su_base.c -- host test for su_base ioctl/signal usage.
 * Links su_base.c with --wrap=open,ioctl,kill,sync so no device is touched.
 * Every ioctl is checked against a guarded buffer: the encoded _IOC_SIZE
 * must not exceed the argument the library passed.
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/ioctl.h>

#include "sysutils/su_base.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static unsigned long last_req;
static int last_reg[3];
static int last_sig = -1;

int __wrap_open(const char *path, int flags, ...)
{
	(void)flags;
	return strcmp(path, "/dev/rtc0") == 0 ? 100 : -1;
}

int __real_close(int fd);
int __wrap_close(int fd) { return fd == 100 ? 0 : __real_close(fd); }

int __wrap_ioctl(int fd, unsigned long req, ...)
{
	va_list ap;
	va_start(ap, req);
	void *arg = va_arg(ap, void *);
	va_end(ap);
	if (fd != 100) return -1;
	last_req = req;
	if (req == _IOWR('R', 0x0a, int)) {
		*(uint32_t *)arg = 16384;          /* half a second */
		return 0;
	}
	if (req == _IOW('R', 0x09, int)) {
		memcpy(last_reg, arg, sizeof(last_reg));
		if (last_reg[0] == 1)
			((int *)arg)[2] = 0x6;         /* mode bits = 2 */
		return 0;
	}
	/* anything else: write _IOC_SIZE bytes like the kernel would */
	if (_IOC_DIR(req) & _IOC_READ)
		memset(arg, 0xa5, _IOC_SIZE(req));
	return 0;
}

int __wrap_kill(pid_t pid, int sig) { (void)pid; last_sig = sig; return 0; }
void __wrap_sync(void) { }

int main(void)
{
	struct { int ms; uint32_t canary[16]; } g;
	for (int i = 0; i < 16; i++) g.canary[i] = 0xdeadbeef;
	CHECK(SU_Base_GetTimeMs(&g.ms) == 0);
	CHECK(g.ms == 500);
	CHECK(_IOC_SIZE(last_req) == sizeof(int));
	for (int i = 0; i < 16; i++) CHECK(g.canary[i] == 0xdeadbeef);

	CHECK(SU_Base_GetWkupMode() == 2);
	CHECK(last_req == _IOW('R', 0x09, int));
	CHECK(last_reg[0] == 1 && last_reg[1] == 0x30);

	CHECK(SU_Base_CtlPwrDown() == 0);
	CHECK(last_req == _IOW('R', 0x09, int));
	CHECK(last_reg[0] == 0 && last_reg[1] == 0x20 && last_reg[2] == 1);

	SU_Base_Shutdown();
	CHECK(last_sig == SIGUSR2);
	SU_Base_Reboot();
	CHECK(last_sig == SIGTERM);

	printf("%s (%d failures)\n", failures ? "FAILED" : "OK", failures);
	return failures != 0;
}
