/* su_cipher.c -- Hardware AES/DES + buffer management. SPDX-License-Identifier: MIT */

#include <stdlib.h>
#include <sys/ioctl.h>

#include "sysutils/su_cipher.h"
#include "su_internal.h"

#define AES_DEV        "/dev/aes"
#define DES_DEV        "/dev/jz-des"
#define CIPHER_BUF_MIN 16
#define CIPHER_BUF_MAX 0x100000

static int g_aes_fd = -1;
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
	if (g_aes_fd < 0) return EXIT_ERR;
	close(g_aes_fd);
	g_aes_fd = -1;
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
	if (g_des_fd < 0) return EXIT_ERR;
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
	close(fd);
	return 0;
}

EXPORT int SU_CIPHER_ConfigHandle(int hCipher, IN_UNF_CIPHER_CTRL *Ctrl)
{
	if (hCipher < 0 || !Ctrl) return INVALID_PARA;
	return ioctl(hCipher, 0, Ctrl) < 0 ? SET_PARA_FAILED : 0;
}

EXPORT int SU_CIPHER_Encrypt(int hCipher, unsigned int *srcAddr,
                             unsigned int *dstAddr, unsigned int dataLen)
{
	if (hCipher < 0 || !srcAddr || !dstAddr) return INVALID_PARA;
	if (dataLen == 0 || dataLen > 1024 * 1024) return SET_DATALEN_ERR;
	struct { unsigned int *src, *dst; unsigned int len; } args = { srcAddr, dstAddr, dataLen };
	return ioctl(hCipher, 1, &args) < 0 ? FAILURE : 0;
}

EXPORT int SU_CIPHER_Decrypt(int hCipher, unsigned int *srcAddr,
                             unsigned int *dstAddr, unsigned int dataLen)
{
	if (hCipher < 0 || !srcAddr || !dstAddr) return INVALID_PARA;
	if (dataLen == 0 || dataLen > 1024 * 1024) return SET_DATALEN_ERR;
	struct { unsigned int *src, *dst; unsigned int len; } args = { srcAddr, dstAddr, dataLen };
	return ioctl(hCipher, 2, &args) < 0 ? FAILURE : 0;
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
