/*
 * su_adc.h -- Analog-to-digital conversion.
 * ABI-compatible with Ingenic SDK libsysutils.so.
 * SPDX-License-Identifier: MIT
 */

#ifndef __SU_ADC_H__
#define __SU_ADC_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int SU_ADC_Init(void);
int SU_ADC_Exit(void);
int SU_ADC_EnableChn(uint32_t chn_num);
int SU_ADC_DisableChn(uint32_t chn_num);
int SU_ADC_GetChnValue(uint32_t chn_num, int *value);

#ifdef __cplusplus
}
#endif

#endif /* __SU_ADC_H__ */
