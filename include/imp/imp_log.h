/*
 * imp_log.h -- IMP logging interface.
 * ABI-compatible with Ingenic SDK libalog.so.
 * SPDX-License-Identifier: MIT
 */

#ifndef __IMP_LOG_H__
#define __IMP_LOG_H__

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
	IMP_LOG_LEVEL_UNKNOWN,
	IMP_LOG_LEVEL_RESERVED,
	IMP_LOG_LEVEL_VERBOSE,
	IMP_LOG_LEVEL_DEBUG,
	IMP_LOG_LEVEL_INFO,
	IMP_LOG_LEVEL_WARN,
	IMP_LOG_LEVEL_ERROR,
	IMP_LOG_LEVEL_FATAL,
	IMP_LOG_LEVEL_SILENT
};

#define IMP_LOG_LEVEL_DEFAULT IMP_LOG_LEVEL_DEBUG

#define IMP_LOG_OUT_STDOUT     0
#define IMP_LOG_OUT_LOCAL_FILE 1
#define IMP_LOG_OUT_SERVER     2
#define IMP_LOG_OUT_DEFAULT    IMP_LOG_OUT_STDOUT

#define IMP_LOG_OP_PID_SHIFT    0
#define IMP_LOG_OP_USTIME_SHIFT 1
#define IMP_LOG_OP_MODULE_SHIFT 2
#define IMP_LOG_OP_FILE_SHIFT   3
#define IMP_LOG_OP_FUNC_SHIFT   4
#define IMP_LOG_OP_LINE_SHIFT   5

#define IMP_LOG_OP_PID    (1 << IMP_LOG_OP_PID_SHIFT)
#define IMP_LOG_OP_USTIME (1 << IMP_LOG_OP_USTIME_SHIFT)
#define IMP_LOG_OP_MODULE (1 << IMP_LOG_OP_MODULE_SHIFT)
#define IMP_LOG_OP_FILE   (1 << IMP_LOG_OP_FILE_SHIFT)
#define IMP_LOG_OP_FUNC   (1 << IMP_LOG_OP_FUNC_SHIFT)
#define IMP_LOG_OP_LINE   (1 << IMP_LOG_OP_LINE_SHIFT)
#define IMP_LOG_OP_NONE   0
#define IMP_LOG_OP_ALL    (IMP_LOG_OP_PID | IMP_LOG_OP_USTIME | IMP_LOG_OP_MODULE \
                         | IMP_LOG_OP_FILE | IMP_LOG_OP_FUNC | IMP_LOG_OP_LINE)
#define IMP_LOG_OP_DEFAULT IMP_LOG_OP_ALL

void imp_log_fun(int le, int op, int out, const char *tag,
                 const char *file, int line, const char *func,
                 const char *fmt, ...)
	__attribute__((format(printf, 8, 9)));

void imp_log_fun_vl(int le, int op, int out, const char *tag,
                    const char *file, int line, const char *func,
                    const char *fmt, va_list ap);

void IMP_Log_Set_Option(int op);
int  IMP_Log_Get_Option(void);

#define IMP_LOG_PRINT(tag, le, op, fmt, ...) \
	imp_log_fun(le, op, IMP_LOG_OUT_STDOUT, tag, \
	            __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

#define IMP_LOG_TO_FILE(tag, le, op, fmt, ...) \
	imp_log_fun(le, op, IMP_LOG_OUT_LOCAL_FILE, tag, \
	            __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

#define IMP_LOG_TO_SERVER(tag, le, op, fmt, ...) \
	imp_log_fun(le, op, IMP_LOG_OUT_SERVER, tag, \
	            __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

#define IMP_LOG_GET_OPTION IMP_Log_Get_Option()
#define IMP_LOG IMP_LOG_TO_SERVER

#define IMP_LOG_UNK(tag, fmt, ...)     IMP_LOG(tag, IMP_LOG_LEVEL_UNKNOWN, IMP_LOG_GET_OPTION, fmt, ##__VA_ARGS__)
#define IMP_LOG_VERBOSE(tag, fmt, ...) IMP_LOG(tag, IMP_LOG_LEVEL_VERBOSE, IMP_LOG_GET_OPTION, fmt, ##__VA_ARGS__)
#define IMP_LOG_DBG(tag, fmt, ...)     IMP_LOG(tag, IMP_LOG_LEVEL_DEBUG,   IMP_LOG_GET_OPTION, fmt, ##__VA_ARGS__)
#define IMP_LOG_INFO(tag, fmt, ...)    IMP_LOG(tag, IMP_LOG_LEVEL_INFO,    IMP_LOG_GET_OPTION, fmt, ##__VA_ARGS__)
#define IMP_LOG_WARN(tag, fmt, ...)    IMP_LOG(tag, IMP_LOG_LEVEL_WARN,    IMP_LOG_GET_OPTION, fmt, ##__VA_ARGS__)
#define IMP_LOG_ERR(tag, fmt, ...)     IMP_LOG(tag, IMP_LOG_LEVEL_ERROR,   IMP_LOG_GET_OPTION, fmt, ##__VA_ARGS__)
#define IMP_LOG_FATAL(tag, fmt, ...)   IMP_LOG(tag, IMP_LOG_LEVEL_FATAL,   IMP_LOG_GET_OPTION, fmt, ##__VA_ARGS__)
#define IMP_LOG_SILENT(tag, fmt, ...)  IMP_LOG(tag, IMP_LOG_LEVEL_SILENT,  IMP_LOG_GET_OPTION, fmt, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* __IMP_LOG_H__ */
