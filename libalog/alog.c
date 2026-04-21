/*
 * alog.c -- IMP logging layer and Android log transport.
 *
 * Drop-in replacement for Ingenic SDK libalog.so.
 * Supports all Ingenic SoCs (A1, T10–T41).
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <pthread.h>

#include "imp/imp_log.h"

/* ------------------------------------------------------------------
 *  Constants
 * ------------------------------------------------------------------ */

#define FMT_BUF_SIZE      1000
#define LOG_FILE_PATH_MAX 100
#define LOG_DEV_PATH      "/dev/log_main"
#define LOG_BUF_COUNT     4     /* main, radio, events, system */
#define PRIORITY_IOCTL    0xc004ae08

#define EXPORT __attribute__((visibility("default")))

/* Android log priority — matches IMP_LOG_LEVEL_* by design. */
enum android_LogPriority {
	ANDROID_LOG_UNKNOWN = 0,
	ANDROID_LOG_DEFAULT,
	ANDROID_LOG_VERBOSE,
	ANDROID_LOG_DEBUG,
	ANDROID_LOG_INFO,
	ANDROID_LOG_WARN,
	ANDROID_LOG_ERROR,
	ANDROID_LOG_FATAL,
	ANDROID_LOG_SILENT,
};

enum {
	LOG_ID_MAIN   = 0,
	LOG_ID_RADIO  = 1,
	LOG_ID_EVENTS = 2,
	LOG_ID_SYSTEM = 3,
};

/* Android log format types (for the format API stubs). */
enum AndroidLogPrintFormat {
	FORMAT_OFF = 0,
	FORMAT_BRIEF,
	FORMAT_PROCESS,
	FORMAT_TAG,
	FORMAT_THREAD,
	FORMAT_RAW,
	FORMAT_TIME,
	FORMAT_THREADTIME,
	FORMAT_LONG,
};

/* ------------------------------------------------------------------
 *  Types
 * ------------------------------------------------------------------ */

/* Kernel logger entry header — matches the Ingenic /dev/log_main driver. */
struct logger_entry {
	uint16_t len;
	uint16_t __pad;
	int32_t  pid;
	int32_t  tid;
	int32_t  sec;
	int32_t  nsec;
	char     msg[];
};

/* Parsed log entry for the format API. */
typedef struct {
	struct timeval tv;
	int            priority;
	int32_t        pid;
	int32_t        tid;
	const char    *tag;
	size_t         tagLen;
	const char    *message;
	size_t         messageLen;
} AndroidLogEntry;

/* Filter rule node. */
typedef struct FilterInfo {
	char               *tag;
	int                 pri;
	struct FilterInfo  *next;
} FilterInfo;

/* Opaque log format handle. */
typedef struct {
	int          format;
	FilterInfo  *filters;
} AndroidLogFormat;

/* Event tag map — stub. */
typedef struct {
	int dummy;
} EventTagMap;

/* IMP format buffer: 1000 chars of content + write position. */
struct fmt_buf {
	char data[FMT_BUF_SIZE];
	int  pos;
};

/* ------------------------------------------------------------------
 *  Forward declarations
 * ------------------------------------------------------------------ */

EXPORT int __android_log_write(int priority, const char *tag, const char *msg);

/* ------------------------------------------------------------------
 *  Global state — protected by g_lock.
 * ------------------------------------------------------------------ */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static int  g_log_level  = IMP_LOG_LEVEL_DEBUG;
static int  g_log_option = IMP_LOG_OP_ALL;
static char g_log_file[LOG_FILE_PATH_MAX] = "";
static FILE *g_log_fp;

static struct fmt_buf *g_buf;
static bool g_buf_init;

/* Android log transport state. */
static int  g_log_fds[LOG_BUF_COUNT] = { -1, -1, -1, -1 };
static bool g_log_open;

static pthread_once_t g_log_once = PTHREAD_ONCE_INIT;

/* priority_control() cached fd — initialized via pthread_once. */
static int g_prio_fd = -1;

/* ------------------------------------------------------------------
 *  Helpers
 * ------------------------------------------------------------------ */

static const char g_prio_char[] = "??.VDIWEFS";

