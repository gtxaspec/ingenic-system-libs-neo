/* su_cipher.c -- Hardware AES/DES + buffer management. SPDX-License-Identifier: MIT */

#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "sysutils/su_cipher.h"
#include "su_internal.h"

#define AES_DEV        "/dev/aes"
#define DES_DEV        "/dev/jz-des"
#define CIPHER_BUF_MIN 16
#define CIPHER_BUF_MAX 0x100000

/*
 * jz-aes kernel driver ABI (/dev/aes, misc device).  The driver owns one 4 KiB
 * DMA buffer per open; userspace learns its physical address with GET_PBUFF and
 * maps it through /dev/mem.  SET_PARA hands over key, IV, mode and the physical
 * and kernel-virtual (KSEG0) addresses of source and destination inside that
 * buffer, START_EN/START_DE run one DMA job.  All fields are 32 bit.
 */
#define JZAES_IOC_MAGIC       'A'
#define IOCTL_AES_GET_PBUFF   _IO(JZAES_IOC_MAGIC, 110)
#define IOCTL_AES_SET_PARA    _IO(JZAES_IOC_MAGIC, 111)
#define IOCTL_AES_START_EN    _IO(JZAES_IOC_MAGIC, 112)
#define IOCTL_AES_START_DE    _IO(JZAES_IOC_MAGIC, 113)

#define AES_DMA_BUF    4096
#define AES_HALF       (AES_DMA_BUF / 2)
#define KSEG0          0x80000000u
#define MAX_HANDLES    64

struct aes_para {
	uint32_t mem_p;
	uint32_t status;
	uint32_t aeskey[4];
	uint32_t aesiv[4];
	uint32_t src_addr_p;
	uint32_t src_addr_v;
	uint32_t dst_addr_p;
	uint32_t dst_addr_v;
	uint32_t enAlg;
	uint32_t enBitWidth;
	uint32_t enWorkMode;
	uint32_t enKeyLen;
	uint32_t dataLen;
};

static int g_aes_fd = -1;
static int g_mem_fd = -1;
static uint32_t g_pbuf;              /* physical address of the driver buffer */
static unsigned char *g_vbuf;        /* /dev/mem mapping of it (uncached) */
static IN_UNF_CIPHER_CTRL g_ctrl[MAX_HANDLES];
static unsigned char g_ctrl_set[MAX_HANDLES];
static int g_des_fd = -1;
static void *g_cipher_buf;
static int g_cipher_buf_len;

EXPORT int SU_CIPHER_Init(void)
{
	if (g_aes_fd >= 0) return REINIT;
	g_aes_fd = open_cloexec(AES_DEV, O_RDONLY);
	return (g_aes_fd < 0) ? INIT_FAILED : 0;
}

EXPORT int SU_CIPHER_Exit(void)
{
	if (g_aes_fd < 0) return UNINIT;
	if (g_vbuf) {
		munmap(g_vbuf, AES_DMA_BUF);
		g_vbuf = NULL;
	}
	if (g_mem_fd >= 0) {
		close(g_mem_fd);
		g_mem_fd = -1;
	}
	close(g_aes_fd);
	g_aes_fd = -1;
	memset(g_ctrl_set, 0, sizeof(g_ctrl_set));
	return 0;
}

EXPORT int SU_CIPHER_DES_Init(void)
{
	if (g_des_fd >= 0) return REINIT;
	g_des_fd = open_cloexec(DES_DEV, O_RDONLY);
	return (g_des_fd < 0) ? INIT_FAILED : 0;
}

EXPORT int SU_CIPHER_DES_Exit(void)
{
	if (g_des_fd < 0) return UNINIT;
	close(g_des_fd);
	g_des_fd = -1;
	return 0;
}

EXPORT int SU_CIPHER_DES_Test(void)
{
	return (g_des_fd < 0) ? UNINIT : 0;
}

EXPORT int SU_CIPHER_CreateHandle(void)
{
	if (g_aes_fd < 0) return UNINIT;
	int fd = dup(g_aes_fd);
	return (fd < 0) ? FAILED_GETHANDLE : fd;
}

EXPORT int SU_CIPHER_DestroyHandle(int fd)
{
	if (fd < 0) return FAILED_DESHANDLE;
	if (fd < MAX_HANDLES) g_ctrl_set[fd] = 0;
	close(fd);
	return 0;
}

EXPORT int SU_CIPHER_ConfigHandle(int hCipher, IN_UNF_CIPHER_CTRL *Ctrl)
{
	if (!Ctrl) return SET_PARA_FAILED;
	if (hCipher < 0 || hCipher >= MAX_HANDLES) return INVALID_PARA;
	if (Ctrl->enAlg != IN_UNF_CIPHER_ALG_AES ||
	    Ctrl->enKeyLen != IN_UNF_CIPHER_KEY_AES_128BIT ||
	    Ctrl->enBitWidth != IN_UNF_CIPHER_BIT_WIDTH_128BIT ||
	    (Ctrl->enWorkMode != IN_UNF_CIPHER_WORK_MODE_ECB &&
	     Ctrl->enWorkMode != IN_UNF_CIPHER_WORK_MODE_CBC))
		return SET_PARA_FAILED;
	g_ctrl[hCipher] = *Ctrl;
	g_ctrl_set[hCipher] = 1;
	return 0;
}

