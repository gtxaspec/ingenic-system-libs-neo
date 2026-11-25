/*
 * test_su_cipher.c -- host test for the su_cipher handle table and locking.
 * Links su_cipher.c with --wrap=open,ioctl,mmap,munmap and models the jz-aes
 * driver: one 4 KiB buffer and one global parameter block for all openers.
 * The "engine" is a XOR with key (and IV in CBC), enough to see whether a
 * job ran with the buffer and parameters of the right handle.
 * Build and run: make -f tests/su_cipher.mk
 * SPDX-License-Identifier: MIT
 */
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "sysutils/su_cipher.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

#define PBUF 0x01234000u

struct aes_para {                    /* as in drivers/misc/jz_aes */
	uint32_t mem_p, status, aeskey[4], aesiv[4];
	uint32_t src_addr_p, src_addr_v, dst_addr_p, dst_addr_v;
	uint32_t enAlg, enBitWidth, enWorkMode, enKeyLen, dataLen;
};

static unsigned char drv_buf[4096];  /* the driver's single DMA buffer */
static struct aes_para drv_para;     /* the driver's single parameter block */
static int drv_busy, drv_overlaps, drv_jobs;
static int aes_fd = -1, mem_fd = -1, aes_opens;

int __real_open(const char *path, int flags, ...);
int __wrap_open(const char *path, int flags, ...)
{
	(void)flags;
	if (strcmp(path, "/dev/aes") == 0) {
		aes_opens++;
		return aes_fd = __real_open("/dev/null", O_RDONLY);
	}
	if (strcmp(path, "/dev/mem") == 0)
		return mem_fd = __real_open("/dev/null", O_RDONLY);
	return -1;
}

void *__real_mmap(void *a, size_t len, int prot, int flags, int fd, off_t off);
void *__wrap_mmap(void *a, size_t len, int prot, int flags, int fd, off_t off)
{
	if (fd == mem_fd && off == (off_t)PBUF && len == sizeof(drv_buf))
		return drv_buf;
	return __real_mmap(a, len, prot, flags, fd, off);
}

int __real_munmap(void *a, size_t len);
int __wrap_munmap(void *a, size_t len)
{
	return a == (void *)drv_buf ? 0 : __real_munmap(a, len);
}

int __wrap_ioctl(int fd, unsigned long req, ...)
{
	va_list ap;
	va_start(ap, req);
	void *arg = va_arg(ap, void *);
	va_end(ap);
	(void)fd;
	switch (req) {
	case _IO('A', 110):
		*(uint32_t *)arg = PBUF;
		return 0;
	case _IO('A', 111):
		if (__sync_lock_test_and_set(&drv_busy, 1))
			__sync_fetch_and_add(&drv_overlaps, 1);
		usleep(200);                 /* widen the window for a second thread */
		memcpy(&drv_para, arg, sizeof(drv_para));
		return 0;
	case _IO('A', 112):
	case _IO('A', 113): {
		usleep(200);
		const struct aes_para *p = &drv_para;
		const unsigned char *k = (const unsigned char *)p->aeskey;
		const unsigned char *iv = (const unsigned char *)p->aesiv;
		if (p->src_addr_p != PBUF || p->dst_addr_p != PBUF + 2048 || p->dataLen > 2048)
			return -1;
		for (uint32_t i = 0; i < p->dataLen; i++)
			drv_buf[2048 + i] = drv_buf[i] ^ k[i % 16] ^ (p->enWorkMode ? iv[i % 16] : 0);
		usleep(200);
		__sync_fetch_and_add(&drv_jobs, 1);
		__sync_lock_release(&drv_busy);
		return 0;
	}
	}
	return -1;
}

static IN_UNF_CIPHER_CTRL ctrl_for(unsigned seed, IN_UNF_CIPHER_WORK_MODE mode)
{
	IN_UNF_CIPHER_CTRL c;
	memset(&c, 0, sizeof(c));
	for (int i = 0; i < 4; i++) {
		c.key[i] = 0x01010101u * (seed + 1) + (unsigned)i;
		c.IV[i] = 0x10203040u ^ (seed << (unsigned)i);
	}
	c.enWorkMode = mode;
	return c;
}

#define THREADS 4
#define ROUNDS  40
#define LEN     4096                 /* two DMA jobs per call */

struct job { int handle; unsigned seed; int bad; };

static void *worker(void *arg)
{
	struct job *j = arg;
	IN_UNF_CIPHER_CTRL c = ctrl_for(j->seed, IN_UNF_CIPHER_WORK_MODE_ECB);
	const unsigned char *k = (const unsigned char *)c.key;
	unsigned int in[LEN / 4], out[LEN / 4];
	for (int r = 0; r < ROUNDS; r++) {
		for (unsigned i = 0; i < LEN / 4; i++)
			in[i] = j->seed * 0x9e3779b9u + (unsigned)r * 977u + i;
		memset(out, 0, sizeof(out));
		if (SU_CIPHER_Encrypt(j->handle, in, out, LEN) != 0) { j->bad++; continue; }
		const unsigned char *s = (const unsigned char *)in, *d = (const unsigned char *)out;
		for (unsigned i = 0; i < LEN; i++)
			if (d[i] != (unsigned char)(s[i] ^ k[i % 16])) { j->bad++; break; }
	}
	return NULL;
}

