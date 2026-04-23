/*
 * su_cipher.h -- Hardware AES/DES encryption.
 * ABI-compatible with Ingenic SDK libsysutils.so.
 * SPDX-License-Identifier: MIT
 */

#ifndef __SU_CIPHER_H__
#define __SU_CIPHER_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	IN_UNF_CIPHER_ALG_AES = 0x0,
	IN_UNF_CIPHER_ALG_DES = 0x1,
} IN_UNF_CIPHER_ALG;

typedef enum {
	IN_UNF_CIPHER_WORK_MODE_ECB   = 0x0,
	IN_UNF_CIPHER_WORK_MODE_CBC   = 0x1,
	IN_UNF_CIPHER_WORK_MODE_OTHER = 0x2,
} IN_UNF_CIPHER_WORK_MODE;

typedef enum {
	IN_UNF_CIPHER_KEY_AES_128BIT = 0x0,
} IN_UNF_CIPHER_KEY_LENGTH;

typedef enum {
	IN_UNF_CIPHER_BIT_WIDTH_128BIT = 0x0,
} IN_UNF_CIPHER_BIT_WIDTH;

typedef struct {
	unsigned int key[4];
	unsigned int IV[4];
	unsigned int enDataLen;
	IN_UNF_CIPHER_ALG       enAlg;
	IN_UNF_CIPHER_BIT_WIDTH enBitWidth;
	IN_UNF_CIPHER_WORK_MODE enWorkMode;
	IN_UNF_CIPHER_KEY_LENGTH enKeyLen;
} IN_UNF_CIPHER_CTRL;

#define REINIT           -10
#define INIT_FAILED      -11
#define FAILED_GETHANDLE -12
#define INVALID_PARA     -13
#define SET_PARA_FAILED  -14
#define FAILURE          -15
#define SET_DATALEN_ERR  -16
#define EXIT_ERR         -17
#define UNINIT           -18
#define FAILED_DESHANDLE -19

int SU_CIPHER_Init(void);
int SU_CIPHER_Exit(void);
int SU_CIPHER_DES_Init(void);
int SU_CIPHER_DES_Exit(void);
int SU_CIPHER_DES_Test(void);
int SU_CIPHER_CreateHandle(void);
int SU_CIPHER_DestroyHandle(int fd);
int SU_CIPHER_ConfigHandle(int hCipher, IN_UNF_CIPHER_CTRL *Ctrl);
int SU_CIPHER_Encrypt(int hCipher, unsigned int *srcAddr,
                      unsigned int *dstAddr, unsigned int dataLen);
int SU_CIPHER_Decrypt(int hCipher, unsigned int *srcAddr,
                      unsigned int *dstAddr, unsigned int dataLen);
int SU_CIPHER_Malloc_Buffer(int size);
int SU_CIPHER_Free_Buffer(void);
int cipher_htonl(const void *src, void *dst, unsigned int len);

#ifdef __cplusplus
}
#endif

#endif /* __SU_CIPHER_H__ */
