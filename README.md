# ingenic-system-libs-neo

Open-source, drop-in replacements for the Ingenic SDK `libalog.so` and `libsysutils.so`.

## Overview

These libraries are required by Ingenic's `libimp.so` media processing SDK. This project provides clean implementations that are:

- **ABI-compatible** — same exported symbols, same struct layouts, same calling conventions
- **Smaller** — ~51% smaller than vendor binaries
- **Thread-safe** — mutex-protected shared state
- **Universal** — single source supports all Ingenic SoCs via runtime detection

## Supported SoCs

A1, T10, T20, T21, T23, T30, T31, T32, T33, T40, T41

## Libraries

### libalog.so

IMP logging framework and Android log transport layer.

- **IMP logging API** — `imp_log_fun()`, `IMP_Log_Get_Option()`, log level/file/output routing
- **Android log writer** — `__android_log_write()` and friends, writing to `/dev/log_main`
- **Android log format** — `android_log_formatLogLine()`, filter rules, log entry parsing

### libsysutils.so

System utility wrappers for hardware peripherals.

| Module | Functions | Hardware |
|--------|-----------|----------|
| **SU_Base** | System info, RTC time/alarm, reboot, suspend | `/proc/cpuinfo`, `/dev/rtc0`, `/dev/mem` |
| **SU_ADC** | Analog-to-digital conversion | `/dev/jz_adc_aux_*` or `/dev/ingenic_adc_aux_*` |
| **SU_Key** | GPIO key/button input events | `/dev/input/`, gpio-keys sysfs |
| **SU_CIPHER** | Hardware AES/DES encryption | `/dev/aes`, `/dev/jz-des` |
| **SU_LED** | LED control via regulator | `/proc/board/power/led*` |

Device paths are detected at runtime — no compile-time SoC selection needed.

## Building

```bash
# Cross-compile for MIPS (Thingino toolchain)
make CROSS_COMPILE=mipsel-linux-

# Strip for deployment
make strip CROSS_COMPILE=mipsel-linux-

# Install
make install DESTDIR=/path/to/staging CROSS_COMPILE=mipsel-linux-
```

Outputs: `libalog.so`, `libalog.a`, `libsysutils.so`, `libsysutils.a`

## Size comparison

| Library | Vendor (T31) | This project | Reduction |
|---------|-------------:|-------------:|----------:|
| libalog.so | 34,900 B | 17,812 B | **-49%** |
| libsysutils.so | 29,500 B | 13,688 B | **-54%** |
| **Total** | **64,400 B** | **31,500 B** | **-51%** |

## License

MIT
