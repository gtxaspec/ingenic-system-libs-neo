/*
 * su_misc.h -- Key events and LED control.
 * ABI-compatible with Ingenic SDK libsysutils.so.
 * SPDX-License-Identifier: MIT
 */

#ifndef __SU_MISC_H__
#define __SU_MISC_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	KEY_RELEASED = 0,
	KEY_PRESSED  = 1,
} SUKeyEvent;

typedef enum {
	LED_OFF = 0,
	LED_ON  = 1,
} SULedCmd;

int SU_Key_OpenEvent(void);
int SU_Key_CloseEvent(int evfd);
int SU_Key_ReadEvent(int evfd, int *keyCode, SUKeyEvent *event);
int SU_Key_EnableEvent(int keyCode);
int SU_Key_DisableEvent(int keyCode);
int SU_LED_Command(int ledNum, SULedCmd cmd);

#ifdef __cplusplus
}
#endif

#endif /* __SU_MISC_H__ */