static const char *g_prio_name[] = {
	"UNK  ", "RSV  ", "VERB ", "DEBUG", "INFO ",
	"WARN ", "ERROR", "FATAL", "SILENT",
};

static char prio_to_char(int pri)
{
	if (pri < 0 || pri >= (int)sizeof(g_prio_char))
		return '?';
	return g_prio_char[pri];
}

static const char *prio_to_name(int pri)
{
	if (pri < 0 || pri >= (int)(sizeof(g_prio_name) / sizeof(g_prio_name[0])))
		return "UNK  ";
	return g_prio_name[pri];
}

static const char *basename_of(const char *path)
{
	if (!path)
		return "?";
	const char *slash = strrchr(path, '/');
	return slash ? slash + 1 : path;
}

/* ------------------------------------------------------------------
 *  Format buffer management
 *
 *  Single global buffer with mutex protection. Matches the original
 *  semantics (one shared 1004-byte allocation) while being thread-safe.
 * ------------------------------------------------------------------ */

static void buf_ensure(void)
{
	if (g_buf_init)
		return;

	if (!g_buf) {
		g_buf = calloc(1, sizeof(*g_buf));
		if (!g_buf) {
			fputs("libalog: format buffer allocation failed\n", stderr);
			return;
		}
	}
	g_buf->pos = 0;
	g_buf_init = true;
}

static void buf_reset(void)
{
	if (g_buf)
		g_buf->pos = 0;
}

static void buf_append_v(const char *fmt, va_list ap)
{
	if (!g_buf)
		return;

	int remain = FMT_BUF_SIZE - g_buf->pos;
	if (remain <= 0)
		return;

	int n = vsnprintf(g_buf->data + g_buf->pos, (size_t)remain, fmt, ap);
	if (n > 0 && n < remain) {
		g_buf->pos += n;
	} else if (n >= remain) {
		g_buf->pos = FMT_BUF_SIZE - 1;
		g_buf->data[FMT_BUF_SIZE - 1] = '\0';
	}
}

static void buf_append(const char *fmt, ...)
	__attribute__((format(printf, 1, 2)));

static void buf_append(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	buf_append_v(fmt, ap);
	va_end(ap);
}

/* ------------------------------------------------------------------
 *  Android log transport
 *
 *  Opens /dev/log_main and writes entries via writev() in the format
 *  expected by the Ingenic kernel driver: priority byte + tag\0 + msg\0
 * ------------------------------------------------------------------ */

static void log_transport_init(void)
{
	int fd = open(LOG_DEV_PATH, O_WRONLY | O_CLOEXEC);
	if (fd < 0) {
		/* No log device — all writes will silently drop. */
		return;
	}

	for (int i = 0; i < LOG_BUF_COUNT; i++)
		g_log_fds[i] = fd;

	g_log_open = true;
}

static int log_writev(int buf_id, const struct iovec *iov, int iovcnt)
{
	if (buf_id < 0 || buf_id >= LOG_BUF_COUNT)
		return -1;

	int fd = g_log_fds[buf_id];
	if (fd < 0)
		return -1;

	ssize_t ret;
	do {
		ret = writev(fd, iov, iovcnt);
	} while (ret < 0 && errno == EINTR);

	return (int)ret;
}

static int log_write_entry(int buf_id, int priority,
                           const char *tag, const char *msg)
{
	if (!tag)
		tag = "";
	if (!msg)
		msg = "";

	pthread_once(&g_log_once, log_transport_init);

	uint8_t prio_byte = (uint8_t)priority;
	struct iovec vec[3];
	vec[0].iov_base = &prio_byte;
	vec[0].iov_len  = 1;
	vec[1].iov_base = (void *)tag;
	vec[1].iov_len  = strlen(tag) + 1;
	vec[2].iov_base = (void *)msg;
	vec[2].iov_len  = strlen(msg) + 1;

	return log_writev(buf_id, vec, 3);
}

/* ------------------------------------------------------------------
 *  IMP logging layer — public API
 * ------------------------------------------------------------------ */

EXPORT int imp_get_log_level(void)
{
	return g_log_level;
}

