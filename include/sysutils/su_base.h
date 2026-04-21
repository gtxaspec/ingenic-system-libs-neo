/*
 * su_base.h -- System base functions (info, time, alarm, power).
 * ABI-compatible with Ingenic SDK libsysutils.so.
 * SPDX-License-Identifier: MIT
 */

#ifndef __SU_BASE_H__
#define __SU_BASE_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEVICE_ID_MAGIC     "53ef"
#define DEVICE_ID_MAGIC_LEN 4
#define DEVICE_ID_LEN       32
#define MAX_INFO_LEN        64

typedef struct {
	char chr[MAX_INFO_LEN];
} SUModelNum;

typedef struct {
	char chr[MAX_INFO_LEN];
} SUVersion;

typedef union {
	char chr[MAX_INFO_LEN];
	uint8_t hex[MAX_INFO_LEN];
} SUDevID;

typedef struct {
	int sec;
	int min;
	int hour;
	int mday;
	int mon;
	int year;
} SUTime;

typedef enum {
	WKUP_KEY   = 1,
	WKUP_ALARM = 2,
} SUWkup;

int SU_Base_GetModelNumber(SUModelNum *modelNum);
int SU_Base_GetVersion(SUVersion *version);
int SU_Base_GetDevID(SUDevID *devID);
int SU_Base_GetTime(SUTime *time);
int SU_Base_SetTime(SUTime *time);
int SU_Base_SUTime2Raw(SUTime *suTime, uint32_t *rawTime);
int SU_Base_Raw2SUTime(uint32_t *rawTime, SUTime *suTime);
int SU_Base_SetAlarm(SUTime *time);
int SU_Base_GetAlarm(SUTime *time);
int SU_Base_EnableAlarm(void);
int SU_Base_DisableAlarm(void);
int SU_Base_PollingAlarm(uint32_t timeoutMsec);
int SU_Base_Shutdown(void);
int SU_Base_Reboot(void);
int SU_Base_Suspend(void);
int SU_Base_SetWkupMode(SUWkup mode);

#ifdef __cplusplus
}
#endif

#endif /* __SU_BASE_H__ */
