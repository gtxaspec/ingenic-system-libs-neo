/*
 * su_pm.h -- Power management (T32/T33).
 * ABI-compatible with Ingenic SDK libsysutils.so.
 * SPDX-License-Identifier: MIT
 */

#ifndef __SU_PM_H__
#define __SU_PM_H__

#ifdef __cplusplus
extern "C" {
#endif

int  SU_PM_Init(int *config);
int  SU_PM_DeInit(void);
void SU_PM_Sleep(int ms);
int  SU_PM_RequestWakeLock(const char *name);
int  SU_PM_ReleaseWakeLock(const char *name);
int  SU_PM_AddThreadListen(void);
int  SU_PM_DelThreadListen(int handle);
int  SU_PM_TheadSuspend(void);
int  SU_PM_ThreadResume(void);
int  SU_PM_WaitThreadSuspend(int timeout_ms);
int  SU_PM_EventSend(int event, int mode);
int  SU_PM_GetEvent(void);
int  SU_PM_InitListenLock(int count);
int  SU_PM_GetListenLockNums(void);
void SU_PM_DumpList(void);
int  SU_PM_Set_CPUOnline(int online);
int  SU_PM_Get_CPUOnlineNums(void);

#ifdef __cplusplus
}
#endif

#endif /* __SU_PM_H__ */