EXPORT void imp_set_log_level(int level)
{
	g_log_level = level;
}

EXPORT int IMP_Log_Get_Option(void)
{
	return g_log_option;
}

EXPORT void IMP_Log_Set_Option(int op)
{
	g_log_option = op;
}

EXPORT void imp_set_log_file(const char *path)
{
	if (!path)
		return;

	size_t len = strlen(path);
	if (len >= LOG_FILE_PATH_MAX) {
		fputs("libalog: log file path too long\n", stderr);
		return;
	}

	pthread_mutex_lock(&g_lock);
	memcpy(g_log_file, path, len + 1);
	pthread_mutex_unlock(&g_lock);
}

EXPORT const char *imp_get_log_file(void)
{
	return g_log_file;
}

static void prio_init(void)
{
	g_prio_fd = open(LOG_DEV_PATH, O_WRONLY | O_CLOEXEC);
}

EXPORT int priority_control(void)
{
	static pthread_once_t prio_once = PTHREAD_ONCE_INIT;
	pthread_once(&prio_once, prio_init);

	if (g_prio_fd < 0)
		return g_log_level;

	int level;
	if (ioctl(g_prio_fd, PRIORITY_IOCTL, &level) < 0)
		return g_log_level;

	return level;
}

EXPORT void imp_log_format_option(int option)
{
	buf_ensure();

	if (option & IMP_LOG_OP_USTIME) {
		struct timeval tv;
		gettimeofday(&tv, NULL);
		buf_append("%ld.%06ld ", (long)tv.tv_sec, (long)tv.tv_usec);
	}

	if (option & IMP_LOG_OP_PID) {
		buf_append("[%d] ", (int)getpid());
	}
}

EXPORT void imp_log_format_info(const char *fmt, ...)
{
	buf_ensure();
	va_list ap;
	va_start(ap, fmt);
	buf_append_v(fmt, ap);
	va_end(ap);
}

EXPORT void imp_log_format_info_vl(const char *fmt, va_list ap)
{
	buf_ensure();
	buf_append_v(fmt, ap);
}

EXPORT void imp_log_print_buf(void)
{
	if (!g_buf)
		return;

	fputs(g_buf->data, stdout);
	buf_reset();
}

EXPORT void imp_log_buf(void)
{
	if (!g_buf)
		return;

	if (!g_log_fp) {
		if (g_log_file[0] == '\0')
			return;
		g_log_fp = fopen(g_log_file, "a");
		if (!g_log_fp) {
			fprintf(stderr, "libalog: cannot open log file: %s\n",
			        g_log_file);
			return;
		}
	}

	fputs(g_buf->data, g_log_fp);
	buf_reset();
}

EXPORT void imp_log_to_logcat(int priority, const char *tag)
{
	if (!g_buf)
		return;

	__android_log_write(priority, tag, g_buf->data);
	buf_reset();
}

/*
 * Format: HH:MM:SS.mmm tag(pid) [LEVEL] file:line: message\n
 * Logcat path writes raw entry to /dev/log_main (reader formats it).
 */
EXPORT void imp_log_fun(int le, int op, int out, const char *tag,
                        const char *file, int line, const char *func,
                        const char *fmt, ...)
{
	int threshold = priority_control();
	if (le < threshold)
		return;

	pthread_mutex_lock(&g_lock);
	buf_ensure();
	buf_reset();

	if (op & IMP_LOG_OP_USTIME) {
		struct timeval tv;
		gettimeofday(&tv, NULL);
		struct tm tm_buf;
		localtime_r(&tv.tv_sec, &tm_buf);
		buf_append("%02d:%02d:%02d.%03ld ",
		           tm_buf.tm_hour, tm_buf.tm_min,
		           tm_buf.tm_sec, tv.tv_usec / 1000);
	}
	if (out != IMP_LOG_OUT_SERVER) {
		if (op & IMP_LOG_OP_MODULE)
			buf_append("%s", tag ? tag : "?");
		if (op & IMP_LOG_OP_PID)
			buf_append("(%d)", (int)getpid());
		if ((op & IMP_LOG_OP_MODULE) || (op & IMP_LOG_OP_PID))
			buf_append(" ");
		buf_append("[%-5s] ", prio_to_name(le));
		if (op & IMP_LOG_OP_FILE)
			buf_append("%s:", basename_of(file));
		if (op & IMP_LOG_OP_LINE)
			buf_append("%d: ", line);
	}
	if (op & IMP_LOG_OP_FUNC)
		buf_append("[%s] ", func ? func : "?");

	va_list ap;
	va_start(ap, fmt);
	buf_append_v(fmt, ap);
	va_end(ap);

	switch (out) {
	case IMP_LOG_OUT_STDOUT:
		imp_log_print_buf();
		break;
	case IMP_LOG_OUT_LOCAL_FILE:
		imp_log_buf();
		break;
	default:
		imp_log_to_logcat(le, tag);
		break;
	}

	pthread_mutex_unlock(&g_lock);
}

