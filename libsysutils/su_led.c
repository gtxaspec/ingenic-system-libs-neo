/* su_led.c -- LED control via regulator sysfs. SPDX-License-Identifier: MIT */

#include "sysutils/su_misc.h"
#include "su_internal.h"

#define LED_FMT "/proc/board/power/led%d"

EXPORT int SU_LED_Command(int ledNum, SULedCmd cmd)
{
	char path[64];
	snprintf(path, sizeof(path), LED_FMT, ledNum);
	return write_file(path, (cmd == LED_ON) ? "1" : "0", 1);
}
