/* su_adc.c -- Analog to digital conversion. SPDX-License-Identifier: MIT */

#include <stdbool.h>
#include <sys/ioctl.h>

#include "sysutils/su_adc.h"
#include "su_internal.h"

#define ADC_CHANNELS_MAX 8

static int g_adc_fds[ADC_CHANNELS_MAX] = {-1, -1, -1, -1, -1, -1, -1, -1};
static bool g_adc_init;

static const char *adc_dev_prefix(void)
{
	static const char *prefix;
	if (!prefix) {
		if (access("/dev/ingenic_adc_aux_0", F_OK) == 0)
			prefix = "/dev/ingenic_adc_aux_";
		else
			prefix = "/dev/jz_adc_aux_";
	}
	return prefix;
}

EXPORT int SU_ADC_Init(void)
{
	if (g_adc_init)
		return 0;

	const char *prefix = adc_dev_prefix();
	for (unsigned i = 0; i < ADC_CHANNELS_MAX; i++) {
		char path[64];
		snprintf(path, sizeof(path), "%s%u", prefix, i);
		g_adc_fds[i] = open_cloexec(path, O_RDONLY);
		if (g_adc_fds[i] < 0) {
			if (i == 0) return -1;
			break;
		}
	}
	g_adc_init = true;
	return 0;
}

EXPORT int SU_ADC_Exit(void)
{
	if (!g_adc_init)
		return 0;
	for (unsigned i = 0; i < ADC_CHANNELS_MAX; i++) {
		if (g_adc_fds[i] >= 0) {
			close(g_adc_fds[i]);
			g_adc_fds[i] = -1;
		}
	}
	g_adc_init = false;
	return 0;
}

EXPORT int SU_ADC_EnableChn(uint32_t chn_num)
{
	if (!g_adc_init || chn_num >= ADC_CHANNELS_MAX || g_adc_fds[chn_num] < 0)
		return -1;
	return ioctl(g_adc_fds[chn_num], 0) < 0 ? -1 : 0;
}

EXPORT int SU_ADC_DisableChn(uint32_t chn_num)
{
	if (!g_adc_init || chn_num >= ADC_CHANNELS_MAX || g_adc_fds[chn_num] < 0)
		return -1;
	return ioctl(g_adc_fds[chn_num], 1) < 0 ? -1 : 0;
}

EXPORT int SU_ADC_GetChnValue(uint32_t chn_num, int *value)
{
	if (!g_adc_init || chn_num >= ADC_CHANNELS_MAX || !value || g_adc_fds[chn_num] < 0)
		return -1;
	ssize_t n = read(g_adc_fds[chn_num], value, sizeof(*value));
	return (n == sizeof(*value)) ? 0 : -1;
}