EXPORT void imp_log_fun_vl(int le, int op, int out, const char *tag,
                           const char *file, int line, const char *func,
                           const char *fmt, va_list ap)
{
	if (le < g_log_level)
		return;

	pthread_mutex_lock(&g_lock);
	buf_ensure();
	buf_reset();

	if (op & IMP_LOG_OP_USTIME) {
		struct timeval tv;
		gettimeofday(&tv, NULL);
		struct tm tm_buf;
		localtime_r(&tv.tv_sec, &tm_buf);
		buf_append("%02d:%02d:%02d.%03ld ",
		           tm_buf.tm_hour, tm_buf.tm_min,
		           tm_buf.tm_sec, tv.tv_usec / 1000);
	}
	if (out != IMP_LOG_OUT_SERVER) {
		if (op & IMP_LOG_OP_MODULE)
			buf_append("%s", tag ? tag : "?");
		if (op & IMP_LOG_OP_PID)
			buf_append("(%d)", (int)getpid());
		if ((op & IMP_LOG_OP_MODULE) || (op & IMP_LOG_OP_PID))
			buf_append(" ");
		buf_append("[%-5s] ", prio_to_name(le));
		if (op & IMP_LOG_OP_FILE)
			buf_append("%s:", basename_of(file));
		if (op & IMP_LOG_OP_LINE)
			buf_append("%d: ", line);
	}
	if (op & IMP_LOG_OP_FUNC)
		buf_append("[%s] ", func ? func : "?");

	buf_append_v(fmt, ap);

	switch (out) {
	case IMP_LOG_OUT_STDOUT:
		imp_log_print_buf();
		break;
	case IMP_LOG_OUT_LOCAL_FILE:
		imp_log_buf();
		break;
	default:
		imp_log_to_logcat(le, tag);
		break;
	}

	pthread_mutex_unlock(&g_lock);
}

/* ------------------------------------------------------------------
 *  Android log writer API — __android_log_*
 *
 *  These are the symbols that libimp.so (and any AOSP-style code)
 *  resolve at runtime. All paths go through log_write_entry() which
 *  writev's to /dev/log_main.
 * ------------------------------------------------------------------ */

EXPORT int __android_log_write(int priority, const char *tag, const char *msg)
{
	return log_write_entry(LOG_ID_MAIN, priority, tag, msg);
}

EXPORT int __android_log_buf_write(int buf_id, int priority,
                                   const char *tag, const char *msg)
{
	return log_write_entry(buf_id, priority, tag, msg);
}

EXPORT int __android_log_vprint(int priority, const char *tag,
                                const char *fmt, va_list ap)
{
	char buf[1024];
	vsnprintf(buf, sizeof(buf), fmt, ap);
	return __android_log_write(priority, tag, buf);
}

EXPORT int __android_log_print(int priority, const char *tag,
                               const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int ret = __android_log_vprint(priority, tag, fmt, ap);
	va_end(ap);
	return ret;
}

EXPORT int __android_log_buf_print(int buf_id, int priority, const char *tag,
                                   const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	return log_write_entry(buf_id, priority, tag, buf);
}