/* map the driver DMA buffer once (needs /dev/aes open and /dev/mem) */
static int cipher_map(void)
{
	if (g_vbuf) return 0;
	if (g_aes_fd < 0) return -1;
	uint32_t pb = 0;
	if (ioctl(g_aes_fd, IOCTL_AES_GET_PBUFF, &pb) < 0 || !pb) return -1;
	int mfd = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
	if (mfd < 0) return -1;
	void *m = mmap(NULL, AES_DMA_BUF, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, (off_t)pb);
	if (m == MAP_FAILED) {
		close(mfd);
		return -1;
	}
	g_mem_fd = mfd;
	g_pbuf = pb;
	g_vbuf = m;
	return 0;
}

/* run [src, src+len) through the engine in 2 KiB DMA jobs; CBC chains the
 * last ciphertext block of a job into the IV of the next one. Key, IV and
 * data words are passed to the engine exactly as the caller supplies them. */
static int cipher_run(int h, const unsigned int *src, unsigned int *dst, unsigned int len, int decrypt)
{
	if (h < 0 || h >= MAX_HANDLES || !g_ctrl_set[h]) return INVALID_PARA;
	if (cipher_map() < 0) return FAILURE;

	const IN_UNF_CIPHER_CTRL *c = &g_ctrl[h];
	const unsigned char *s = (const unsigned char *)src;
	unsigned char *d = (unsigned char *)dst;
	uint32_t iv[4];
	memcpy(iv, c->IV, sizeof(iv));

	for (unsigned int off = 0; off < len;) {
		unsigned int n = len - off;
		if (n > AES_HALF) n = AES_HALF;

		memcpy(g_vbuf, s + off, n);
		struct aes_para p;
		memset(&p, 0, sizeof(p));
		p.mem_p = g_pbuf;
		memcpy(p.aeskey, c->key, sizeof(p.aeskey));
		memcpy(p.aesiv, iv, sizeof(p.aesiv));
		p.src_addr_p = g_pbuf;
		p.src_addr_v = KSEG0 | g_pbuf;
		p.dst_addr_p = g_pbuf + AES_HALF;
		p.dst_addr_v = KSEG0 | (g_pbuf + AES_HALF);
		p.enAlg = c->enAlg;
		p.enBitWidth = c->enBitWidth;
		p.enWorkMode = c->enWorkMode;
		p.enKeyLen = c->enKeyLen;
		p.dataLen = n;

		uint32_t next_iv[4];
		memcpy(next_iv, g_vbuf + n - 16, sizeof(next_iv));   /* last input block */

		if (ioctl(g_aes_fd, IOCTL_AES_SET_PARA, &p) < 0) return FAILURE;
		if (ioctl(g_aes_fd, decrypt ? IOCTL_AES_START_DE : IOCTL_AES_START_EN, 0) < 0) return FAILURE;

		memcpy(d + off, g_vbuf + AES_HALF, n);
		if (c->enWorkMode == IN_UNF_CIPHER_WORK_MODE_CBC) {
			if (decrypt)
				memcpy(iv, next_iv, sizeof(iv));          /* last ciphertext block = input */
			else
				memcpy(iv, d + off + n - 16, sizeof(iv)); /* last ciphertext block = output */
		}
		off += n;
	}
	return 0;
}

EXPORT int SU_CIPHER_Encrypt(int hCipher, unsigned int *srcAddr,
                             unsigned int *dstAddr, unsigned int dataLen)
{
	if (hCipher < 0 || !srcAddr || !dstAddr) return INVALID_PARA;
	if (dataLen == 0 || dataLen > 1024 * 1024 || (dataLen & 15)) return SET_DATALEN_ERR;
	return cipher_run(hCipher, srcAddr, dstAddr, dataLen, 0);
}

EXPORT int SU_CIPHER_Decrypt(int hCipher, unsigned int *srcAddr,
                             unsigned int *dstAddr, unsigned int dataLen)
{
	if (hCipher < 0 || !srcAddr || !dstAddr) return INVALID_PARA;
	if (dataLen == 0 || dataLen > 1024 * 1024 || (dataLen & 15)) return SET_DATALEN_ERR;
	return cipher_run(hCipher, srcAddr, dstAddr, dataLen, 1);
}

EXPORT int SU_CIPHER_Malloc_Buffer(int size)
{
	if (size < CIPHER_BUF_MIN || size > CIPHER_BUF_MAX) return -1;
	void *buf = malloc((size_t)size);
	if (!buf) return -1;
	g_cipher_buf = buf;
	g_cipher_buf_len = size;
	return 0;
}

EXPORT int SU_CIPHER_Free_Buffer(void)
{
	if (!g_cipher_buf) return -1;
	free(g_cipher_buf);
	g_cipher_buf = NULL;
	g_cipher_buf_len = 0;
	return 0;
}

EXPORT int cipher_htonl(const void *src, void *dst, unsigned int len)
{
	if (len < CIPHER_BUF_MIN || len > CIPHER_BUF_MAX) return -1;
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