int main(void)
{
	/* occupy the low descriptors so that handles are well above 64 */
	int pad[100];
	for (int i = 0; i < 100; i++) pad[i] = __real_open("/dev/null", O_RDONLY);

	CHECK(SU_CIPHER_CreateHandle() == UNINIT);
	CHECK(SU_CIPHER_Init() == 0);
	CHECK(SU_CIPHER_Init() == REINIT);

	struct job jobs[THREADS];
	pthread_t th[THREADS];
	for (unsigned i = 0; i < THREADS; i++) {
		IN_UNF_CIPHER_CTRL c = ctrl_for(i, IN_UNF_CIPHER_WORK_MODE_ECB);
		jobs[i] = (struct job){ SU_CIPHER_CreateHandle(), i, 0 };
		CHECK(jobs[i].handle > 64);
		CHECK(SU_CIPHER_ConfigHandle(jobs[i].handle, &c) == 0);
	}
	CHECK(aes_opens == 1);               /* handles do not reopen the device */

	/* 20 more handles: the table grows, each keeps its own config */
	int more[20];
	for (unsigned i = 0; i < 20; i++) {
		IN_UNF_CIPHER_CTRL c = ctrl_for(100 + i, IN_UNF_CIPHER_WORK_MODE_ECB);
		more[i] = SU_CIPHER_CreateHandle();
		CHECK(more[i] > 64 && SU_CIPHER_ConfigHandle(more[i], &c) == 0);
	}
	for (unsigned i = 0; i < 20; i += 2)   /* free every other one */
		CHECK(SU_CIPHER_DestroyHandle(more[i]) == 0);
	for (unsigned i = 1; i < 20; i += 2) {
		IN_UNF_CIPHER_CTRL c = ctrl_for(100 + i, IN_UNF_CIPHER_WORK_MODE_ECB);
		unsigned int in[4] = { 1, 2, 3, 4 }, out[4];
		CHECK(SU_CIPHER_Encrypt(more[i], in, out, 16) == 0);
		for (int w = 0; w < 4; w++) CHECK(out[w] == (in[w] ^ c.key[w]));
		CHECK(SU_CIPHER_DestroyHandle(more[i]) == 0);
	}

	/* unknown, destroyed and unconfigured handles */
	unsigned int blk[4] = { 0 }, res[4];
	IN_UNF_CIPHER_CTRL c0 = ctrl_for(7, IN_UNF_CIPHER_WORK_MODE_ECB);
	CHECK(SU_CIPHER_ConfigHandle(pad[0], &c0) == INVALID_PARA);
	CHECK(SU_CIPHER_ConfigHandle(-1, &c0) == INVALID_PARA);
	CHECK(SU_CIPHER_Encrypt(more[1], blk, res, 16) == INVALID_PARA);
	int fresh = SU_CIPHER_CreateHandle();
	CHECK(SU_CIPHER_Encrypt(fresh, blk, res, 16) == INVALID_PARA);

	/* CBC over three DMA jobs: decrypt(encrypt(x)) == x needs the IV chain */
	IN_UNF_CIPHER_CTRL cbc = ctrl_for(9, IN_UNF_CIPHER_WORK_MODE_CBC);
	static unsigned int plain[1500], enc[1500], dec[1500];
	for (unsigned i = 0; i < 1500; i++) plain[i] = i * 2654435761u;
	CHECK(SU_CIPHER_ConfigHandle(fresh, &cbc) == 0);
	CHECK(SU_CIPHER_Encrypt(fresh, plain, enc, sizeof(plain)) == 0);
	CHECK(SU_CIPHER_Decrypt(fresh, enc, dec, sizeof(plain)) == 0);
	CHECK(memcmp(plain, dec, sizeof(plain)) == 0 && memcmp(plain, enc, sizeof(plain)) != 0);

	/* threads with different keys share the one driver buffer */
	drv_jobs = 0;
	for (int i = 0; i < THREADS; i++) pthread_create(&th[i], NULL, worker, &jobs[i]);
	for (int i = 0; i < THREADS; i++) pthread_join(th[i], NULL);
	for (int i = 0; i < THREADS; i++) CHECK(jobs[i].bad == 0);
	CHECK(drv_overlaps == 0);
	CHECK(drv_jobs == THREADS * ROUNDS * (LEN / 2048));

	for (int i = 0; i < THREADS; i++) CHECK(SU_CIPHER_DestroyHandle(jobs[i].handle) == 0);
	CHECK(SU_CIPHER_Exit() == 0);
	CHECK(SU_CIPHER_Exit() == UNINIT);
	CHECK(SU_CIPHER_ConfigHandle(fresh, &cbc) == INVALID_PARA);   /* table is gone */

	printf("%s (%d failures)\n", failures ? "FAILED" : "OK", failures);
	return failures != 0;
}