EXPORT void __android_log_assert(const char *cond, const char *tag,
                                 const char *fmt, ...)
{
	char buf[1024];
	if (fmt) {
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(buf, sizeof(buf), fmt, ap);
		va_end(ap);
	} else if (cond) {
		snprintf(buf, sizeof(buf), "Assertion failed: %s", cond);
	} else {
		snprintf(buf, sizeof(buf), "Unspecified assertion failed");
	}

	__android_log_write(ANDROID_LOG_FATAL, tag, buf);
	abort();
}

EXPORT int __android_log_bwrite(int32_t tag, const void *payload, size_t len)
{
	struct iovec vec[2];
	vec[0].iov_base = &tag;
	vec[0].iov_len  = sizeof(tag);
	vec[1].iov_base = (void *)payload;
	vec[1].iov_len  = len;

	pthread_once(&g_log_once, log_transport_init);
	return log_writev(LOG_ID_EVENTS, vec, 2);
}

EXPORT int __android_log_btwrite(int32_t tag, char type,
                                 const void *payload, size_t len)
{
	struct iovec vec[3];
	vec[0].iov_base = &tag;
	vec[0].iov_len  = sizeof(tag);
	vec[1].iov_base = &type;
	vec[1].iov_len  = 1;
	vec[2].iov_base = (void *)payload;
	vec[2].iov_len  = len;

	pthread_once(&g_log_once, log_transport_init);
	return log_writev(LOG_ID_EVENTS, vec, 3);
}

EXPORT int __android_log_dev_available(void)
{
	pthread_once(&g_log_once, log_transport_init);
	return g_log_open ? 1 : 0;
}

/* ------------------------------------------------------------------
 *  Fake log device — fakeLogOpen / fakeLogClose / fakeLogWritev
 *
 *  The original AOSP fake_log_device.c. On these cameras, the real
 *  /dev/log_main is always used; the fakeLog functions exist only as
 *  exported symbols for ABI compatibility. Our "fake" writes to stderr.
 * ------------------------------------------------------------------ */

EXPORT void fakeLogOpen(const char *path __attribute__((unused)),
                        int flags __attribute__((unused)))
{
	/* nothing — transport init happens lazily via pthread_once */
}

EXPORT void fakeLogClose(int fd __attribute__((unused)))
{
	/* nothing */
}

EXPORT int fakeLogWritev(int fd __attribute__((unused)),
                         const struct iovec *iov, int iovcnt)
{
	ssize_t total = 0;
	for (int i = 0; i < iovcnt; i++) {
		ssize_t n = write(STDERR_FILENO, iov[i].iov_base, iov[i].iov_len);
		if (n < 0)
			return -1;
		total += n;
	}
	return (int)total;
}

/* ------------------------------------------------------------------
 *  Android log format API
 *
 *  Minimal but functional implementations. These parse log entries from
 *  the kernel ring buffer and format them for display. Used by logcat-
 *  style tools; most camera apps never call them.
 * ------------------------------------------------------------------ */

EXPORT AndroidLogFormat *android_log_format_new(void)
{
	AndroidLogFormat *fmt = calloc(1, sizeof(*fmt));
	if (fmt)
		fmt->format = FORMAT_BRIEF;
	return fmt;
}

EXPORT void android_log_format_free(AndroidLogFormat *fmt)
{
	if (!fmt)
		return;

	FilterInfo *f = fmt->filters;
	while (f) {
		FilterInfo *next = f->next;
		free(f->tag);
		free(f);
		f = next;
	}
	free(fmt);
}

EXPORT int android_log_setPrintFormat(AndroidLogFormat *fmt, int formatId)
{
	if (!fmt || formatId < FORMAT_OFF || formatId > FORMAT_LONG)
		return -1;
	fmt->format = formatId;
	return 0;
}

EXPORT int android_log_formatFromString(const char *str)
{
	if (!str)
		return FORMAT_OFF;
	if (strcmp(str, "brief")      == 0) return FORMAT_BRIEF;
	if (strcmp(str, "process")    == 0) return FORMAT_PROCESS;
	if (strcmp(str, "tag")        == 0) return FORMAT_TAG;
	if (strcmp(str, "thread")     == 0) return FORMAT_THREAD;
	if (strcmp(str, "raw")        == 0) return FORMAT_RAW;
	if (strcmp(str, "time")       == 0) return FORMAT_TIME;
	if (strcmp(str, "threadtime") == 0) return FORMAT_THREADTIME;
	if (strcmp(str, "long")       == 0) return FORMAT_LONG;
	return -1;
}

EXPORT int android_log_addFilterRule(AndroidLogFormat *fmt,
                                     const char *rule)
{
	if (!fmt || !rule)
		return -1;

	/* Parse "tag:priority" — e.g. "MyApp:D" */
	const char *colon = strchr(rule, ':');
	if (!colon || colon[1] == '\0')
		return -1;

	size_t tag_len = (size_t)(colon - rule);
	char pri_char = colon[1];
	int pri;

	switch (pri_char) {
	case 'V': case 'v': pri = ANDROID_LOG_VERBOSE; break;
	case 'D': case 'd': pri = ANDROID_LOG_DEBUG;   break;
	case 'I': case 'i': pri = ANDROID_LOG_INFO;    break;
	case 'W': case 'w': pri = ANDROID_LOG_WARN;    break;
	case 'E': case 'e': pri = ANDROID_LOG_ERROR;   break;
	case 'F': case 'f': pri = ANDROID_LOG_FATAL;   break;
	case 'S': case 's': pri = ANDROID_LOG_SILENT;  break;
	case '*':           pri = ANDROID_LOG_DEFAULT;  break;
	default:            return -1;
	}

	FilterInfo *f = calloc(1, sizeof(*f));
	if (!f)
		return -1;

	f->tag = strndup(rule, tag_len);
	if (!f->tag) {
		free(f);
		return -1;
	}
	f->pri = pri;
	f->next = fmt->filters;
	fmt->filters = f;
	return 0;
}

EXPORT int android_log_addFilterString(AndroidLogFormat *fmt,
                                       const char *str)
{
	if (!fmt || !str)
		return -1;

	char *copy = strdup(str);
	if (!copy)
		return -1;

	int ret = 0;
	char *saveptr;
	char *tok = strtok_r(copy, " \t", &saveptr);
	while (tok) {
		if (android_log_addFilterRule(fmt, tok) < 0)
			ret = -1;
		tok = strtok_r(NULL, " \t", &saveptr);
	}

	free(copy);
	return ret;
}

EXPORT int android_log_shouldPrintLine(AndroidLogFormat *fmt,
                                       const char *tag, int priority)
{
	if (!fmt)
		return 1;
	if (!tag)
		tag = "";

	int result_pri = ANDROID_LOG_DEFAULT;
	for (FilterInfo *f = fmt->filters; f; f = f->next) {
		if (strcmp(f->tag, "*") == 0 || strcmp(f->tag, tag) == 0)
			result_pri = f->pri;
	}

	if (result_pri == ANDROID_LOG_DEFAULT)
		result_pri = ANDROID_LOG_INFO;

	return priority >= result_pri;
}

EXPORT int android_log_processLogBuffer(struct logger_entry *entry,
                                        AndroidLogEntry *out)
{
	if (!entry || !out)
		return -1;
	if (entry->len < 3)
		return -1;

	out->tv.tv_sec  = entry->sec;
	out->tv.tv_usec = entry->nsec / 1000;
	out->pid = entry->pid;
	out->tid = entry->tid;
	out->priority = entry->msg[0];

	out->tag = entry->msg + 1;
	const char *msg_start = memchr(out->tag, '\0', entry->len - 1);
	if (!msg_start)
		return -1;
	msg_start++;

	out->tagLen = (size_t)(msg_start - out->tag) - 1;
	out->message = msg_start;
	out->messageLen = entry->len - (size_t)(msg_start - entry->msg);

	return 0;
}

EXPORT int android_log_processBinaryLogBuffer(
	struct logger_entry *entry __attribute__((unused)),
	AndroidLogEntry *out __attribute__((unused)))
{
	/* Binary event logs are never used on these cameras. */
	return -1;
}

EXPORT char *android_log_formatLogLine(AndroidLogFormat *fmt,
                                       char *defaultBuffer,
                                       size_t defaultBufferSize,
                                       const AndroidLogEntry *entry,
                                       size_t *outLength)
{
	if (!entry || !defaultBuffer || defaultBufferSize == 0)
		return NULL;

	const char *tag = entry->tag ? entry->tag : "";
	const char *msg = entry->message ? entry->message : "";
	char pc = prio_to_char(entry->priority);

	struct tm tm_buf;
	time_t sec = entry->tv.tv_sec;
	localtime_r(&sec, &tm_buf);

	int n;
	int format = fmt ? fmt->format : FORMAT_BRIEF;

	switch (format) {
	case FORMAT_TAG:
		n = snprintf(defaultBuffer, defaultBufferSize,
		             "%c/%-8s: %s\n", pc, tag, msg);
		break;
	case FORMAT_PROCESS:
		n = snprintf(defaultBuffer, defaultBufferSize,
		             "%c(%5d) %s  (%s)\n",
		             pc, entry->pid, msg, tag);
		break;
	case FORMAT_THREAD:
		n = snprintf(defaultBuffer, defaultBufferSize,
		             "%c(%5d:%5d) %s\n",
		             pc, entry->pid, entry->tid, msg);
		break;
	case FORMAT_RAW:
		n = snprintf(defaultBuffer, defaultBufferSize, "%s\n", msg);
		break;
	case FORMAT_TIME:
		n = snprintf(defaultBuffer, defaultBufferSize,
		             "%02d-%02d %02d:%02d:%02d.%03ld %c/%-8s: %s\n",
		             tm_buf.tm_mon + 1, tm_buf.tm_mday,
		             tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
		             entry->tv.tv_usec / 1000, pc, tag, msg);
		break;
	case FORMAT_THREADTIME:
		n = snprintf(defaultBuffer, defaultBufferSize,
		             "%02d-%02d %02d:%02d:%02d.%03ld %5d %5d %c %-8s: %s\n",
		             tm_buf.tm_mon + 1, tm_buf.tm_mday,
		             tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
		             entry->tv.tv_usec / 1000,
		             entry->pid, entry->tid, pc, tag, msg);
		break;
	case FORMAT_LONG:
		n = snprintf(defaultBuffer, defaultBufferSize,
		             "[ %02d-%02d %02d:%02d:%02d.%03ld %5d:%5d %c/%-8s ]\n%s\n\n",
		             tm_buf.tm_mon + 1, tm_buf.tm_mday,
		             tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
		             entry->tv.tv_usec / 1000,
		             entry->pid, entry->tid, pc, tag, msg);
		break;
	default: /* FORMAT_BRIEF */
		n = snprintf(defaultBuffer, defaultBufferSize,
		             "%c/%-8s(%5d): %s\n", pc, tag, entry->pid, msg);
		break;
	}

	if (n < 0)
		return NULL;
	if (outLength)
		*outLength = (size_t)n < defaultBufferSize
		           ? (size_t)n : defaultBufferSize - 1;
	return defaultBuffer;
}

EXPORT int android_log_printLogLine(AndroidLogFormat *fmt,
                                    int fd,
                                    const AndroidLogEntry *entry)
{
	char buf[4096];
	size_t len;
	char *line = android_log_formatLogLine(fmt, buf, sizeof(buf),
	                                       entry, &len);
	if (!line)
		return -1;

	ssize_t ret;
	do {
		ret = write(fd, line, len);
	} while (ret < 0 && errno == EINTR);

	return (int)ret;
}

/* ------------------------------------------------------------------
 *  Event tag map — stubs
 * ------------------------------------------------------------------ */

EXPORT EventTagMap *android_openEventTagMap(
	const char *path __attribute__((unused)))
{
	return calloc(1, sizeof(EventTagMap));
}

EXPORT void android_closeEventTagMap(EventTagMap *map)
{
	free(map);
}

EXPORT const char *android_lookupEventTag(
	EventTagMap *map __attribute__((unused)),
	int tag __attribute__((unused)))
{
	return NULL;
}

/* ------------------------------------------------------------------
 *  logprint_run_tests — no-op
 * ------------------------------------------------------------------ */

EXPORT int logprint_run_tests(void)
{
	return 0;
}
